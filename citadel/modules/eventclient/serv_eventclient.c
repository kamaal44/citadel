/*
 * Copyright (c) 1998-2009 by the citadel.org team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <termios.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <sys/types.h>
#include <syslog.h>

#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#include <sys/wait.h>
#include <ctype.h>
#include <string.h>
#include <limits.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <libcitadel.h>
#include <curl/curl.h>
#include <curl/multi.h>
#include "citadel.h"
#include "server.h"
#include "citserver.h"
#include "support.h"

#include "ctdl_module.h"

#ifdef EXPERIMENTAL_SMTP_EVENT_CLIENT

#include "event_client.h"
#include "serv_curl.h"

ev_loop *event_base;

/*****************************************************************************
 *                   libevent / curl integration                             *
 *****************************************************************************/
#define MOPT(s, v)							\
	do {								\
		sta = curl_multi_setopt(mhnd, (CURLMOPT_##s), (v));	\
		if (sta) {						\
			syslog(LOG_ERR, "EVCURL: error setting option " #s " on curl multi handle: %s\n", curl_easy_strerror(sta)); \
			exit (1);					\
		}							\
	} while (0)

typedef struct _evcurl_global_data {
	int magic;
	CURLM *mhnd;
	ev_timer timeev;
	int nrun;
} evcurl_global_data;

typedef struct _sockwatcher_data 
{
	evcurl_global_data *global;
	ev_io ioev;
} sockwatcher_data;

ev_async WakeupCurl;
evcurl_global_data global;

static void
gotstatus(evcurl_global_data *global, int nnrun) 
{
	CURLM *mhnd;
	CURLMsg *msg;
	int nmsg;
/*
  if (EVCURL_GLOBAL_MAGIC != global.magic)
  {
  CtdlLogPrintf(CTDL_ERR, "internal error: gotstatus on wrong struct");
  return;
  }
*/
	global->nrun = nnrun;
	mhnd = global->mhnd;

	syslog(LOG_DEBUG, "CURLEV: gotstatus(): about to call curl_multi_info_read\n");
	while ((msg = curl_multi_info_read(mhnd, &nmsg))) {
		syslog(LOG_ERR, "EVCURL: got curl multi_info message msg=%d\n", msg->msg);
		if (CURLMSG_DONE == msg->msg) {
			CURL *chnd;
			char *chandle;
			CURLcode sta;
			CURLMcode msta;
			AsyncIO  *IO;

			chandle = NULL;;
			chnd = msg->easy_handle;
			sta = curl_easy_getinfo(chnd, CURLINFO_PRIVATE, &chandle);
			syslog(LOG_ERR, "EVCURL: request complete\n");
			if (sta)
				syslog(LOG_ERR, "EVCURL: error asking curl for private cookie of curl handle: %s\n", curl_easy_strerror(sta));
			IO = (AsyncIO *)chandle;
			
			sta = msg->data.result;
			if (sta) {
				syslog(LOG_ERR, "EVCURL: error description: %s\n", IO->HttpReq.errdesc);
				syslog(LOG_ERR, "EVCURL: error performing request: %s\n", curl_easy_strerror(sta));
			}
			sta = curl_easy_getinfo(chnd, CURLINFO_RESPONSE_CODE, &IO->HttpReq.httpcode);
			if (sta)
				syslog(LOG_ERR, "EVCURL: error asking curl for response code from request: %s\n", curl_easy_strerror(sta));
			syslog(LOG_ERR, "EVCURL: http response code was %ld\n", (long)IO->HttpReq.httpcode);
			msta = curl_multi_remove_handle(mhnd, chnd);
			if (msta)
				syslog(LOG_ERR, "EVCURL: warning problem detaching completed handle from curl multi: %s\n", curl_multi_strerror(msta));

			IO->HttpReq.attached = 0;
			IO->SendDone(IO);
			curl_easy_cleanup(IO->HttpReq.chnd);
			curl_slist_free_all(IO->HttpReq.headers);
			FreeStrBuf(&IO->HttpReq.ReplyData);
			FreeURL(&IO->ConnectMe);
			RemoveContext(IO->CitContext);
			IO->Terminate(IO);
		}
	}
}

static void
stepmulti(evcurl_global_data *global, curl_socket_t fd) {
	int nnrun;
	CURLMcode msta;
	
	if (global == NULL) {
	    syslog(LOG_DEBUG, "EVCURL: stepmulti(NULL): wtf?\n");
	    return;
	}
	msta = curl_multi_socket_action(global->mhnd, fd, 0, &nnrun);
	syslog(LOG_DEBUG, "EVCURL: stepmulti(): calling gotstatus()\n");
	if (msta)
		syslog(LOG_ERR, "EVCURL: error in curl processing events on multi handle, fd %d: %s\n", (int)fd, curl_multi_strerror(msta));
	if (global->nrun != nnrun)
		gotstatus(global, nnrun);
}

static void
gottime(struct ev_loop *loop, ev_timer *timeev, int events) {
	syslog(LOG_DEBUG, "EVCURL: waking up curl for timeout\n");
	evcurl_global_data *global = (void *)timeev->data;
	stepmulti(global, CURL_SOCKET_TIMEOUT);
}

static void
gotio(struct ev_loop *loop, ev_io *ioev, int events) {
	syslog(LOG_DEBUG, "EVCURL: waking up curl for io on fd %d\n", (int)ioev->fd);
	sockwatcher_data *sockwatcher = (void *)ioev->data;
	stepmulti(sockwatcher->global, ioev->fd);
}

static size_t
gotdata(void *data, size_t size, size_t nmemb, void *cglobal) {
	AsyncIO *IO = (AsyncIO*) cglobal;
	//evcurl_request_data *D = (evcurl_request_data*) data;
	syslog(LOG_DEBUG, "EVCURL: gotdata(): calling CurlFillStrBuf_callback()\n");

	if (IO->HttpReq.ReplyData == NULL)
	{
	    IO->HttpReq.ReplyData = NewStrBufPlain(NULL, SIZ);
	}
	return CurlFillStrBuf_callback(data, size, nmemb, IO->HttpReq.ReplyData);
}

static int
gotwatchtime(CURLM *multi, long tblock_ms, void *cglobal) {
	syslog(LOG_DEBUG, "EVCURL: gotwatchtime called %ld ms\n", tblock_ms);
	evcurl_global_data *global = cglobal;
	ev_timer_stop(EV_DEFAULT, &global->timeev);
	if (tblock_ms < 0 || 14000 < tblock_ms)
		tblock_ms = 14000;
	ev_timer_set(&global->timeev, 0.5e-3 + 1.0e-3 * tblock_ms, 14.0);
	ev_timer_start(EV_DEFAULT_UC, &global->timeev);
	curl_multi_perform(global, CURL_POLL_NONE);
	return 0;
}

static int
gotwatchsock(CURL *easy, curl_socket_t fd, int action, void *cglobal, void *csockwatcher) {
	evcurl_global_data *global = cglobal;
	CURLM *mhnd = global->mhnd;
	sockwatcher_data *sockwatcher = csockwatcher;

	syslog(LOG_DEBUG, "EVCURL: gotwatchsock called fd=%d action=%d\n", (int)fd, action);

	if (!sockwatcher) {
		syslog(LOG_ERR,"EVCURL: called first time to register this sockwatcker\n");
		sockwatcher = malloc(sizeof(sockwatcher_data));
		sockwatcher->global = global;
		ev_init(&sockwatcher->ioev, &gotio);
		sockwatcher->ioev.data = (void *)sockwatcher;
		curl_multi_assign(mhnd, fd, sockwatcher);
	}
	if (CURL_POLL_REMOVE == action) {
		syslog(LOG_ERR,"EVCURL: called last time to unregister this sockwatcher\n");
		ev_io_stop(event_base, &sockwatcher->ioev);
		free(sockwatcher);
	} else {
		int events = (action & CURL_POLL_IN ? EV_READ : 0) | (action & CURL_POLL_OUT ? EV_WRITE : 0);
		ev_io_stop(EV_DEFAULT, &sockwatcher->ioev);
		if (events) {
			ev_io_set(&sockwatcher->ioev, fd, events);
			ev_io_start(EV_DEFAULT, &sockwatcher->ioev);
		}
	}
	return 0;
}

void curl_init_connectionpool(void) 
{
	CURLM *mhnd ;

	ev_timer_init(&global.timeev, &gottime, 14.0, 14.0);
	global.timeev.data = (void *)&global;
	global.nrun = -1;
	CURLcode sta = curl_global_init(CURL_GLOBAL_ALL);

	if (sta) 
	{
		syslog(LOG_ERR,"EVCURL: error initializing curl library: %s\n", curl_easy_strerror(sta));
		exit(1);
	}
	mhnd = global.mhnd = curl_multi_init();
	if (!mhnd)
	{
		syslog(LOG_ERR,"EVCURL: error initializing curl multi handle\n");
		exit(3);
	}

	MOPT(SOCKETFUNCTION, &gotwatchsock);
	MOPT(SOCKETDATA, (void *)&global);
	MOPT(TIMERFUNCTION, &gotwatchtime);
	MOPT(TIMERDATA, (void *)&global);

	return;
}




int evcurl_init(AsyncIO *IO, 
		void *CustomData, 
		const char* Desc,
		IO_CallBack CallBack, 
		IO_CallBack Terminate)
{
	CURLcode sta;
	CURL *chnd;

	syslog(LOG_DEBUG, "EVCURL: evcurl_init called ms\n");
	IO->HttpReq.attached = 0;
	IO->SendDone = CallBack;
	IO->Terminate = Terminate;
	chnd = IO->HttpReq.chnd = curl_easy_init();
	if (!chnd)
	{
		syslog(LOG_ERR, "EVCURL: error initializing curl handle\n");
		return 1;
	}

	strcpy(IO->HttpReq.errdesc, Desc);

	OPT(VERBOSE, (long)1);
		/* unset in production */
	OPT(NOPROGRESS, (long)1); 
	OPT(NOSIGNAL, (long)1);
	OPT(FAILONERROR, (long)1);
	OPT(ENCODING, "");
	OPT(FOLLOWLOCATION, (long)1);
	OPT(MAXREDIRS, (long)7);
	OPT(USERAGENT, CITADEL);

	OPT(TIMEOUT, (long)1800);
	OPT(LOW_SPEED_LIMIT, (long)64);
	OPT(LOW_SPEED_TIME, (long)600);
	OPT(CONNECTTIMEOUT, (long)600); 
	OPT(PRIVATE, (void *)IO);


	OPT(WRITEFUNCTION, &gotdata); 
	OPT(WRITEDATA, (void *)IO);
	OPT(ERRORBUFFER, IO->HttpReq.errdesc);

	if (
		(!IsEmptyStr(config.c_ip_addr))
		&& (strcmp(config.c_ip_addr, "*"))
		&& (strcmp(config.c_ip_addr, "::"))
		&& (strcmp(config.c_ip_addr, "0.0.0.0"))
	) {
		OPT(INTERFACE, config.c_ip_addr);
	}
		/* point to a structure that points back to the perl structure and stuff */
	syslog(LOG_DEBUG, "EVCURL: Loading URL: %s\n", IO->ConnectMe->PlainUrl);
	OPT(URL, IO->ConnectMe->PlainUrl);
	if (StrLength(IO->ConnectMe->CurlCreds))
	{
		OPT(HTTPAUTH, (long)CURLAUTH_BASIC);
		OPT(USERPWD, ChrPtr(IO->ConnectMe->CurlCreds));
	}
#ifdef CURLOPT_HTTP_CONTENT_DECODING
	OPT(HTTP_CONTENT_DECODING, 1);
	OPT(ENCODING, "");
#endif
	if (StrLength(IO->HttpReq.PostData) > 0)
	{ 
		OPT(POSTFIELDS, ChrPtr(IO->HttpReq.PostData));
		OPT(POSTFIELDSIZE, StrLength(IO->HttpReq.PostData));

	}
	else if ((IO->HttpReq.PlainPostDataLen != 0) && (IO->HttpReq.PlainPostData != NULL))
	{
		OPT(POSTFIELDS, IO->HttpReq.PlainPostData);
		OPT(POSTFIELDSIZE, IO->HttpReq.PlainPostDataLen);
	}

	if (IO->HttpReq.headers != NULL)
		OPT(HTTPHEADER, IO->HttpReq.headers);

	return 1;
}

void
evcurl_handle_start(AsyncIO *IO) 
{
	CURLMcode msta;
	
	syslog(LOG_DEBUG, "EVCURL: attaching to curl multi handle\n");
	msta = curl_multi_add_handle(global.mhnd, IO->HttpReq.chnd);
	if (msta)
		syslog(LOG_ERR, "EVCURL: error attaching to curl multi handle: %s\n", curl_multi_strerror(msta));
	IO->HttpReq.attached = 1;
	ev_async_send (event_base, &WakeupCurl);
}

static void WakeupCurlCallback(EV_P_ ev_async *w, int revents)
{
	syslog(LOG_DEBUG, "EVCURL: waking up curl multi handle\n");

	curl_multi_perform(&global, CURL_POLL_NONE);
}

static void evcurl_shutdown (void)
{
	curl_multi_cleanup(global.mhnd);
}
/*****************************************************************************
 *                       libevent integration                                *
 *****************************************************************************/
/*
 * client event queue plus its methods.
 * this currently is the main loop (which may change in some future?)
 */
int evbase_count = 0;
int event_add_pipe[2] = {-1, -1};
pthread_mutex_t EventQueueMutex; /* locks the access to the following vars: */
HashList *QueueEvents = NULL;
HashList *InboundEventQueue = NULL;
HashList *InboundEventQueues[2] = { NULL, NULL };

ev_async AddJob;   
ev_async ExitEventLoop;

static void QueueEventAddCallback(EV_P_ ev_async *w, int revents)
{
	HashList *q;
	void *v;
	HashPos  *It;
	long len;
	const char *Key;

	/* get the control command... */
	pthread_mutex_lock(&EventQueueMutex);

	if (InboundEventQueues[0] == InboundEventQueue) {
		InboundEventQueue = InboundEventQueues[1];
		q = InboundEventQueues[0];
	}
	else {
		InboundEventQueue = InboundEventQueues[0];
		q = InboundEventQueues[1];
	}
	pthread_mutex_unlock(&EventQueueMutex);

	It = GetNewHashPos(q, 0);
	while (GetNextHashPos(q, It, &len, &Key, &v))
	{
		IOAddHandler *h = v;
		h->EvAttch(h->IO);
	}
	DeleteHashPos(&It);
	DeleteHashContent(&q);
	syslog(LOG_DEBUG, "EVENT Q Read done.\n");
}


static void EventExitCallback(EV_P_ ev_async *w, int revents)
{
	ev_break(event_base, EVBREAK_ALL);

	syslog(LOG_DEBUG, "EVENT Q exiting.\n");
}



void InitEventQueue(void)
{
	struct rlimit LimitSet;

	pthread_mutex_init(&EventQueueMutex, NULL);

	if (pipe(event_add_pipe) != 0) {
		syslog(LOG_EMERG, "Unable to create pipe for libev queueing: %s\n", strerror(errno));
		abort();
	}
	LimitSet.rlim_cur = 1;
	LimitSet.rlim_max = 1;
	setrlimit(event_add_pipe[1], &LimitSet);

	QueueEvents = NewHash(1, Flathash);
	InboundEventQueues[0] = NewHash(1, Flathash);
	InboundEventQueues[1] = NewHash(1, Flathash);
	InboundEventQueue = InboundEventQueues[0];
}
/*
 * this thread operates the select() etc. via libev.
 * 
 * 
 */
void *client_event_thread(void *arg) 
{
	struct CitContext libev_client_CC;

	CtdlFillSystemContext(&libev_client_CC, "LibEv Thread");
//	citthread_setspecific(MyConKey, (void *)&smtp_queue_CC);
	syslog(LOG_DEBUG, "client_ev_thread() initializing\n");

	event_base = ev_default_loop (EVFLAG_AUTO);

	ev_async_init(&AddJob, QueueEventAddCallback);
	ev_async_start(event_base, &AddJob);
	ev_async_init(&ExitEventLoop, EventExitCallback);
	ev_async_start(event_base, &ExitEventLoop);
	ev_async_init(&WakeupCurl, WakeupCurlCallback);
	ev_async_start(event_base, &WakeupCurl);

	curl_init_connectionpool();

	ev_run (event_base, 0);


///what todo here?	CtdlClearSystemContext();
	ev_loop_destroy (EV_DEFAULT_UC);
	
	DeleteHash(&QueueEvents);
	InboundEventQueue = NULL;
	DeleteHash(&InboundEventQueues[0]);
	DeleteHash(&InboundEventQueues[1]);
/*	citthread_mutex_destroy(&EventQueueMutex); TODO */
	evcurl_shutdown();

	return(NULL);
}
/*------------------------------------------------------------------------------*/
/*
 * DB-Queue; does async bdb operations.
 * has its own set of handlers.
 */
ev_loop *event_db;
int evdb_count = 0;
int evdb_add_pipe[2] = {-1, -1};
pthread_mutex_t DBEventQueueMutex; /* locks the access to the following vars: */
HashList *DBQueueEvents = NULL;
HashList *DBInboundEventQueue = NULL;
HashList *DBInboundEventQueues[2] = { NULL, NULL };

ev_async DBAddJob;   
ev_async DBExitEventLoop;

extern void ShutDownDBCLient(AsyncIO *IO);

static void DBQueueEventAddCallback(EV_P_ ev_async *w, int revents)
{
	HashList *q;
	void *v;
	HashPos  *It;
	long len;
	const char *Key;

	/* get the control command... */
	pthread_mutex_lock(&DBEventQueueMutex);

	if (DBInboundEventQueues[0] == DBInboundEventQueue) {
		DBInboundEventQueue = DBInboundEventQueues[1];
		q = DBInboundEventQueues[0];
	}
	else {
		DBInboundEventQueue = DBInboundEventQueues[0];
		q = DBInboundEventQueues[1];
	}
	pthread_mutex_unlock(&DBEventQueueMutex);

	It = GetNewHashPos(q, 0);
	while (GetNextHashPos(q, It, &len, &Key, &v))
	{
		IOAddHandler *h = v;
		eNextState rc;
		rc = h->EvAttch(h->IO);
		switch (rc)
		{
		case eAbort:
		    ShutDownDBCLient(h->IO);
		default:
		    break;
		}
	}
	DeleteHashPos(&It);
	DeleteHashContent(&q);
	syslog(LOG_DEBUG, "DBEVENT Q Read done.\n");
}


static void DBEventExitCallback(EV_P_ ev_async *w, int revents)
{
	syslog(LOG_DEBUG, "EVENT Q exiting.\n");
	ev_break(event_db, EVBREAK_ALL);
}



void DBInitEventQueue(void)
{
	struct rlimit LimitSet;

	pthread_mutex_init(&DBEventQueueMutex, NULL);

	if (pipe(evdb_add_pipe) != 0) {
		syslog(LOG_EMERG, "Unable to create pipe for libev queueing: %s\n", strerror(errno));
		abort();
	}
	LimitSet.rlim_cur = 1;
	LimitSet.rlim_max = 1;
	setrlimit(evdb_add_pipe[1], &LimitSet);

	DBQueueEvents = NewHash(1, Flathash);
	DBInboundEventQueues[0] = NewHash(1, Flathash);
	DBInboundEventQueues[1] = NewHash(1, Flathash);
	DBInboundEventQueue = DBInboundEventQueues[0];
}

/*
 * this thread operates writing to the message database via libev.
 * 
 * 
 */
void *db_event_thread(void *arg) 
{
	struct CitContext libev_msg_CC;

	CtdlFillSystemContext(&libev_msg_CC, "LibEv DB IO Thread");
//	citthread_setspecific(MyConKey, (void *)&smtp_queue_CC);
	syslog(LOG_DEBUG, "client_msgev_thread() initializing\n");

	event_db = ev_loop_new (EVFLAG_AUTO);

	ev_async_init(&DBAddJob, DBQueueEventAddCallback);
	ev_async_start(event_db, &DBAddJob);
	ev_async_init(&DBExitEventLoop, DBEventExitCallback);
	ev_async_start(event_db, &DBExitEventLoop);

	ev_run (event_db, 0);


//// what to do here?	CtdlClearSystemContext();
	ev_loop_destroy (event_db);

	DeleteHash(&DBQueueEvents);
	DBInboundEventQueue = NULL;
	DeleteHash(&DBInboundEventQueues[0]);
	DeleteHash(&DBInboundEventQueues[1]);
/*	citthread_mutex_destroy(&DBEventQueueMutex); TODO */

	return(NULL);
}

#endif

CTDL_MODULE_INIT(event_client)
{
#ifdef EXPERIMENTAL_SMTP_EVENT_CLIENT
	if (!threading)
	{
		InitEventQueue();
		DBInitEventQueue();
		CtdlThreadCreate(/*"Client event", */ client_event_thread);
		CtdlThreadCreate(/*"DB event", */db_event_thread);
/// todo register shutdown callback.
	}
#endif
	return "event";
}