/* -*- Mode: ab-c -*- */

#include <config.h>
#include <libnbio.h>
#include "impl.h"

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/poll.h>

/* XXX this should be elimitated by using more bookkeeping */
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

/* Wrapper for UNIX. */
int __fdt_read(nbio_fd_t *fdt, void *buf, int count)
{
	return read(fdt->fd, buf, count);
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

	/* XXX should allow methods to override -- ie, WSARecv on win32 */
	if (((got = fdt_read(fdt, cur->data+cur->offset, target)) < 0) && (errno != EINTR) && (errno != EAGAIN)) {
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

		if ((rr = fdt_read(fdt, cur->data+cur->offset, 1)) <= 0)
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

	if (((wrote = fdt_write(fdt->fd, cur->data+cur->offset, target)) < 0) && (errno != EINTR) && (errno != EAGAIN)) {
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
	int flags;

	if ((flags = fcntl(fd, F_GETFL, 0)) == -1)
		return -1;

	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_block(int fd)
{
	int flags;
	
	if ((flags = fcntl(fd, F_GETFL, 0)) == -1)
		return -1;

	return fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
}


nbio_fd_t *nbio_addfd(nbio_t *nb, int type, int fd, int pri, nbio_handler_t handler, void *priv, int rxlen, int txlen)
{
	nbio_fd_t *newfd;

	if (!nb || (pri < 0) || (rxlen < 0) || (txlen < 0)) {
		errno = EINVAL;
		return NULL;
	}

	if ((type != NBIO_FDTYPE_STREAM) && 
			(type != NBIO_FDTYPE_LISTENER) && 
			(type != NBIO_FDTYPE_DGRAM)) {
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

	if (pfdadd(nb, newfd) == -1) {
		/* XXX free up chains */
		free(newfd);
		errno = ENOMEM;
		return NULL;
	}

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
	}

	pfdaddfinish(nb, newfd);

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

#if 0
	if (fdt->rxchain || fdt->txchain)
		fprintf(stderr, "WARNING: unfreed buffers on closed connection (%d/%p)\n", fdt->fd, fdt);
#endif

	fdt_setpollnone(nb, fdt);

	close(fdt->fd);
	fdt->fd = -1;

	pfdrem(nb, fdt);

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

	pfdfree(fdt);

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

int nbio_poll(nbio_t *nb, int timeout)
{
	return pfdpoll(nb, timeout);
}

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

	if (!fdt) {
		errno = EINVAL;
		return -1;
	}

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

	if (!fdt) {
		errno = EINVAL;
		return -1;
	}

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
