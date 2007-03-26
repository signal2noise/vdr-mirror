/*
 * transfer.c: Transfer mode
 *
 * See the main source file 'vdr.c' for copyright information and
 * how to reach the author.
 *
 * $Id: transfer.c 1.33 2006/01/29 17:24:39 kls Exp $
 */

#include "transfer.h"

#define TRANSFERBUFSIZE  MEGABYTE(2)
#define POLLTIMEOUTS_BEFORE_DEVICECLEAR 6

// --- cTransfer -------------------------------------------------------------

cTransfer::cTransfer(int VPid, const int *APids, const int *DPids, const int *SPids)
:cReceiver(0, -1, VPid, APids, Setup.UseDolbyDigital ? DPids : NULL, SPids)
,cThread("transfer")
{
//  ringBuffer = new cRingBufferLinear(TRANSFERBUFSIZE, TS_SIZE * 2, true, "Transfer");
  remux = new cRemux(VPid, APids, Setup.UseDolbyDigital ? DPids : NULL, SPids);
  remux->SetTimeouts(50, 20);
}

cTransfer::~cTransfer()
{
  cReceiver::Detach();
  cPlayer::Detach();
  delete remux;
//  delete ringBuffer;
}

void cTransfer::Activate(bool On)
{
  if (On)
     Start();
  else
     Cancel(3);
}

void cTransfer::Receive(uchar *Data, int Length)
{
  if (IsAttached() && Running()) {
    static FILE *tsOut = NULL;
/*    
    if (!tsOut)
    {
    tsOut = ::fopen("/mnt/hd/video/aufnahme.ts", "wb");
    }
    if (tsOut)
    {
    ::fwrite(Data, 1, Length, tsOut);
    }
*/					      

/*
     int p = ringBuffer->Put(Data, Length);
     if (p != Length && Running())
        ringBuffer->ReportOverflow(Length - p);
*/
	   remux->Put(Data,Length);
     return;
     }
}

void cTransfer::Action(void)
{
  int PollTimeouts = 0;
  uchar *p = NULL;
  int Result = 0;
  while (Running()) {
        if (!p)
           //p = remux->Get(Result);
	   p = remux->Get(Result,NULL, 1);
        if (p) {
           cPoller Poller;
           if (DevicePoll(Poller, 100)) {
              PollTimeouts = 0;
              int w = PlayPes(p, Result);
              if (w > 0) {
                 p += w;
                 Result -= w;
                 remux->Del(w);
                 if (Result <= 0)
                    p = NULL;
                 }
              else if (w < 0 && FATALERRNO)
                 LOG_ERROR;
              }
           else {
              PollTimeouts++;
              if (PollTimeouts == POLLTIMEOUTS_BEFORE_DEVICECLEAR) {
                 dsyslog("clearing device because of consecutive poll timeouts");
                 DeviceClear();
//                 ringBuffer->Clear();
                 remux->Clear();
                 PlayPes(NULL, 0);
                 p = NULL;
                 }
              }
           }
        }
}

// --- cTransferControl ------------------------------------------------------

cDevice *cTransferControl::receiverDevice = NULL;

cTransferControl::cTransferControl(cDevice *ReceiverDevice, int VPid, const int *APids, const int *DPids, const int *SPids)
:cControl(transfer = new cTransfer(VPid, APids, DPids, SPids), true)
{
  ReceiverDevice->AttachReceiver(transfer);
  receiverDevice = ReceiverDevice;
}

cTransferControl::~cTransferControl()
{
  receiverDevice = NULL;
  delete transfer;
}
