/*
 * device.c: The basic device interface
 *
 * See the main source file 'vdr.c' for copyright information and
 * how to reach the author.
 *
 * $Id: device.c 1.137 2006/09/03 10:13:25 kls Exp $
 */

#include "device.h"
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "audio.h"
#include "channels.h"
#include "i18n.h"
#include "livebuffer.h"
#include "player.h"
#include "receiver.h"
#include "status.h"
#include "transfer.h"

bool scanning_on_receiving_device = false;

// --- cPesAssembler ---------------------------------------------------------

class cPesAssembler {
private:
  uchar *data;
  uint32_t tag;
  int length;
  int size;
  bool Realloc(int Size);
public:
  cPesAssembler(void);
  ~cPesAssembler();
  int ExpectedLength(void) { return PacketSize(data); }
  static int PacketSize(const uchar *data);
  int Length(void) { return length; }
  const uchar *Data(void) { return data; } // only valid if Length() >= 4
  void Reset(void);
  void Put(uchar c);
  void Put(const uchar *Data, int Length);
  bool IsPes(void);
  };

cPesAssembler::cPesAssembler(void)
{
  data = NULL;
  size = 0;
  Reset();
}

cPesAssembler::~cPesAssembler()
{
  free(data);
}

void cPesAssembler::Reset(void)
{
  tag = 0xFFFFFFFF;
  length = 0;
}

bool cPesAssembler::Realloc(int Size)
{
  if (Size > size) {
     size = max(Size, 2048);
     data = (uchar *)realloc(data, size);
     if (!data) {
        esyslog("ERROR: can't allocate memory for PES assembler");
        length = 0;
        size = 0;
        return false;
        }
     }
  return true;
}

void cPesAssembler::Put(uchar c)
{
  if (length < 4) {
     tag = (tag << 8) | c;
     if ((tag & 0xFFFFFF00) == 0x00000100) {
        if (Realloc(4)) {
           *(uint32_t *)data = htonl(tag);
           length = 4;
           }
        }
     else if (length < 3)
        length++;
     }
  else if (Realloc(length + 1))
     data[length++] = c;
}

void cPesAssembler::Put(const uchar *Data, int Length)
{
  while (length < 4 && Length > 0) {
        Put(*Data++);
        Length--;
        }
  if (Length && Realloc(length + Length)) {
     memcpy(data + length, Data, Length);
     length += Length;
     }
}

int cPesAssembler::PacketSize(const uchar *data)
{
  // we need atleast 6 bytes of data here !!!
  switch (data[3]) {
    default:
    case 0x00 ... 0xB8: // video stream start codes
    case 0xB9: // Program end
    case 0xBC: // Programm stream map
    case 0xF0 ... 0xFF: // reserved
         return 6;

    case 0xBA: // Pack header
         if ((data[4] & 0xC0) == 0x40) // MPEG2
            return 14;
         // to be absolutely correct we would have to add the stuffing bytes
         // as well, but at this point we only may have 6 bytes of data avail-
         // able. So it's up to the higher level to resync...
         //return 14 + (data[13] & 0x07); // add stuffing bytes
         else // MPEG1
            return 12;

    case 0xBB: // System header
    case 0xBD: // Private stream1
    case 0xBE: // Padding stream
    case 0xBF: // Private stream2 (navigation data)
    case 0xC0 ... 0xCF: // all the rest (the real packets)
    case 0xD0 ... 0xDF:
    case 0xE0 ... 0xEF:
         return 6 + data[4] * 256 + data[5];
    }
}

// --- cDevice ---------------------------------------------------------------

// The default priority for non-primary devices:
#define DEFAULTPRIORITY  -1

int cDevice::numDevices = 0;
int cDevice::useDevice = 0;
int cDevice::nextCardIndex = 0;
int cDevice::currentChannel = 1;
cDevice *cDevice::device[MAXDEVICES] = { NULL };
cDevice *cDevice::primaryDevice = NULL;

cDevice::cDevice(void)
{
  cardIndex = nextCardIndex++;

  SetDescription("receiver on device %d", CardIndex() + 1);

  SetVideoFormat(Setup.VideoFormat);

  mute = false;
  volume = Setup.CurrentVolume;

  sectionHandler = NULL;
  eitFilter = NULL;
  patFilter = NULL;
  sdtFilter = NULL;
  nitFilter = NULL;

  ciHandler = NULL;
  player = NULL;
  pesAssembler = new cPesAssembler;
  ClrAvailableTracks();
  currentAudioTrack = ttNone;
  currentAudioTrackMissingCount = 0;

  for (int i = 0; i < MAXRECEIVERS; i++)
      receiver[i] = NULL;

  if (numDevices < MAXDEVICES)
     device[numDevices++] = this;
  else
     esyslog("ERROR: too many devices!");
}

cDevice::~cDevice()
{
  Detach(player);
  for (int i = 0; i < MAXRECEIVERS; i++)
      Detach(receiver[i]);
  delete ciHandler;
  delete nitFilter;
  delete sdtFilter;
  delete patFilter;
  delete eitFilter;
  delete sectionHandler;
  delete pesAssembler;
}

bool cDevice::WaitForAllDevicesReady(int Timeout)
{
  for (time_t t0 = time(NULL); time(NULL) - t0 < Timeout; ) {
      bool ready = true;
      for (int i = 0; i < numDevices; i++) {
          if (device[i] && !device[i]->Ready())
             ready = false;
          }
      if (ready)
         return true;
      }
  return false;
}

void cDevice::SetUseDevice(int n)
{
  if (n < MAXDEVICES)
     useDevice |= (1 << n);
}

int cDevice::NextCardIndex(int n)
{
  if (n > 0) {
     nextCardIndex += n;
     if (nextCardIndex >= MAXDEVICES)
        esyslog("ERROR: nextCardIndex too big (%d)", nextCardIndex);
     }
  else if (n < 0)
     esyslog("ERROR: invalid value in IncCardIndex(%d)", n);

  dsyslog ("[diseqc:]  cDevice::NextCardIndex: ret   %d \n", nextCardIndex);
  return nextCardIndex;
}

int cDevice::DeviceNumber(void) const
{
  for (int i = 0; i < numDevices; i++) {
      if (device[i] == this)
         return i;
      }
  return -1;
}

void cDevice::MakePrimaryDevice(bool On)
{
}

bool cDevice::SetPrimaryDevice(int n)
{
  n--;
  if (0 <= n && n < numDevices && device[n]) {
     isyslog("setting primary device to %d", n + 1);
     if (primaryDevice)
        primaryDevice->MakePrimaryDevice(false);
     primaryDevice = device[n];
     primaryDevice->MakePrimaryDevice(true);
     primaryDevice->SetVideoFormat(Setup.VideoFormat);
     return true;
     }
  esyslog("ERROR: invalid primary device number: %d", n + 1);
  return false;
}

bool cDevice::HasDecoder(void) const
{
  return false;
}

cSpuDecoder *cDevice::GetSpuDecoder(void)
{
  return NULL;
}

cDevice *cDevice::ActualDevice(void)
{
  cDevice *d = cTransferControl::ReceiverDevice();
  if (!d)
     d = PrimaryDevice();
  return d;
}

cDevice *cDevice::GetDevice(int Index)
{
  return (0 <= Index && Index < numDevices) ? device[Index] : NULL;
}

cDevice *cDevice::GetDevice(const cChannel *Channel, int Priority, bool *NeedsDetachReceivers)
{
  cDevice *d = NULL;
  uint Impact = 0xFFFFFFFF; // we're looking for a device with the least impact
  for (int i = 0; i < numDevices; i++) {
      bool ndr;
      if (device[i]->ProvidesChannel(Channel, Priority, &ndr)) { // this device is basicly able to do the job
         // Put together an integer number that reflects the "impact" using
         // this device would have on the overall system. Each condition is represented
         // by one bit in the number (or several bits, if the condition is actually
         // a numeric value). The sequence in which the conditions are listed corresponds
         // to their individual severity, where the one listed first will make the most
         // difference, because it results in the most significant bit of the result.
         uint imp = 0;
         imp <<= 1; imp |= !device[i]->Receiving(true) || ndr;                     // use receiving devices if we don't need to detach existing receivers
         imp <<= 1; imp |= device[i]->Receiving();                                 // avoid devices that are receiving
         //imp <<= 1; imp |= device[i] == cTransferControl::ReceiverDevice();        // avoid the Transfer Mode receiver device
         imp <<= 1; imp |= device[i] == ActualDevice();                            // avoid the actual device (in case of Transfer Mode)
         imp <<= 1; imp |= device[i]->IsPrimaryDevice();                           // avoid the primary device
         imp <<= 1; imp |= device[i]->HasDecoder();                                // avoid full featured cards
         imp <<= 8; imp |= min(max(device[i]->Priority() + MAXPRIORITY, 0), 0xFF); // use the device with the lowest priority (+MAXPRIORITY to assure that values -99..99 can be used)
         imp <<= 8; imp |= min(max(device[i]->ProvidesCa(Channel), 0), 0xFF);      // use the device that provides the lowest number of conditional access methods
         //imp <<= 1; imp |= device[i]->IsPrimaryDevice();                           // avoid the primary device
         //imp <<= 1; imp |= device[i]->HasDecoder();                                // avoid full featured cards
         if (imp < Impact) {
            // This device has less impact than any previous one, so we take it.
            Impact = imp;
            d = device[i];
            if (NeedsDetachReceivers)
               *NeedsDetachReceivers = ndr;
            }
         }
      }
  return d;
}

void cDevice::Shutdown(void)
{
  primaryDevice = NULL;
  for (int i = 0; i < numDevices; i++) {
      delete device[i];
      device[i] = NULL;
      }
}

uchar *cDevice::GrabImage(int &Size, bool Jpeg, int Quality, int SizeX, int SizeY)
{
  return NULL;
}

bool cDevice::GrabImageFile(const char *FileName, bool Jpeg, int Quality, int SizeX, int SizeY)
{
  int result = 0;
  int fd = open(FileName, O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC, DEFFILEMODE);
  if (fd >= 0) {
     int ImageSize;
     uchar *Image = GrabImage(ImageSize, Jpeg, Quality, SizeX, SizeY);
     if (Image) {
        if (safe_write(fd, Image, ImageSize) == ImageSize)
           isyslog("grabbed image to %s", FileName);
        else {
           LOG_ERROR_STR(FileName);
           result |= 1;
           }
        free(Image);
        }
     else
        result |= 1;
     close(fd);
     }
  else {
     LOG_ERROR_STR(FileName);
     result |= 1;
     }
  return result == 0;
}

void cDevice::SetVideoDisplayFormat(eVideoDisplayFormat VideoDisplayFormat)
{
  cSpuDecoder *spuDecoder = GetSpuDecoder();
  if (spuDecoder) {
     if (Setup.VideoFormat)
        spuDecoder->setScaleMode(cSpuDecoder::eSpuNormal);
     else {
        switch (VideoDisplayFormat) {
               case vdfPanAndScan:
                    spuDecoder->setScaleMode(cSpuDecoder::eSpuPanAndScan);
                    break;
               case vdfLetterBox:
                    spuDecoder->setScaleMode(cSpuDecoder::eSpuLetterBox);
                    break;
               case vdfCenterCutOut:
                    spuDecoder->setScaleMode(cSpuDecoder::eSpuNormal);
                    break;
               }
        }
     }
}

void cDevice::SetVideoFormat(bool VideoFormat16_9)
{
}

eVideoSystem cDevice::GetVideoSystem(void)
{
  return vsPAL;
}

//#define PRINTPIDS(s) { char b[500]; char *q = b; q += sprintf(q, "%d %s ", CardIndex(), s); for (int i = 0; i < MAXPIDHANDLES; i++) q += sprintf(q, " %s%4d %d", i == ptOther ? "* " : "", pidHandles[i].pid, pidHandles[i].used); dsyslog(b); }
#define PRINTPIDS(s)

bool cDevice::HasPid(int Pid) const
{
  for (int i = 0; i < MAXPIDHANDLES; i++) {
      if (pidHandles[i].pid == Pid)
         return true;
      }
  return false;
}

bool cDevice::AddPid(int Pid, ePidType PidType)
{
  int slotOnDev = GetSlotOnDev(this);
  if (Pid || PidType == ptPcr) {
     int n = -1;
     int a = -1;
     if (PidType != ptPcr) { // PPID always has to be explicit
        for (int i = 0; i < MAXPIDHANDLES; i++) {
            if (i != ptPcr) {
               if (pidHandles[i].pid == Pid)
                  n = i;
               else if (a < 0 && i >= ptOther && !pidHandles[i].used)
                  a = i;
               }
            }
        }
     if (n >= 0) {
        // The Pid is already in use
        if (++pidHandles[n].used == 2 && n <= ptTeletext) {
           // It's a special PID that may have to be switched into "tap" mode
           PRINTPIDS("A");
           if (!SetPid(&pidHandles[n], n, true)) {
              esyslog("ERROR: can't set PID %d on device %d", Pid, CardIndex() + 1);
              if (PidType <= ptTeletext)
                 DetachAll(Pid);
              DelPid(Pid, PidType);
              return false;
              }
	   ((cDevice::GetDevice(0))->CiHandler())->SetPid(Pid, true, slotOnDev);
           }
        PRINTPIDS("a");
        return true;
        }
     else if (PidType < ptOther) {
        // The Pid is not yet in use and it is a special one
        n = PidType;
        }
     else if (a >= 0) {
        // The Pid is not yet in use and we have a free slot
        n = a;
        }
     else {
        esyslog("ERROR: no free slot for PID %d on device %d", Pid, CardIndex() + 1);
        return false;
        }
     if (n >= 0) {
        pidHandles[n].pid = Pid;
        pidHandles[n].used = 1;
        PRINTPIDS("C");
        if (!SetPid(&pidHandles[n], n, true)) {
           esyslog("ERROR: can't set PID %d on device %d", Pid, CardIndex() + 1);
           if (PidType <= ptTeletext)
              DetachAll(Pid);
           DelPid(Pid, PidType);
           return false;
           }
	((cDevice::GetDevice(0))->CiHandler())->SetPid(Pid, true, slotOnDev);
        }
     }
  return true;
}

void cDevice::DelPid(int Pid, ePidType PidType)
{
  int slotOnDev = GetSlotOnDev(this);
  if (Pid || PidType == ptPcr) {
     int n = -1;
     if (PidType == ptPcr)
        n = PidType; // PPID always has to be explicit
     else {
        for (int i = 0; i < MAXPIDHANDLES; i++) {
            if (pidHandles[i].pid == Pid) {
               n = i;
               break;
               }
            }
        }
     if (n >= 0 && pidHandles[n].used) {
        PRINTPIDS("D");
        if (--pidHandles[n].used < 2) {
           SetPid(&pidHandles[n], n, false);
           if (pidHandles[n].used == 0) {
              pidHandles[n].handle = -1;
              pidHandles[n].pid = 0;
              (cDevice::GetDevice(0))->CiHandler()->SetPid(Pid, false, slotOnDev);
              }
           }
        PRINTPIDS("E");
        }
     }
}

bool cDevice::SetPid(cPidHandle *Handle, int Type, bool On)
{
  return false;
}

void cDevice::StartSectionHandler(void)
{
  if (!sectionHandler) {
     sectionHandler = new cSectionHandler(this);
     AttachFilter(eitFilter = new cEitFilter);
     AttachFilter(patFilter = new cPatFilter);
     AttachFilter(sdtFilter = new cSdtFilter(patFilter));
     AttachFilter(nitFilter = new cNitFilter);
     }
}

int cDevice::OpenFilter(u_short Pid, u_char Tid, u_char Mask)
{
  return -1;
}

void cDevice::AttachFilter(cFilter *Filter)
{
  if (sectionHandler)
     sectionHandler->Attach(Filter);
}

void cDevice::Detach(cFilter *Filter)
{
  if (sectionHandler)
     sectionHandler->Detach(Filter);
}

bool cDevice::ProvidesSource(int Source) const
{
  return false;
}

bool cDevice::ProvidesTransponder(const cChannel *Channel) const
{
  return false;
}

bool cDevice::ProvidesTransponderExclusively(const cChannel *Channel) const
{
  for (int i = 0; i < numDevices; i++) {
      if (device[i] && device[i] != this && device[i]->ProvidesTransponder(Channel))
         return false;
      }
  return true;
}

bool cDevice::ProvidesChannel(const cChannel *Channel, int Priority, bool *NeedsDetachReceivers) const
{
  return false;
}

bool cDevice::IsTunedToTransponder(const cChannel *Channel)
{
  return false;
}

bool cDevice::MaySwitchTransponder(void)
{
  return !Receiving(true) && !(pidHandles[ptAudio].pid || pidHandles[ptVideo].pid || pidHandles[ptDolby].pid);
}

bool cDevice::SwitchChannel(const cChannel *Channel, bool LiveView)
{
  if (LiveView)
     isyslog("switching to channel %d", Channel->Number());
  for (int i = 3; i--;) {
      switch (SetChannel(Channel, LiveView)) {
        case scrOk:           return true;
        case scrNotAvailable: Skins.Message(mtInfo, tr("Channel not available!"));
                              return false;
        case scrNoTransfer:   Skins.Message(mtError, tr("Can't start Transfer Mode!"));
                              return false;
        case scrFailed:       break; // loop will retry
        }
      esyslog("retrying");
      }
  return false;
}

bool cDevice::SwitchChannel(int Direction)
{
  bool result = false;
  Direction = sgn(Direction);
  if (Direction) {
     int n = CurrentChannel() + Direction;
     int first = n;
     cChannel *channel;
     while ((channel = Channels.GetByNumber(n, Direction)) != NULL) 
     {
           // try only channels which are currently available
        if (cStatus::MsgChannelProtected(0, channel) == false)      // PIN PATCH
           if (PrimaryDevice()->ProvidesChannel(channel, Setup.PrimaryLimit) || PrimaryDevice()->CanReplay() && GetDevice(channel, 0))
              break;
           n = channel->Number() + Direction;
           }
     if (channel) {
        int d = n - first;
        if (abs(d) == 1)
           dsyslog("skipped channel %d", first);
        else if (d)
           dsyslog("skipped channels %d..%d", first, n - sgn(d));
        if (PrimaryDevice()->SwitchChannel(channel, true))
           result = true;
        }
     else if (n != first)
        Skins.Message(mtError, tr("Channel not available!"));
     }
  return result;
}

eSetChannelResult cDevice::SetChannel(const cChannel *Channel, bool LiveView)
{
  // I hope 'LiveView = false' indicates a channel switch for recording, // PIN PATCH
  // I really don't know, but it works ...                               // PIN PATCH
  if (LiveView && cStatus::MsgChannelProtected(this, Channel) == true)   // PIN PATCH
     return scrNotAvailable;                                             // PIN PATCH
   
  if (LiveView)
     StopReplay();

  // If this card is switched to an other transponder, any receivers still
  // attached to it need to be automatically detached:
  bool NeedsDetachReceivers = false;

  // If this card can't receive this channel, we must not actually switch
  // the channel here, because that would irritate the driver when we
  // start replaying in Transfer Mode immediately after switching the channel:
  bool NeedsTransferMode = (LiveView && IsPrimaryDevice() && !ProvidesChannel(Channel, Setup.PrimaryLimit, &NeedsDetachReceivers));

  NeedsTransferMode = LiveView;

  if (LiveView && Setup.LiveBuffer)
     NeedsTransferMode = true;   

  eSetChannelResult Result = scrOk;

  // If this DVB card can't receive this channel, let's see if we can
  // use the card that actually can receive it and transfer data from there:

  if (NeedsTransferMode) {
     cDevice *CaDevice = GetDevice(Channel, 0, &NeedsDetachReceivers);
     if (CaDevice && CanReplay()) {
        cStatus::MsgChannelSwitch(this, 0); // only report status if we are actually going to switch the channel
        if (CaDevice->SetChannel(Channel, false) == scrOk) { // calling SetChannel() directly, not SwitchChannel()!
           if (NeedsDetachReceivers)
              CaDevice->DetachAllReceivers();
           if (Setup.LiveBuffer)
              cLiveBufferManager::ChannelSwitch(CaDevice,Channel);
           else
           cControl::Launch(new cTransferControl(CaDevice, Channel->Vpid(), Channel->Apids(), Channel->Dpids(), Channel->Spids()));
           }
        else
           Result = scrNoTransfer;
        }
     else
        Result = scrNotAvailable;
     }
  else {
     Channels.Lock(false);
     cStatus::MsgChannelSwitch(this, 0); // only report status if we are actually going to switch the channel
     // Stop section handling:
     if (sectionHandler) {
        sectionHandler->SetStatus(false);
        sectionHandler->SetChannel(NULL);
        }
     // Tell the ciHandler about the channel switch and add all PIDs of this
     // channel to it, for possible later decryption:
/*
     if (ciHandler) {
        ciHandler->SetSource(Channel->Source(), Channel->Transponder());
// Men at work - please stand clear! ;-)
#ifdef XXX_DO_MULTIPLE_CA_CHANNELS
        if (Channel->Ca() >= CA_ENCRYPTED_MIN) {
#endif
           ciHandler->AddPid(Channel->Sid(), Channel->Vpid(), 2);
           for (const int *Apid = Channel->Apids(); *Apid; Apid++)
               ciHandler->AddPid(Channel->Sid(), *Apid, 4);
           for (const int *Dpid = Channel->Dpids(); *Dpid; Dpid++)
               ciHandler->AddPid(Channel->Sid(), *Dpid, 0);
#ifdef XXX_DO_MULTIPLE_CA_CHANNELS
           bool CanDecrypt = ciHandler->CanDecrypt(Channel->Sid());//XXX
           dsyslog("CanDecrypt %d %d %d %s", CardIndex() + 1, CanDecrypt, Channel->Number(), Channel->Name());//XXX
           }
#endif
        }
*/
     if (NeedsDetachReceivers)
        DetachAllReceivers();
     if (SetChannelDevice(Channel, LiveView)) {
        // Start section handling:
        if (sectionHandler) {
           sectionHandler->SetChannel(Channel);
           sectionHandler->SetStatus(true);
           }
        // Start decrypting any PIDs that might have been set in SetChannelDevice():
        //(cDevice::GetDevice(0))->CiHandler()->StartDecrypting();
        }
     else
        Result = scrFailed;
     Channels.Unlock();
     }

  if (Result == scrOk) {
     if (LiveView && IsPrimaryDevice()) {
        currentChannel = Channel->Number();
        // Set the available audio tracks:
        ClrAvailableTracks();
        for (int i = 0; i < MAXAPIDS; i++)
            SetAvailableTrack(ttAudio, i, Channel->Apid(i), Channel->Alang(i));
        if (Setup.UseDolbyDigital) {
           for (int i = 0; i < MAXDPIDS; i++)
               SetAvailableTrack(ttDolby, i, Channel->Dpid(i), Channel->Dlang(i));
           }
        if (!NeedsTransferMode)
           EnsureAudioTrack(true);
        }
     cStatus::MsgChannelSwitch(this, Channel->Number()); // only report status if channel switch successfull
     if (Channel->Ca()) {// > CACONFBASE) {
       int slotOnDev = GetSlotOnDev(this);
       cCiHandler *ciHandler0 = (cDevice::GetDevice(0))->CiHandler();
       ciHandler0->SetSource(Channel->Source(), Channel->Transponder(), slotOnDev);
       ciHandler0->AddPid(Channel->Sid(), Channel->Vpid(), 2, slotOnDev);
       for (const int *Apid = Channel->Apids(); *Apid; Apid++)
         ciHandler0->AddPid(Channel->Sid(), *Apid, 4, slotOnDev);
       for (const int *Dpid = Channel->Dpids(); *Dpid; Dpid++)
         ciHandler0->AddPid(Channel->Sid(), *Dpid, 0, slotOnDev);
       ciHandler0->StartDecrypting();
     }
  }

  return Result;
}

void cDevice::ForceTransferMode(void)
{
  if (!cTransferControl::ReceiverDevice()) {
     cChannel *Channel = Channels.GetByNumber(CurrentChannel());
     if (Channel)
        SetChannelDevice(Channel, false); // this implicitly starts Transfer Mode
     }
}

bool cDevice::SetChannelDevice(const cChannel *Channel, bool LiveView)
{
  return false;
}

bool cDevice::HasLock(int TimeoutMs)
{
  return true;
}

bool cDevice::HasProgramme(void)
{
  return Replaying() || pidHandles[ptAudio].pid || pidHandles[ptVideo].pid;
}

int cDevice::GetAudioChannelDevice(void)
{
  return 0;
}

void cDevice::SetAudioChannelDevice(int AudioChannel)
{
}

void cDevice::SetVolumeDevice(int Volume)
{
}

void cDevice::SetDigitalAudioDevice(bool On)
{
}

void cDevice::SetAudioTrackDevice(eTrackType Type)
{
}

bool cDevice::ToggleMute(void)
{
  int OldVolume = volume;
  mute = !mute;
  //XXX why is it necessary to use different sequences???
  if (mute) {
     SetVolume(0, true);
     Audios.MuteAudio(mute); // Mute external audio after analog audio
     }
  else {
     Audios.MuteAudio(mute); // Enable external audio before analog audio
     SetVolume(OldVolume, true);
     }
  volume = OldVolume;
  return mute;
}

int cDevice::GetAudioChannel(void)
{
  int c = GetAudioChannelDevice();
  return (0 <= c && c <= 2) ? c : 0;
}

void cDevice::SetAudioChannel(int AudioChannel)
{
  if (0 <= AudioChannel && AudioChannel <= 2)
     SetAudioChannelDevice(AudioChannel);
}

void cDevice::SetVolume(int Volume, bool Absolute)
{
  int OldVolume = volume;
  volume = min(max(Absolute ? Volume : volume + Volume, 0), MAXVOLUME);
  SetVolumeDevice(volume);
  Absolute |= mute;
  cStatus::MsgSetVolume(Absolute ? volume : volume - OldVolume, Absolute);
  if (volume > 0) {
     mute = false;
     Audios.MuteAudio(mute);
     }
}

void cDevice::ClrAvailableTracks(bool DescriptionsOnly, bool IdsOnly)
{
  if (DescriptionsOnly) {
     for (int i = ttNone; i < ttMaxTrackTypes; i++)
         *availableTracks[i].description = 0;
     }
  else {
     if (IdsOnly) {
        for (int i = ttNone; i < ttMaxTrackTypes; i++)
            availableTracks[i].id = 0;
        }
     else
        memset(availableTracks, 0, sizeof(availableTracks));
     pre_1_3_19_PrivateStream = false;
     SetAudioChannel(0); // fall back to stereo
     currentAudioTrackMissingCount = 0;
     currentAudioTrack = ttNone;
     }
}

bool cDevice::SetAvailableTrack(eTrackType Type, int Index, uint16_t Id, const char *Language, const char *Description)
{
  eTrackType t = eTrackType(Type + Index);
  if (Type == ttAudio && IS_AUDIO_TRACK(t) ||
      Type == ttDolby && IS_DOLBY_TRACK(t)) {
     if (Language)
        strn0cpy(availableTracks[t].language, Language, sizeof(availableTracks[t].language));
     if (Description)
        strn0cpy(availableTracks[t].description, Description, sizeof(availableTracks[t].description));
     if (Id) {
        availableTracks[t].id = Id; // setting 'id' last to avoid the need for extensive locking
        int numAudioTracks = NumAudioTracks();
        if (!availableTracks[currentAudioTrack].id && numAudioTracks && currentAudioTrackMissingCount++ > numAudioTracks * 10)
           EnsureAudioTrack();
        else if (t == currentAudioTrack)
           currentAudioTrackMissingCount = 0;
        }
     return true;
     }
  else
     esyslog("ERROR: SetAvailableTrack called with invalid Type/Index (%d/%d)", Type, Index);
  return false;
}

const tTrackId *cDevice::GetTrack(eTrackType Type)
{
  return (ttNone < Type && Type < ttMaxTrackTypes) ? &availableTracks[Type] : NULL;
}

int cDevice::NumAudioTracks(void) const
{
  int n = 0;
  for (int i = ttAudioFirst; i <= ttDolbyLast; i++) {
      if (availableTracks[i].id)
         n++;
      }
  return n;
}

bool cDevice::SetCurrentAudioTrack(eTrackType Type)
{
  if (ttNone < Type && Type < ttDolbyLast) {
     cMutexLock MutexLock(&mutexCurrentAudioTrack);
     if (IS_DOLBY_TRACK(Type))
        SetDigitalAudioDevice(true);
     currentAudioTrack = Type;
     if (player)
        player->SetAudioTrack(currentAudioTrack, GetTrack(currentAudioTrack));
     else
        SetAudioTrackDevice(currentAudioTrack);
     if (IS_AUDIO_TRACK(Type))
        SetDigitalAudioDevice(false);
     return true;
     }
  return false;
}

void cDevice::EnsureAudioTrack(bool Force)
{
  if (Force || !availableTracks[currentAudioTrack].id) {
     eTrackType PreferredTrack = ttAudioFirst;
     int PreferredAudioChannel = 0;
     int LanguagePreference = -1;
     int StartCheck = Setup.CurrentDolby ? ttDolbyFirst : ttAudioFirst;
     int EndCheck = ttDolbyLast;
     for (int i = StartCheck; i <= EndCheck; i++) {
         const tTrackId *TrackId = GetTrack(eTrackType(i));
         int pos = 0;
         if (TrackId && TrackId->id && I18nIsPreferredLanguage(Setup.AudioLanguages, TrackId->language, LanguagePreference, &pos)) {
            PreferredTrack = eTrackType(i);
            PreferredAudioChannel = pos;
            }
         if (Setup.CurrentDolby && i == ttDolbyLast) {
            i = ttAudioFirst - 1;
            EndCheck = ttAudioLast;
            }
         }
     // Make sure we're set to an available audio track:
     const tTrackId *Track = GetTrack(GetCurrentAudioTrack());
     if (Force || !Track || !Track->id || PreferredTrack != GetCurrentAudioTrack()) {
        if (!Force) // only log this for automatic changes
           dsyslog("setting audio track to %d (%d)", PreferredTrack, PreferredAudioChannel);
        SetCurrentAudioTrack(PreferredTrack);
        SetAudioChannel(PreferredAudioChannel);
        }
     }
}

bool cDevice::CanReplay(void) const
{
  return HasDecoder();
}

bool cDevice::SetPlayMode(ePlayMode PlayMode)
{
  return false;
}

int64_t cDevice::GetSTC(void)
{
  return -1;
}

void cDevice::TrickSpeed(int Speed)
{
}

void cDevice::Clear(void)
{
  Audios.ClearAudio();
}

void cDevice::Play(void)
{
  Audios.MuteAudio(mute);
}

void cDevice::Freeze(void)
{
  Audios.MuteAudio(true);
}

void cDevice::Mute(void)
{
  Audios.MuteAudio(true);
}

void cDevice::StillPicture(const uchar *Data, int Length)
{
}

bool cDevice::Replaying(void) const
{
  return player != NULL;
}

bool cDevice::Transferring(void) const
{
  return dynamic_cast<cTransfer *>(player) != NULL || dynamic_cast<cLivePlayer *>(player) != NULL;
}

bool cDevice::AttachPlayer(cPlayer *Player)
{
  if (CanReplay()) {
     if (player)
        Detach(player);
     pesAssembler->Reset();
     player = Player;
     if (!Transferring())
        ClrAvailableTracks(false, true);
     SetPlayMode(player->playMode);
     player->device = this;
     player->Activate(true);
     return true;
     }
  return false;
}

void cDevice::Detach(cPlayer *Player)
{
  if (Player && player == Player) {
     player->Activate(false);
     player->device = NULL;
     player = NULL;
     SetPlayMode(pmNone);
     SetVideoDisplayFormat(eVideoDisplayFormat(Setup.VideoDisplayFormat));
     Audios.ClearAudio();
     }
}

void cDevice::StopReplay(void)
{
  if (player) {
     Detach(player);
     if (IsPrimaryDevice())
        cControl::Shutdown();
     }
}

bool cDevice::Poll(cPoller &Poller, int TimeoutMs)
{
  return false;
}

bool cDevice::Flush(int TimeoutMs)
{
  return true;
}

int cDevice::PlayVideo(const uchar *Data, int Length)
{
  return -1;
}

int cDevice::PlayAudio(const uchar *Data, int Length, uchar Id)
{
  return -1;
}

int cDevice::PlayPesPacket(const uchar *Data, int Length, bool VideoOnly)
{
  cMutexLock MutexLock(&mutexCurrentAudioTrack);
  bool FirstLoop = true;
  uchar c = Data[3];
  const uchar *Start = Data;
  const uchar *End = Start + Length;
  while (Start < End) {
        int d = End - Start;
        int w = d;
        switch (c) {
          case 0xBE:          // padding stream, needed for MPEG1
          case 0xE0 ... 0xEF: // video
               w = PlayVideo(Start, d);
               break;
          case 0xC0 ... 0xDF: // audio
               SetAvailableTrack(ttAudio, c - 0xC0, c);
               if (!VideoOnly && c == availableTracks[currentAudioTrack].id) {
                  w = PlayAudio(Start, d, c);
                  if (FirstLoop)
                     Audios.PlayAudio(Data, Length, c);
                  }
	       else if (c==0xc0)   // GA: Opportunistic speed up of AV-Sync until tracks are known
		       w = PlayAudio(Start, d, c);

               break;
          case 0xBD: { // private stream 1
               int PayloadOffset = Data[8] + 9;
               uchar SubStreamId = Data[PayloadOffset];
               uchar SubStreamType = SubStreamId & 0xF0;
               uchar SubStreamIndex = SubStreamId & 0x1F;

               // Compatibility mode for old VDR recordings, where 0xBD was only AC3:
pre_1_3_19_PrivateStreamDeteced:
               if (pre_1_3_19_PrivateStream) {
                  SubStreamId = c;
                  SubStreamType = 0x80;
                  SubStreamIndex = 0;
                  }
               switch (SubStreamType) {
                 case 0x20: // SPU
                 case 0x30: // SPU
                      break;
                 case 0x80: // AC3 & DTS
                      if (Setup.UseDolbyDigital) {
                         SetAvailableTrack(ttDolby, SubStreamIndex, SubStreamId);
                         if (!VideoOnly && SubStreamId == availableTracks[currentAudioTrack].id) {
                            w = PlayAudio(Start, d, SubStreamId);
                            if (FirstLoop)
                               Audios.PlayAudio(Data, Length, SubStreamId);
                            }
                         }
                      break;
                 case 0xA0: // LPCM
                      SetAvailableTrack(ttAudio, SubStreamIndex, SubStreamId);
                      if (!VideoOnly && SubStreamId == availableTracks[currentAudioTrack].id) {
                         w = PlayAudio(Start, d, SubStreamId);
                         if (FirstLoop)
                            Audios.PlayAudio(Data, Length, SubStreamId);
                         }
                      break;
                 default:
                      // Compatibility mode for old VDR recordings, where 0xBD was only AC3:
                      if (!pre_1_3_19_PrivateStream) {
                         dsyslog("switching to pre 1.3.19 Dolby Digital compatibility mode");
                         ClrAvailableTracks();
                         pre_1_3_19_PrivateStream = true;
                         goto pre_1_3_19_PrivateStreamDeteced;
                         }
                 }
               }
               break;
          default:
               ;//esyslog("ERROR: unexpected packet id %02X", c);
          }
        if (w > 0)
           Start += w;
        else {
           if (Start != Data)
              esyslog("ERROR: incomplete PES packet write!");
           return Start == Data ? w : Start - Data;
           }
        FirstLoop = false;
        }
  return Length;
}

int cDevice::PlayPes(const uchar *Data, int Length, bool VideoOnly)
{
  if (!Data) {
     pesAssembler->Reset();
     return 0;
     }
  int Result = 0;
  if (pesAssembler->Length()) {
     // Make sure we have a complete PES header:
     while (pesAssembler->Length() < 6 && Length > 0) {
           pesAssembler->Put(*Data++);
           Length--;
           Result++;
           }
     if (pesAssembler->Length() < 6)
        return Result; // Still no complete PES header - wait for more
     int l = pesAssembler->ExpectedLength();
     int Rest = min(l - pesAssembler->Length(), Length);
     // GA
     if (Rest<0) {  
            pesAssembler->Reset();
            return Length;
     }
     pesAssembler->Put(Data, Rest);
     Data += Rest;
     Length -= Rest;
     Result += Rest;
     if (pesAssembler->Length() < l)
        return Result; // Still no complete PES packet - wait for more
     // Now pesAssembler contains one complete PES packet.
     int w = PlayPesPacket(pesAssembler->Data(), pesAssembler->Length(), VideoOnly);
     if (w > 0)
        pesAssembler->Reset();
     return Result > 0 ? Result : w < 0 ? w : 0;
     }
  int i = 0;
#if 0
  while (i <= Length - 6) {
        if (Data[i] == 0x00 && Data[i + 1] == 0x00 && Data[i + 2] == 0x01) {
           int l = cPesAssembler::PacketSize(&Data[i]);
           if (i + l > Length) {
              // Store incomplete PES packet for later completion:
              pesAssembler->Put(Data + i, Length - i);
              return Length;
              }
           int w = PlayPesPacket(Data + i, l, VideoOnly);
           if (w > 0)
              i += l;
           else
              return i == 0 ? w : i;
           }
        else
           i++;
  }
#else
	// GA: The usual speedup, I wish gcc would be a bit more intelligent...
	int endpos= Length - 6;
	int n;
	while(i<=endpos) {
		n=FindPacketHeader(Data,i,endpos);
		if (n>=0) {
			i=n;
			int l = cPesAssembler::PacketSize(&Data[i]);
			if (i + l > Length) {
				// Store incomplete PES packet for later completion:
				pesAssembler->Put(Data + i, Length - i); 
				return Length;
			}
			int w = PlayPesPacket(Data + i, l, VideoOnly);
			if (w > 0)
				i += l;
			else
				return i == 0 ? w : i;
			continue;
		}
		else 
			break;                
	}    
#endif 

  if (i < Length)
     pesAssembler->Put(Data + i, Length - i);
  return Length;
}

int cDevice::Ca(void) const
{
  int ca = 0;
  for (int i = 0; i < MAXRECEIVERS; i++) {
      if (receiver[i] && (ca = receiver[i]->ca) != 0)
         break; // all receivers have the same ca
      }
  return ca;
}

int cDevice::Priority(void) const
{
  int priority = IsPrimaryDevice() ? Setup.PrimaryLimit - 1 : DEFAULTPRIORITY;
  for (int i = 0; i < MAXRECEIVERS; i++) {
      if (receiver[i])
         priority = max(receiver[i]->priority, priority);
      }
  return priority;
}

bool cDevice::Ready(void)
{
  return true;
}

int cDevice::ProvidesCa(const cChannel *Channel) const
{
  int Ca = Channel->Ca();
  /*
  if (Ca == CardIndex() + 1)
     return 1; // exactly _this_ card was requested
  if (Ca && Ca <= CA_DVB_MAX)
     return 0; // a specific card was requested, but not _this_ one
  */
  return !Ca; // by default every card can provide FTA
}

bool cDevice::Receiving(bool CheckAny) const
{
  for (int i = 0; i < MAXRECEIVERS; i++) {
      if (receiver[i] && (CheckAny || receiver[i]->priority >= 0)) // cReceiver with priority < 0 doesn't count
         return true;
      }
  return false;
}
#if 0
void cDevice::Action(void)
{
  if (Running() && OpenDvr()) {
     while (Running()) {
           // Read data from the DVR device:
           uchar *b = NULL;
           if (GetTSPacket(b)) {
              if (b) {
                 int Pid = (((uint16_t)b[1] & PID_MASK_HI) << 8) | b[2];
                 // Distribute the packet to all attached receivers:
                 Lock();
                 for (int i = 0; i < MAXRECEIVERS; i++) {
                     if (receiver[i] && receiver[i]->WantsPid(Pid))
                        receiver[i]->Receive(b, TS_SIZE);
                     }
                 Unlock();
                 }
              }
           else
              break;
           }
     CloseDvr();
     }
}
#else
// GA Speedup
#define TS_MAX_READ (1024)  // max 192K buffer
void cDevice::Action(void)
{
	uchar buf[TS_MAX_READ*188];
	if (Running() && OpenDvr()) {
		SetPriority(-5); // This thread is important...
		while (Running()) {                    
			int l;
			l=GetTSPackets(buf,TS_MAX_READ); // Read data from the DVR device
			if (l>0) {  
				Lock();
				int burstlen=188;
				
				// Look for bursts of the same PID
				for(int n=0;n<l;n+=burstlen) {
					uchar *b=buf+n;
					int m;
					int Pid = (((uint16_t)b[1] & PID_MASK_HI) << 8) | b[2];
					int nextPid=-1;
					burstlen=188;   
					for(m=n+188;m<l;m+=188) {
                                               nextPid=(((uint16_t)buf[m+1] & PID_MASK_HI) << 8) | buf[m+2];
                                               if (nextPid!=Pid)
                                                       break;
                                               burstlen+=188;
					}
					// Distribute the packets to all attached receivers:
					
					for (int i = 0; i < MAXRECEIVERS; i++) {
						if (receiver[i] && receiver[i]->WantsPid(Pid))
							receiver[i]->Receive(b, burstlen);
					}
					
				}
				Unlock();
			}
		}
		CloseDvr();
	}
}
#endif
bool cDevice::OpenDvr(void)
{
  return false;
}

void cDevice::CloseDvr(void)
{
}

bool cDevice::GetTSPacket(uchar *&Data)
{
  return false;
}

int cDevice::GetTSPackets(uchar *Data, int count)
{
  return 0;
}

bool cDevice::AttachReceiver(cReceiver *Receiver)
{
  if (!Receiver)
     return false;
  if (Receiver->device == this)
     return true;
// activate the following line if you need it - actually the driver should be fixed!
//#define WAIT_FOR_TUNER_LOCK
#ifdef WAIT_FOR_TUNER_LOCK
#define TUNER_LOCK_TIMEOUT 5000 // ms
  if (!HasLock(TUNER_LOCK_TIMEOUT)) {
     esyslog("ERROR: device %d has no lock, can't attach receiver!", CardIndex() + 1);
     return false;
     }
#endif
  cMutexLock MutexLock(&mutexReceiver);
  for (int i = 0; i < MAXRECEIVERS; i++) {
      if (!receiver[i]) {
         for (int n = 0; n < Receiver->numPids; n++) {
             if (!AddPid(Receiver->pids[n])) {
                for ( ; n-- > 0; )
                    DelPid(Receiver->pids[n]);
                return false;
                }
             }
         Receiver->Activate(true);
         Lock();
         Receiver->device = this;
         receiver[i] = Receiver;
         Unlock();
         if (!Running())
            Start();
         (cDevice::GetDevice(0))->CiHandler()->StartDecrypting();
         return true;
         }
      }
  esyslog("ERROR: no free receiver slot!");
  return false;
}

void cDevice::Detach(cReceiver *Receiver)
{
  if (!Receiver || Receiver->device != this)
     return;
  bool receiversLeft = false;
  cMutexLock MutexLock(&mutexReceiver);
  for (int i = 0; i < MAXRECEIVERS; i++) {
      if (receiver[i] == Receiver) {
         Receiver->Activate(false);
         Lock();
         receiver[i] = NULL;
         Receiver->device = NULL;
         Unlock();
         for (int n = 0; n < Receiver->numPids; n++)
             DelPid(Receiver->pids[n]);
         }
      else if (receiver[i])
         receiversLeft = true;
      }
  (cDevice::GetDevice(0))->CiHandler()->StartDecrypting();
//  if (!receiversLeft)
//     Cancel(3);
}

void cDevice::DetachAll(int Pid)
{
  if (Pid) {
     cMutexLock MutexLock(&mutexReceiver);
     for (int i = 0; i < MAXRECEIVERS; i++) {
         cReceiver *Receiver = receiver[i];
         if (Receiver && Receiver->WantsPid(Pid))
            Detach(Receiver);
         }
     }
}

void cDevice::DetachAllReceivers(void)
{
  cMutexLock MutexLock(&mutexReceiver);
  for (int i = 0; i < MAXRECEIVERS; i++) {
      if (receiver[i])
         Detach(receiver[i]);
      }
}
//Start by Klaus   
bool cDevice::PlayerCanHandleMenuCalls() 
{ 
   return player?(player->CanHandleMenuCalls() && player->IsAttached()):false; 
}

bool cDevice::DVDPlayerIsInMenuDomain()
{
   return player?(player->PlayerIsInMenuDomain() && player->IsAttached()):false; 
}
//End by Klaus

// --- cTSBuffer -------------------------------------------------------------

cTSBuffer::cTSBuffer(int File, int Size, int CardIndex)
{
  SetDescription("TS buffer on device %d", CardIndex);
  f = File;
  cardIndex = CardIndex;
  delivered = false;
  ringBuffer = new cRingBufferLinear(Size, TS_SIZE, true, "TS");
  ringBuffer->SetTimeouts(100, 100);
  Start();
}

cTSBuffer::~cTSBuffer()
{
  Cancel(3);
  delete ringBuffer;
}

void cTSBuffer::Action(void)
{
  if (ringBuffer) {
     bool firstRead = true;
     cPoller Poller(f);
     while (Running()) {
           if (firstRead || Poller.Poll(100)) {
              firstRead = false;
              int r = ringBuffer->Read(f);
              if (r < 0 && FATALERRNO) {
                 if (errno == EOVERFLOW)
                    esyslog("ERROR: driver buffer overflow on device %d", cardIndex);
                 else {
                    LOG_ERROR;
                    break;
                    }
                 }
              }
           }
     }
}

uchar *cTSBuffer::Get(void)
{
  int Count = 0;
  if (delivered) {
     ringBuffer->Del(TS_SIZE);
     delivered = false;
     }
  uchar *p = ringBuffer->Get(Count);
  if (p && Count >= TS_SIZE) {
     if (*p != TS_SYNC_BYTE) {
        for (int i = 1; i < Count; i++) {
            if (p[i] == TS_SYNC_BYTE) {
               Count = i;
               break;
               }
            }
        ringBuffer->Del(Count);
        esyslog("ERROR: skipped %d bytes to sync on TS packet on device %d", Count, cardIndex);
        return NULL;
        }
     delivered = true;
     return p;
     }
  return NULL;
}
