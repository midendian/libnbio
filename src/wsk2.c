/* -*- Mode: ab-c -*- */

#include <config.h>
#include <libnbio.h>
#include "impl.h"

#ifdef NBIO_USE_WINSOCK2

static void wsa_seterrno(void)
{
	int err;

	err = WSAGetLastError();

	if (err == WSANOTINITIALISED)
		errno = EINVAL; /* sure, why not */
	else if (err == WSAENETDOWN)
		errno = ENETDOWN;
	else if (err == WSAEFAULT)
		errno = EFAULT;
	else if (err == WSAENOTCONN)
		errno = ENOTCONN;
	else if (err == WSAEINTR)
		errno = EINTR;
	else if (err == WSAEINPROGRESS)
		errno = EINPROGRESS;
	else if (err == WSAENETRESET)
		errno = ENETRESET;
	else if (err == WSAENOTSOCK)
		errno = ENOTSOCK;
	else if (err == WSAEOPNOTSUPP)
		errno = EOPNOTSUPP;
	else if (err == WSAESHUTDOWN)
		errno = ESHUTDOWN;
	else if (err == WSAEWOULDBLOCK)
		errno = EAGAIN;
	else if (err == WSAEMSGSIZE)
		errno = EMSGSIZE;
	else if (err == WSAEINVAL)
		errno = EINVAL;
	else if (err == WSACONNABORTED)
		errno = ECONNABORTED;
	else if (err == WSAETIMEDOUT)
		errno = ETIMEDOUT;
	else if (err == WSAECONNRESET)
		errno = ECONNRESET;
	else
		errno = EINVAL;

	return;
}

ssize_t __fdt_wsaread(int fd, void *buf, size_t count)
{
	int ret;

	if ((ret = recv(fd, buf, len, 0)) == SOCKET_ERROR) {
		wsa_seterrno();
		return -1;
	}

	return ret;
}

ssize_t __fdt_wsawrite(int fd, const void *buf, size_t count)
{
	int ret;

	if ((ret = send(fd, buf, len, 0)) == SOCKET_ERROR) {
		wsa_seterrno();
		return -1;
	}

	return ret;
}

int pfdinit(nbio_t *nb, int pfdsize)
{
	return;
}

void pfdkill(nbio_t *nb)
{
	return;
}

int pfdadd(nbio_t *nb, nbio_fd_t *newfd)
{
	return;
}

void pfdaddfinish(nbio_t *nb, nbio_fd_t *newfd)
{
	return;
}

void pfdrem(nbio_t *nb, nbio_fd_t *fdt)
{
	return;
}

void pfdfree(nbio_fd_t *fdt)
{
	return;
}

int pfdpoll(nbio_t *nb, int timeout)
{
	return;
}

void fdt_setpollin(nbio_t *nb, nbio_fd_t *fdt, int val)
{
	return;
}

void fdt_setpollout(nbio_t *nb, nbio_fd_t *fdt, int val)
{
	return;
}

void fdt_setpollnone(nbio_t *nb, nbio_fd_t *fdt)
{
	return;
}

#endif /* NBIO_USE_WINSOCK2 */
