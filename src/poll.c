/* -*- Mode: ab-c -*- */

#include <config.h>
#include <libnbio.h>
#include "impl.h"

#if !defined(NBIO_USE_KQUEUE) && !defined(NBIO_USE_WINSOCK2)

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <sys/poll.h>

#define NBIO_PFD_INVAL -1

/* nbio_t->intdata */
struct pfdnbdata {
	struct pollfd *pfds;
	int pfdsize;
	int pfdlast;
};

static int setpfdlast(nbio_t *nb)
{
	struct pfdnbdata *pnd = (struct pfdnbdata *)nb->intdata;
	int i;

	for (i = pnd->pfdsize-1; (i > -1) && 
			(pnd->pfds[i].fd == NBIO_PFD_INVAL); i--)
		;

	if (i < 0)
		i = 0;

	pnd->pfdlast = i;

	return i;
}

static void fdt_setpollin(nbio_t *nb, nbio_fd_t *fdt, int val)
{
	struct pollfd *pfd = (struct pollfd *)fdt->intdata;

	pfd->events |= POLLHUP;

	if (val)
		pfd->events |= POLLIN;
	else
		pfd->events &= ~POLLIN;

	return;
}

static void fdt_setpollout(nbio_t *nb, nbio_fd_t *fdt, int val)
{
	struct pollfd *pfd = (struct pollfd *)fdt->intdata;

	pfd->events |= POLLHUP;

	if (val)
		pfd->events |= POLLOUT;
	else
		pfd->events &= ~POLLOUT;

	return;
}

static void fdt_setpollnone(nbio_t *nb, nbio_fd_t *fdt)
{
	struct pollfd *pfd = (struct pollfd *)fdt->intdata;

	pfd->events = POLLHUP;
	pfd->revents = 0;

	return;
}

static struct pollfd *findunusedpfd(nbio_t *nb)
{
	struct pfdnbdata *pnd = (struct pfdnbdata *)nb->intdata;
	int i;

	for (i = 0; (i < pnd->pfdsize) && 
			(pnd->pfds[i].fd != NBIO_PFD_INVAL); i++)
		;

	if (i >= pnd->pfdsize)
		return NULL;

	return pnd->pfds + i;
}

int pfdadd(nbio_t *nb, nbio_fd_t *newfd)
{
	struct pollfd *pfd;

	if (!(pfd = newfd->intdata = (void *)findunusedpfd(nb)))
		return -1;

	pfd->fd = newfd->fd;

	return 0;
}

void pfdaddfinish(nbio_t *nb, nbio_fd_t *newfd)
{

	setpfdlast(nb);

	return;
}

void pfdrem(nbio_t *nb, nbio_fd_t *fdt)
{
	struct pollfd *pfd = (struct pollfd *)fdt->intdata;

	if (!pfd)
		return;

	pfd->fd = NBIO_PFD_INVAL;
	pfd->events = pfd->revents = 0;
	pfd = NULL;

	setpfdlast(nb);

	return;
}

void pfdfree(nbio_fd_t *fdt)
{
	return; /* not used */
}

int pfdinit(nbio_t *nb, int pfdsize)
{
	struct pfdnbdata *pnd;
	int i;

	if (!(pnd = nb->intdata = malloc(sizeof(struct pfdnbdata))))
		return -1;

	pnd->pfdsize = pfdsize;
	if (!(pnd->pfds = malloc(sizeof(struct pollfd) * pnd->pfdsize)))
		return -1;

	for (i = 0; i < pnd->pfdsize; i++) {
		pnd->pfds[i].fd = NBIO_PFD_INVAL;
		pnd->pfds[i].events = 0;
	}

	setpfdlast(nb);

	return 0;
}

void pfdkill(nbio_t *nb)
{
	struct pfdnbdata *pnd = (struct pfdnbdata *)nb->intdata;

	free(pnd->pfds);
	free(pnd);

	nb->intdata = NULL;

	return;
}

int pfdpoll(nbio_t *nb, int timeout)
{
	struct pfdnbdata *pnd = (struct pfdnbdata *)nb->intdata;
	int pollret, curpri;

	if (!nb) {
		errno = EINVAL;
		return -1;
	}

	errno = 0;
	if ((pollret = poll(pnd->pfds, pnd->pfdlast+1, timeout)) == -1) {

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
			struct pollfd *pfd;

			if (cur->fd == -1) {
				*prev = cur->next;
				freefdt(cur);
				continue;
			} 

			if (cur->pri != curpri) {
				prev = &cur->next;
				continue;
			}

			pfd = (struct pollfd *)cur->intdata;

			if (pfd && pfd->revents & POLLIN) {
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

			if (pfd && pfd->revents & POLLOUT) {
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

			if (pfd && ((pfd->revents & POLLERR) ||
					 (pfd->revents & POLLHUP))) {

				if ((cur->fd != NBIO_PFD_INVAL) && cur->handler)
					cur->handler(nb, NBIO_EVENT_EOF, cur);
			}

			if ((cur->flags & NBIO_FDT_FLAG_CLOSEONFLUSH) && !cur->txchain)
				cur->handler(nb, NBIO_EVENT_EOF, cur);

			prev = &cur->next;
		}
	}

	return pollret;
}

#endif /* !def KQUEUE && !def WINSOCK2 */

