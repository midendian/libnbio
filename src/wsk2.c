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

#endif /* NBIO_USE_WINSOCK2 */

