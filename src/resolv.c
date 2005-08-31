/*
 * libnbio - Portable wrappers for non-blocking sockets
 * Copyright (c) 2000-2005 Adam Fritzler <mid@zigamorph.net>, et al
 *
 * libnbio is free software; you can redistribute it and/or modify it under the
 * terms of the GNU Lesser General Public License (version 2.1) as published by
 * the Free Software Foundation.
 *
 * libnbio is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include <libnbio.h>
#include "resolv.h"

#if 0
#define LIBNBIO_RESOLV_RESOLVCONFFN "/etc/resolv.conf"
#define LIBNBIO_RESOLV_HOSTSFN "/etc/hosts"
#else
#define LIBNBIO_RESOLV_RESOLVCONFFN "/Users/mid/resolv.conf"
#define LIBNBIO_RESOLV_HOSTSFN "/Users/mid/hosts"
#endif

struct hfline {
	socklen_t socklen;
	struct sockaddr sock;
	char *name;
	char *aliases; /* unparsed, I guess */

	struct hfline *next;
};

/* new transaction is created every time gethostbyname is called */
struct rtrans {
};

/* stored in nbio_t */
struct nbio__resolvinfo {

	/* pending resolver transactions */
	struct rtrans *trans;

	/* /etc/resolv.conf */
	time_t resolvstamp; /* ctime on last parse */
#define LIBNBIO_RESOLV_DEBUG_DEFAULT 10
	int debug;
#define LIBNBIO_RESOLV_MAXNAMELEN 256
	char *domainsuffix;
#define LIBNBIO_RESOLV_MAXDNS 3 /* unix standard (MAXDNS) */
	char *nameservers[LIBNBIO_RESOLV_MAXDNS];
#define LIBNBIO_RESOLV_NDOTS_DEFAULT 1 /* unix standard */
	int ndots; /* resolv.conf: option ndots */

	/* /etc/hosts */
	time_t hoststamp; /* ctime of /etc/hosts on last parse */
	struct hfline *hosts; /* parsed /etc/hosts */

	/* XXX need a resolver cache for DNS */
};


static int readln(int fd, char *buf, int buflen)
{
	int i;

	*buf = '\0';
	for (i = 0; i < buflen; i++, buf++) {
		int r;
		r = read(fd, buf, 1);
		if (r <= 0)
			break;
		else if ((*buf == '\n') || (*buf == '\r'))
			break;
	}

	*buf = '\0';
	return i;
}

#define ISWHITESPACE(x) ( ((x) == ' ') || ((x) == '\t') || \
			    ((x) == '\n') || ((x) == '\r') )
#define FIRSTCHAR(x) do { \
	while ( (*(x) != '\0') && ISWHITESPACE(*(x)) ) \
		(x)++; \
} while (0);
#define FIRSTWHITE(x) do { \
	while ( (*(x) != '\0') && !ISWHITESPACE(*(x)) ) \
		(x)++; \
} while (0);
#define TRIMRIGHT(x) do { \
	char *__c; \
	__c = (x) + strlen((x)); \
	while (ISWHITESPACE(*__c)) \
		__c--; \
	__c = '\0'; \
} while (0);


/* called from libnbio.c::nbio_init() */
int nbio_resolv__init(nbio_t *nb)
{
	struct nbio__resolvinfo *ri;

	if (!(ri = (struct nbio__resolvinfo *)malloc(sizeof(struct nbio__resolvinfo))))
		return -1;
	memset(ri, 0, sizeof(struct nbio__resolvinfo));

	ri->debug = LIBNBIO_RESOLV_DEBUG_DEFAULT;

	nb->resolv = ri;
	return 0;
}

static int resolvinfo_resolv_load(nbio_t *nb)
{
	return 0;
}

static void resolvinfo_resolv_free(nbio_t *nb)
{
	int i;

	if (nb->resolv->domainsuffix) {
		free(nb->resolv->domainsuffix);
		nb->resolv->domainsuffix = NULL;
	}
	for (i = 0; i < LIBNBIO_RESOLV_MAXDNS; i++) {
		if (nb->resolv->nameservers[i]) {
			free(nb->resolv->nameservers[i]);
			nb->resolv->nameservers[i] = NULL;
		}
	}

	return;
}

static struct hfline *hf__alloc(void)
{
	struct hfline *hf;

	if (!(hf = (struct hfline *)malloc(sizeof(struct hfline))))
		return NULL;
	memset(hf, 0, sizeof(struct hfline));

	return hf;
}

static void hf__free(struct hfline *hf)
{

	if (hf->name)
		free(hf->name);
	if (hf->aliases)
		free(hf->aliases);
	free(hf);

	return;
}

static struct hfline *hf__new(struct in_addr *in, const char *name, const char *aliases)
{
	struct hfline *hf;
	struct sockaddr_in *sin;

	if (!(hf = hf__alloc()))
		return NULL;
	if (!(hf->name = strdup(name)) ||
	    (aliases && !(hf->aliases = strdup(aliases)))) {
		hf__free(hf);
		return NULL;
	}

	/* always IPv4 for now. */
	sin = (struct sockaddr_in *)&hf->sock;
	sin->sin_family = AF_INET;
	memcpy(&sin->sin_addr, in, sizeof(struct in_addr));

	return hf;
}

static void resolvinfo_hosts_add(nbio_t *nb, const char *addr, const char *name, const char *aliases)
{
	struct in_addr in;
	struct hfline *hf;

	if (!addr || !name)
		return;

	if (inet_aton(addr, &in) != 1) {
		if (nb->resolv->debug > 0)
			fprintf(stderr, "resolvinfo_hosts_add: invalid address in hosts file: '%s'\n", addr);
		return; /* invalid address */
	}

	if (!(hf = hf__new(&in, name, aliases)))
		return;
	hf->next = nb->resolv->hosts;
	nb->resolv->hosts = hf;

	return;
}

#define LIBNBIO_RESOLV_MAXLINELEN 2048
static int resolvinfo_hosts_load(nbio_t *nb)
{
	char buf[LIBNBIO_RESOLV_MAXLINELEN];
	int fd;

	if ((fd = open(LIBNBIO_RESOLV_HOSTSFN, O_RDONLY)) == -1)
		return -1;

	while (readln(fd, buf, sizeof(buf)) > 0) {
		char *s;
		char *addr, *name, *aliases = NULL;

		if ((s = strchr(buf, '#')))
			*s = '\0';

		/* ADDR WS NAME [WS [ALIAS[ WS ALIAS]]] */
		s = buf;
		FIRSTCHAR(s);
		if (!strlen(s))
			continue; /* address required */
		addr = s;
		FIRSTWHITE(s);
		if (!strlen(s))
			continue; /* WS before host name required */
		*(s++) = '\0';
		FIRSTCHAR(s);
		if (!strlen(s))
			continue; /* host name required */
		name = s;
		FIRSTWHITE(s);
		if (strlen(s)) { /* aliases optional */
			*(s++) = '\0';
			FIRSTCHAR(s);
			aliases = strlen(s) ? s : NULL;
		}

		TRIMRIGHT(addr);
		TRIMRIGHT(name);

		resolvinfo_hosts_add(nb, addr, name, aliases);
	}

	close(fd);

	return 0;
}

static void resolvinfo_hosts_free(nbio_t *nb)
{
	struct hfline *hf;

	for (hf = nb->resolv->hosts; hf; ) {
		struct hfline *tmp;

		tmp = hf->next;
		hf__free(hf);
		hf = tmp;
	}

	return;
}

/* called from libnbio.c::nbio_kill() */
void nbio_resolv__free(nbio_t *nb)
{

	if (!nb->resolv)
		return;

	resolvinfo_resolv_free(nb);
	resolvinfo_resolv_free(nb);
	free(nb->resolv);

	return;
}

static int updateconfig(nbio_t *nb)
{
	struct stat st;

	if ((stat(LIBNBIO_RESOLV_RESOLVCONFFN, &st) == -1) ||
	    (st.st_ctime != nb->resolv->hoststamp)) {

		if (nb->resolv->debug > 0)
			fprintf(stderr, "nbio_resolv__updateconfig: reloading resolv.conf\n");

		resolvinfo_resolv_free(nb);

		nb->resolv->ndots = LIBNBIO_RESOLV_NDOTS_DEFAULT;

		resolvinfo_resolv_load(nb);

		if (!nb->resolv->domainsuffix) {
			char hn[LIBNBIO_RESOLV_MAXNAMELEN+1], *c;

			/* default domain suffix comes from gethostbyname */
			if ((gethostname(hn, sizeof(hn)) == 0) &&
			    (c = strchr(hn, '.')) &&
			    (strlen(c) > 1)) {
				nb->resolv->domainsuffix = strdup(c + 1);
				if (nb->resolv->debug > 0)
					fprintf(stderr, "nbio_resolv__updateconfig: using '%s' as domain suffix\n", nb->resolv->domainsuffix);
			}
		}
	}

	if ((stat(LIBNBIO_RESOLV_HOSTSFN, &st) == -1) ||
	    (st.st_ctime != nb->resolv->hoststamp)) {

		if (nb->resolv->debug > 0)
			fprintf(stderr, "nbio_resolv__updateconfig: reloading hosts file\n");

		resolvinfo_hosts_free(nb);

		resolvinfo_hosts_load(nb);
	}

	return 0;
}

/*
 * We try to work as much like a traditional gethostbyname() as possible.
 * Basically, any function that calls gethostbyname() can be cut in half:
 * the part before and the part after the gethostbyname call.  Move the second
 * half to the callback.  The callback will give a struct hostent * as a
 * parameter, instead of gethostbyname() returning it.  The callback returning
 * -1 should be the same as elsewhere in libnbio: the nbio_poll loop should
 * exit.
 *
 * General procedure:
 *   1) If it's an address in dotted-quad notation already, reformat/expand
 *      as appropriate, and return.
 *   2) Check in /etc/hosts (which is cached).  If there, return.
 *   3) Build DNS query according to rules in /etc/resolv.conf, send that.
 *   4) Return on success or NULL on timeout/error.
 */
int nbio_gethostbyname(nbio_t *nb, nbio_gethostbyname_callback_t ufunc, void *udata, const char *query)
{

	if (updateconfig(nb) == -1)
		return -1;

	return -1;
}

/*
 *
 */
int nbio_resolv__looppass(nbio_t *nb)
{
}

