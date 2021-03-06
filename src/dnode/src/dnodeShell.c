/*
 * Copyright (c) 2019 TAOS Data, Inc. <jhtao@taosdata.com>
 *
 * This program is free software: you can use, redistribute, and/or modify
 * it under the terms of the GNU Affero General Public License, version 3
 * or later ("AGPL"), as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define _DEFAULT_SOURCE
#include "os.h"
#include "taoserror.h"
#include "taosdef.h"
#include "taosmsg.h"
#include "trpc.h"
#include "tglobal.h"
#include "dnode.h"
#include "dnodeLog.h"
#include "dnodeRead.h"
#include "dnodeWrite.h"
#include "dnodeShell.h"

static void (*dnodeProcessShellMsgFp[TSDB_MSG_TYPE_MAX])(SRpcMsg *);
static void   dnodeProcessMsgFromShell(SRpcMsg *pMsg);
static int    dnodeRetrieveUserAuthInfo(char *user, char *spi, char *encrypt, char *secret, char *ckey);
static void  *tsDnodeShellRpc = NULL;
static int32_t tsDnodeQueryReqNum  = 0;
static int32_t tsDnodeSubmitReqNum = 0;

int32_t dnodeInitShell() {
  dnodeProcessShellMsgFp[TSDB_MSG_TYPE_SUBMIT]   = dnodeWrite;
  dnodeProcessShellMsgFp[TSDB_MSG_TYPE_QUERY]    = dnodeRead;
  dnodeProcessShellMsgFp[TSDB_MSG_TYPE_RETRIEVE] = dnodeRead;

  int32_t numOfThreads = tsNumOfCores * tsNumOfThreadsPerCore;
  numOfThreads = (int32_t) ((1.0 - tsRatioOfQueryThreads) * numOfThreads / 2.0);
  if (numOfThreads < 1) {
    numOfThreads = 1;
  }

  SRpcInit rpcInit;
  memset(&rpcInit, 0, sizeof(rpcInit));
  rpcInit.localIp      = tsAnyIp ? "0.0.0.0" : tsPrivateIp;
  rpcInit.localPort    = tsDnodeShellPort;
  rpcInit.label        = "DND-shell";
  rpcInit.numOfThreads = numOfThreads;
  rpcInit.cfp          = dnodeProcessMsgFromShell;
  rpcInit.sessions     = TSDB_SESSIONS_PER_DNODE;
  rpcInit.connType     = TAOS_CONN_SERVER;
  rpcInit.idleTime     = tsShellActivityTimer * 1500;
  rpcInit.afp          = dnodeRetrieveUserAuthInfo;

  tsDnodeShellRpc = rpcOpen(&rpcInit);
  if (tsDnodeShellRpc == NULL) {
    dError("failed to init shell rpc server");
    return -1;
  }

  dPrint("shell rpc server is opened");
  return 0;
}

void dnodeCleanupShell() {
  if (tsDnodeShellRpc) {
    rpcClose(tsDnodeShellRpc);
    tsDnodeShellRpc = NULL;
  }
}

void dnodeProcessMsgFromShell(SRpcMsg *pMsg) {
  SRpcMsg rpcMsg;
  rpcMsg.handle = pMsg->handle;
  rpcMsg.pCont = NULL;
  rpcMsg.contLen = 0;

  if (dnodeGetRunStatus() != TSDB_DNODE_RUN_STATUS_RUNING) {
    dError("RPC %p, shell msg is ignored since dnode not running", pMsg->handle);
    rpcMsg.code = TSDB_CODE_NOT_READY;
    rpcSendResponse(&rpcMsg);
    rpcFreeCont(pMsg->pCont);
    return;
  }

  if (pMsg->msgType == TSDB_MSG_TYPE_QUERY) {
    atomic_fetch_add_32(&tsDnodeQueryReqNum, 1);
  } else if (pMsg->msgType == TSDB_MSG_TYPE_SUBMIT) {
    atomic_fetch_add_32(&tsDnodeSubmitReqNum, 1);
  } else {}

  if ( dnodeProcessShellMsgFp[pMsg->msgType] ) {
    (*dnodeProcessShellMsgFp[pMsg->msgType])(pMsg);
  } else {
    dError("RPC %p, msg:%s from shell is not handled", pMsg->handle, taosMsg[pMsg->msgType]);
    rpcMsg.code = TSDB_CODE_MSG_NOT_PROCESSED;
    rpcSendResponse(&rpcMsg);
    rpcFreeCont(pMsg->pCont);
  }
}

static int dnodeRetrieveUserAuthInfo(char *user, char *spi, char *encrypt, char *secret, char *ckey) {
  return TSDB_CODE_SUCCESS;
}

SDnodeStatisInfo dnodeGetStatisInfo() {
  SDnodeStatisInfo info = {0};
  if (dnodeGetRunStatus() == TSDB_DNODE_RUN_STATUS_RUNING) {
    //info.httpReqNum   = httpGetReqCount();
    info.queryReqNum  = atomic_exchange_32(&tsDnodeQueryReqNum, 0);
    info.submitReqNum = atomic_exchange_32(&tsDnodeSubmitReqNum, 0);
  }

  return info;
}
