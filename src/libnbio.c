/* -*- Mode: ab-c -*- */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <libnbio.h>
#include <sys/poll.h>
#include <time.h>

#ifdef NBIO_USE_KQUEUE
#include <sys/types.h>
#include <sys/event.h>
#endif

#ifdef NBIO_USE_KQUEUE

static struct kevent *getnextchange(nbio_t *nb)
{
	struct kevent *ret = NULL;

	if (nb->kqchangecount >= nb->kqchangeslen)
		return NULL;

	ret = nb->kqchanges+nb->kqchangecount;
	nb->kqchangecount++;

	return ret;
}

/*
 * XXX this API generates superfluous calls to kevent...
 * should make use of kevent's changelist functionality.
 */
static void fdt_setpollin(nbio_t *nb, nbio_fd_t *fdt, int val)
{
	struct kevent *kev;

	if (!nb || !fdt)
		return;

	if (!(kev = getnextchange(nb))) {
		fprintf(stderr, "libnbio: fdt_setpollin: getnextchange failed!\n");
		return;
	}

	kev->ident = fdt->fd;
	kev->filter = EVFILT_READ;
	kev->flags = val ? EV_ADD : EV_DELETE;
	kev->fflags = 0;
	kev->udata = (void *)fdt;

	return;
}

static void fdt_setpollout(nbio_t *nb, nbio_fd_t *fdt, int val)
{
	struct kevent *kev;

	if (!nb || !fdt)
		return;

	if (!(kev = getnextchange(nb))) {
		fprintf(stderr, "libnbio: fdt_setpollout: getnextchange failed!\n");
		return;
	}

	kev->ident = fdt->fd;
	kev->filter = EVFILT_WRITE;
	kev->flags = val ? EV_ADD : EV_DELETE;
	kev->fflags = 0;
	kev->udata = (void *)fdt;

	return;
}

static void fdt_setpollnone(nbio_t *nb, nbio_fd_t *fdt)
{
	if (!nb || !fdt)
		return;

	fdt_setpollin(nb, fdt, 0);
	fdt_setpollout(nb, fdt, 0);

	return;
}

#else /* NBIO_USE_KQUEUE */

#define NBIO_PFD_INVAL -1

static int setpfdlast(nbio_t *nb)
{
	int i;

	for (i = nb->pfdsize-1; (i > -1) && (nb->pfds[i].fd == NBIO_PFD_INVAL); i--)
		;

	if (i < 0)
		i = 0;

	nb->pfdlast = i;

	return i;
}

static void fdt_setpollin(nbio_t *nb, nbio_fd_t *fdt, int val)
{
	if (!fdt || !fdt->pfd)
		return;

	fdt->pfd->events |= POLLHUP;

	if (val)
		fdt->pfd->events |= POLLIN;
	else
		fdt->pfd->events &= ~POLLIN;

	return;
}

static void fdt_setpollout(nbio_t *nb, nbio_fd_t *fdt, int val)
{
	if (!fdt || !fdt->pfd)
		return;

	fdt->pfd->events |= POLLHUP;

	if (val)
		fdt->pfd->events |= POLLOUT;
	else
		fdt->pfd->events &= ~POLLOUT;

	return;
}

static void fdt_setpollnone(nbio_t *nb, nbio_fd_t *fdt)
{
	if (!fdt || !fdt->pfd)
		return;

	fdt->pfd->events = POLLHUP;
	fdt->pfd->revents = 0;

	return;
}

static struct pollfd *findunusedpfd(nbio_t *nb)
{
	int i;

	if (!nb) {
		errno = EINVAL;
		return NULL;
	}

	for (i = 0; (i < nb->pfdsize) && (nb->pfds[i].fd != NBIO_PFD_INVAL); i++)
		;

	if (i >= nb->pfdsize)
		return NULL;

	return &nb->pfds[i];
}
#endif /* USEKQUEUE */

static void setmaxpri(nbio_t *nb)
{
	nbio_fd_t *cur;
	int max = 0;

	for (cur = nb->fdlist; cur; cur = cur->next) {
		if (cur->fd == -1)
			continue;
		if (cur->pri > max)
			max = cur->pri;
	}

	nb->maxpri = max;
	return;
}

nbio_fd_t *nbio_getfdt(nbio_t *nb, int fd)
{
	nbio_fd_t *cur;

	if (!nb) {
		errno = EINVAL;
		return NULL;
	}

	for (cur = nb->fdlist; cur; cur = cur->next) {
		if (cur->fd == fd)
			return cur;
	}

	errno = ENOENT;
	return NULL;
}

static int streamread_nodelim(nbio_t *nb, nbio_fd_t *fdt)
{
	nbio_buf_t *cur;
	int target, got;

	for (cur = fdt->rxchain; cur; cur = cur->next) {
		/* Find a non-zero buffer that still has space left in it */
		if (cur->len && (cur->offset < cur->len))
			break;
	}

	if (!cur) {
		fdt_setpollin(nb, fdt, 0);
		return 0;
	}

	target = cur->len - cur->offset;

	if (((got = read(fdt->fd, cur->data+cur->offset, target)) < 0) && (errno != EINTR) && (errno != EAGAIN)) {
		return fdt->handler(nb, NBIO_EVENT_ERROR, fdt);
	}

	if (got == 0)
		return fdt->handler(nb, NBIO_EVENT_EOF, fdt);

	if (got < 0)
		got = 0; /* a non-fatal error occured; zero bytes actually read */
  
	cur->offset += got;

	if (cur->offset >= cur->len)
		return fdt->handler(nb, NBIO_EVENT_READ, fdt);

	return 0; /* don't call handler unless we filled a buffer */
}

static int streamread_delim(nbio_t *nb, nbio_fd_t *fdt)
{
	nbio_buf_t *cur;
	int got, rr, target;

	rr = 0;
	for (cur = fdt->rxchain; cur; cur = cur->next) {
		/* Find a non-zero buffer that still has space left in it */
		if (cur->len && (cur->offset < cur->len))
			break;
	}

	if (!cur) {
		fdt_setpollin(nb, fdt, 0);
		return 0;
	}

	for (got = 0, target = 0; (got < (cur->len - cur->offset)) && !target; ) {
		nbio_delim_t *cd;

		if ((rr = read(fdt->fd, cur->data+cur->offset, 1)) <= 0)
			break;

		cur->offset += rr;
		got += rr;

		for (cd = fdt->delims; cd; cd = cd->next) {
			if ((cur->offset >= cd->len) && 
				(memcmp(cur->data+cur->offset-cd->len,
						cd->data, cd->len) == 0)) {

				target++;

				if (!(fdt->flags & NBIO_FDT_FLAG_KEEPDELIM))
					memset(cur->data+cur->offset-cd->len, '\0', cd->len);
				break;
			}
		}

	}

	if ((rr < 0) && (errno != EAGAIN))
		return fdt->handler(nb, NBIO_EVENT_ERROR, fdt);

	if ((cur->offset >= cur->len) || target) {
		if ((got = fdt->handler(nb, NBIO_EVENT_READ, fdt)) < 0)
			return got;
	}

	if (rr == 0)
		return fdt->handler(nb, NBIO_EVENT_EOF, fdt);

	return 0; /* don't call handler unless we filled a buffer or got delim  */
}

static int streamread(nbio_t *nb, nbio_fd_t *fdt)
{
	if ((fdt->flags & NBIO_FDT_FLAG_RAW) ||
		(fdt->flags & NBIO_FDT_FLAG_RAWREAD))
		return fdt->handler(nb, NBIO_EVENT_READ, fdt);

	if (fdt->delims)
		return streamread_delim(nb, fdt);
	return streamread_nodelim(nb, fdt);
}

static int streamwrite(nbio_t *nb, nbio_fd_t *fdt)
{
	nbio_buf_t *cur;
	int target, wrote;
	int pollout = 0;
	time_t now;

	if (fdt->flags & NBIO_FDT_FLAG_RAW)
		return fdt->handler(nb, NBIO_EVENT_WRITE, fdt);

	now = time(NULL);

	for (cur = fdt->txchain; cur; cur = cur->next) {
		/* Find a non-zero buffer that still needs data written
			also checks for the trigger time */
		if (cur->len && (cur->offset < cur->len)) {
			if (cur->trigger <= now)
				break;
			else
				pollout = 1; /* keep checking on this fd */
		}
	}

	if (!cur) {
		fdt_setpollout(nb, fdt, pollout);
		return 0; /* nothing to do */
	}

	target = cur->len - cur->offset;

	if (((wrote = write(fdt->fd, cur->data+cur->offset, target)) < 0) && (errno != EINTR) && (errno != EAGAIN)) {
		return fdt->handler(nb, NBIO_EVENT_ERROR, fdt);
	}

	if (wrote < 0)
		wrote = 0; /* a non-fatal error occured; zero bytes actually written */
  
	cur->offset += wrote;

	if (cur->offset >= cur->len) {
		int ret;

		ret = fdt->handler(nb, NBIO_EVENT_WRITE, fdt);

		if (!fdt->txchain && (fdt->flags & NBIO_FDT_FLAG_CLOSEONFLUSH))
			ret = fdt->handler(nb, NBIO_EVENT_EOF, fdt);

		return ret;
	}

	return 0; /* don't call handler unless we finished a buffer */
}

/*
 * DGRAM sockets are completely unbuffered.
 */
static int dgramread(nbio_t *nb, nbio_fd_t *fdt)
{
	return fdt->handler(nb, NBIO_EVENT_READ, fdt);
}

static int dgramwrite(nbio_t *nb, nbio_fd_t *fdt)
{
	return fdt->handler(nb, NBIO_EVENT_WRITE, fdt);
}

#ifdef NBIO_USE_KQUEUE

static int pfdinit(nbio_t *nb, int pfdsize)
{

	if (!nb || (pfdsize <= 0))
		return -1;

	nb->kqeventslen = pfdsize;
	nb->kqchangeslen = pfdsize*2;

	if (!(nb->kqevents = malloc(sizeof(struct kevent)*nb->kqeventslen)))
		return -1;
	if (!(nb->kqchanges = malloc(sizeof(struct kevent)*nb->kqchangeslen))) {
		free(nb->kqevents);
		return -1;
	}

	nb->kqchangecount = 0;

	if ((nb->kq = kqueue()) == -1) {
		int sav;

		sav = errno;
		free(nb->kqevents);
		free(nb->kqchanges);
		errno = sav;

		return -1;
	}

	return 0;
}

static void pfdkill(nbio_t *nb)
{

	/* XXX I guess... the inverse of kqueue() isn't documented... */
	close(nb->kq);

	free(nb->kqevents);
	nb->kqeventslen = 0;

	free(nb->kqchanges);
	nb->kqchangeslen = 0;
	nb->kqchangecount = 0;

	return;
}

#else

static int pfdinit(nbio_t *nb, int pfdsize)
{
	int i;

	nb->pfdsize = pfdsize;
	if (!(nb->pfds = malloc(sizeof(struct pollfd)*nb->pfdsize)))
		return -1;

	for (i = 0; i < nb->pfdsize; i++) {
		nb->pfds[i].fd = NBIO_PFD_INVAL;
		nb->pfds[i].events = 0;
	}

	setpfdlast(nb);

	return 0;
}

static void pfdkill(nbio_t *nb)
{

	free(nb->pfds);
	nb->pfds = NULL;

	return;
}

#endif /* NBIO_USE_KQUEUE */

int nbio_init(nbio_t *nb, int pfdsize)
{

	if (!nb || (pfdsize <= 0))
		return -1;

	memset(nb, 0, sizeof(nbio_t));

	if (pfdinit(nb, pfdsize) == -1)
		return -1;

	setmaxpri(nb);

	return 0;
}

int nbio_kill(nbio_t *nb)
{
	nbio_fd_t *cur;

	if (!nb) {
		errno = EINVAL;
		return -1;
	}

	for (cur = nb->fdlist; cur; cur = cur->next)
		nbio_closefdt(nb, cur);

	nbio_cleanuponly(nb); /* to clean up the list */

	pfdkill(nb);

	return 0;
}

/* XXX handle malloc==NULL case better here */
static int preallocchains(nbio_fd_t *fdt, int rxlen, int txlen)
{
	nbio_buf_t *newbuf;

	while (rxlen) {
		if (!(newbuf = malloc(sizeof(nbio_buf_t))))
			return -1;

		newbuf->next = fdt->rxchain_freelist;
		fdt->rxchain_freelist = newbuf;

		rxlen--;
	}

	while (txlen) {
		if (!(newbuf = malloc(sizeof(nbio_buf_t))))
			return -1;

		newbuf->next = fdt->txchain_freelist;
		fdt->txchain_freelist = newbuf;

		txlen--;
	}

	return 0;
}

static int set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_block(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}


/*
 *
 */
nbio_fd_t *nbio_addfd(nbio_t *nb, int type, int fd, int pri, nbio_handler_t handler, void *priv, int rxlen, int txlen)
{
	nbio_fd_t *newfd;

	if (!nb || (pri < 0) || (rxlen < 0) || (txlen < 0)) {
		errno = EINVAL;
		return NULL;
	}

	if (nbio_getfdt(nb, fd)) {
		errno = EEXIST;
		return NULL;
	}

	if (set_nonblock(fd) < 0)
		return NULL;

	if (!(newfd = malloc(sizeof(nbio_fd_t)))) {
		errno = ENOMEM;
		return NULL;
	}

	newfd->type = type;
	newfd->fd = fd;
	newfd->flags = NBIO_FDT_FLAG_NONE;
	newfd->pri = pri;
	newfd->delims = NULL;
	newfd->handler = handler;
	newfd->priv = priv;
	newfd->rxchain = newfd->txchain = newfd->txchain_tail = NULL;
	newfd->rxchain_freelist = newfd->txchain_freelist = NULL;
	if (preallocchains(newfd, rxlen, txlen) < 0) {
		free(newfd);
		return NULL;
	}

#ifndef NBIO_USE_KQUEUE
	if (!(newfd->pfd = findunusedpfd(nb))) {
		/* XXX free up chains */
		free(newfd);
		errno = ENOMEM;
		return NULL;
	}
	newfd->pfd->fd = fd;
#endif

	fdt_setpollnone(nb, newfd);

	if (newfd->type == NBIO_FDTYPE_STREAM) {
		/* will be set when buffers are added */
		fdt_setpollin(nb, newfd, 0);
		fdt_setpollout(nb, newfd, 0);
	} else if (newfd->type == NBIO_FDTYPE_LISTENER) {
		fdt_setpollin(nb, newfd, 1);
		fdt_setpollout(nb, newfd, 0);
	} else if (newfd->type == NBIO_FDTYPE_DGRAM) {
		fdt_setpollin(nb, newfd, 1);
		fdt_setpollout(nb, newfd, 0);
	} else {
		fprintf(stderr, "WARNING: addfd: unknown fdtype\n");
		fdt_setpollin(nb, newfd, 1);
		fdt_setpollout(nb, newfd, 0);
	}

#ifndef NBIO_USE_KQUEUE
	setpfdlast(nb);
#endif

	newfd->next = nb->fdlist;
	nb->fdlist = newfd;

	setmaxpri(nb);

	return newfd;
}

int nbio_closefdt(nbio_t *nb, nbio_fd_t *fdt)
{
	if (!nb || !fdt) {
		errno = EINVAL;
		return -1;
	}

	if (fdt->fd == -1)
		return 0;

	if (fdt->rxchain || fdt->txchain)
		fprintf(stderr, "WARNING: unfreed buffers on closed connection (%d/%p)\n", fdt->fd, fdt);

	fdt_setpollnone(nb, fdt);

	close(fdt->fd);
	fdt->fd = -1;

#ifndef NBIO_USE_KQUEUE
	if (fdt->pfd) {
		fdt->pfd->fd = NBIO_PFD_INVAL;
		fdt->pfd->events = fdt->pfd->revents = 0;
		fdt->pfd = NULL;

		setpfdlast(nb);
	}
#endif

	setmaxpri(nb);

	return 0;
}

static void freefdt(nbio_fd_t *fdt)
{
	nbio_buf_t *buf, *tmp;

	nbio_cleardelim(fdt);

	for (buf = fdt->rxchain_freelist; buf; ) {
		tmp = buf;
		buf = buf->next;
		free(tmp);
	}

	for (buf = fdt->txchain_freelist; buf; ) {
		tmp = buf;
		buf = buf->next;
		free(tmp);
	}

	free(fdt);

	return;
}

/* Call the EOF callback on all connections in order to close them */
void nbio_alleofforce(nbio_t *nb)
{
	nbio_fd_t *cur;

	nbio_flushall(nb);

	for (cur = nb->fdlist; cur; cur = cur->next) {
		if (cur->fd == -1)
			continue;
		if (cur->handler)
			cur->handler(nb, NBIO_EVENT_EOF, cur);
	}

	return;
}

void nbio_flushall(nbio_t *nb)
{
	nbio_fd_t *cur;

	for (cur = nb->fdlist; cur; cur = cur->next) {
		int i;

		if (cur->fd == -1)
			continue;

		/* Call each ten times just to make sure */
		for (i = 10; i; i--) {
			if (cur->type == NBIO_FDTYPE_LISTENER)
				;
			else if (cur->type == NBIO_FDTYPE_STREAM)
				streamwrite(nb, cur);
			else if (cur->type == NBIO_FDTYPE_DGRAM)
				dgramwrite(nb, cur);
		}
	}
  
	return;
}

/* Do all cleanups that are normally done in nbio_poll */
int nbio_cleanuponly(nbio_t *nb)
{
	nbio_fd_t *cur = NULL, **prev = NULL;

	for (prev = &nb->fdlist; (cur = *prev); ) {
		if (cur->fd == -1) {
			*prev = cur->next;
			freefdt(cur);
			continue;
		} 

		if ((cur->flags & NBIO_FDT_FLAG_CLOSEONFLUSH) && !cur->txchain)
			cur->handler(nb, NBIO_EVENT_EOF, cur);

		prev = &cur->next;
	}

	return 0;
}

#ifdef NBIO_USE_KQUEUE

int nbio_poll(nbio_t *nb, int timeout)
{
	struct timespec to;
	int kevret, i;
	nbio_fd_t *fdt;

	if (!nb) {
		errno = EINVAL;
		return -1;
	}

	if (timeout > 0) {
		to.tv_sec = 0;
		to.tv_nsec = timeout*1000;
	}

	errno = 0;
	
	if ((kevret = kevent(nb->kq, nb->kqchanges, nb->kqchangecount, nb->kqevents, nb->kqeventslen, (timeout > 0)?&to:NULL)) == -1) {
		perror("kevent");
		return -1;
	}

	/* As long as it doesn't return -1, the changelist has been processed */
	nb->kqchangecount = 0;

	if (kevret == 0)
		return 0;

	for (i = 0; i < kevret; i++) {

		if (!nb->kqevents[i].udata) {
			fprintf(stderr, "no udata!\n");
			continue;
		}

		fdt = (nbio_fd_t *)nb->kqevents[i].udata;

		if (nb->kqevents[i].filter == EVFILT_READ) {

			if (fdt->type == NBIO_FDTYPE_LISTENER) {
				if (fdt->handler(nb, NBIO_EVENT_READ, fdt) == -1)
					return -1;
			} else if (fdt->type == NBIO_FDTYPE_STREAM) {
				if (streamread(nb, fdt) == -1)
					return -1;
			} else if (fdt->type == NBIO_FDTYPE_DGRAM) {
				if (dgramread(nb, fdt) == -1)
					return -1;
			}

		} else if (nb->kqevents[i].filter == EVFILT_WRITE) {

			if (fdt->type == NBIO_FDTYPE_LISTENER)
				; /* invalid */
			else if (fdt->type == NBIO_FDTYPE_STREAM) {
				if (streamwrite(nb, fdt) == -1)
					return -1;
			} else if (fdt->type == NBIO_FDTYPE_DGRAM) {
				if (dgramwrite(nb, fdt) == -1)
					return -1;
			}
		}

		if (nb->kqevents[i].flags & EV_EOF) {
			if (fdt->handler(nb, NBIO_EVENT_EOF, fdt) == -1)
				return -1;
		}

	}

	return nbio_cleanuponly(nb);
}

#else

int nbio_poll(nbio_t *nb, int timeout)
{
	int pollret, curpri;

	if (!nb) {
		errno = EINVAL;
		return -1;
	}

	errno = 0;
	if ((pollret = poll(nb->pfds, nb->pfdlast+1, timeout)) == -1) {

		/* Never return EINTR from nbio_poll... */
		if (errno == EINTR) {
			errno = 0;
			return 0;
		}

		return -1;

	} else if (pollret == 0) {
		return 0;
	}

	for (curpri = nb->maxpri; curpri >= 0; curpri--) {
		nbio_fd_t *cur = NULL, **prev = NULL;

		for (prev = &nb->fdlist; (cur = *prev); ) {

			if (cur->fd == -1) {
				*prev = cur->next;
				freefdt(cur);
				continue;
			} 

			if (cur->pri != curpri) {
				prev = &cur->next;
				continue;
			}

			if (cur->pfd && cur->pfd->revents & POLLIN) {
				if (cur->type == NBIO_FDTYPE_LISTENER) {
					if (cur->handler(nb, NBIO_EVENT_READ, cur) < 0)
						return -1; /* XXX do something better */
				} else if (cur->type == NBIO_FDTYPE_STREAM) {
					if (streamread(nb, cur) < 0)
						return -1; /* XXX do something better */
				} else if (cur->type == NBIO_FDTYPE_DGRAM) {
					if (dgramread(nb, cur) < 0)
						return -1; /* XXX do something better */
				}
			}

			if (cur->pfd && cur->pfd->revents & POLLOUT) {
				if (cur->type == NBIO_FDTYPE_LISTENER)
					; /* invalid? */
				else if (cur->type == NBIO_FDTYPE_STREAM) {
					if (streamwrite(nb, cur) < 0)
						return -1; /* XXX do something better */
				} else if (cur->type == NBIO_FDTYPE_DGRAM) {
					if (dgramwrite(nb, cur) < 0)
						return -1; /* XXX do something better */
				} 
			}

			if (cur->pfd && ((cur->pfd->revents & POLLERR) ||
					 (cur->pfd->revents & POLLHUP))) {
				if ((cur->fd != -1) && cur->handler)
					cur->handler(nb, NBIO_EVENT_EOF, cur);
			}

			if ((cur->flags & NBIO_FDT_FLAG_CLOSEONFLUSH) && !cur->txchain)
				cur->handler(nb, NBIO_EVENT_EOF, cur);

			prev = &cur->next;
		}
	}

	return pollret;
}
#endif /* NBIO_USE_KQUEUE */

int nbio_setpri(nbio_t *nb, nbio_fd_t *fdt, int pri)
{
	if (!nb || !fdt || (pri < 0)) {
		errno = EINVAL;
		return -1;
	}

	fdt->pri = pri;

	setmaxpri(nb);

	return 0;
}

int nbio_setraw(nbio_t *nb, nbio_fd_t *fdt, int val)
{
	if (!fdt)
		return -1;

	if (val == 2) {
		fdt->flags |= NBIO_FDT_FLAG_RAWREAD;
		fdt_setpollin(nb, fdt, 1);
		fdt_setpollout(nb, fdt, 0);
	} else if (val) {
		fdt->flags |= NBIO_FDT_FLAG_RAW;
		fdt_setpollin(nb, fdt, 1);
		fdt_setpollout(nb, fdt, 1);
	} else {
		fdt->flags &= ~(NBIO_FDT_FLAG_RAW | NBIO_FDT_FLAG_RAWREAD);
		fdt_setpollnone(nb, fdt);
		if (fdt->rxchain)
			fdt_setpollin(nb, fdt, 1);
		if (fdt->txchain)
			fdt_setpollout(nb, fdt, 1);
	}

	return 0;
}

int nbio_setcloseonflush(nbio_fd_t *fdt, int val)
{
	if (!fdt)
		return -1;

	if (val)
		fdt->flags |= NBIO_FDT_FLAG_CLOSEONFLUSH;
	else
		fdt->flags &= ~NBIO_FDT_FLAG_CLOSEONFLUSH;

	return 0;
}

int nbio_adddelim(nbio_t *nb, nbio_fd_t *fdt, const unsigned char *delim, const unsigned char delimlen)
{
	nbio_delim_t *nd;

	if (!nb || !fdt || (fdt->type != NBIO_FDTYPE_STREAM)) {
		errno = EINVAL;
		return -1;
	}

	if (!delimlen || !delim || (delimlen > NBIO_MAX_DELIMITER_LEN)) {
		errno = EINVAL;
		return -1;
	}

	if (!(nd = malloc(sizeof(nbio_delim_t))))
		return -1;

	nd->len = delimlen;
	memcpy(nd->data, delim, delimlen);

	nd->next = fdt->delims;
	fdt->delims = nd;

	return 0;
}

int nbio_cleardelim(nbio_fd_t *fdt)
{
	nbio_delim_t *cur;

	if (!fdt || (fdt->type != NBIO_FDTYPE_STREAM)) {
		errno = EINVAL;
		return -1;
	}

	for (cur = fdt->delims; cur; ) {
		nbio_delim_t *tmp;

		tmp = cur->next;
		free(cur);
		cur = tmp;
	}

	fdt->delims = NULL;

	return 0;
}

int nbio_setkeepdelim(nbio_fd_t *fdt, int val)
{
	
	if (!fdt || (fdt->type != NBIO_FDTYPE_STREAM)) {
		errno = EINVAL;
		return -1;
	}

	if (val)
		fdt->flags |= NBIO_FDT_FLAG_KEEPDELIM;
	else
		fdt->flags &= ~NBIO_FDT_FLAG_KEEPDELIM;

	return 0;
}

static nbio_buf_t *getrxbuf(nbio_fd_t *fdt)
{
	nbio_buf_t *ret;

	if (!fdt || !fdt->rxchain_freelist) {
		errno = EINVAL;
		return NULL;
	}

	ret = fdt->rxchain_freelist;
	fdt->rxchain_freelist = fdt->rxchain_freelist->next;

	return ret;
}

static nbio_buf_t *gettxbuf(nbio_fd_t *fdt)
{
	nbio_buf_t *ret;

	if (!fdt || !fdt->txchain_freelist) {
		errno = EINVAL;
		return NULL;
	}

	ret = fdt->txchain_freelist;
	fdt->txchain_freelist = fdt->txchain_freelist->next;

	return ret;
}

static void givebackrxbuf(nbio_fd_t *fdt, nbio_buf_t *buf)
{
	buf->next = fdt->rxchain_freelist;
	fdt->rxchain_freelist = buf;

	return;
}

static void givebacktxbuf(nbio_fd_t *fdt, nbio_buf_t *buf)
{
	buf->next = fdt->txchain_freelist;
	fdt->txchain_freelist = buf;

	return;
}

int nbio_addrxvector_time(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf, int buflen, int offset, time_t trigger)
{
	nbio_buf_t *newbuf, *cur;

	if (!(newbuf = getrxbuf(fdt))) {
		errno = ENOMEM;
		return -1;
	}

	newbuf->data = buf;
	newbuf->len = buflen;
	newbuf->offset = offset;
	newbuf->trigger = trigger;
	newbuf->next = NULL;

	if (fdt->rxchain) {
		for (cur = fdt->rxchain; cur->next; cur = cur->next)
			;
		cur->next = newbuf;
	} else
		fdt->rxchain = newbuf;

	if (fdt->rxchain)
		fdt_setpollin(nb, fdt, 1);

	return 0;
}

int nbio_addrxvector(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf, int buflen, int offset)
{
	return nbio_addrxvector_time(nb, fdt, buf, buflen, offset, 0); /* ASAP */
}

int nbio_remrxvector(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf)
{
	nbio_buf_t *cur = NULL;

	if (!fdt->rxchain)
		;
	else if (fdt->rxchain->data == buf) {
		cur = fdt->rxchain;
		fdt->rxchain = fdt->rxchain->next;
	} else {
		for (cur = fdt->rxchain; cur->next; cur = cur->next) {
			if (cur->next->data == buf) {
				nbio_buf_t *tmp;

				tmp = cur->next;
				cur->next = cur->next->next;
				cur = tmp;

				break;
			}
		}
	}

	if (!cur) {
		errno = ENOENT;
		return -1;
	}

	givebackrxbuf(fdt, cur);

	if (!fdt->rxchain)
		fdt_setpollin(nb, fdt, 0);

	return 0; /* caller must free the region */
}

unsigned char *nbio_remtoprxvector(nbio_t *nb, nbio_fd_t *fdt, int *len, int *offset)
{
	nbio_buf_t *ret;
	unsigned char *buf;

	if (!fdt) {
		errno = EINVAL;
		return NULL;
	}

	if (!fdt->rxchain) {
		errno = ENOENT;
		return NULL;
	}

	ret = fdt->rxchain;
	fdt->rxchain = fdt->rxchain->next;

	if (len)
		*len = ret->len;
	if (offset)
		*offset = ret->offset;
	buf = ret->data;

	givebackrxbuf(fdt, ret);

	if (!fdt->rxchain)
		fdt_setpollin(nb, fdt, 0);

	return buf;
}

int nbio_addtxvector_time(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf, int buflen, time_t trigger)
{
	nbio_buf_t *newbuf;

	if (!fdt || !buf || !buflen) {
		errno = EINVAL;
		return -1;
	}

	if (!(newbuf = gettxbuf(fdt))) {
		errno = ENOMEM;
		return -1;
	}

	newbuf->data = buf;
	newbuf->len = buflen;
	newbuf->offset = 0;
	newbuf->trigger = trigger;
	newbuf->next = NULL;

	if (fdt->txchain_tail) {
		fdt->txchain_tail->next = newbuf;
		fdt->txchain_tail = fdt->txchain_tail->next;
	} else
		fdt->txchain = fdt->txchain_tail = newbuf;

	if (fdt->txchain)
		fdt_setpollout(nb, fdt, 1);

	return 0;
}

int nbio_addtxvector(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf, int buflen)
{
	return nbio_addtxvector_time(nb, fdt, buf, buflen, 0);
}

int nbio_rxavail(nbio_t *nb, nbio_fd_t *fdt)
{
	return !!fdt->rxchain_freelist;
}

int nbio_txavail(nbio_t *nb, nbio_fd_t *fdt)
{
	return !!fdt->txchain_freelist;
}

int nbio_remtxvector(nbio_t *nb, nbio_fd_t *fdt, unsigned char *buf)
{
	nbio_buf_t *cur = NULL;

	if (!fdt->txchain)
		;
	else if (fdt->txchain->data == buf) {
		cur = fdt->txchain;
		fdt->txchain = fdt->txchain->next;
	} else {
		for (cur = fdt->txchain; cur->next; cur = cur->next) {
			if (cur->next->data == buf) {
				nbio_buf_t *tmp;

				tmp = cur->next;
				cur->next = cur->next->next;
				cur = tmp;

				break;
			}
		}
	}

	if (!cur) {
		errno = ENOENT;
		return -1;
	}

	if (cur == fdt->txchain_tail)
		fdt->txchain_tail = fdt->txchain_tail->next;
	if (!fdt->txchain_tail)
		fdt->txchain_tail = fdt->txchain;

	givebacktxbuf(fdt, cur);

	if (!fdt->txchain)
		fdt_setpollout(nb, fdt, 0);

	return 0; /* caller must free the region */
}

unsigned char *nbio_remtoptxvector(nbio_t *nb, nbio_fd_t *fdt, int *len, int *offset)
{
	nbio_buf_t *ret;
	unsigned char *buf;

	if (!fdt) {
		errno = EINVAL;
		return NULL;
	}

	if (!fdt->txchain) {
		errno = ENOENT;
		return NULL;
	}

	ret = fdt->txchain;
	fdt->txchain = fdt->txchain->next;

	if (ret == fdt->txchain_tail)
		fdt->txchain_tail = fdt->txchain;

	if (len)
		*len = ret->len;
	if (offset)
		*offset = ret->offset;
	buf = ret->data;

	givebacktxbuf(fdt, ret);

	if (!fdt->txchain)
		fdt_setpollout(nb, fdt, 0);

	return buf;
}
