/*
 * Dropbear - a SSH2 server
 *
 * Copied from OpenSSH-3.5p1 source, modified by Matt Johnston 2003
 * 
 * Author: Tatu Ylonen <ylo@cs.hut.fi>
 * Copyright (c) 1995 Tatu Ylonen <ylo@cs.hut.fi>, Espoo, Finland
 *                    All rights reserved
 * Allocating a pseudo-terminal, and making it the controlling tty.
 *
 * As far as I am concerned, the code I have written for this software
 * can be used freely for any purpose.  Any derived versions of this
 * software must be clearly marked as such, and if the derived work is
 * incompatible with the protocol description in the RFC file, it must be
 * called by a name other than "ssh" or "Secure Shell".
 */

/*RCSID("OpenBSD: sshpty.c,v 1.7 2002/06/24 17:57:20 deraadt Exp ");*/

#include "includes.h"
#include "dbutil.h"
#include "errno.h"
#include "sshpty.h"

/* Pty allocated with _getpty gets broken if we do I_PUSH:es to it. */
#if defined(HAVE__GETPTY) || defined(HAVE_OPENPTY)
#undef HAVE_DEV_PTMX
#endif

#ifdef HAVE_PTY_H
# include <pty.h>
#endif
#if defined(USE_DEV_PTMX) && defined(HAVE_STROPTS_H)
# include <stropts.h>
#endif

#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

/*
 * Allocates and opens a pty.  Returns 0 if no pty could be allocated, or
 * nonzero if a pty was successfully allocated.  On success, open file
 * descriptors for the pty and tty sides and the name of the tty side are
 * returned (the buffer must be able to hold at least 64 characters).
 */

#include <stlx/pty.h>

int
pty_allocate(int *ptyfd, int *ttyfd, char *namebuf, int namebuflen)
{
	if (pty_create(ptyfd, ttyfd) < 0) {
		dropbear_log(LOG_WARNING, "pty_allocate: pty_create failed");
		return 0;
	}
	snprintf(namebuf, namebuflen, "stellux-pty/%d", *ttyfd);
	return 1;
}

/* Releases the tty.  No-op on Stellux (handle-based PTYs, no fs nodes). */

void
pty_release(const char *tty_name)
{
	(void)tty_name;
}

/* No-op on Stellux — PTY slave is wired via proc_set_handle, not controlling tty. */

void
pty_make_controlling_tty(int *ttyfd, const char *tty_name)
{
	(void)ttyfd;
	(void)tty_name;
}

/* Changes the window size associated with the pty. */

void
pty_change_window_size(int ptyfd, int row, int col,
	int xpixel, int ypixel)
{
	struct winsize w;

	w.ws_row = row;
	w.ws_col = col;
	w.ws_xpixel = xpixel;
	w.ws_ypixel = ypixel;
	(void) ioctl(ptyfd, TIOCSWINSZ, &w);
}

/* No-op on Stellux — handle-based PTYs have no filesystem ownership. */

void
pty_setowner(struct passwd *pw, const char *tty_name)
{
	(void)pw;
	(void)tty_name;
}
