/* -*- Mode: ab-c -*- */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <libnbio.h>
#include "impl.h"

/* XXX ick. */
#if !defined(NBIO_USE_WINSOCK2)

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#include <fcntl.h>
#include <stdlib.h>
#include <errno.h>

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

#endif 

