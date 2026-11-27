#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

#include "idle.hpp"

using namespace std;

static const char *APP_ID = "sway-audio-idle-inhibit";
static const char *REASON = "Audio is playing";

static const char *LINE_INHIBITED = "IDLE INHIBITED";
static const char *LINE_FAILED = "IDLE INHIBIT FAILED";
static const char *LINE_NOT_INHIBITED = "NOT IDLE INHIBITED";

static const char *MANAGER_NAME = "org.gnome.SessionManager";
static const char *MANAGER_PATH = "/org/gnome/SessionManager";
static const char *MANAGER_INTERFACE = "org.gnome.SessionManager";
// GSM_INHIBITOR_FLAG_IDLE
static const uint32_t MANAGER_FLAG_IDLE = 8;
// Calls to the session manager are given up after this long, so that a
// session manager that hangs can't stop the logind fallback.
static const uint64_t MANAGER_CALL_TIMEOUT_USEC = 3 * 1000 * 1000;

// Takes a logind "idle" inhibitor lock and returns a copy of its fd, or -1 if
// the lock could not be taken.
//
// A new system bus connection is opened for every call. sd-bus only runs the
// authentication handshake of a connection when the connection is used, so a
// connection opened at startup and left untouched until the first audio stream
// is dropped by the bus daemon (auth_timeout, 30 s on dbus-daemon's system
// bus) or given up by sd-bus itself (90 s). The call then fails with ENOTCONN.
// The inhibitor is held by the returned fd, so closing the connection
// afterwards does not release it.
static int take_inhibit_lock() {
	sd_bus *bus = nullptr;
	int ret = sd_bus_open_system(&bus);
	if (ret < 0) {
		fprintf(stderr, "Could not connect to the system bus: %s\n",
				strerror(-ret));
		return -1;
	}

	int lock_fd = -1;
	sd_bus_message *message = nullptr;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	ret = sd_bus_call_method(
		bus, "org.freedesktop.login1", "/org/freedesktop/login1",
		"org.freedesktop.login1.Manager", "Inhibit", &error, &message, "ssss",
		"idle", APP_ID, REASON, "block");
	if (ret < 0) {
		fprintf(stderr, "Could not send inhibit signal! %s: %s\n",
				error.name ? error.name : "(unknown)",
				error.message ? error.message : strerror(-ret));
	} else {
		int fd = -1;
		ret = sd_bus_message_read(message, "h", &fd);
		if (ret < 0) {
			fprintf(stderr, "Could not get DBus response: %s\n",
					strerror(-ret));
		} else {
			// Clone the FD (will be invalid once we unref the message)
			lock_fd = fcntl(fd, F_DUPFD_CLOEXEC, 3);
			if (lock_fd < 0) {
				fprintf(stderr, "Could not copy lock fd: %s\n",
						strerror(errno));
			}
		}
	}

	sd_bus_error_free(&error);
	sd_bus_message_unref(message);
	sd_bus_flush_close_unref(bus);
	return lock_fd;
}

Idle::Idle() {
	wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (wakeFd < 0) {
		fprintf(stderr, "Could not create eventfd: %s\n", strerror(errno));
		exit(1);
	}
	thread(&Idle::run, this).detach();
}

void Idle::update(bool isRunning) {
	{
		lock_guard<mutex> guard(stateMutex);
		active = isRunning;
		pendingReport = true;
	}
	uint64_t one = 1;
	if (write(wakeFd, &one, sizeof(one)) < 0 && errno != EAGAIN) {
		fprintf(stderr, "Could not wake the idle thread: %s\n",
				strerror(errno));
	}
}

void Idle::connectSessionBus() {
	int ret = sd_bus_open_user(&sessionBus);
	if (ret < 0) {
		// No session bus: only the logind lock can be used.
		sessionBus = nullptr;
		return;
	}
	sd_bus_set_method_call_timeout(sessionBus, MANAGER_CALL_TIMEOUT_USEC);

	// Watch the owner before asking for it, so that no change is missed.
	ret = sd_bus_add_match(sessionBus, nullptr,
						   "type='signal',sender='org.freedesktop.DBus',"
						   "path='/org/freedesktop/DBus',"
						   "interface='org.freedesktop.DBus',"
						   "member='NameOwnerChanged',"
						   "arg0='org.gnome.SessionManager'",
						   &Idle::onOwnerChanged, this);
	if (ret < 0) {
		fprintf(stderr, "Could not watch the session manager: %s\n",
				strerror(-ret));
		dropSessionBus();
		return;
	}

	sd_bus_message *reply = nullptr;
	if (sd_bus_call_method(sessionBus, "org.freedesktop.DBus",
						   "/org/freedesktop/DBus", "org.freedesktop.DBus",
						   "GetNameOwner", nullptr, &reply, "s",
						   MANAGER_NAME) >= 0) {
		const char *owner = nullptr;
		if (sd_bus_message_read(reply, "s", &owner) >= 0 && owner) {
			managerOwner = owner;
		}
	}
	sd_bus_message_unref(reply);
}

void Idle::dropSessionBus() {
	// The session manager drops our inhibitor along with the connection.
	sd_bus_flush_close_unref(sessionBus);
	sessionBus = nullptr;
	managerOwner.clear();
	haveCookie = false;
}

int Idle::onOwnerChanged(sd_bus_message *message, void *userdata,
						 sd_bus_error *) {
	Idle *self = (Idle *)userdata;
	const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
	if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0 ||
		strcmp(name, MANAGER_NAME) != 0) {
		return 0;
	}
	// An inhibitor belongs to the owner that handed out its cookie. Release
	// it there, in case that process is still running; the new owner gets an
	// inhibitor of its own.
	self->uninhibitManager();
	self->managerOwner = newOwner;
	self->managerFailed = false;
	self->lockFailed = false;
	return 0;
}

void Idle::inhibitManager() {
	sd_bus_message *reply = nullptr;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	string owner = managerOwner;
	int ret = sd_bus_call_method(sessionBus, owner.c_str(), MANAGER_PATH,
								 MANAGER_INTERFACE, "Inhibit", &error, &reply,
								 "susu", APP_ID, 0u, REASON, MANAGER_FLAG_IDLE);
	if (ret >= 0)
		ret = sd_bus_message_read(reply, "u", &cookie);
	if (ret >= 0) {
		haveCookie = true;
		cookieOwner = owner;
	} else {
		fprintf(stderr, "Could not inhibit idle through %s: %s\n", MANAGER_NAME,
				error.message ? error.message : strerror(-ret));
		managerFailed = true;
	}
	sd_bus_error_free(&error);
	sd_bus_message_unref(reply);
}

void Idle::uninhibitManager() {
	if (!haveCookie)
		return;
	haveCookie = false;
	if (!sessionBus)
		return;
	// Sent to the unique name, so it can only reach the owner that handed out
	// the cookie. It fails harmlessly when that owner is gone.
	sd_bus_call_method(sessionBus, cookieOwner.c_str(), MANAGER_PATH,
					   MANAGER_INTERFACE, "Uninhibit", nullptr, nullptr, "u",
					   cookie);
}

void Idle::sync(bool wantActive, bool report) {
	if (report)
		lockFailed = false; // retry logind on every audio event

	if (wantActive) {
		if (!haveCookie && !managerOwner.empty() && !managerFailed &&
			sessionBus) {
			inhibitManager();
		}
		if (haveCookie) {
			// Hand over: the session manager's inhibitor is in place.
			if (lockFd >= 0) {
				close(lockFd);
				lockFd = -1;
			}
		} else if (lockFd < 0 && !lockFailed) {
			lockFd = take_inhibit_lock();
			lockFailed = lockFd < 0;
		}
	} else {
		managerFailed = false;
		uninhibitManager();
		if (lockFd >= 0) {
			close(lockFd);
			lockFd = -1;
		}
	}

	const char *line;
	if (!wantActive) {
		line = LINE_NOT_INHIBITED;
	} else if (haveCookie || lockFd >= 0) {
		line = LINE_INHIBITED;
	} else {
		line = LINE_FAILED;
	}
	// Besides one line per audio event, print when the state changes on its
	// own (the session manager came or went), but not before the first event.
	if (report || (lastLine && line != lastLine)) {
		cout << line << endl;
		lastLine = line;
	}
}

void Idle::run() {
	connectSessionBus();

	for (;;) {
		if (sessionBus) {
			int ret;
			while ((ret = sd_bus_process(sessionBus, nullptr)) > 0) {
			}
			if (ret < 0) {
				fprintf(stderr, "Lost the session bus: %s\n", strerror(-ret));
				dropSessionBus();
			}
		}

		bool wantActive, report;
		{
			lock_guard<mutex> guard(stateMutex);
			wantActive = active;
			report = pendingReport;
			pendingReport = false;
		}
		sync(wantActive, report);

		struct pollfd fds[2] = {
			{wakeFd, POLLIN, 0},
			{-1, 0, 0},
		};
		int timeoutMs = -1;
		if (sessionBus) {
			fds[1].fd = sd_bus_get_fd(sessionBus);
			fds[1].events = (short)sd_bus_get_events(sessionBus);
			uint64_t until = 0;
			if (sd_bus_get_timeout(sessionBus, &until) > 0 &&
				until != UINT64_MAX) {
				struct timespec now;
				clock_gettime(CLOCK_MONOTONIC, &now);
				uint64_t nowUsec =
					(uint64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
				timeoutMs =
					until > nowUsec ? (int)((until - nowUsec + 999) / 1000) : 0;
			}
		}
		if (poll(fds, 2, timeoutMs) < 0 && errno != EINTR) {
			fprintf(stderr, "poll failed: %s\n", strerror(errno));
		}
		if (fds[0].revents & POLLIN) {
			uint64_t count;
			while (read(wakeFd, &count, sizeof(count)) > 0) {
			}
		}
	}
}
