/* -*- Mode: ab-c -*- */

#ifndef __IMPL_H__
#define __IMPL_H__

#ifdef NBIO_USE_WINSOCK2
#define fdt_read __fdt_wsaread
#define fdt_write __fdt_wsawrite
#else
#define fdt_read __fdt_read
#define fdt_write __fdt_write
#endif

/* libnbio.c (used by both kqueue and poll) */
int __fdt_read(nbio_fd_t *fdt, void *buf, int count);
int __fdt_write(nbio_fd_t *fdt, const void *buf, int count);

/* for calling inside poll implementations */
void freefdt(nbio_fd_t *fdt);


/* Common API */

/* provided by implementation */
int pfdinit(nbio_t *nb, int pfdsize);
void pfdkill(nbio_t *nb);
int pfdadd(nbio_t *nb, nbio_fd_t *newfd);
void pfdaddfinish(nbio_t *nb, nbio_fd_t *newfd);
void pfdrem(nbio_t *nb, nbio_fd_t *fdt);
void pfdfree(nbio_fd_t *fdt);
int pfdpoll(nbio_t *nb, int timeout);
void fdt_setpollin(nbio_t *nb, nbio_fd_t *fdt, int val);
void fdt_setpollout(nbio_t *nb, nbio_fd_t *fdt, int val);
void fdt_setpollnone(nbio_t *nb, nbio_fd_t *fdt);

/* provided by libnbio.c */
int __fdt_ready_in(nbio_t *nb, nbio_fd_t *fdt);
int __fdt_ready_out(nbio_t *nb, nbio_fd_t *fdt);
int __fdt_ready_eof(nbio_t *nb, nbio_fd_t *fdt);
int __fdt_ready_all(nbio_t *nb, nbio_fd_t *fdt);

#endif /* __IMPL_H__ */

