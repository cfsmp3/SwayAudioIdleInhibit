#pragma once

#if HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#elif HAVE_ELOGIND
#include <elogind/sd-bus.h>
#include <elogind/sd-login.h>
#endif

#include <cstdint>
#include <mutex>
#include <string>

// Holds an idle inhibitor while audio is active.
//
// When gnome-session's session manager owns org.gnome.SessionManager on the
// session bus (GNOME, GNOME Flashback, Regolith), the inhibitor is taken there,
// which every idle handler in such a session honours. Otherwise a logind
// "idle" inhibitor lock is taken. A worker thread owns the session
// bus connection and every inhibitor; update() only records the new state.
class Idle {
  public:
	Idle();
	void update(bool isRunning);

  private:
	// Shared with the PulseAudio thread, guarded by stateMutex.
	std::mutex stateMutex;
	bool active = false;
	bool pendingReport = false;

	// eventfd that wakes the worker after update().
	int wakeFd = -1;

	// Owned by the worker thread.
	sd_bus *sessionBus = nullptr;
	std::string managerOwner; // unique name owning the session manager
	std::string cookieOwner;  // unique name that handed out cookie
	uint32_t cookie = 0;
	bool haveCookie = false;
	bool managerFailed = false; // Inhibit failed in this active period
	int lockFd = -1;			// logind inhibitor lock
	bool lockFailed = false;	// logind lock failed since the last update
	const char *lastLine = nullptr;

	void run();
	void connectSessionBus();
	void dropSessionBus();
	void sync(bool wantActive, bool report);
	void inhibitManager();
	void uninhibitManager();
	static int onOwnerChanged(sd_bus_message *message, void *userdata,
							  sd_bus_error *error);
};
