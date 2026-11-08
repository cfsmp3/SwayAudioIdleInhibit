#pragma once

#if HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#include <systemd/sd-login.h>
#elif HAVE_ELOGIND
#include <elogind/sd-bus.h>
#include <elogind/sd-login.h>
#endif

class Idle {
	int fd = -1;

	void block();
	void release_block();

  public:
	void update(bool isRunning);
};
