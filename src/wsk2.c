/* -*- Mode: ab-c -*- */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <libnbio.h>
#include "impl.h"

#ifdef NBIO_USE_WINSOCK2

#include <winsock2.h>

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

int fdt_read(nbio_fd_t *fdt, void *buf, int count)
{
	int ret;

	if ((ret = recv(fd, buf, len, 0)) == SOCKET_ERROR) {
		wsa_seterrno();
		return -1;
	}

	return ret;
}

int fdt_write(nbio_fd_t *fdt, const void *buf, int count)
{
	int ret;

	if ((ret = send(fd, buf, len, 0)) == SOCKET_ERROR) {
		wsa_seterrno();
		return -1;
	}

	return ret;
}

void fdt_close(nbio_fd_t *fdt)
{

	closesocket(fdt->fd);

	return;
}

int fdt_setnonblock(int fd)
{
	return -1;
}

static int wsainit(void)
{
	WORD reqver;
	WSADATA wsadata;

	reqver = MAKEWORD(2,2);

	if ((WSAStartup(reqver, &wsadata)) != 0)
		return -1;

	if ((LOBYTE(wsadata.wVersion != 2) || (HIBYTE(wsadata.wVersion != 2)) {
		WSACleanup();
		return -1;
	}

	return 0;
}

int pfdinit(nbio_t *nb, int pfdsize)
{

	if (wsainit() == -1)
		return -1;

	return;
}

void pfdkill(nbio_t *nb)
{

	WSACleanup();

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

