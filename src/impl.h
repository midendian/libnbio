/* -*- Mode: ab-c -*- */

#ifndef __IMPL_H__
#define __IMPL_H__

#ifdef NBIO_USE_WINSOCK2
#define fdt_read __fdt_wsaread
#else
#define fdt_read __fdt_read
#endif

/* libnbio.c (used by both kqueue and poll) */
int __fdt_read(nbio_fd_t *fdt, void *buf, int count);

/* Common API */
int pfdinit(nbio_t *nb, int pfdsize);
void pfdkill(nbio_t *nb);
int pfdadd(nbio_t *nb, nbio_fd_t *newfd);
void pfdaddfinish(nbio_t *nb, nbio_fd_t *newfd);
void pfdrem(nbio_t *nb, nbio_fd_t *fdt);
void pfdfree(nbio_fd_t *fdt);
int pfdpoll(nbio_t *nb, int timeout);

#endif /* __IMPL_H__ */

