
#include <stdlib.h>
#include <errno.h>
#include <string.h>

#include "nbmsnp.h"
#include "md5.h"

#define DEFAULTMIMEHEADER "MIME-Version: 1.0\r\nContent-Type: text/plain; charset=UTF-8\r\nX-MMS-IM-Format: FN=Lucida%20Sans%20Unicode; EF=; CO=0; CS=0; PF=22\r\n\r\n"

struct waitingmsg {
	char name[MSN_MAX_NAME_LEN+1];
	char *msg;
	time_t entrytime;
	struct waitingmsg *next;
};

static struct waitingmsg *waitinglist = NULL;
static int waitinglistlen = 0;

#define MAX_WL_LEN 100

static int msn_domd5(char *destbuf, const char *passwd, const char *append)
{
	md5_byte_t digest[16];
	md5_state_t state;
	char tmpbuf[128];
	int i;

	sprintf(tmpbuf, "%s%s", append, passwd);

	md5_init(&state);
	md5_append(&state, (const md5_byte_t *)tmpbuf, strlen(tmpbuf));
	md5_finish(&state, digest);

	for (i = 0; i < 16; i++, destbuf += 2)
		sprintf(destbuf, "%02x", digest[i]);

	return 0;
}

struct msn_error {
	int num;
	const char desc[128];
	int fatal;
};

static const struct msn_error msn_errors[] = {
	{200, "Syntax error", 1},
	{201, "Invalid Parameter", 0},
	{205, "Invalid user", 0},
	{206, "FQDN Missing", 0},
	{207, "Already login", 1},
	{208, "Invalid user name", 0},
	{209, "Invalid friendly name", 0},
	{210, "List full", 0},
	{215, "Already there", 0},
	{216, "Not on list", 0},
	{217, "Unknown (217)", 0},
	{218, "Already in the mode", 0},
	{219, "Already in opposite list", 0},
	{280, "Switchboard failed", 1},
	{281, "Notify XFR failed", 1},
	{300, "Required fields missing", 0},
	{302, "Not logged in", 0},
	{500, "Internal server error", 1},
	{501, "DB server", 1},
	{510, "File operation", 1},
	{520, "Memory alloc", 1},
	{600, "Server busy", 1},
	{601, "Server unavailable", 1},
	{602, "Peer NS down", 1},
	{603, "DB Connect", 1},
	{604, "Server going down", 1},
	{707, "Create connection", 1},
	{711, "Blocking write", 1},
	{712, "Session overload", 1},
	{713, "User too active", 1},
	{714, "Too many sessions", 1},
	{715, "Not expected", 1},
	{717, "Bad friend file", 1},
	{910, "Service too busy", 1}, /* not in spec */
	{911, "Authentication failed", 1},
	{913, "Not allowed when offline", 0},
	{919, "Service not available", 1}, /* not in spec */
	{920, "Not accepting new users", 1},
	{921, "Temporarily unavailable", 1} /* not in spec */
};
#define MSN_ERROR_COUNT (sizeof(msn_errors)/sizeof(struct msn_error))

static const char *msn_strerror(int err)
{
	int i;

	for (i = 0; i < MSN_ERROR_COUNT; i++) {
		if (msn_errors[i].num == err)
			return msn_errors[i].desc;
	}

	return NULL;
}

static int msn_errisfatal(int err)
{
	int i;

	for (i = 0; i < MSN_ERROR_COUNT; i++) {
		if (msn_errors[i].num == err)
			return msn_errors[i].fatal;
	}

	return 1; /* unknown errors default to FATAL */
}

#define iscmd(b, c) (strncasecmp(b, c, 3) == 0)
static const char *wl_getawaitinguser(void);

int processmsncmd(nbio_fd_t *fdt, char *buf, char *payload)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;

	if (iscmd(buf, "MSG")) {

		if ((mci->type == MCI_TYPE_SB) && 
				(mci->flags & MCI_FLAG_HASNAME) &&
				payload) {
			char *sender, *sendercustom, *lenstr;

			/* MSG mid@zigamorph.net mid@zigamorph.net 90 */

			sender = buf + strlen("MSG") + 1;
			sendercustom = index(sender, ' ');
			*sendercustom = '\0';
			sendercustom++;
			lenstr = index(sendercustom, ' ');
			*lenstr = '\0';
			lenstr++;

			if (strcasecmp(sender, mci->name) != 0) {
				fdterror(fdt, "chat has become non-binary -- leaving.");
				return -2;
			}

			if (strstr(payload, "text/x-msmsgscontrol")) {
				; /* ignore these ("typing") */
			} else if ((payload = strstr(payload, "\r\n\r\n"))) {
				payload += strlen("\r\n\r\n");
				sendmsnmsg_typing(mci->mi, fdt, sender);
				imfromservice(fdt, sender, payload);
			}
		}
	
	} else if (iscmd(buf, "VER")) {

		if ((mci->type == MCI_TYPE_DS) &&
				(mci->mi->state == MI_STATE_SENTVER)) {

			msn_sendinf(fdt, NULL);
			mci->mi->state = MI_STATE_SENTINF;

		} else if ((mci->type == MCI_TYPE_NS) &&
				(mci->mi->state == MI_STATE_NS_SENTVER)) {

			msn_sendinf(fdt, NULL);
			mci->mi->state = MI_STATE_NS_SENTINF;
		}

	} else if (iscmd(buf, "INF")) {
		char *tridstr, *sp;

		tridstr = buf + strlen("INF") + 1;
		sp = index(tridstr, ' ');
		*sp = '\0';
		sp++;

		if ((mci->type == MCI_TYPE_DS) &&
				(mci->mi->state == MI_STATE_SENTINF)) {
			msn_sendusr(fdt, sp, "I", mci->mi->login);
			mci->mi->state = MI_STATE_SENTUSR_I;
		} else if ((mci->type == MCI_TYPE_NS) &&
				(mci->mi->state == MI_STATE_NS_SENTINF)) {
			msn_sendusr(fdt, sp, "I", mci->mi->login);
			mci->mi->state = MI_STATE_NS_SENTUSR_I;
		}

	} else if (iscmd(buf, "USR")) {
		char *tridstr, *sp, *seq = NULL, *info = NULL;

		tridstr = buf + strlen("USR") + 1;
		sp = index(tridstr, ' ');
		*sp = '\0';
		sp++;

		if (strncasecmp(sp, "OK", 2) != 0) {
			seq = index(sp, ' ');
			*seq = '\0';
			seq++;
			info = index(seq, ' ');
			*info = '\0';
			info++;
		}

		if ((mci->type == MCI_TYPE_NS) &&
				(mci->mi->state == MI_STATE_NS_SENTUSR_I)) {
			char hash[128];

			msn_domd5(hash, mci->mi->password, info);
			msn_sendusr(fdt, sp, "S", hash);
			mci->mi->state = MI_STATE_NS_SENTUSR_S;

		} else if ((mci->type == MCI_TYPE_NS) &&
				(mci->mi->state == MI_STATE_NS_SENTUSR_S)) {

			if (strncasecmp(sp, "OK", 2) != 0)
				return -1;

			dprintf("logged in\n");

			mci->mi->state = MI_STATE_LOGGEDIN;

			msn_sendchg(fdt, "NLN");

		} else if (mci->type == MCI_TYPE_SB) {

			if (strncasecmp(sp, "OK", 2) != 0)
				return -1;

			if (mci->flags & MCI_FLAG_HASNAME)
				msn_sendcal(fdt, mci->name);
		}

	} else if (iscmd(buf, "XFR")) {
		char *tridstr, *type, *ip, *sp, *cki = NULL;

		tridstr = buf + strlen("XFR") + 1;
		type = index(tridstr, ' ');
		*type = '\0';
		type++;
		ip = index(type, ' ');
		*ip = '\0';
		ip++;
		sp = index(ip, ' ');
		*sp = '\0';
		sp++;
		if (strncmp(sp, "CKI", 3) == 0) {
			cki = index(sp, ' ');
			*cki = '\0';
			cki++;
		}

		if ((mci->type == MCI_TYPE_DS) &&
				(mci->mi->state == MI_STATE_SENTUSR_I) &&
				(strncasecmp(type, "NS", 2) == 0)) {

			dvprintf("connecting to NS %s...\n", ip);
			if (addmsnconn(mci->mi, ip, MCI_TYPE_NS, NULL, NULL, -1) == -1)
				return -1;
			mci->mi->state = MI_STATE_XFR_TO_NS;

		} else if ((mci->type == MCI_TYPE_NS) &&
				(strcmp(type, "SB") == 0) &&
				cki) {
			const char *waitinguser;

			waitinguser = wl_getawaitinguser();

			if (addmsnconn(mci->mi, ip, MCI_TYPE_SB, waitinguser, cki, -1) == -1)
				return 0; /* Not being able to open an SB is not fatal */
		}

	} else if (iscmd(buf, "RNG")) {
		char *tridstr, *ip, *sp, *cki, *name, *custom;

		tridstr = buf + strlen("RNG") + 1;
		ip = index(tridstr, ' ');
		*ip = '\0';
		ip++;
		sp = index(ip, ' ');
		*sp = '\0';
		sp++;
		cki = index(sp, ' ');
		*cki = '\0';
		cki++;
		name = index(cki, ' ');
		*name = '\0';
		name++;
		if ((custom = index(name, ' '))) {
			*custom = '\0';
			custom++;
		}

		if (addmsnconn(mci->mi, ip, MCI_TYPE_SB, name, cki, atoi(tridstr)) == -1)
			return 0; /* Not being able to open an SB is not fatal */
		
	} else if (iscmd(buf, "IRO")) {
		char *tridstr, *num, *total, *name, *custom;

		tridstr = buf + strlen("IRO") + 1;
		num = index(tridstr, ' ');
		*num = '\0';
		num++;
		total = index(num, ' ');
		*total = '\0';
		total++;
		name = index(total, ' ');
		*name = '\0';
		name++;
		if ((custom = index(name, ' '))) {
			*custom = '\0';
			custom++;
		}

		if ((mci->flags & MCI_FLAG_HASNAME) && 
				(strcasecmp(mci->name, name) != 0)) {
			fdterror(fdt, "chat has become non-binary -- leaving.");
			return -2;
		}

		mci->flags |= MCI_FLAG_SBREADY;

		wl_trywaiting(mci->mi);

	} else if (iscmd(buf, "JOI")) {
		char *tridstr, *name, *custom;

		tridstr = buf + strlen("JOI") + 1;
		name = index(tridstr, ' ');
		*name = '\0';
		name++;
		if ((custom = index(name, ' '))) {
			*custom = '\0';
			custom++;
		}

		if ((mci->flags & MCI_FLAG_HASNAME) &&
				(strcasecmp(mci->name, name) != 0)) {
			fdterror(fdt, "chat has become non-binary -- leaving.");
			return -2;
		}

		mci->flags |= MCI_FLAG_SBREADY;

		wl_trywaiting(mci->mi);

	} else if (iscmd(buf, "ANS")) {
		char *tridstr, *status;

		tridstr = buf + strlen("ANS") + 1;
		status = index(tridstr, ' ');
		*status = '\0';
		status++;

		if ((mci->type == MCI_TYPE_SB) &&
				(strncmp(status, "OK", 2) == 0)) {
			mci->flags |= MCI_FLAG_SBREADY;
		}

	} else if (iscmd(buf, "BYE")) {

		if (mci->type == MCI_TYPE_SB)
			return -2;
		else if (mci->type == MCI_TYPE_NS) {
			fdterror(fdt, "being kicked off");
			return -1;
		}

	} else if (iscmd(buf, "QNG")) {

		/* 
		 * Ignored.
		 *
		 * The timer is reset when ANY data is recieved, not 
		 * just a QNG 
		 */

	} else if (iscmd(buf, "CAL")) {

		/* usually RINGING */

	} else if (iscmd(buf, "LST")) {
		char *tridstr, *list, *serid, *num, *total, *handle = NULL, *custom = NULL;

		tridstr = buf + strlen("LST") + 1;
		list = index(tridstr, ' ');
		*list = '\0';
		list++;
		serid = index(list, ' ');
		*serid = '\0';
		serid++;
		num = index(serid, ' ');
		*num = '\0';
		num++;
		total = index(num, ' ');
		*total = '\0';
		total++;

		if (atoi(num) >= atoi(total))
			mci->mi->flags &= ~MI_RUNFLAG_PNGCURBED;

		if ((handle = index(total, ' '))) {
			*handle = '\0';
			handle++;
			custom = index(handle, ' ');
			*custom = '\0';
			custom++;
		}

	} else if ((buf[0] >= 0) && (buf[0] <= 9)) {
		int code;

		buf[3] = '\0';
		code = atoi(buf);

		dvprintf("error code %d: %s\n", code, msn_strerror(code));

		if (msn_errisfatal(code))
			return -1;
	}

	return 0;
}

int sendmsncmd(nbio_fd_t *fdt, const char *cmd, int usetrid, ntrid_t trid, const char *args)
{
	char *buf;
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	int buflen;
	int rn = 1;

	buflen = strlen(cmd) + 2 /* \r\n */ + 1 /* null */;
	if (usetrid)
		buflen += 1 + strlen("1234567890123");
	if (args)
		buflen += 1 + strlen(args);

	if (!(buf = malloc(buflen))) {
		fdtperror(fdt, "sendmsncmd: malloc", errno, cmd);
		return -1;
	}

	if (strncmp(cmd, "MSG", 3) == 0)
		rn = 0;

	if (usetrid && args) {
		snprintf(buf, buflen,
				"%s %lu %s%s",
				cmd,
				trid,
				args,
				rn ? "\r\n" : "");
	} else if (usetrid) {
		snprintf(buf, buflen,
				"%s %lu%s",
				cmd,
				trid,
				rn ? "\r\n" : "");
	} else {
		snprintf(buf, buflen,
				"%s%s",
				cmd,
				rn ? "\r\n" : "");
	}

	if (mci->mi->flags & MI_RUNFLAG_VERBOSE) {
		dvprintf("[%s%s%s, %s, %s] <--- SENT: %s\n",
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

	if (nbio_addtxvector(mci->mi->nb, fdt, buf, strlen(buf)) == -1) {
		fdtperror(fdt, "sendmsncmd: nbio_addtxvector", errno, cmd);
		free(buf);
		return -1;
	}

	return 0;
}

ntrid_t sendmsnmsg_typing(struct msninfo *mi, nbio_fd_t *passedfdt, const char *name)
{
	char typingmsg[401];

	snprintf(typingmsg, sizeof(typingmsg),
		"MIME-Version: 1.0\r\n"
		"Content-Type: text/x-msmsgscontrol\r\n"
		"TypingUser: %s\r\n\r\n\r\n",
		mi->login);

	return sendmsnmsg(mi, passedfdt, name, typingmsg);
}

static const char *wl_getawaitinguser(void)
{

	if (!waitinglist)
		return NULL;

	return waitinglist->name;
}

static nbio_fd_t *findsbbyname(struct msninfo *mi, const char *name)
{
	nbio_fd_t *fdt;

	for (fdt = mi->nb->fdlist; fdt; fdt = fdt->next) {
		struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;

		if (fdt->type != NBIO_FDTYPE_STREAM)
			continue;

		if (mci->type != MCI_TYPE_SB)
			continue;

		if (!(mci->flags & MCI_FLAG_HASNAME))
			continue;

		if (!(mci->flags & MCI_FLAG_SBREADY))
			continue;

		if (strcasecmp(mci->name, name) == 0)
			return fdt;
	}

	return NULL;
}

void wl_trywaiting(struct msninfo *mi)
{
	struct waitingmsg *cur, **prev;
	time_t now;

	now = time(NULL);

	for (prev = &waitinglist; (cur = *prev); ) {
		nbio_fd_t *fdt;

		if ((now - cur->entrytime) > 60) {

			*prev = cur->next;

			dvprintf("message timed out waiting for SB: %s / %s\n", cur->name, cur->msg);

			free(cur->msg);
			free(cur);
			waitinglistlen--;

		} else if ((fdt = findsbbyname(mi, cur->name))) {

			*prev = cur->next;

			sendmsnmsg(mi, fdt, cur->name, cur->msg);

			free(cur->msg);
			free(cur);
			waitinglistlen--;

		} else {

			/*
			 * At the very worst, this will request far too many
			 * SB connections.
			 *
			 * But hey, that's not my problem.
			 *
			 */
			msn_sendxfr(mi->nsconn, "SB");

			prev = &cur->next;
		}
	}

	return;
}

static int wl_addwaiting(struct msninfo *mi, const char *name, const char *msg)
{
	struct waitingmsg *wm;

	if (waitinglistlen >= MAX_WL_LEN)
		return -1;

	if (!(wm = malloc(sizeof(struct waitingmsg))))
		return -1;
	memset(wm, 0, sizeof(struct waitingmsg));

	wm->entrytime = time(NULL);
	strncpy(wm->name, name, sizeof(wm->name));
	if (!(wm->msg = strdup(msg))) {
		free(wm);
		return -1;
	}

	dvprintf("enqueuing message for later delivery: %s / %s\n", wm->name, wm->msg);

	msn_sendxfr(mi->nsconn, "SB");

	wm->next = waitinglist;
	waitinglist = wm;
	waitinglistlen++;

	return 0;
}

ntrid_t sendmsnmsg(struct msninfo *mi, nbio_fd_t *passedfdt, const char *name, const char *msg)
{
	ntrid_t trid;
	char buf[1024];
	nbio_fd_t *dstfdt;
	char *tmpstr = NULL;

	/*
	 * Detect if the message includes the MIME header.
	 *
	 *  XXX should use a message flag instead of this.
	 */
	if (!strstr(msg, "Content-Type:")) {
		char stdhdr[] = DEFAULTMIMEHEADER;

		if (!(tmpstr = malloc(strlen(msg)+strlen(stdhdr)+1)))
			return -1;
		sprintf(tmpstr, "%s%s", stdhdr, msg);

		msg = tmpstr;
	}

	if (passedfdt)
		dstfdt = passedfdt;
	else if (!(dstfdt = findsbbyname(mi, name))) {

		wl_addwaiting(mi, name, msg);

		if (tmpstr)
			free(tmpstr);

		return 0;
	}

	trid = mi->nexttrid;

	snprintf(buf, sizeof(buf),
		"%c %d\r\n%s",
		'U', /* unacknowledged */
		strlen(msg),
		msg);

	if (tmpstr)
		free(tmpstr);
	
	if (sendmsncmd(dstfdt, "MSG", 1, trid, buf) == -1)
		return 0;

	return mi->nexttrid++;
}

void msn_sendpng(nbio_fd_t *fdt)
{

	sendmsncmd(fdt, "PNG", 0, 0, NULL);

	return;
}

ntrid_t msn_sendadd(nbio_fd_t *fdt, const char *list, const char *handle, const char *custom)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	ntrid_t trid;
	char buf[512];

	trid = mci->mi->nexttrid;

	snprintf(buf, sizeof(buf),
			"%s %s %s",
			list, handle, custom);

	if (sendmsncmd(fdt, "ADD", 1, trid, buf) == -1)
		return 0;

	return mci->mi->nexttrid++;
}

ntrid_t msn_sendans(nbio_fd_t *fdt, const char *handle, const char *authinfo, int sessid)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	ntrid_t trid;
	char buf[512];

	trid = mci->mi->nexttrid;

	snprintf(buf, sizeof(buf),
			"%s %s %d",
			handle,
			authinfo,
			sessid);

	if (sendmsncmd(fdt, "ANS", 1, trid, buf) == -1)
		return 0;

	return mci->mi->nexttrid++;
}

ntrid_t msn_sendusr(nbio_fd_t *fdt, const char *sp, const char *step, const char *info)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	ntrid_t trid;
	char buf[512];

	trid = mci->mi->nexttrid;

	snprintf(buf, sizeof(buf),
			"%s %s%s%s",
			sp,
			step ? step : "",
			step ? " " : "",
			info);

	if (sendmsncmd(fdt, "USR", 1, trid, buf) == -1)
		return 0;

	return mci->mi->nexttrid++;
}

ntrid_t msn_sendchg(nbio_fd_t *fdt, const char *state)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	ntrid_t trid;

	trid = mci->mi->nexttrid;

	if (sendmsncmd(fdt, "CHG", 1, trid, state) == -1)
		return 0;

	return mci->mi->nexttrid++;
}

ntrid_t msn_sendxfr(nbio_fd_t *fdt, const char *type)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	ntrid_t trid;

	trid = mci->mi->nexttrid;

	if (sendmsncmd(fdt, "XFR", 1, trid, type) == -1)
		return 0;

	return mci->mi->nexttrid++;
}

ntrid_t msn_sendinf(nbio_fd_t *fdt, const char *data)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	ntrid_t trid;

	trid = mci->mi->nexttrid;

	if (sendmsncmd(fdt, "INF", 1, trid, data) == -1)
		return 0;

	return mci->mi->nexttrid++;
}

ntrid_t msn_sendlst(nbio_fd_t *fdt, const char *l)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	ntrid_t trid;

	trid = mci->mi->nexttrid;

	if (sendmsncmd(fdt, "LST", 1, trid, l) == -1)
		return 0;

	/* LST is a synchronous operation. */
	mci->mi->flags |= MI_RUNFLAG_PNGCURBED;

	return mci->mi->nexttrid++;
}

ntrid_t msn_sendver(nbio_fd_t *fdt, const char *vers)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	ntrid_t trid;

	trid = mci->mi->nexttrid;

	if (sendmsncmd(fdt, "VER", 1, trid, vers) == -1)
		return 0;

	return mci->mi->nexttrid++;
}

ntrid_t msn_sendcal(nbio_fd_t *fdt, const char *name)
{
	struct msnconninfo *mci = (struct msnconninfo *)fdt->priv;
	ntrid_t trid;

	trid = mci->mi->nexttrid;

	if (sendmsncmd(fdt, "CAL", 1, trid, name) == -1)
		return 0;

	return mci->mi->nexttrid++;
}


