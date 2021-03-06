/*
 * This module handles shared rooms, inter-Citadel mail, and outbound
 * mailing list processing.
 *
 * Copyright (c) 2000-2015 by the citadel.org team
 *
 * This program is open source software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * ** NOTE **   A word on the S_NETCONFIGS semaphore:
 * This is a fairly high-level type of critical section.  It ensures that no
 * two threads work on the netconfigs files at the same time.  Since we do
 * so many things inside these, here are the rules:
 *  1. begin_critical_section(S_NETCONFIGS) *before* begin_ any others.
 *  2. Do *not* perform any I/O with the client during these sections.
 *
 */


#include "sysdep.h"
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <signal.h>
#include <pwd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
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
#ifdef HAVE_SYSCALL_H
# include <syscall.h>
#else 
# if HAVE_SYS_SYSCALL_H
#  include <sys/syscall.h>
# endif
#endif

#include <sys/wait.h>
#include <string.h>
#include <limits.h>
#include <libcitadel.h>
#include "citadel.h"
#include "server.h"
#include "citserver.h"
#include "support.h"
#include "config.h"
#include "user_ops.h"
#include "database.h"
#include "msgbase.h"
#include "internet_addressing.h"
#include "clientsocket.h"
#include "citadel_dirs.h"
#include "threads.h"
#include "context.h"
#include "ctdl_module.h"

struct CitContext networker_client_CC;

#define NODE ChrPtr(((AsyncNetworker*)IO->Data)->node)
#define N ((AsyncNetworker*)IO->Data)->n

int NetworkClientDebugEnabled = 0;

#define NCDBGLOG(LEVEL) if ((LEVEL != LOG_DEBUG) || (NetworkClientDebugEnabled != 0))

#define EVN_syslog(LEVEL, FORMAT, ...)					\
	NCDBGLOG(LEVEL) syslog(LEVEL,					\
			       "%s[%ld]CC[%d]NW[%s][%ld]" FORMAT,	\
			       IOSTR, IO->ID, CCID, NODE, N, __VA_ARGS__)

#define EVNM_syslog(LEVEL, FORMAT)					\
	NCDBGLOG(LEVEL) syslog(LEVEL,					\
			       "%s[%ld]CC[%d]NW[%s][%ld]" FORMAT,	\
			       IOSTR, IO->ID, CCID, NODE, N)

#define EVNCS_syslog(LEVEL, FORMAT, ...) \
	NCDBGLOG(LEVEL) syslog(LEVEL, "%s[%ld]NW[%s][%ld]" FORMAT,	\
			       IOSTR, IO->ID, NODE, N, __VA_ARGS__)

#define EVNCSM_syslog(LEVEL, FORMAT) \
	NCDBGLOG(LEVEL) syslog(LEVEL, "%s[%ld]NW[%s][%ld]" FORMAT,	\
			       IOSTR, IO->ID, NODE, N)


typedef enum _eNWCState {
	eGreating,
	eAuth,
	eNDOP,
	eREAD,
	eReadBLOB,
	eCLOS,
	eNUOP,
	eWRIT,
	eWriteBLOB,
	eUCLS,
	eQUIT
}eNWCState;

typedef enum _eNWCVState {
	eNWCVSLookup,
	eNWCVSConnecting,
	eNWCVSConnFail,
	eNWCVSGreating,
	eNWCVSAuth,
	eNWCVSAuthFailNTT,
	eNWCVSAuthFail,
	eNWCVSNDOP,
	eNWCVSNDOPDone,
	eNWCVSNUOP,
	eNWCVSNUOPDone,
	eNWCVSFail
}eNWCVState;

ConstStr NWCStateStr[] = {
	{HKEY("Looking up Host")},
	{HKEY("Connecting host")},
	{HKEY("Failed to connect")},
	{HKEY("Rread Greeting")},
	{HKEY("Authenticating")},
	{HKEY("Auth failed by NTT")},
	{HKEY("Auth failed")},
	{HKEY("Downloading")},
	{HKEY("Downloading Success")},
	{HKEY("Uploading Spoolfile")},
	{HKEY("Uploading done")},
	{HKEY("failed")}
};

void SetNWCState(AsyncIO *IO, eNWCVState State)
{
	CitContext* CCC = IO->CitContext;
	memcpy(CCC->cs_clientname, NWCStateStr[State].Key, NWCStateStr[State].len + 1);
}

typedef struct _async_networker {
        AsyncIO IO;
	DNSQueryParts HostLookup;
	eNWCState State;
	long n;
        StrBuf *SpoolFileName;
        StrBuf *tempFileName;
	StrBuf *node;
	StrBuf *host;
	StrBuf *port;
	StrBuf *secret;
	StrBuf		*Url;
} AsyncNetworker;

typedef eNextState(*NWClientHandler)(AsyncNetworker* NW);
eNextState nwc_get_one_host_ip(AsyncIO *IO);

eNextState nwc_connect_ip(AsyncIO *IO);

eNextState NWC_SendQUIT(AsyncNetworker *NW);
eNextState NWC_DispatchWriteDone(AsyncIO *IO);

void DeleteNetworker(void *vptr)
{
	AsyncNetworker *NW = (AsyncNetworker *)vptr;
        FreeStrBuf(&NW->SpoolFileName);
        FreeStrBuf(&NW->tempFileName);
	FreeStrBuf(&NW->node);
	FreeStrBuf(&NW->host);
	FreeStrBuf(&NW->port);
	FreeStrBuf(&NW->secret);
	FreeStrBuf(&NW->Url);
	FreeStrBuf(&NW->IO.ErrMsg);
	FreeAsyncIOContents(&NW->IO);
	if (NW->HostLookup.VParsedDNSReply != NULL) {
		NW->HostLookup.DNSReplyFree(NW->HostLookup.VParsedDNSReply);
		NW->HostLookup.VParsedDNSReply = NULL;
	}
	free(NW);
}

#define NWC_DBG_SEND() EVN_syslog(LOG_DEBUG, ": > %s", ChrPtr(NW->IO.SendBuf.Buf))
#define NWC_DBG_READ() EVN_syslog(LOG_DEBUG, ": < %s\n", ChrPtr(NW->IO.IOBuf))
#define NWC_OK (strncasecmp(ChrPtr(NW->IO.IOBuf), "+OK", 3) == 0)

eNextState NWC_SendFailureMessage(AsyncIO *IO)
{
	AsyncNetworker *NW = IO->Data;
	long lens[2];
	const char *strs[2];

	EVN_syslog(LOG_DEBUG, "NWC: %s\n", __FUNCTION__);

	strs[0] = ChrPtr(NW->node);
	lens[0] = StrLength(NW->node);
	
	strs[1] = ChrPtr(NW->IO.ErrMsg);
	lens[1] = StrLength(NW->IO.ErrMsg);
	CtdlAideFPMessage(
		ChrPtr(NW->IO.ErrMsg),
		"Networker error",
		2, strs, (long*) &lens,
		CCID, IO->ID,
		EvGetNow(IO));
	
	return eAbort;
}

eNextState FinalizeNetworker(AsyncIO *IO)
{
	AsyncNetworker *NW = (AsyncNetworker *)IO->Data;

	CtdlNetworkTalkingTo(SKEY(NW->node), NTT_REMOVE);

	DeleteNetworker(IO->Data);
	return eAbort;
}

eNextState NWC_ReadGreeting(AsyncNetworker *NW)
{
	char connected_to[SIZ];
	AsyncIO *IO = &NW->IO;
	SetNWCState(IO, eNWCVSGreating);
	NWC_DBG_READ();
	/* Read the server greeting */
	/* Check that the remote is who we think it is and warn the Aide if not */
	extract_token (connected_to, ChrPtr(NW->IO.IOBuf), 1, ' ', sizeof connected_to);
	if (strcmp(connected_to, ChrPtr(NW->node)) != 0)
	{
		if (NW->IO.ErrMsg == NULL)
			NW->IO.ErrMsg = NewStrBuf();
		StrBufPrintf(NW->IO.ErrMsg,
			     "Connected to node \"%s\" but I was expecting to connect to node \"%s\".",
			     connected_to, ChrPtr(NW->node));
		EVN_syslog(LOG_ERR, "%s\n", ChrPtr(NW->IO.ErrMsg));

		return EventQueueDBOperation(IO, NWC_SendFailureMessage, 1);
	}
	return eSendReply;
}

eNextState NWC_SendAuth(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	SetNWCState(IO, eNWCVSAuth);
	/* We're talking to the correct node.  Now identify ourselves. */
	StrBufPrintf(NW->IO.SendBuf.Buf, "NETP %s|%s\n", 
		     CtdlGetConfigStr("c_nodename"), 
		     ChrPtr(NW->secret));
	NWC_DBG_SEND();
	return eSendReply;
}

eNextState NWC_ReadAuthReply(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	NWC_DBG_READ();
	if (ChrPtr(NW->IO.IOBuf)[0] == '2')
	{
		return eSendReply;
	}
	else
	{
		int Error = atol(ChrPtr(NW->IO.IOBuf));
		if (NW->IO.ErrMsg == NULL)
			NW->IO.ErrMsg = NewStrBuf();
		StrBufPrintf(NW->IO.ErrMsg,
			     "Connected to node \"%s\" but my secret wasn't accurate.\nReason was:%s\n",
			     ChrPtr(NW->node), ChrPtr(NW->IO.IOBuf) + 4);
		if (Error == 552) {
			SetNWCState(IO, eNWCVSAuthFailNTT);
			EVN_syslog(LOG_INFO,
				   "Already talking to %s; skipping this time.\n",
				   ChrPtr(NW->node));
			
		}
		else {
			SetNWCState(IO, eNWCVSAuthFailNTT);
			EVN_syslog(LOG_ERR, "%s\n", ChrPtr(NW->IO.ErrMsg));
			return EventQueueDBOperation(IO, NWC_SendFailureMessage, 1);
		}
		return eAbort;
	}
}

eNextState NWC_SendNDOP(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	SetNWCState(IO, eNWCVSNDOP);
	NW->tempFileName = NewStrBuf();
	NW->SpoolFileName = NewStrBuf();
	StrBufPrintf(NW->SpoolFileName,
		     "%s/%s.%lx%x",
		     ctdl_netin_dir,
		     ChrPtr(NW->node),
		     time(NULL),// TODO: get time from libev
		     rand());
	StrBufStripSlashes(NW->SpoolFileName, 1);
	StrBufPrintf(NW->tempFileName, 
		     "%s/%s.%lx%x",
		     ctdl_nettmp_dir,
		     ChrPtr(NW->node),
		     time(NULL),// TODO: get time from libev
		     rand());
	StrBufStripSlashes(NW->tempFileName, 1);
	/* We're talking to the correct node.  Now identify ourselves. */
	StrBufPlain(NW->IO.SendBuf.Buf, HKEY("NDOP\n"));
	NWC_DBG_SEND();
	return eSendReply;
}

eNextState NWC_ReadNDOPReply(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	int TotalSendSize;
	NWC_DBG_READ();
	if (ChrPtr(NW->IO.IOBuf)[0] == '2')
	{
		int LogLevel = LOG_DEBUG;

		NW->IO.IOB.TotalSentAlready = 0;

		TotalSendSize = atol (ChrPtr(NW->IO.IOBuf) + 4);

		if (TotalSendSize > 0)
			LogLevel = LOG_INFO;

		EVN_syslog(LogLevel,
			   "Expecting to transfer %d bytes to %s\n",
			   TotalSendSize,
			   ChrPtr(NW->tempFileName));

		if (TotalSendSize <= 0) {
			NW->State = eNUOP - 1;
		}
		else {
			int fd;
			fd = open(ChrPtr(NW->tempFileName), 
				  O_EXCL|O_CREAT|O_NONBLOCK|O_WRONLY, 
				  S_IRUSR|S_IWUSR);
			if (fd < 0)
			{
				SetNWCState(IO, eNWCVSFail);
				EVN_syslog(LOG_CRIT,
				       "cannot open %s: %s\n", 
				       ChrPtr(NW->tempFileName), 
				       strerror(errno));

				NW->State = eQUIT - 1;
				return eAbort;
			}
			FDIOBufferInit(&NW->IO.IOB, &NW->IO.RecvBuf, fd, TotalSendSize);
		}
		return eSendReply;
	}
	else
	{
		SetNWCState(IO, eNWCVSFail);
		return eAbort;
	}
}

eNextState NWC_SendREAD(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	eNextState rc;

	if (NW->IO.IOB.TotalSentAlready < NW->IO.IOB.TotalSendSize)
	{
		/*
		 * If shutting down we can exit here and unlink the temp file.
		 * this shouldn't loose us any messages.
		 */
		if (server_shutting_down)
		{
			FDIOBufferDelete(&NW->IO.IOB);
			unlink(ChrPtr(NW->tempFileName));
			FDIOBufferDelete(&IO->IOB);
			SetNWCState(IO, eNWCVSFail);
			return eAbort;
		}
		StrBufPrintf(NW->IO.SendBuf.Buf, "READ "LOFF_T_FMT"|%ld\n",
			     NW->IO.IOB.TotalSentAlready,
			     NW->IO.IOB.TotalSendSize);
/*
			     ((NW->IO.IOB.TotalSendSize - NW->IO.IOB.TotalSentAlready > IGNET_PACKET_SIZE)
			      ? IGNET_PACKET_SIZE : 
			      (NW->IO.IOB.TotalSendSize - NW->IO.IOB.TotalSentAlready))
			);
*/
		NWC_DBG_SEND();
		return eSendReply;
	}
	else 
	{
		NW->State = eCLOS;
		rc = NWC_DispatchWriteDone(&NW->IO);
		NWC_DBG_SEND();

		return rc;
	}
}

eNextState NWC_ReadREADState(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	NWC_DBG_READ();
	if (ChrPtr(NW->IO.IOBuf)[0] == '6')
	{
		NW->IO.IOB.ChunkSendRemain = 
			NW->IO.IOB.ChunkSize = atol(ChrPtr(NW->IO.IOBuf)+4);
		return eReadFile;
	}
	FDIOBufferDelete(&IO->IOB);
	return eAbort;
}
eNextState NWC_ReadREADBlobDone(AsyncNetworker *NW);
eNextState NWC_ReadREADBlob(AsyncNetworker *NW)
{
	eNextState rc;
	AsyncIO *IO = &NW->IO;
	NWC_DBG_READ();
	if (NW->IO.IOB.TotalSendSize == NW->IO.IOB.TotalSentAlready)
	{
		NW->State ++;

		FDIOBufferDelete(&NW->IO.IOB);

		if (link(ChrPtr(NW->tempFileName), ChrPtr(NW->SpoolFileName)) != 0) {
			EVN_syslog(LOG_ALERT, 
			       "Could not link %s to %s: %s\n",
			       ChrPtr(NW->tempFileName), 
			       ChrPtr(NW->SpoolFileName), 
			       strerror(errno));
		}
		else {
			EVN_syslog(LOG_INFO, 
			       "moved %s to %s\n",
			       ChrPtr(NW->tempFileName), 
			       ChrPtr(NW->SpoolFileName));
		}

		unlink(ChrPtr(NW->tempFileName));
		rc = NWC_DispatchWriteDone(&NW->IO);
		NW->State --;
		return rc;
	}
	else {
		NW->State --;
		NW->IO.IOB.ChunkSendRemain = NW->IO.IOB.ChunkSize;
		return eSendReply; //NWC_DispatchWriteDone(&NW->IO);
	}
}

eNextState NWC_ReadREADBlobDone(AsyncNetworker *NW)
{
	eNextState rc;
	AsyncIO *IO = &NW->IO;
/* we don't have any data to debug print here. */
	if (NW->IO.IOB.TotalSentAlready >= NW->IO.IOB.TotalSendSize)
	{
		NW->State ++;

		FDIOBufferDelete(&NW->IO.IOB);
		if (link(ChrPtr(NW->tempFileName), ChrPtr(NW->SpoolFileName)) != 0) {
			EVN_syslog(LOG_ALERT, 
			       "Could not link %s to %s: %s\n",
			       ChrPtr(NW->tempFileName), 
			       ChrPtr(NW->SpoolFileName), 
			       strerror(errno));
		}
		else {
			EVN_syslog(LOG_INFO, 
			       "moved %s to %s\n",
			       ChrPtr(NW->tempFileName), 
			       ChrPtr(NW->SpoolFileName));
		}
	
		unlink(ChrPtr(NW->tempFileName));
		rc = NWC_DispatchWriteDone(&NW->IO);
		NW->State --;
		return rc;
	}
	else {
		NW->State --;
		NW->IO.IOB.ChunkSendRemain = NW->IO.IOB.ChunkSize;
		return NWC_DispatchWriteDone(&NW->IO);
	}
}
eNextState NWC_SendCLOS(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	SetNWCState(IO, eNWCVSNDOPDone);
	StrBufPlain(NW->IO.SendBuf.Buf, HKEY("CLOS\n"));
	NWC_DBG_SEND();
	return eSendReply;
}

eNextState NWC_ReadCLOSReply(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	NWC_DBG_READ();
	FDIOBufferDelete(&IO->IOB);
	if (ChrPtr(NW->IO.IOBuf)[0] != '2')
		return eTerminateConnection;
	return eSendReply;
}


eNextState NWC_SendNUOP(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	eNextState rc;
	long TotalSendSize;
	struct stat statbuf;
	int fd;

	SetNWCState(IO, eNWCVSNUOP);
	StrBufPrintf(NW->SpoolFileName,
		     "%s/%s",
		     ctdl_netout_dir,
		     ChrPtr(NW->node));
	StrBufStripSlashes(NW->SpoolFileName, 1);

	fd = open(ChrPtr(NW->SpoolFileName), O_EXCL|O_NONBLOCK|O_RDONLY);
	if (fd < 0) {
		if (errno != ENOENT) {
			EVN_syslog(LOG_CRIT,
			       "cannot open %s: %s\n", 
			       ChrPtr(NW->SpoolFileName), 
			       strerror(errno));
		}
		NW->State = eQUIT;
		rc = NWC_SendQUIT(NW);
		NWC_DBG_SEND();
		return rc;
	}

	if (fstat(fd, &statbuf) == -1) {
		EVN_syslog(LOG_CRIT, "FSTAT FAILED %s [%s]--\n", 
			   ChrPtr(NW->SpoolFileName), 
			   strerror(errno));
		if (fd > 0) close(fd);
		return eAbort;
	}
	TotalSendSize = statbuf.st_size;
	if (TotalSendSize == 0) {
		EVNM_syslog(LOG_DEBUG,
		       "Nothing to send.\n");
		NW->State = eQUIT;
		rc = NWC_SendQUIT(NW);
		NWC_DBG_SEND();
		if (fd > 0) close(fd);
		return rc;
	}
	else
       	{
		EVN_syslog(LOG_INFO,
			   "sending %s to %s\n", 
			   ChrPtr(NW->SpoolFileName),
			   ChrPtr(NW->node));
	}

	FDIOBufferInit(&NW->IO.IOB, &NW->IO.SendBuf, fd, TotalSendSize);

	StrBufPlain(NW->IO.SendBuf.Buf, HKEY("NUOP\n"));
	NWC_DBG_SEND();
	return eSendReply;

}
eNextState NWC_ReadNUOPReply(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	NWC_DBG_READ();
	if (ChrPtr(NW->IO.IOBuf)[0] != '2') {
		FDIOBufferDelete(&IO->IOB);
		return eAbort;
	}
	return eSendReply;
}

eNextState NWC_SendWRIT(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	StrBufPrintf(NW->IO.SendBuf.Buf, "WRIT "LOFF_T_FMT"\n", 
		     NW->IO.IOB.TotalSendSize - NW->IO.IOB.TotalSentAlready);
	NWC_DBG_SEND();
	return eSendReply;
}
eNextState NWC_ReadWRITReply(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	NWC_DBG_READ();
	if (ChrPtr(NW->IO.IOBuf)[0] != '7')
	{
		FDIOBufferDelete(&IO->IOB);
		return eAbort;
	}

	NW->IO.IOB.ChunkSendRemain = 
		NW->IO.IOB.ChunkSize = atol(ChrPtr(NW->IO.IOBuf)+4);
	return eSendFile;
}

eNextState NWC_SendBlobDone(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	eNextState rc;
	if (NW->IO.IOB.TotalSentAlready >= IO->IOB.TotalSendSize)
	{
		NW->State ++;

		FDIOBufferDelete(&IO->IOB);
		rc =  NWC_DispatchWriteDone(IO);
		NW->State --;
		return rc;
	}
	else {
		NW->State --;
		IO->IOB.ChunkSendRemain = IO->IOB.ChunkSize;
		rc = NWC_DispatchWriteDone(IO);
		NW->State --;
		return rc;
	}
}

eNextState NWC_SendUCLS(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	StrBufPlain(NW->IO.SendBuf.Buf, HKEY("UCLS 1\n"));
	NWC_DBG_SEND();
	return eSendReply;

}
eNextState NWC_ReadUCLS(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	NWC_DBG_READ();

	EVN_syslog(LOG_NOTICE,
		   "Sent %s [%ld] octets to <%s>\n",
		   ChrPtr(NW->SpoolFileName),
		   NW->IO.IOB.ChunkSize,
		   ChrPtr(NW->node));

	if (ChrPtr(NW->IO.IOBuf)[0] == '2') {
		EVN_syslog(LOG_DEBUG, "Removing <%s>\n", ChrPtr(NW->SpoolFileName));
		unlink(ChrPtr(NW->SpoolFileName));
	}
	FDIOBufferDelete(&IO->IOB);
	SetNWCState(IO, eNWCVSNUOPDone);
	return eSendReply;
}

eNextState NWC_SendQUIT(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	StrBufPlain(NW->IO.SendBuf.Buf, HKEY("QUIT\n"));

	NWC_DBG_SEND();
	return eSendReply;
}

eNextState NWC_ReadQUIT(AsyncNetworker *NW)
{
	AsyncIO *IO = &NW->IO;
	NWC_DBG_READ();

	return eAbort;
}


NWClientHandler NWC_ReadHandlers[] = {
	NWC_ReadGreeting,
	NWC_ReadAuthReply,
	NWC_ReadNDOPReply,
	NWC_ReadREADState,
	NWC_ReadREADBlob,
	NWC_ReadCLOSReply,
	NWC_ReadNUOPReply,
	NWC_ReadWRITReply,
	NWC_SendBlobDone,
	NWC_ReadUCLS,
	NWC_ReadQUIT};

long NWC_ConnTimeout = 100;

const long NWC_SendTimeouts[] = {
	100,
	100,
	100,
	100,
	100,
	100,
	100,
	100
};
const ConstStr NWC[] = {
	{HKEY("Connection broken during ")},
	{HKEY("Connection broken during ")},
	{HKEY("Connection broken during ")},
	{HKEY("Connection broken during ")},
	{HKEY("Connection broken during ")},
	{HKEY("Connection broken during ")},
	{HKEY("Connection broken during ")},
	{HKEY("Connection broken during ")}
};

NWClientHandler NWC_SendHandlers[] = {
	NULL,
	NWC_SendAuth,
	NWC_SendNDOP,
	NWC_SendREAD,
	NWC_ReadREADBlobDone,
	NWC_SendCLOS,
	NWC_SendNUOP,
	NWC_SendWRIT,
	NWC_SendBlobDone,
	NWC_SendUCLS,
	NWC_SendQUIT
};

const long NWC_ReadTimeouts[] = {
	100,
	100,
	100,
	100,
	100,
	100,
	100,
	100,
	100,
	100
};




eNextState nwc_get_one_host_ip_done(AsyncIO *IO)
{
	AsyncNetworker *NW = IO->Data;
	struct hostent *hostent;

	QueryCbDone(IO);

	hostent = NW->HostLookup.VParsedDNSReply;
	if ((NW->HostLookup.DNSStatus == ARES_SUCCESS) && 
	    (hostent != NULL) ) {
		memset(&NW->IO.ConnectMe->Addr, 0, sizeof(struct in6_addr));
		if (NW->IO.ConnectMe->IPv6) {
			memcpy(&NW->IO.ConnectMe->Addr.sin6_addr.s6_addr, 
			       &hostent->h_addr_list[0],
			       sizeof(struct in6_addr));
			
			NW->IO.ConnectMe->Addr.sin6_family = hostent->h_addrtype;
			NW->IO.ConnectMe->Addr.sin6_port   = htons(atol(ChrPtr(NW->port)));//// TODO use the one from the URL.
		}
		else {
			struct sockaddr_in *addr = (struct sockaddr_in*) &NW->IO.ConnectMe->Addr;
			/* Bypass the ns lookup result like this: IO->Addr.sin_addr.s_addr = inet_addr("127.0.0.1"); */
//			addr->sin_addr.s_addr = htonl((uint32_t)&hostent->h_addr_list[0]);
			memcpy(&addr->sin_addr.s_addr, 
			       hostent->h_addr_list[0], 
			       sizeof(uint32_t));
			
			addr->sin_family = hostent->h_addrtype;
			addr->sin_port   = htons(504);/// default citadel port
			
		}
		return nwc_connect_ip(IO);
	}
	else
		return eAbort;
}


eNextState nwc_get_one_host_ip(AsyncIO *IO)
{
	AsyncNetworker *NW = IO->Data;
	/* 
	 * here we start with the lookup of one host.
	 */ 

	EVN_syslog(LOG_DEBUG, "NWC: %s\n", __FUNCTION__);

	EVN_syslog(LOG_DEBUG, 
		   "NWC client[%ld]: looking up %s-Record %s : %d ...\n", 
		   NW->n, 
		   (NW->IO.ConnectMe->IPv6)? "aaaa": "a",
		   NW->IO.ConnectMe->Host, 
		   NW->IO.ConnectMe->Port);

	QueueQuery((NW->IO.ConnectMe->IPv6)? ns_t_aaaa : ns_t_a, 
		   NW->IO.ConnectMe->Host, 
		   &NW->IO, 
		   &NW->HostLookup, 
		   nwc_get_one_host_ip_done);
	IO->NextState = eReadDNSReply;
	return IO->NextState;
}
/**
 * @brief lineread Handler; understands when to read more POP3 lines, and when this is a one-lined reply.
 */
eReadState NWC_ReadServerStatus(AsyncIO *IO)
{
//	AsyncNetworker *NW = IO->Data;
	eReadState Finished = eBufferNotEmpty; 

	switch (IO->NextState) {
	case eSendDNSQuery:
	case eReadDNSReply:
	case eDBQuery:
	case eConnect:
	case eTerminateConnection:
	case eAbort:
		Finished = eReadFail;
		break;
	case eSendReply: 
	case eSendMore:
	case eReadMore:
	case eReadMessage: 
		Finished = StrBufChunkSipLine(IO->IOBuf, &IO->RecvBuf);
		break;
	case eReadFile:
	case eSendFile:
	case eReadPayload:
		break;
	}
	return Finished;
}



eNextState NWC_FailNetworkConnection(AsyncIO *IO)
{
	SetNWCState(IO, eNWCVSConnFail);
	return EventQueueDBOperation(IO, NWC_SendFailureMessage, 1);
}

void NWC_SetTimeout(eNextState NextTCPState, AsyncNetworker *NW)
{
	double Timeout = 0.0;

	//EVN_syslog(LOG_DEBUG, "%s - %d\n", __FUNCTION__, NextTCPState);

	switch (NextTCPState) {
	case eSendMore:
	case eSendReply:
	case eReadMessage:
		Timeout = NWC_ReadTimeouts[NW->State];
		break;
	case eReadFile:
	case eSendFile:
	case eReadPayload:
		Timeout = 100000;
		break;
	case eSendDNSQuery:
	case eReadDNSReply:
	case eDBQuery:
	case eReadMore:
	case eConnect:
	case eTerminateConnection:
	case eAbort:
		return;
	}
	if (Timeout > 0) {
		AsyncIO *IO = &NW->IO;
		EVN_syslog(LOG_DEBUG, 
			   "%s - %d %f\n",
			   __FUNCTION__,
			   NextTCPState,
			   Timeout);
		SetNextTimeout(&NW->IO, Timeout*100);
	}
}


eNextState NWC_DispatchReadDone(AsyncIO *IO)
{
	EVN_syslog(LOG_DEBUG, "%s\n", __FUNCTION__);
	AsyncNetworker *NW = IO->Data;
	eNextState rc;

	rc = NWC_ReadHandlers[NW->State](NW);

	if ((rc != eReadMore) &&
	    (rc != eAbort) && 
	    (rc != eDBQuery)) {
		NW->State++;
	}

	NWC_SetTimeout(rc, NW);

	return rc;
}
eNextState NWC_DispatchWriteDone(AsyncIO *IO)
{
	EVN_syslog(LOG_DEBUG, "%s\n", __FUNCTION__);
	AsyncNetworker *NW = IO->Data;
	eNextState rc;

	rc = NWC_SendHandlers[NW->State](NW);
	NWC_SetTimeout(rc, NW);
	return rc;
}

/*****************************************************************************/
/*                     Networker CLIENT ERROR CATCHERS                       */
/*****************************************************************************/
eNextState NWC_Terminate(AsyncIO *IO)
{
	EVN_syslog(LOG_DEBUG, "%s\n", __FUNCTION__);
	FinalizeNetworker(IO);
	return eAbort;
}

eNextState NWC_TerminateDB(AsyncIO *IO)
{
	EVN_syslog(LOG_DEBUG, "%s\n", __FUNCTION__);
	FinalizeNetworker(IO);
	return eAbort;
}

eNextState NWC_Timeout(AsyncIO *IO)
{
	AsyncNetworker *NW = IO->Data;
	EVN_syslog(LOG_DEBUG, "%s\n", __FUNCTION__);

	if (NW->IO.ErrMsg == NULL)
		NW->IO.ErrMsg = NewStrBuf();
	StrBufPrintf(NW->IO.ErrMsg, "Timeout while talking to %s \r\n", ChrPtr(NW->host));
	return NWC_FailNetworkConnection(IO);
}
eNextState NWC_ConnFail(AsyncIO *IO)
{
	AsyncNetworker *NW = IO->Data;

	EVN_syslog(LOG_DEBUG, "%s\n", __FUNCTION__);
	if (NW->IO.ErrMsg == NULL)
		NW->IO.ErrMsg = NewStrBuf();
	StrBufPrintf(NW->IO.ErrMsg, "failed to connect %s \r\n", ChrPtr(NW->host));

	return NWC_FailNetworkConnection(IO);
}
eNextState NWC_DNSFail(AsyncIO *IO)
{
	AsyncNetworker *NW = IO->Data;

	EVN_syslog(LOG_DEBUG, "%s\n", __FUNCTION__);
	if (NW->IO.ErrMsg == NULL)
		NW->IO.ErrMsg = NewStrBuf();
	StrBufPrintf(NW->IO.ErrMsg, "failed to look up %s \r\n", ChrPtr(NW->host));

	return NWC_FailNetworkConnection(IO);
}
eNextState NWC_Shutdown(AsyncIO *IO)
{
	EVN_syslog(LOG_DEBUG, "%s\n", __FUNCTION__);

	FinalizeNetworker(IO);
	return eAbort;
}


eNextState nwc_connect_ip(AsyncIO *IO)
{
	AsyncNetworker *NW = IO->Data;

	SetNWCState(&NW->IO, eNWCVSConnecting);
	EVN_syslog(LOG_DEBUG, "%s\n", __FUNCTION__);
	EVN_syslog(LOG_NOTICE, "Connecting to <%s> at %s:%s\n", 
		   ChrPtr(NW->node), 
		   ChrPtr(NW->host),
		   ChrPtr(NW->port));
	
	return EvConnectSock(IO,
			     NWC_ConnTimeout,
			     NWC_ReadTimeouts[0],
			     1);
}

static int NetworkerCount = 0;
void RunNetworker(AsyncNetworker *NW)
{
	NW->n = NetworkerCount++;
	CtdlNetworkTalkingTo(SKEY(NW->node), NTT_ADD);
	syslog(LOG_DEBUG, "NW[%s][%ld]: polling\n", ChrPtr(NW->node), NW->n);
	ParseURL(&NW->IO.ConnectMe, NW->Url, 504);

	InitIOStruct(&NW->IO,
		     NW,
		     eReadMessage,
		     NWC_ReadServerStatus,
		     NWC_DNSFail,
		     NWC_DispatchWriteDone,
		     NWC_DispatchReadDone,
		     NWC_Terminate,
		     NWC_TerminateDB,
		     NWC_ConnFail,
		     NWC_Timeout,
		     NWC_Shutdown);

	safestrncpy(((CitContext *)NW->IO.CitContext)->cs_host, 
		    ChrPtr(NW->host),
		    sizeof(((CitContext *)NW->IO.CitContext)->cs_host)); 

	if (NW->IO.ConnectMe->IsIP) {
		SetNWCState(&NW->IO, eNWCVSLookup);
		QueueEventContext(&NW->IO,
				  nwc_connect_ip);
	}
	else { /* uneducated admin has chosen to add DNS to the equation... */
		SetNWCState(&NW->IO, eNWCVSConnecting);
		QueueEventContext(&NW->IO,
				  nwc_get_one_host_ip);
	}
}

/*
 * Poll other Citadel nodes and transfer inbound/outbound network data.
 * Set "full" to nonzero to force a poll of every node, or to zero to poll
 * only nodes to which we have data to send.
 */
void network_poll_other_citadel_nodes(int full_poll, HashList *ignetcfg)
{
	const char *key;
	long len;
	HashPos *Pos;
	void *vCfg;
	AsyncNetworker *NW;
	StrBuf *SpoolFileName;
	
	int poll = 0;
	
	if (GetCount(ignetcfg) ==0) {
		MARKM_syslog(LOG_DEBUG, "network: no neighbor nodes are configured - not polling.\n");
		return;
	}
	become_session(&networker_client_CC);

	SpoolFileName = NewStrBufPlain(ctdl_netout_dir, -1);

	Pos = GetNewHashPos(ignetcfg, 0);

	while (GetNextHashPos(ignetcfg, Pos, &len, &key, &vCfg))
	{
		/* Use the string tokenizer to grab one line at a time */
		if(server_shutting_down)
			return;/* TODO free stuff*/
		CtdlNodeConf *pNode = (CtdlNodeConf*) vCfg;
		poll = 0;
		NW = (AsyncNetworker*)malloc(sizeof(AsyncNetworker));
		memset(NW, 0, sizeof(AsyncNetworker));
		
		NW->node = NewStrBufDup(pNode->NodeName);
		NW->host = NewStrBufDup(pNode->Host);
		NW->port = NewStrBufDup(pNode->Port);
		NW->secret = NewStrBufDup(pNode->Secret);
		
		if ( (StrLength(NW->node) != 0) && 
		     (StrLength(NW->secret) != 0) &&
		     (StrLength(NW->host) != 0) &&
		     (StrLength(NW->port) != 0))
		{
			poll = full_poll;
			if (poll == 0)
			{
				StrBufAppendBufPlain(SpoolFileName, HKEY("/"), 0);
				StrBufAppendBuf(SpoolFileName, NW->node, 0);
				StrBufStripSlashes(SpoolFileName, 1);
				
				if (access(ChrPtr(SpoolFileName), R_OK) == 0) {
					poll = 1;
				}
			}
		}
		if (poll && 
		    (StrLength(NW->host) > 0) && 
		    strcmp("0.0.0.0", ChrPtr(NW->host)))
		{
			NW->Url = NewStrBuf();
			StrBufPrintf(NW->Url, "citadel://%s@%s:%s", 
				     ChrPtr(NW->secret),
				     ChrPtr(NW->host),
				     ChrPtr(NW->port));
			if (!CtdlNetworkTalkingTo(SKEY(NW->node), NTT_CHECK))
			{
				RunNetworker(NW);
				continue;
			}
		}
		DeleteNetworker(NW);
	}
	FreeStrBuf(&SpoolFileName);
	DeleteHashPos(&Pos);
}


void network_do_clientqueue(void)
{
	HashList *working_ignetcfg;
	int full_processing = 1;
	static time_t last_run = 0L;

	/*
	 * Run the full set of processing tasks no more frequently
	 * than once every n seconds
	 */
	if ( (time(NULL) - last_run) < CtdlGetConfigLong("c_net_freq") )
	{
		full_processing = 0;
		syslog(LOG_DEBUG, "Network full processing in %ld seconds.\n",
			CtdlGetConfigLong("c_net_freq") - (time(NULL)- last_run)
		);
	}

	working_ignetcfg = CtdlLoadIgNetCfg();
	/*
	 * Poll other Citadel nodes.  Maybe.  If "full_processing" is set
	 * then we poll everyone.  Otherwise we only poll nodes we have stuff
	 * to send to.
	 */
	network_poll_other_citadel_nodes(full_processing, working_ignetcfg);
	DeleteHash(&working_ignetcfg);
}

void LogDebugEnableNetworkClient(const int n)
{
	NetworkClientDebugEnabled = n;
}
/*
 * Module entry point
 */
CTDL_MODULE_INIT(network_client)
{
	if (!threading)
	{
		CtdlFillSystemContext(&networker_client_CC, "CitNetworker");
		
		CtdlRegisterSessionHook(network_do_clientqueue, EVT_TIMER, PRIO_SEND + 10);
		CtdlRegisterDebugFlagHook(HKEY("networkclient"), LogDebugEnableNetworkClient, &NetworkClientDebugEnabled);

	}
	return "networkclient";
}
