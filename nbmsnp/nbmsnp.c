/*
 * Microsoft MSNP5.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif

#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif

#include <string.h>
#include <signal.h>
#include <errno.h>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include "nbmsnp.h"

static nbio_t gnb;

/* Used by d[v]printf macros. */
char *dprintf_ctime(void)
{
	static char retbuf[64];
	struct tm *lt;
	time_t now;

	now = time(NULL);
	lt = localtime(&now);
	strftime(retbuf, 64, "%b %e %H:%M:%S %Z", lt);

	return retbuf;
}


int conndeath(nbio_t *nb, nbio_fd_t *fdt);

static void die(int status)
{

	nbio_alleofforce(&gnb);
	nbio_kill(&gnb);

	exit(status);
}

static void sigint(int signum)
{

	die(0);

	return;
}

static void sigusr2(int signum)
{
	struct msninfo *mi = (struct msninfo *)gnb.priv;
	nbio_fd_t *fdt;

	dprintf("clearing all SBs...\n");

	for (fdt = gnb.fdlist; fdt; fdt = fdt->next) {
		struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;

		if (fdt->type != NBIO_FDTYPE_STREAM)
			continue;

		if (!mci || (mci->type != MCI_TYPE_SB))
			continue;

		if (mci->flags & MCI_FLAG_HASNAME)
			dvprintf("closing SB for %s\n", mci->name);

		conndeath(&gnb, fdt);
	}

	return;
}

static int msn_callback(void *nbv, int event, nbio_fd_t *fdt);

int main(int argc, char **argv)
{
	static const char defaulthost[] = {"messenger.hotmail.com"};
	struct msninfo mi = {
		MI_STATE_DISCONNECTED,
		MI_RUNFLAG_NONE,
		&gnb,
		NULL, NULL, NULL, NULL, 0, 0, /* MSN stuff */
	};
	const char *host = NULL;
	char password[128+1] = {""};
	int i;
	time_t lastrun, loginstart, lastping;
	int logintimeout = 1;

#if 0
	while ((i = getopt(argc, argv, "l:p:s:hvT:")) != EOF) {
		if (i == 'l')
			mi.login = optarg;
		else if (i == 'p') {

			strncpy(password, optarg, sizeof(password));

			/*
			 * This will clear the password from the tables so that
			 * it cannot be publicly viewed via ps or /proc.
			 */
			memset(argv[optind-1], 0, strlen(argv[optind-1]));
		} else if (i == 's')
			host = optarg;
		else if (i == 'v')
			mi.flags |= MI_RUNFLAG_VERBOSE;
		else if (i == 'T') {
			if ((logintimeout = atoi(optarg)) < 0)
				goto usage;
		} else {
		usage:
			printf("nbmsnp v0.10 -- Adam Fritzler (mid@activebuddy.com)\n");
			printf("Usage:\n");
			printf("\tnbmsnp [-v] [-T n] [-s host] -l sn -p passwd\n");
			printf("\t\t-l name\t\tLogin screen name\n");
			printf("\t\t-p password\tLogin password\n");
			printf("\t\t-s hostname\tLogin server host name\n");
			printf("\t\t-v \t\tVerbose\n");
			printf("\t\t-T min\t\tLogin timeout (in minutes)\n");
			printf("\n");
			fflush(stdout);
			exit(1);
			break;
		}
	}

	mi.password = password;

	if (!mi.login || !mi.password || !strlen(mi.password)) {
		dprintf("need login and password\n");
		goto usage;
	}

#else /* for win32... */
	mi.login = "username@host.com";
	strncpy(password, "password", sizeof(password));
	mi.password = password;
	mi.flags |= MI_RUNFLAG_VERBOSE;
#endif

	nbio_init(&gnb, 8192);
	gnb.priv = (void *)&mi;

	dprintf("starting\n");
	dvprintf("login timeout in %d minutes\n", logintimeout);

	signal(SIGINT, sigint);
	signal(SIGTERM, sigint);

#ifdef SIGPIPE
	signal(SIGPIPE, SIG_IGN);
#endif

#ifdef SIGUSR2
 	signal(SIGUSR2, sigusr2);
#endif

	if (!host)
		host = defaulthost;

	mi.state = MI_STATE_CONNECTING;
	dvprintf("connecting to %s...\n", host);
	if (addmsnconn(&mi, host, MCI_TYPE_DS, NULL, NULL, -1) == -1) {
		dprintf("addmsnconn failed\n");
		die(1);
		return -1;
	}

	lastping = loginstart = lastrun = mi.lastnsdata = time(NULL);

	while (mi.state > MI_STATE_DISCONNECTED) {
		time_t now;

		now = time(NULL);

		if ((now - lastrun) > 60) {
			wl_trywaiting(&mi);
			lastrun = now;
		}

		if ((mi.state != MI_STATE_LOGGEDIN) && 
				((now - loginstart) > (logintimeout*60))) {
			dvprintf("MSN is taking too long to log in ... giving up after %ld seconds.\n", (now - loginstart));
			mi.state = MI_STATE_DISCONNECTED;
			break;
		}

		if ((mi.state == MI_STATE_LOGGEDIN) &&
				(mi.flags & MI_RUNFLAG_PNGENA) &&
				!(mi.flags & MI_RUNFLAG_PNGCURBED) &&
				((now - lastping) > 15)) {
			msn_sendpng(mi.nsconn);
			lastping = now;
		}

		/*
		 * Check for the failure case (too long without NS data while
		 * ENAPNG is set).
		 */
		if ((mi.state == MI_STATE_LOGGEDIN) && 
				(mi.flags & MI_RUNFLAG_PNGENA) &&
				!(mi.flags & MI_RUNFLAG_PNGCURBED) &&
				((now - mi.lastnsdata) > 60)) {
			dvprintf("too long without NS data... (%ld seconds)\n", (now - mi.lastnsdata));
			break;
		}

		/*
		 * Check for the impending failure case (too long without NS
		 * data while ENAPNG is not set -- and set it).
		 */
		if ((mi.state == MI_STATE_LOGGEDIN) &&
				!(mi.flags & MI_RUNFLAG_PNGENA) && 
				(now - mi.lastnsdata) > 30) {
			dvprintf("gone %ld seconds without NS data, enabling PNG\n", now - mi.lastnsdata);
			mi.flags |= MI_RUNFLAG_PNGENA;
		}

		if (nbio_poll(mi.nb, 5*1000) == -1)
			break;
	}

	dprintf("stopping (natural)\n");

	die(0);

	return 0;
}

#define MSN_CMD_MAXLEN 512
#define MSN_CMD_DELIM "\r\n"
#define MSN_CMD_DELIM_LEN 2

static int addmsnvec(nbio_t *nb, nbio_fd_t *fdt)
{
	unsigned char *buf;
	int buflen = MSN_CMD_MAXLEN + MSN_CMD_DELIM_LEN + 1;

	if (!(buf = malloc(buflen)))
		return fdtperror(fdt, "addmsnvec: malloc", errno, "");
	memset(buf, 0, buflen);

	if (nbio_addrxvector(nb, fdt, buf, buflen, 0) < 0) {
		fdtperror(fdt, "addmsnvec: nbio_addrxvector", errno, "");
		free(buf);
		return -1;
	}

	return 0;
}

static int connectmsn_handler(void *nbv, int event, nbio_fd_t *fdt)
{
	nbio_t *nb = (nbio_t *)nbv;
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;

	dvprintf("connectmsn_handler event = %d\n", event);

	if (event == NBIO_EVENT_CONNECTED) {

		if (!(mci->fdt = nbio_addfd(nb, NBIO_FDTYPE_STREAM, fdt->fd, 0, msn_callback, (void *)mci, 2, 64))) {
			dvprintf("nbio_addfd: %s\n", strerror(errno));
			nbio_closefdt(nb, fdt); /* XXX is this right? */
			free(mci);
			return 0;
		}

		nbio_adddelim(nb, mci->fdt, MSN_CMD_DELIM, MSN_CMD_DELIM_LEN);

		if (mci->type == MCI_TYPE_NS)
			mci->mi->nsconn = mci->fdt;

		fdterror(mci->fdt, "connection completed");

		mci->state = MCI_STATE_WAITINGFORCMD;
		if (addmsnvec(nb, mci->fdt) == -1)
			return conndeath(nb, mci->fdt);

		if ((mci->type == MCI_TYPE_DS) || (mci->type == MCI_TYPE_NS)) {
	
			/* This starts the state machine commands.c */
			fdterror(mci->fdt, "sending versions...");
			msn_sendver(mci->fdt, "MSNP5");

			if ((mci->type == MCI_TYPE_DS) && (mci->mi->state == MI_STATE_CONNECTING))
				mci->mi->state = MI_STATE_SENTVER;
			else if ((mci->type == MCI_TYPE_NS) && (mci->mi->state == MI_STATE_XFR_TO_NS))
				mci->mi->state = MI_STATE_NS_SENTVER;

		} else if ((mci->type == MCI_TYPE_SB) &&
				(mci->flags & MCI_FLAG_HASNAME) &&
				(mci->flags & MCI_FLAG_HASCKI) &&
				(mci->flags & MCI_FLAG_HASSESSID)) {
	
			msn_sendans(mci->fdt, mci->mi->login, mci->cki, mci->sessid);

		} else if ((mci->type == MCI_TYPE_SB) &&
				(mci->flags & MCI_FLAG_HASNAME) &&
				(mci->flags & MCI_FLAG_HASCKI)) {

			msn_sendusr(mci->fdt, mci->mi->login, NULL, mci->cki);

		} else {
			fdterror(mci->fdt, "unknown connection competed -- killing");
			return conndeath(nb, mci->fdt);
		}


	} else if (event == NBIO_EVENT_CONNECTFAILED) {

		dvprintf("unable to connect to %s\n", mci->hostname);

		return conndeath(nb, fdt);
	}

	return 0;
}

static int connectmsn(nbio_t *nb, const char *host, void *priv)
{
	struct sockaddr_in sa;
	struct hostent *hp;
	unsigned short port = 1863;
	char *newhost;

	newhost = strdup(host);
	if (strchr(newhost, ':')) {
		port = (unsigned short) atoi(strchr(newhost, ':')+1);
		*(strchr(newhost, ':')) = '\0';
	}

	if (!(hp = gethostbyname(newhost ? newhost : host))) { 
		dvprintf("connectmsn: unable to connect to %s -- gethostbyname failed\n", host);
		free(newhost);
		return -1;
	}

	free(newhost);

	memset(&sa, 0, sizeof(struct sockaddr_in));
	sa.sin_port = htons(port);
	memcpy(&sa.sin_addr, hp->h_addr, hp->h_length);
	sa.sin_family = hp->h_addrtype;

	if (nbio_connect(&gnb, (struct sockaddr *)&sa, 
				sizeof(struct sockaddr_in), 
				connectmsn_handler, priv) == -1) {
		dvprintf("connectmsn: unable to connect to %s -- %s\n", host, strerror(errno));
		return -1;
	}

#if 0
	fd = socket(hp->h_addrtype, SOCK_STREAM, 0);
	fcntl(fd, F_SETFL, O_NONBLOCK); /* XXX save flags */

	if ((connect(fd, (struct sockaddr *)&sa, sizeof(struct sockaddr_in)) == -1) && (errno != EINPROGRESS)) {
		dvprintf("connectmsn: unable to connect to %s -- %s\n", host, strerror(errno));
		close(fd);
		return -1;
	}

	return fd;
#endif
	return 0;
}

int addmsnconn(struct msninfo *mi, const char *host, int type, const char *name, const char *cki, int sessid)
{
	struct msnconninfo *mci;
	int fd;

	if (!(mci = malloc(sizeof(struct msnconninfo))))
		return -1;
	memset(mci, 0, sizeof(struct msnconninfo));

	mci->mi = mi;
	mci->type = type;
	mci->state = MCI_STATE_CONNECTING;
	mci->flags = MCI_FLAG_NONE;

	strncpy(mci->hostname, host, sizeof(mci->hostname));
	mci->flags |= MCI_FLAG_HASHOSTNAME;

	if (name) {
		strncpy(mci->name, name, sizeof(mci->name));
		mci->flags |= MCI_FLAG_HASNAME;
	}

	if (cki) {
		strncpy(mci->cki, cki, sizeof(mci->cki));
		mci->flags |= MCI_FLAG_HASCKI;
	}

	if (sessid != -1) {
		mci->sessid = sessid;
		mci->flags |= MCI_FLAG_HASSESSID;
	}

	if ((fd = connectmsn(mi->nb, host, mci)) == -1) {
		free(mci);
		return -1;
	}

	return 0;
}

static void clearbufs(nbio_t *nb, nbio_fd_t *fdt)
{
	unsigned char *buf;
	int i;

	if (!fdt)
		return;

	for (i = 0; (buf = nbio_remtoprxvector(nb, fdt, NULL, NULL)); i++)
		free(buf);
	for (i = 0; (buf = nbio_remtoptxvector(nb, fdt, NULL, NULL)); i++)
		free(buf);

	return;
}

/* Return -1 if the connection was required for normal operation. */
int conndeath(nbio_t *nb, nbio_fd_t *fdt)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	int fatal = 0;

	clearbufs(nb, fdt);
	nbio_closefdt(nb, fdt);

	if (fdt == mci->mi->nsconn)
		mci->mi->nsconn = NULL;

	if ((((mci->type == MCI_TYPE_DS) && 
				(mci->mi->state < MI_STATE_XFR_TO_NS))) || 
			(mci->type == MCI_TYPE_NS)) {

		fdterror(fdt, "major connection death");
		dvprintf("mi->state = %d\n", mci->mi->state);

		mci->mi->state = MI_STATE_DISCONNECTED;
		fatal = 1;

	} else if (mci->type == MCI_TYPE_SB) {

		if (mci->mi->flags & MI_RUNFLAG_VERBOSE) {
			dvprintf("switchboard died (%s, %s)\n",
					(mci->flags & MCI_FLAG_HASNAME) ? 
						mci->name :
						"unassociated",
					(mci->flags & MCI_FLAG_HASHOSTNAME) ?
						mci->hostname :
						"hostname unknown");
		}

	}

	free(mci);

	return 0;
}

/* This returns -1 only because that can make it a more useful construction. */
int fdterror(nbio_fd_t *fdt, const char *msg)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;

	dvprintf("[%s%s%s, %s, %s] %s\n",
			(mci && (mci->type == MCI_TYPE_DS)) ? "DS" : "",
			(mci && (mci->type == MCI_TYPE_NS)) ? "NS" : "",
			(mci && (mci->type == MCI_TYPE_SB)) ? "SB" : "",
			(mci && (mci->flags & MCI_FLAG_HASNAME)) ?
				mci->name : "unknown",
			(mci && (mci->flags & MCI_FLAG_HASHOSTNAME)) ? 
				mci->hostname : "unknown host",
			msg);

	return -1;
}

/* This returns -1 only because that can make it a more useful construction. */
int fdtperror(nbio_fd_t *fdt, const char *func, int err, const char *msg)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;

	dvprintf("[%s%s%s, %s, %s] %s: %s %s\n",
			(mci && (mci->type == MCI_TYPE_DS)) ? "DS" : "",
			(mci && (mci->type == MCI_TYPE_NS)) ? "NS" : "",
			(mci && (mci->type == MCI_TYPE_SB)) ? "SB" : "",
			(mci && (mci->flags & MCI_FLAG_HASNAME)) ?
				mci->name : "unknown",
			(mci && (mci->flags & MCI_FLAG_HASHOSTNAME)) ? 
				mci->hostname : "unknown host",
			func,
			strerror(err),
			msg);

	return -1;
}

/*
 * Return:
 * 	 0 - No error
 * 	-1 - Fatal error
 * 	-2 - Connection error, continue but close fdt.
 *
 */
static int handlecmd(nbio_fd_t *fdt)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	int len, offset;
	unsigned char *buf;

	if (!(buf = nbio_remtoprxvector(mci->mi->nb, fdt, &len, &offset))) {
		fdtperror(fdt, "handlecmd: nbio_remtoprxvector", errno, "no data buffer waiting!\n");
		return -2;
	}

	if (mci->type == MCI_TYPE_NS) {
		if (mci->mi->flags & MI_RUNFLAG_PNGENA) {
			mci->mi->flags &= ~MI_RUNFLAG_PNGENA;
			dvprintf("NS connection recovered -- went %ld seconds without data\n", time(NULL) - mci->mi->lastnsdata);
		}
		mci->mi->lastnsdata = time(NULL);
	}

	if (mci->state == MCI_STATE_WAITINGFORCMD) {
		int ret = 0;

		if (strncmp(buf, "MSG", 3) == 0) {
			char *dataptr, *handle, *custom, *lenstr;
			int msglen;

			/*
			 * MSGs are special in that they contain a length
			 * field that forces us to read farther and in an 
			 * undelimited state.
			 *
			 * This, right here, is why I hate the MSNP protocol.
			 *
			 * (Okay, so the SB mess is part of it, too. But mostly
			 * this. And their inconsistent use of TrIDs.)
			 *
			 */

			/* Grr.. They don't put TrID's on MSG's.  Damnit. */
			dataptr = buf+4; /* "MSG " */
			handle = dataptr;

			if (!(custom = strchr(handle, ' '))) {
				fdterror(fdt, "handlecmd: parse error for MSG");
				free(buf);
				return -2;
			}
			custom++;
			if (!(lenstr = strchr(custom, ' '))) {
				fdterror(fdt, "handlecmd: parse error for MSG (len)");
				free(buf);
				return -2;
			}
			lenstr++;

			if ((msglen = atoi(lenstr)) < 1) {
				fdterror(fdt, "handlecmd: parse error for MSG (invalid len)\n");
				free(buf);
				return -2;
			}

			/*
			 * Put the old buffer back onto the queue, allocate a
			 * new one just for the MSG payload, then add it to the
			 * queue.
			 */
			if (nbio_addrxvector(mci->mi->nb, fdt, buf, len, len) == -1) {
				free(buf);
				fdtperror(fdt, "handlecmd: nbio_addrxvector", errno, "(while adding MSG back)");
				return -2;
			}
			if (!(buf = malloc(msglen+1))) {
				fdtperror(fdt, "handlecmd: malloc", errno, "(while allocating payload buffer");
				return -2;
			}
			memset(buf, 0, msglen+1);
			if (nbio_addrxvector(mci->mi->nb, fdt, buf, msglen, 0) == -1) {
				free(buf);
				fdtperror(fdt, "handlecmd: nbio_addrxvector", errno, "(while adding MSG payload)");
				return -2;
			}


			/* payload is not terminated, shut off delimiting */
			nbio_cleardelim(fdt);

			mci->state = MCI_STATE_WAITINGFORPAYLOAD;

			return 0;

		}
	
		if (mci->mi->flags & MI_RUNFLAG_VERBOSE) {
			dvprintf("[%s%s%s, %s, %s] ---> RECEIVED: %s\n",
				(mci && 
				 (mci->type == MCI_TYPE_DS)) ?
					"DS" : "",
				(mci && 
				 (mci->type == MCI_TYPE_NS)) ? 
					"NS" : "",
				(mci && 
				 (mci->type == MCI_TYPE_SB)) ? 
					"SB" : "",
				(mci && 
				 (mci->flags & MCI_FLAG_HASNAME)) ?
					mci->name : "unknown",
				(mci && 
				 (mci->flags & MCI_FLAG_HASHOSTNAME)) ? 
					mci->hostname : "unknown host",
				buf);
		}

		if ((ret = processmsncmd(fdt, buf, NULL)) < 0) {
			free(buf);
			return ret;
		}

		free(buf);
		addmsnvec(mci->mi->nb, fdt);
		mci->state = MCI_STATE_WAITINGFORCMD; /* no change */

		return 0;

	} else if (mci->state == MCI_STATE_WAITINGFORPAYLOAD) {
		char *payload;
		int plen, poff;
		int ret = 0;

		if (!(payload = nbio_remtoprxvector(mci->mi->nb, fdt, &plen, &poff))) {
			fdtperror(fdt, "handlecmd: nbio_remtoprxvector", errno, "no data buffer waiting! (payload)\n");

			return -2;
		}

		if (mci->mi->flags & MI_RUNFLAG_VERBOSE) {
			dvprintf("[%s%s%s, %s, %s] ---> RECEIVED: %s\n",
				(mci && 
				 (mci->type == MCI_TYPE_DS)) ?
					"DS" : "",
				(mci && 
				 (mci->type == MCI_TYPE_NS)) ? 
					"NS" : "",
				(mci && 
				 (mci->type == MCI_TYPE_SB)) ? 
					"SB" : "",
				(mci && 
				 (mci->flags & MCI_FLAG_HASNAME)) ?
					mci->name : "unknown",
				(mci && 
				 (mci->flags & MCI_FLAG_HASHOSTNAME)) ? 
					mci->hostname : "unknown host",
				buf);
			dvprintf("[%s%s%s, %s, %s] ---> PAYLOAD: %s\n",
				(mci && 
				 (mci->type == MCI_TYPE_DS)) ?
					"DS" : "",
				(mci && 
				 (mci->type == MCI_TYPE_NS)) ? 
					"NS" : "",
				(mci && 
				 (mci->type == MCI_TYPE_SB)) ? 
					"SB" : "",
				(mci && 
				 (mci->flags & MCI_FLAG_HASNAME)) ?
					mci->name : "unknown",
				(mci && 
				 (mci->flags & MCI_FLAG_HASHOSTNAME)) ? 
					mci->hostname : "unknown host",
				payload);
		}

		if ((ret = processmsncmd(fdt, buf, payload)) < 0) {
			free(payload);
			free(buf);
			return ret;
		}

		free(payload);
		free(buf);

		mci->state = MCI_STATE_WAITINGFORCMD;
		addmsnvec(mci->mi->nb, fdt);

		/* go back to delimited mode */
		if (nbio_adddelim(mci->mi->nb, fdt, MSN_CMD_DELIM, MSN_CMD_DELIM_LEN) == -1) {
			fdtperror(fdt, "handlecmd: nbio_adddelim", errno, "");
			return -2;
		}

		return 0;
	}

	return 0;
}

static int msn_callback(void *nbv, int event, nbio_fd_t *fdt)
{
	nbio_t *nb = (nbio_t *)nbv;
	struct msnconninfo *mci;

	if (!fdt || !(mci = (struct msnconninfo *)fdt->priv))
		abort();

	if (mci->state == MCI_STATE_CONNECTING)
		abort();

	if (event == NBIO_EVENT_READ) {
		int status;

		if ((status = handlecmd(fdt)) == -1) {

			conndeath(nb, fdt);

			return -1;

		} else if (status == -2)
			return conndeath(nb, fdt);

		return 0;

	} else if (event == NBIO_EVENT_WRITE) {
		unsigned char *buf;
		int offset, len;

		if ((buf = nbio_remtoptxvector(nb, fdt, &len, &offset)))
			free(buf);

		return 0;

	} else if ((event == NBIO_EVENT_ERROR) || (NBIO_EVENT_EOF)) {

		if (mci->mi->flags & MI_RUNFLAG_VERBOSE)
			fdterror(fdt, "EVENT_ERROR/EOF");

		return conndeath(nb, fdt);
	}

	fdterror(fdt, "this never happened!\n");

	return -1; /* unknown */
}

int imfromservice(nbio_fd_t *srcfdt, const char *name, const char *msg)
{
	struct msnconninfo *mci = (struct msnconninfo *)srcfdt->priv;

	if (mci->mi->flags & MI_RUNFLAG_VERBOSE)
		dvprintf("imfromservice: %s / %s\n", name, msg);

	/* XXX do something here */

	return 1;
}


