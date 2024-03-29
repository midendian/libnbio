/*
 * libnbio - Portable wrappers for non-blocking sockets
 * Copyright (c) 2000-2005 Adam Fritzler <mid@zigamorph.net>, et al
 *
 * libnbio is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (version 2.1) as published by
 * the Free Software Foundation.
 *
 * libnbio is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifndef __LIBNBIO_H__
#define __LIBNBIO_H__

#ifdef __cplusplus
extern "C" {
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#include <time.h> /* for time_t */

#define NBIO_MAX_DELIMITER_LEN 4 /* normally one of \n, \r, \r\n, \r\n\r\n */

#ifdef NBIO_USE_WINSOCK2

#include <winsock2.h>
#include <errcompat.h>

typedef SOCKET nbio_sockfd_t;

#else

typedef int nbio_sockfd_t;
#endif

typedef struct nbio_buf_s {
	unsigned char *data;
	int len;
	int offset;
	time_t trigger; /* time at which the event should be triggered */
	void *intdata;
	struct nbio_buf_s *next;
} nbio_buf_t;


#define NBIO_FDTYPE_LISTENER  0
#define NBIO_FDTYPE_STREAM    1
#define NBIO_FDTYPE_DGRAM     2


#define NBIO_EVENT_READ           0 /* buffer read (or socket is readable) */
#define NBIO_EVENT_WRITE          1 /* buffer written (or socket is writable) */
#define NBIO_EVENT_ERROR          2 /* error encountered */
#define NBIO_EVENT_EOF            3 /* EOF encountered */
#define NBIO_EVENT_CONNECTED      4 /* connection succeeded */
#define NBIO_EVENT_CONNECTFAILED  5 /* connection failed */
#define NBIO_EVENT_RESOLVERESULT  6 /* result of a resolver operation */
#define NBIO_EVENT_TIMEREXPIRE    7 /* timer expired */
#define NBIO_EVENT_INCOMINGCONN   8 /* (listener only) new incoming conn */

typedef unsigned short nbio_fdt_flags_t;

#define NBIO_FDT_FLAG_NONE         0x0000

/*
 * Raw mode works just like normal mode, except that rxvecs are not
 * used at all.  When the READ event is trigered, it means the socket
 * is ready, nothing else.  The user callback will have to read the
 * data manually into its own buffer.
 *
 */
#define NBIO_FDT_FLAG_RAW          0x0001
#define NBIO_FDT_FLAG_RAWREAD      0x0004

/*
 * The close-on-flush flag tells the system to close the fdt as soon
 * as there is no more data waiting to be written. If there are no
 * pending txvecs, then this equates to "close on next call to nbio_poll()".
 *
 * This is useful for doing things where you would normally do:
 *    write();
 *    close();
 * With libnbio, you should do:
 *    nbio_addtxvector();
 *    nbio_setcloseonflush(fdt, 1);
 *
 */
#define NBIO_FDT_FLAG_CLOSEONFLUSH 0x0002

/*
 * Normally on delimited streams, the delimiter will be zeroed before
 * making it to the read callback.  Setting this flag will prevent
 * this behavior and the delimiter will be in the returned data.
 */
#define NBIO_FDT_FLAG_KEEPDELIM    0x0008

#define NBIO_FDT_FLAG_IGNORE       0x0010
#define NBIO_FDT_FLAG_CLOSED       0x0020

/*
 * An internally managed connection.
 */
#define NBIO_FDT_FLAG_INTERNAL     0x0040


typedef struct nbio_delim_s {
	unsigned char len;
	unsigned char data[NBIO_MAX_DELIMITER_LEN];
	struct nbio_delim_s *next;
} nbio_delim_t;


typedef struct nbio_fd_s {
	int type;
	nbio_sockfd_t fd;
	nbio_fdt_flags_t flags;
	int (*handler)(void *, int event, struct nbio_fd_s *); /* nbio_handler_t */
	void *priv;
	nbio_delim_t *delims;
	int pri;
	nbio_buf_t *rxchain;
	nbio_buf_t *rxchain_freelist;
	nbio_buf_t *txchain;
	nbio_buf_t *txchain_tail;
	nbio_buf_t *txchain_freelist;
	void *intdata;
	int timerinterval;
	time_t timernextfire;
	struct nbio_fd_s *next;
} nbio_fd_t;

typedef int (*nbio_handler_t)(void *, int event, nbio_fd_t *);

/* used only by resolv.c */
struct nbio__resolvinfo;

typedef struct {
	void *fdlist;
	int maxpri;
	void *intdata;
	void *priv;
	struct nbio__resolvinfo *resolv;
#if 0
#ifdef NBIO_USEKQUEUE
	int kq;
	struct kevent *kqevents;
	int kqeventslen;
	struct kevent *kqchanges;
	int kqchangeslen;
	int kqchangecount;
#endif
#endif
} nbio_t;

int nbio_init(nbio_t *nb, int pfdsize);
int nbio_kill(nbio_t *nb);
void nbio_alleofforce(nbio_t *nb);
void nbio_flushall(nbio_t *nb);
nbio_fd_t *nbio_iter(nbio_t *nb, int (*matcher)(nbio_t *nb, void *ud, nbio_fd_t *fdt), void *userdata);
nbio_fd_t *nbio_getfdt(nbio_t *nb, nbio_sockfd_t fd);
nbio_fd_t *nbio_addfd(nbio_t *nb, int type, nbio_sockfd_t fd, int pri, nbio_handler_t handler, void *priv, int rxlen, int txlen);
int nbio_closefdt(nbio_t *nb, nbio_fd_t *fdt);
int nbio_sfd_close(nbio_t *nb, nbio_sockfd_t fd);
int nbio_setraw(nbio_t *nb, nbio_fd_t *fdt, int val);
int nbio_setcloseonflush(nbio_fd_t *fdt, int val);
int nbio_cleanuponly(nbio_t *nb);
int nbio_poll(nbio_t *nb, int timeout);
int nbio_setpri(nbio_t *nb, nbio_fd_t *fdt, int pri);
int nbio_connect(nbio_t *nb, const struct sockaddr *addr, int addrlen, nbio_handler_t handler, void *priv);
int nbio_settimer(nbio_t *nb, nbio_fd_t *fdt, int interval);
int nbio_sfd_read(nbio_t *nb, nbio_sockfd_t fd, void *buf, int count);
int nbio_sfd_write(nbio_t *nb, nbio_sockfd_t fd, const void *buf, int count);
nbio_sockfd_t nbio_sfd_accept(nbio_t *nb, nbio_sockfd_t fd, struct sockaddr *saret, int *salen);
nbio_sockfd_t nbio_getincomingconn(nbio_t *nb, nbio_fd_t *fdt, struct sockaddr *saret, int *salen);
nbio_sockfd_t nbio_sfd_newlistener(nbio_t *nb, const char *addr, unsigned short port);
nbio_sockfd_t nbio_sfd_new_stream(nbio_t *nb);
int nbio_sfd_setnonblocking(nbio_t *nb, nbio_sockfd_t fd);
int nbio_sfd_connect(nbio_t *nb, nbio_sockfd_t fd, struct sockaddr *sa, int salen);
int nbio_sfd_bind(nbio_t *nb, nbio_sockfd_t fd, struct sockaddr *sa, int salen);
int nbio_sfd_listen(nbio_t *nb, nbio_sockfd_t fd);

int nbio_addrxvector(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf, int buflen, int offset);
int nbio_addrxvector_time(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf, int buflen, int offset, time_t trigger);
int nbio_remrxvector(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf);
unsigned char *nbio_remtoprxvector(nbio_t *nb, nbio_fd_t *fdt, int *len, int *offset);
int nbio_addtxvector(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf, int buflen);
int nbio_addtxvector_time(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf, int buflen, time_t trigger);
int nbio_remtxvector(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf);
unsigned char *nbio_remtoptxvector(nbio_t *nb, nbio_fd_t *fdt, int *len, int *offset);
int nbio_rxavail(nbio_t *nb, nbio_fd_t *fdt);
int nbio_txavail(nbio_t *nb, nbio_fd_t *fdt);


/*
 * Stream delimiters.
 *
 * When a delimiter is found in a stream, the application is called
 * regardless of whether a buffer was completly filled or not.  Additionally,
 * the delimiter will be read off the stream, but will not be copied into the
 * user buffer (unless NBIO_FDT_FLAG_KEEPDELIM is set).
 *
 * For undelimited streams, either never call adddelim() or setdelim(),
 * or call cleardelim().
 *
 */

/*
 * Add a delimiter to watch for.  New delimiters will be found before older
 * delimiters (prepend).
 */
int nbio_adddelim(nbio_t *nb, nbio_fd_t *fdt, const unsigned char *delim, const unsigned char delimlen);

/*
 * Clear all delimiters.
 */
int nbio_cleardelim(nbio_fd_t *fdt);

/*
 * Set or clear the KEEPDELIM flag.
 */
int nbio_setkeepdelim(nbio_fd_t *fdt, int val);

/*
 * Non-blocking DNS resolution.  Returns struct hostent to callback, just as
 * gethostbyname() would have.  Returning -1 from the callback is the same as
 * if it was returned from a nbio_poll callback (will kill the loop).
 *
 * Only supports /etc/hosts and DNS ("files" and "dns" in nsswitch.conf terms).
 * Does not support NIS(+), LDAP, etc, because they're a pain in the ass.
 */
typedef int (*nbio_gethostbyname_callback_t)(nbio_t *nb, void *udata, const char *query, struct hostent *hp);
int nbio_gethostbyname(nbio_t *nb, nbio_gethostbyname_callback_t ufunc, void *udata, const char *query);

#ifdef __cplusplus
}
#endif


#endif /* __LIBNBIO_H__ */
