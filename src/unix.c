/* -*- Mode: ab-c -*- */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* XXX ick. */
#if !defined(NBIO_USE_WINSOCK2)

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>

#include <libnbio.h>
#include "impl.h"

int fdt_setnonblock(int fd)
{
	int flags;

	if ((flags = fcntl(fd, F_GETFL, 0)) == -1)
		return -1;

	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int fdt_read(nbio_fd_t *fdt, void *buf, int count)
{
	return read(fdt->fd, buf, count);
}

int fdt_write(nbio_fd_t *fdt, const void *buf, int count)
{
	return write(fdt->fd, buf, count);
}

void fdt_close(nbio_fd_t *fdt)
{

	close(fdt->fd);

	return;
}

struct connectinginfo {
	nbio_handler_t handler;
	void *handlerpriv;
};

static int fdt_connect_handler(void *nbv, int event, nbio_fd_t *fdt)
{
	nbio_t *nb = (nbio_t *)nbv;
	struct connectinginfo *ci = (struct connectinginfo *)fdt->priv;
	int error = 0;
	int len = sizeof(error);

	if ((event != NBIO_EVENT_READ) && (event != NBIO_EVENT_WRITE)) {

		if (ci->handler) 
			error = ci->handler(nb, NBIO_EVENT_CONNECTFAILED, fdt);

		free(ci);
		fdt->flags &= ~NBIO_FDT_FLAG_IGNORE;
		nbio_closefdt(nb, fdt);

		return error;
	}

	/* So it looks right to the user. */
	fdt->flags |= NBIO_FDT_FLAG_IGNORE; /* This is evil. */
	fdt->priv = ci->handlerpriv;
	nbio_setraw(nb, fdt, 0);

	if (getsockopt(fdt->fd, SOL_SOCKET, SO_ERROR, &error, &len) == -1)
		error = errno;

	if (error) {

		if (ci->handler)
			error = ci->handler(nb, NBIO_EVENT_CONNECTFAILED, fdt);

		free(ci);
		fdt->flags &= ~NBIO_FDT_FLAG_IGNORE;
		nbio_closefdt(nb, fdt);

		return error;
	}

	if (!ci->handler || 
			(ci->handler(nb, NBIO_EVENT_CONNECTED, fdt) == -1)) {

		free(ci);
		fdt->flags &= ~NBIO_FDT_FLAG_IGNORE;
		nbio_closefdt(nb, fdt);

		return -1;
	}

	fdt->fd = -1; /* prevent it from being close()'d by closefdt */

	free(ci);
	fdt->flags &= ~NBIO_FDT_FLAG_IGNORE;
	nbio_closefdt(nb, fdt);

	return 0;
}

int fdt_connect(nbio_t *nb, const struct sockaddr *addr, int addrlen, nbio_handler_t handler, void *priv)
{
	int fd;
	struct connectinginfo *ci;
	nbio_fd_t *fdt;

	if (!nb || !addr) {
		errno = EINVAL;
		return -1;
	}

	if ((fd = socket(addr->sa_family, SOCK_STREAM, 0)) == -1)
		return -1;

	if (fdt_setnonblock(fd) == -1) {
		close(fd);
		errno = EINVAL;
		return -1;
	}


	if ((connect(fd, (struct sockaddr *)addr, addrlen) == -1) &&
			(errno != EAGAIN) && (errno != EWOULDBLOCK) &&
			(errno != EINPROGRESS)) {
		close(fd);
		return -1;
	}

	if (!(ci = malloc(sizeof(struct connectinginfo)))) {
		close(fd);
		errno = ENOMEM;
		return -1;
	}

	ci->handler = handler;
	ci->handlerpriv = priv;

	if (!(fdt = nbio_addfd(nb, NBIO_FDTYPE_STREAM, fd, 0, fdt_connect_handler, (void *)ci, 0, 0))) {
		close(fd);
		free(ci);
		errno = EINVAL;
		return -1;
	}

	nbio_setraw(nb, fdt, 1);

	return 0;
}

#endif 
