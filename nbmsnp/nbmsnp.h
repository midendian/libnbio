/*
 * nbmsnp - Example code for libnbio
 * Copyright (c) 2002 Adam Fritzler <mid@zigamorph.net>
 *
 * nbmsnp is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License (version 2) as published by the Free
 * Software Foundation.
 *
 * nbmsnp is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifndef __NBMSNP_H__
#define __NBMSNP_H__

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdarg.h>

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include <libnbio.h>

#ifdef WIN32
#define snprintf _snprintf
#endif

#define MODULE_NAME "nbmsnp"

#define MSN_MAX_NAME_LEN 128

typedef unsigned long ntrid_t;

#define MI_STATE_DISCONNECTED 0
#define MI_STATE_CONNECTING   1
#define MI_STATE_SENTVER      2
#define MI_STATE_SENTINF      3
#define MI_STATE_SENTUSR_I    4
#define MI_STATE_XFR_TO_NS    5
#define MI_STATE_NS_SENTVER   6
#define MI_STATE_NS_SENTINF   7
#define MI_STATE_NS_SENTUSR_I 8
#define MI_STATE_NS_SENTUSR_S 9
#define MI_STATE_LOGGEDIN    10

#define MI_RUNFLAG_NONE    0x00000000

/*
 * Output all I/O operations.
 *
 * XXX Should be seperated into several smaller options.
 *
 */
#define MI_RUNFLAG_VERBOSE 0x00000002

/*
 * Pings are currently enabled and will be sent every n seconds until a
 * response is received (and the flag is disabled).
 *
 * This is a run-time state, not a configurable option.
 *
 */
#define MI_RUNFLAG_PNGENA  0x00010000

/*
 * Pings are temporarily being curbed because a synchronous operation was
 * issued.  The flag is cleared when the response is received.
 *
 * This is a run-time state, not a configurable option.
 *
 */
#define MI_RUNFLAG_PNGCURBED 0x00020000

/* fdt->priv->mi */
struct msninfo {
	int state;
	unsigned long flags;

	nbio_t *nb;

	/* MSN-related */
	const char *login;
	const char *password;
	nbio_fd_t *nsconn;
	ntrid_t nexttrid;
	time_t lastnsdata;
};

#define MCI_TYPE_DS 1
#define MCI_TYPE_NS 2
#define MCI_TYPE_SB 3

#define MCI_STATE_CONNECTING 0
#define MCI_STATE_WAITINGFORCMD 1
#define MCI_STATE_WAITINGFORPAYLOAD 2

#define MCI_FLAG_NONE    0x00000000
#define MCI_FLAG_HASNAME 0x00000001 /* mci->name is valid */
#define MCI_FLAG_HASHOSTNAME 0x00000002 /* mci->hostname is valid */
#define MCI_FLAG_HASCKI  0x00000004
#define MCI_FLAG_HASSESSID 0x00000008
#define MCI_FLAG_SBREADY 0x00000010

/* fdt->priv */
struct msnconninfo {
	int type;
	int state;
	unsigned long flags;
	nbio_fd_t *fdt;
	struct msninfo *mi;
	char name[MSN_MAX_NAME_LEN+1];
	char hostname[128+1];
	char cki[64+1];
	int sessid;
};


/* nbmsnp.c */
int addmsnconn(struct msninfo *mi, const char *host, int type, const char *name, const char *cki, int sessid);
int fdterror(nbio_fd_t *fdt, const char *msg);
int fdtperror(nbio_fd_t *fdt, const char *func, int err, const char *msg);
int imfromservice(nbio_fd_t *srcfdt, const char *name, const char *msg);
char *dprintf_ctime(void);

/* special.c */
int admin_isadmin(const char *name);
int admin_add(const char *name);
void admin_remove(const char *name);
int handlespecial(nbio_fd_t *srcfdt, const char *name, const char *msg);
void trid_timeout(struct msninfo *mi, int n);
int handlenscmdreply(nbio_fd_t *fdt, char *buf, char *payload);

/* commands.c */
ntrid_t msn_sendadd(nbio_fd_t *fdt, const char *list, const char *handle, const char *custom);
int sendmsncmd(nbio_fd_t *fdt, const char *cmd, int usetrid, ntrid_t trid, const char *args);
ntrid_t msn_sendcal(nbio_fd_t *fdt, const char *name);
void wl_trywaiting(struct msninfo *mi);
ntrid_t msn_sendxfr(nbio_fd_t *fdt, const char *type);
ntrid_t sendmsnmsg_typing(struct msninfo *mi, nbio_fd_t *passedfdt, const char *name);
ntrid_t sendmsnmsg(struct msninfo *mi, nbio_fd_t *passedfdt, const char *name, const char *msg);
ntrid_t msn_sendchg(nbio_fd_t *fdt, const char *state);
ntrid_t msn_sendans(nbio_fd_t *fdt, const char *handle, const char *authinfo, int sessid);
ntrid_t msn_sendver(nbio_fd_t *fdt, const char *vers);
ntrid_t msn_sendlst(nbio_fd_t *fdt, const char *l);
ntrid_t msn_sendusr(nbio_fd_t *fdt, const char *sp, const char *step, const char *info);
ntrid_t msn_sendinf(nbio_fd_t *fdt, const char *data);
void msn_sendpng(nbio_fd_t *fdt);

int processmsncmd(nbio_fd_t *fdt, char *buf, char *payload);

int startsync(struct msninfo *mi);
int doingsync(void);
int loglist(struct msninfo *mi, const char *list, const char *requestor, const char *fn);


#define dprintf(x) { \
	printf("%s  %s: " x, dprintf_ctime(), MODULE_NAME); \
	fflush(stdout); \
}

#ifdef WIN32
__inline void dvprintf(const char *format, ...)
{
	va_list lst;

	printf("%s %s: ", dprintf_ctime(), MODULE_NAME);

	va_start(lst, format);

	vprintf(format, lst);
	fflush(stdout);

	return;
}

#else
#define dvprintf(x, y...) { \
	printf("%s  %s: " x, dprintf_ctime(), MODULE_NAME, y); \
	fflush(stdout); \
}
#endif

#endif /* __NBMSNP_H__ */

