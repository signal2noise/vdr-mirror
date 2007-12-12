
/*
 * menu.c: The actual menu implementations
 *
 * See the main source file 'vdr.c' for copyright information and
 * how to reach the author.
 *
 * $Id: menu.c 1.446 2006/12/02 11:12:02 kls Exp $
 */

#include "menu.h"
#include <ctype.h>
#include <sys/ioctl.h> //TB: needed to determine CAM-state
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "channels.h"
#include "config.h"
#include "cutter.h"
#include "eitscan.h"
#include "i18n.h"
#include "interface.h"
#include "plugin.h"
#include "recording.h"
#include "remote.h"
#include "sources.h"
#include "status.h"
#include "themes.h"
#include "timers.h"
#include "transfer.h"
#include "videodir.h"
#include "diseqc.h"
#include "help.h"


#define MAXWAIT4EPGINFO   3 // seconds
#define MODETIMEOUT       3 // seconds
#define DISKSPACECHEK     5 // seconds between disk space checks in the main menu
#define NEWTIMERLIMIT   120 // seconds until the start time of a new timer created from the Schedule menu,
                            // within which it will go directly into the "Edit timer" menu to allow
                            // further parameter settings

#define MAXRECORDCONTROLS (MAXDEVICES * MAXRECEIVERS)
#define MAXINSTANTRECTIME (24 * 60 - 1) // 23:59 hours
#define MAXWAITFORCAMMENU 4 // seconds to wait for the CAM menu to open
#define MINFREEDISK       300 // minimum free disk space (in MB) required to start recording
#define NODISKSPACEDELTA  300 // seconds between "Not enough disk space to start recording!" messages

#define CHNUMWIDTH  (numdigits(Channels.MaxNumber()) + 1)

// --- cMenuEditCaItem -------------------------------------------------------

class cMenuEditCaItem : public cMenuEditIntItem {
protected:
  virtual void Set(void);
public:
  cMenuEditCaItem(const char *Name, int *Value, bool EditingBouquet);
  eOSState ProcessKey(eKeys Key);
  };

cMenuEditCaItem::cMenuEditCaItem(const char *Name, int *Value, bool EditingBouquet)
:cMenuEditIntItem(Name, Value, 0, EditingBouquet ? 3 : CA_ENCRYPTED_MAX )
{
  Set();
}

void cMenuEditCaItem::Set(void)
{
  char s[64];
  if (*value == CA_FTA)
    strcpy(s, tr("Free To Air"));
  else if (*value == 3)
    sprintf(s, "%s (%s)", (cDevice::GetDevice(0))->CiHandler()->GetCamName(2) ? "Neotion" : tr("no"), tr("internal CAM"));
  else if (*value == 2)
    sprintf(s, "%s (%s)", (cDevice::GetDevice(0))->CiHandler()->GetCamName(1) ? (cDevice::GetDevice(0))->CiHandler()->GetCamName(1) : tr("No CI at"), tr("upper slot"));
  else if (*value == 1)
    sprintf(s, "%s (%s)", (cDevice::GetDevice(0))->CiHandler()->GetCamName(0) ? (cDevice::GetDevice(0))->CiHandler()->GetCamName(0) : tr("No CI at"), tr("lower slot"));

  if (*value <= 3)
     SetValue(s);
  else if (*value >= CA_ENCRYPTED_MIN)
     SetValue(tr("encrypted"));
  else
     cMenuEditIntItem::Set();
}

eOSState cMenuEditCaItem::ProcessKey(eKeys Key)
{
  eOSState state = cMenuEditItem::ProcessKey(Key);

  if (state == osUnknown) {
     if ((NORMALKEY(Key) == kLeft || NORMALKEY(Key) == kRight) && *value >= CA_ENCRYPTED_MIN)
        *value = CA_FTA;
     else
        return cMenuEditIntItem::ProcessKey(Key);
     Set();
     state = osContinue;
     }
  return state;
}


#if 0 //moved to menu.h
// --- cMenuEditSrcItem ------------------------------------------------------

class cMenuEditSrcItem : public cMenuEditIntItem {
private:
  const cSource *source;
protected:
  virtual void Set(void);
public:
  cMenuEditSrcItem(const char *Name, int *Value);
  eOSState ProcessKey(eKeys Key);
  };
#endif

cMenuEditSrcItem::cMenuEditSrcItem(const char *Name, int *Value)
:cMenuEditIntItem(Name, Value, 0)
{
  source = Sources.Get(*Value);
  Set();
}

void cMenuEditSrcItem::Set(void)
{
  if (source) {
     char *buffer = NULL;
     asprintf(&buffer, "%s - %s", *cSource::ToString(source->Code()), source->Description());
     SetValue(buffer);
     free(buffer);
     }
  else
     cMenuEditIntItem::Set();
}

eOSState cMenuEditSrcItem::ProcessKey(eKeys Key)
{
  eOSState state = cMenuEditItem::ProcessKey(Key);

  if (state == osUnknown) {
     if (NORMALKEY(Key) == kLeft) { // TODO might want to increase the delta if repeated quickly?
        if (source && source->Prev()) {
           source = (cSource *)source->Prev();
           *value = source->Code();
           }
        }
     else if (NORMALKEY(Key) == kRight) {
        if (source) {
           if (source->Next())
              source = (cSource *)source->Next();
           }
        else
           source = Sources.First();
        if (source)
           *value = source->Code();
        }
     else
        return state; // we don't call cMenuEditIntItem::ProcessKey(Key) here since we don't accept numerical input
     Set();
     state = osContinue;
     }
  return state;
}

// --- cMenuEditSrcEItem ------------------------------------------------------
#define DBG ""

#if defined DEBUG_DISEQC
#   define DBG " DEBUG [diseqc]:  -- "
#   define DLOG(x...) dsyslog(x)
#   define DPRINT(x...) fprintf(stderr,x)
#else
# define DPRINT(x...)
# define DLOG(x...)
#endif



class cMenuEditSrcEItem : public cMenuEditIntItem {
private:
  const cSource *source;
  int *Diseqc;
  int tuner; ///???
  bool HasRotor(int Tuner);
protected:
  virtual void Set(void);
public:
  cMenuEditSrcEItem(const char *Name, int *Value, int diseqc[MAXTUNERS], int Tuner);
  eOSState ProcessKey(eKeys Key);
  };

cMenuEditSrcEItem::cMenuEditSrcEItem(const char *Name, int *Value, int diseqc[MAXTUNERS], int Tuner)
:cMenuEditIntItem(Name, Value, 0)
{
  source = Sources.Get(*Value);
  DLOG (DBG " cMenuEditSrcEItem Value: %d HasGets? %s Discr  \"%s\" T: %d ", *Value, source?"YES":"NO", source->Description(), Tuner);
  Diseqc = diseqc;
  tuner = Tuner;
  Set();
}

bool cMenuEditSrcEItem::HasRotor(int Tuner)
{
  DLOG (DBG " cMenuEditSrcEItem HasRotor tuner  %d  ", Tuner );
  if (Diseqc && Tuner!=tuner) {
     bool erg =  ((Diseqc[Tuner] & (DISEQC12 | GOTOX)) && !(Diseqc[Tuner] & ROTORLNB) && cDevice::GetDevice(Tuner-1) && cDevice::GetDevice(Tuner-1)->ProvidesSource(cSource::stSat));
     DLOG (DBG " cMenuEditSrcEItem HasRotor returns %s", erg?"true":"false" );
     return erg;
     }
  else
     return false;
}

void cMenuEditSrcEItem::Set(void)
{
  DLOG (DBG " cMenuEditSrcEItem Set() ");
  if (source) {
     DLOG (DBG " Have Source ");
     char *buffer = NULL;
     asprintf(&buffer, "%s - %s", *cSource::ToString(source->Code()), source->Description());
     SetValue(buffer);
     free(buffer);
     }
  else {
     DLOG (DBG " NO Source ???");
     switch (*value) {
        case 0: {
                  char buffer[] = "Rotor - DiSEqC1.2";
                  SetValue(buffer);
                  break;
                }
        case 1: {
                  char buffer[] = "Rotor - GotoX";
                  SetValue(buffer);
                  break;
                }
        default:{
                  char *buffer = NULL;
                  asprintf(&buffer, "%s %d", tr("Rotor - shared LNB"), *value-1);
                  SetValue(buffer);
                  free(buffer);
                  break;
                }
        }
     }
}

eOSState cMenuEditSrcEItem::ProcessKey(eKeys Key)
{
  eOSState state = cMenuEditItem::ProcessKey(Key);

  if (state == osUnknown) {
     if (NORMALKEY(Key) == kLeft) {
        if (source && source->Prev()) {
           source = (cSource *)source->Prev();
           *value = source->Code();
           }
        else {
           if (source) {
              source = NULL;
              *value = 0;
              }
            else if (!(*value)) {
              *value+=1;
              }
            else {
              int i;
              for (i=*value; i<=4 && !HasRotor(i); i++);
              if (i<=4)
                 *value=i+1;
              }
           }
        }
     else if (NORMALKEY(Key) == kRight) {
        if (source) {
           if (source->Next())
              source = (cSource *)source->Next();
           }
        else if (*value) {
           *value-=1;
           while (*value>=2 && !HasRotor(*value-1))
             *value-=1;
           }
        else
           source = Sources.First();
        if (source)
           *value = source->Code();
        }
     else
        return state;
     Set();
     state = osContinue;
     }
  return state;
}

// --- cMenuEditRShItem ------------------------------------------------------

class cMenuEditRShItem : public cMenuEditIntItem {
private:
  int *Diseqc;
  bool HasRotor(int Tuner);
public:
  cMenuEditRShItem(const char *Name, int *Value, int diseqc[MAXTUNERS]);
  eOSState ProcessKey(eKeys Key);
  };

cMenuEditRShItem::cMenuEditRShItem(const char *Name, int *Value, int diseqc[MAXTUNERS])
:cMenuEditIntItem(Name, Value, 0)
{
  Diseqc = diseqc;
  while (!HasRotor(*value) && *value>0)
    *value-=1;
  while (!HasRotor(*value) && *value<MAXTUNERS)
    *value+=1;
  Set();
}

bool cMenuEditRShItem::HasRotor(int Tuner)
{
  if (Diseqc)
     return ((Diseqc[Tuner] & (DISEQC12 | GOTOX)) && !(Diseqc[Tuner] & ROTORLNB) && cDevice::GetDevice(Tuner-1) && cDevice::GetDevice(Tuner-1)->ProvidesSource(cSource::stSat));
  else
     return false;
}

eOSState cMenuEditRShItem::ProcessKey(eKeys Key)
{
  eOSState state = cMenuEditItem::ProcessKey(Key);

  if (state == osUnknown) {
     if (NORMALKEY(Key) == kRight) {
        int i;
        for (i=*value+1; i<=4 && !HasRotor(i); i++);
        if (i<=4)
           *value=i;
        }
     else if (NORMALKEY(Key) == kLeft) {
        int i;
        for (i=*value-1; i && !HasRotor(i); i--);
        if (i)
           *value=i;
        }
     else
        return state;
     Set();
     state = osContinue;
     }
  return state;
}

// --- cMenuEditMapItem ------------------------------------------------------

class cMenuEditMapItem : public cMenuEditItem {
protected:
  int *value;
  const tChannelParameterMap *map;
  const char *zeroString;
  virtual void Set(void);
public:
  cMenuEditMapItem(const char *Name, int *Value, const tChannelParameterMap *Map, const char *ZeroString = NULL);
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuEditMapItem::cMenuEditMapItem(const char *Name, int *Value, const tChannelParameterMap *Map, const char *ZeroString)
:cMenuEditItem(Name)
{
  value = Value;
  map = Map;
  zeroString = ZeroString;
  Set();
}

void cMenuEditMapItem::Set(void)
{
  int n = MapToUser(*value, map);
  if (n == 999)
     SetValue(tr("auto"));
  else if (n == 0 && zeroString)
     SetValue(zeroString);
  else if (n >= 0) {
     char buf[16];
     snprintf(buf, sizeof(buf), "%d", n);
     SetValue(buf);
     }
  else
     SetValue("???");
}

eOSState cMenuEditMapItem::ProcessKey(eKeys Key)
{
  eOSState state = cMenuEditItem::ProcessKey(Key);

  if (state == osUnknown) {
     int newValue = *value;
     int n = DriverIndex(*value, map);
     if (NORMALKEY(Key) == kLeft) { // TODO might want to increase the delta if repeated quickly?
        if (n-- > 0)
           newValue = map[n].driverValue;
        }
     else if (NORMALKEY(Key) == kRight) {
        if (map[++n].userValue >= 0)
           newValue = map[n].driverValue;
        }
     else
        return state;
     if (newValue != *value) {
        *value = newValue;
        Set();
        }
     state = osContinue;
     }
  return state;
}

// --- cMenuEditChannel ------------------------------------------------------

class cMenuEditChannel : public cOsdMenu {
private:
  cChannel *channel;
  cChannel data;
  char name[256];
  void Setup(void);
public:
  cMenuEditChannel(cChannel *Channel, bool New = false);
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuEditChannel::cMenuEditChannel(cChannel *Channel, bool New)
:cOsdMenu(tr("Edit channel"), 16)
{
  channel = Channel;
  if (channel) {
     data = *channel;
     if (New) {
        channel = NULL;
        data.nid = 0;
        data.tid = 0;
        data.rid = 0;
        }
     Setup();
     }
}

void cMenuEditChannel::Setup(void)
{
  int current = Current();
  char type = **cSource::ToString(data.source);
#define ST(s) if (strchr(s, type))

  Clear();

    strn0cpy(name, data.name, sizeof(name));
    Add(new cMenuEditStrItem( tr("Name"),          name, sizeof(name), tr(FileNameChars)));
    Add(new cMenuEditSrcItem( tr("Source"),       &data.source));
    Add(new cMenuEditIntItem( tr("Frequency"),    &data.frequency));
    ST(" S ")  Add(new cMenuEditChrItem( tr("Polarization"), &data.polarization, "hvlr"));
    ST("CS ")  Add(new cMenuEditIntItem( tr("Srate"),        &data.srate));
    ST("C T")  Add(new cMenuEditMapItem( tr("CoderateH"),    &data.coderateH,    CoderateValues, tr("none")));
    ST(" S ")  Add(new cMenuEditMapItem( tr("CoderateH"),    &data.coderateH,    CoderateValuesS, tr("none")));
    ST("  T")  Add(new cMenuEditMapItem( tr("CoderateL"),    &data.coderateL,    CoderateValues, tr("none")));
    Add(new cMenuEditIntItem( tr("Vpid"),         &data.vpid,  0, 0x1FFF));
    Add(new cMenuEditIntItem( tr("Ppid"),         &data.ppid,  0, 0x1FFF));
    Add(new cMenuEditIntItem( tr("Apid1"),        &data.apids[0], 0, 0x1FFF));
    Add(new cMenuEditIntItem( tr("Apid2"),        &data.apids[1], 0, 0x1FFF));
    Add(new cMenuEditIntItem( tr("Tpid"),         &data.tpid,  0, 0x1FFF));
    Add(new cMenuEditCaItem(  "CI-Slot",           &data.caids[0], false));//XXX
    Add(new cMenuEditIntItem( tr("Sid"),          &data.sid, 1, 0xFFFF));
    ST("C T")  Add(new cMenuEditMapItem( tr("Modulation"),   &data.modulation,   ModulationValues, "QPSK"));
    ST(" S ")  Add(new cMenuEditMapItem( tr("Modulation"),   &data.modulation,   ModulationValuesS, "4"));
    ST("  T")  Add(new cMenuEditMapItem( tr("Bandwidth"),    &data.bandwidth,    BandwidthValues));
    ST("  T")  Add(new cMenuEditMapItem( tr("Transmission"), &data.transmission, TransmissionValues));
    ST("  T")  Add(new cMenuEditMapItem( tr("Guard"),        &data.guard,        GuardValues));
    ST("  T")  Add(new cMenuEditMapItem( tr("Hierarchy"),    &data.hierarchy,    HierarchyValues, tr("none")));
    ST(" S ")  Add(new cMenuEditMapItem( tr("Rolloff"),      &data.rolloff,      RolloffValues, "35"));
/*
  // Parameters for all types of sources:
  strn0cpy(name, data.name, sizeof(name));
  Add(new cMenuEditStrItem( tr("Name"),          name, sizeof(name), tr(FileNameChars)));
  Add(new cMenuEditSrcItem( tr("Source"),       &data.source));
  Add(new cMenuEditIntItem( tr("Frequency"),    &data.frequency));
  Add(new cMenuEditIntItem( tr("Vpid"),         &data.vpid,  0, 0x1FFF));
  Add(new cMenuEditIntItem( tr("Ppid"),         &data.ppid,  0, 0x1FFF));
  Add(new cMenuEditIntItem( tr("Apid1"),        &data.apids[0], 0, 0x1FFF));
  Add(new cMenuEditIntItem( tr("Apid2"),        &data.apids[1], 0, 0x1FFF));
  Add(new cMenuEditIntItem( tr("Dpid1"),        &data.dpids[0], 0, 0x1FFF));
  Add(new cMenuEditIntItem( tr("Dpid2"),        &data.dpids[1], 0, 0x1FFF));
  Add(new cMenuEditIntItem( tr("Tpid"),         &data.tpid,  0, 0x1FFF));
  Add(new cMenuEditCaItem(  tr("CA"),           &data.caids[0]));
  Add(new cMenuEditIntItem( tr("Sid"),          &data.sid, 1, 0xFFFF));
  Add(new cMenuEditIntItem( tr("Nid"),          &data.nid, 0));
  Add(new cMenuEditIntItem( tr("Tid"),          &data.tid, 0));
  Add(new cMenuEditIntItem( tr("Rid"),          &data.rid, 0));

  // Parameters for specific types of sources:
  ST(" S ")  Add(new cMenuEditChrItem( tr("Polarization"), &data.polarization, "hvlr"));
  ST("CS ")  Add(new cMenuEditIntItem( tr("Srate"),        &data.srate));
  ST("CST")  Add(new cMenuEditMapItem( tr("Inversion"),    &data.inversion,    InversionValues, tr("off")));
  ST("CST")  Add(new cMenuEditMapItem( tr("CoderateH"),    &data.coderateH,    CoderateValues, tr("none")));
  ST("  T")  Add(new cMenuEditMapItem( tr("CoderateL"),    &data.coderateL,    CoderateValues, tr("none")));
  ST("C T")  Add(new cMenuEditMapItem( tr("Modulation"),   &data.modulation,   ModulationValues, "QPSK"));
  ST("  T")  Add(new cMenuEditMapItem( tr("Bandwidth"),    &data.bandwidth,    BandwidthValues));
  ST("  T")  Add(new cMenuEditMapItem( tr("Transmission"), &data.transmission, TransmissionValues));
  ST("  T")  Add(new cMenuEditMapItem( tr("Guard"),        &data.guard,        GuardValues));
  ST("  T")  Add(new cMenuEditMapItem( tr("Hierarchy"),    &data.hierarchy,    HierarchyValues, tr("none")));
*/
  SetCurrent(Get(current));
  Display();
}

eOSState cMenuEditChannel::ProcessKey(eKeys Key)
{
  int oldSource = data.source;
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     if (Key == kOk) {
        if (Channels.HasUniqueChannelID(&data, channel)) {
           data.name = strcpyrealloc(data.name, name);
           if (channel) {
              *channel = data;
              isyslog("edited channel %d %s", channel->Number(), *data.ToText());
              state = osBack;
              }
           else {
              channel = new cChannel;
              *channel = data;
              Channels.Add(channel);
              Channels.ReNumber();
              isyslog("added channel %d %s", channel->Number(), *data.ToText());
              state = osUser1;
              }
           Channels.SetModified(true);
           }
        else {
           Skins.Message(mtError, tr("Channel settings are not unique!"));
           state = osContinue;
           }
        }
     }
  if (Key != kNone && (data.source & cSource::st_Mask) != (oldSource & cSource::st_Mask))
     Setup();
  return state;
}

// --- cMenuChannelItem ------------------------------------------------------

class cMenuChannelItem : public cOsdItem {
public:
  enum eChannelSortMode { csmNumber, csmName, csmProvider };
private:
  static eChannelSortMode sortMode;
  cChannel *channel;
  cSchedulesLock schedulesLock;
  const cSchedules *schedules;
  const cEvent *event;
  char szProgressPart[12];
  bool isSet;
public:
  cMenuChannelItem(cChannel *Channel);
  static void SetSortMode(eChannelSortMode SortMode) { sortMode = SortMode; }
  static void IncSortMode(void) { sortMode = eChannelSortMode((sortMode == csmProvider) ? csmNumber : sortMode + 1); }
  static eChannelSortMode SortMode(void) { return sortMode; }
  virtual int Compare(const cListObject &ListObject) const;
  virtual void Set(void);
  bool IsSet(void);
  cChannel *Channel(void) { return channel; }
  };

cMenuChannelItem::eChannelSortMode cMenuChannelItem::sortMode = csmNumber;

cMenuChannelItem::cMenuChannelItem(cChannel *Channel)
{
  channel = Channel;
  event = NULL;
  isSet = false;
  szProgressPart[0] = '\0'; /* initialize to valid string */
  if (channel->GroupSep())
     SetSelectable(false);
  //Set();
}

int cMenuChannelItem::Compare(const cListObject &ListObject) const
{
  cMenuChannelItem *p = (cMenuChannelItem *)&ListObject;
  int r = -1;
  if (sortMode == csmProvider)
     //r = strcoll(channel->Provider(), p->channel->Provider());
     r = strcasecmp(channel->Provider(), p->channel->Provider());
  if (sortMode == csmName || r == 0)
     //r = strcoll(channel->Name(), p->channel->Name());
     r = strcasecmp(channel->Name(), p->channel->Name());
  if (sortMode == csmNumber || r == 0)
     r = channel->Number() - p->channel->Number();
  return r;
}

bool cMenuChannelItem::IsSet(void)
{
	  return isSet;
}

void cMenuChannelItem::Set(void)
{
  char *buffer = NULL;
  if (!channel->GroupSep()) {
     schedules = cSchedules::Schedules(schedulesLock);
     if (schedules) {
	const cSchedule *Schedule = schedules->GetSchedule(channel->GetChannelID());
	if (Schedule) {
	   event = Schedule->GetPresentEvent();
	   if (event) {
	      char szProgress[9] = "";
	      int frac = 0;
	      frac = (int)roundf( (float)(time(NULL) - event->StartTime()) / (float)(event->Duration()) * 8.0 );
	      frac = min(8,max(0, frac));
	      for(int i = 0; i < frac; i++)
		 szProgress[i] = '|';
	      szProgress[frac]=0;
	      sprintf(szProgressPart, "%c%-8s%c\t", '[', szProgress, ']');
	   }
	}
     }
     if (sortMode == csmProvider)
        asprintf(&buffer, "%d\t%s - %s", channel->Number(), channel->Provider(), channel->Name());
     else
        asprintf(&buffer, "%d\t%-.17s\t%s    %-.20s", channel->Number(), channel->Name(), szProgressPart, event?event->Title():" ");
     }
  else
     asprintf(&buffer, "-----\t %-.17s\t------------------", channel->Name());
  event = NULL;
  isSet = true;
  SetText(buffer, false);
}

// --- cMenuChannels ---------------------------------------------------------

#define CHANNELNUMBERTIMEOUT 1500 //ms

class cMenuChannels : public cOsdMenu {
private:
  int number;
  cTimeMs numberTimer;
  void Setup(void);
  cChannel *GetChannel(int Index);
  void Propagate(void);
protected:
  eOSState Number(eKeys Key);
  eOSState Switch(void);
  eOSState Edit(void);
  eOSState New(void);
  eOSState Delete(void);
  virtual void Move(int From, int To);
public:
  cMenuChannels(void);
  ~cMenuChannels();
  virtual eOSState ProcessKey(eKeys Key);
  virtual void Display(void);
  };

cMenuChannels::cMenuChannels(void)
:cOsdMenu(tr("Channels"), CHNUMWIDTH)
{
  number = 0;
  Setup();
  Channels.IncBeingEdited();
  SetCols(5, 18, 6);
}

cMenuChannels::~cMenuChannels()
{
  Channels.DecBeingEdited();
}

void cMenuChannels::Setup(void)
{
  cChannel *currentChannel = GetChannel(Current());
  if (!currentChannel)
     currentChannel = Channels.GetByNumber(cDevice::CurrentChannel());
  cMenuChannelItem *currentItem = NULL;
  Clear();
  for (cChannel *channel = Channels.First(); channel; channel = Channels.Next(channel)) {
      if (!channel->GroupSep() || cMenuChannelItem::SortMode() == cMenuChannelItem::csmNumber && *channel->Name()) {
         cMenuChannelItem *item = new cMenuChannelItem(channel);
         Add(item);
         if (channel == currentChannel)
            currentItem = item;
         }
      }
  if (cMenuChannelItem::SortMode() != cMenuChannelItem::csmNumber)
     Sort();
  SetCurrent(currentItem);
  SetHelp(tr("Button$Edit"), tr("Button$New"), tr("Button$Delete"), tr("Button$Mark"));
  Display();
}

cChannel *cMenuChannels::GetChannel(int Index)
{
  cMenuChannelItem *p = (cMenuChannelItem *)Get(Index);
  return p ? (cChannel *)p->Channel() : NULL;
}

void cMenuChannels::Propagate(void)
{
  Channels.ReNumber();
  for (cMenuChannelItem *ci = (cMenuChannelItem *)First(); ci; ci = (cMenuChannelItem *)ci->Next())
      ci->Set();
  Display();
  Channels.SetModified(true);
}

eOSState cMenuChannels::Number(eKeys Key)
{
  if (HasSubMenu())
     return osContinue;
  if (numberTimer.TimedOut())
     number = 0;
  if (!number && Key == k0) {
     cMenuChannelItem::IncSortMode();
     Setup();
     }
  else {
     number = number * 10 + Key - k0;
     for (cMenuChannelItem *ci = (cMenuChannelItem *)First(); ci; ci = (cMenuChannelItem *)ci->Next()) {
         if (!ci->Channel()->GroupSep() && ci->Channel()->Number() == number) {
            SetCurrent(ci);
            Display();
            break;
            }
         }
     numberTimer.Set(CHANNELNUMBERTIMEOUT);
     }
  return osContinue;
}

eOSState cMenuChannels::Switch(void)
{
  if (HasSubMenu())
     return osContinue;
  cChannel *ch = GetChannel(Current());
  if (ch)
     return cDevice::PrimaryDevice()->SwitchChannel(ch, true) ? osEnd : osContinue;
  return osEnd;
}

eOSState cMenuChannels::Edit(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cChannel *ch = GetChannel(Current());
  if (ch)
     return AddSubMenu(new cMenuEditChannel(ch));
  return osContinue;
}

eOSState cMenuChannels::New(void)
{
  if (HasSubMenu())
     return osContinue;
  return AddSubMenu(new cMenuEditChannel(GetChannel(Current()), true));
}

eOSState cMenuChannels::Delete(void)
{
  if (!HasSubMenu() && Count() > 0) {
     int CurrentChannelNr = cDevice::CurrentChannel();
     cChannel *CurrentChannel = Channels.GetByNumber(CurrentChannelNr);
     int Index = Current();
     cChannel *channel = GetChannel(Current());
     int DeletedChannel = channel->Number();
     // Check if there is a timer using this channel:
     if (channel->HasTimer()) {
        Skins.Message(mtError, tr("Channel is being used by a timer!"));
        return osContinue;
        }
     if (Interface->Confirm(tr("Delete channel?"))) {
        if (CurrentChannel && channel == CurrentChannel) {
           int n = Channels.GetNextNormal(CurrentChannel->Index());
           if (n < 0)
              n = Channels.GetPrevNormal(CurrentChannel->Index());
           CurrentChannel = Channels.Get(n);
           CurrentChannelNr = 0; // triggers channel switch below
           }
        Channels.Del(channel);
        cOsdMenu::Del(Index);
        Propagate();
        isyslog("channel %d deleted", DeletedChannel);
        if (CurrentChannel && CurrentChannel->Number() != CurrentChannelNr) {
           if (!cDevice::PrimaryDevice()->Replaying() || cDevice::PrimaryDevice()->Transferring())
              Channels.SwitchTo(CurrentChannel->Number());
           else
              cDevice::SetCurrentChannel(CurrentChannel);
           }
        }
     }
  return osContinue;
}

void cMenuChannels::Move(int From, int To)
{
  int CurrentChannelNr = cDevice::CurrentChannel();
  cChannel *CurrentChannel = Channels.GetByNumber(CurrentChannelNr);
  cChannel *FromChannel = GetChannel(From);
  cChannel *ToChannel = GetChannel(To);
  if (FromChannel && ToChannel) {
     int FromNumber = FromChannel->Number();
     int ToNumber = ToChannel->Number();
     Channels.Move(FromChannel, ToChannel);
     cOsdMenu::Move(From, To);
     Propagate();
     isyslog("channel %d moved to %d", FromNumber, ToNumber);
     if (CurrentChannel && CurrentChannel->Number() != CurrentChannelNr) {
        if (!cDevice::PrimaryDevice()->Replaying() || cDevice::PrimaryDevice()->Transferring())
           Channels.SwitchTo(CurrentChannel->Number());
        else
           cDevice::SetCurrentChannel(CurrentChannel);
        }
     }
}

#define LOAD_RANGE 20

void cMenuChannels::Display(void){
   int start = Current() - LOAD_RANGE;
   int end = Current() + LOAD_RANGE;
   for(int i = start; i<end; i++){
      cMenuChannelItem *p = (cMenuChannelItem *)Get(i);
      if(p) {
        if(!p->IsSet())
          p->Set();
      }
   }
   cOsdMenu::Display();
}

eOSState cMenuChannels::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  switch (state) {
    case osUser1: {
         cChannel *channel = Channels.Last();
         if (channel) {
            Add(new cMenuChannelItem(channel), true);
            return CloseSubMenu();
            }
         }
         break;
    default:
         if (state == osUnknown) {
            switch (Key) {
              case k0 ... k9:
                            return Number(Key);
              case kOk:     return Switch();
              case kRed:    return Edit();
              case kGreen:  return New();
              case kYellow: return Delete();
              case kBlue:   if (!HasSubMenu())
                               Mark();
                            break;
              default: break;
              }
            }
    }
  return state;
}

// --- cMenuEditBouquet ------------------------------------------------------

class cMenuEditBouquet : public cOsdMenu {
private:
  cChannel *channel;
  cChannel data;
  char name[256];
  void Setup(void);
  int bouquetCaId;
public:
  cMenuEditBouquet(cChannel *Channel, bool New);
  virtual eOSState ProcessKey(eKeys Key);
};

cMenuEditBouquet::cMenuEditBouquet(cChannel *Channel, bool New)
: cOsdMenu(tr("Edit Bouquet"), 24)
{
  channel = Channel;
  if (channel)
    data = *channel;

  if (New) {
    channel = NULL;
    data.groupSep = true;
    data.nid = 0;
    data.tid = 0;
    data.rid = 0;
    }
  bouquetCaId = 0;
  Setup();
}

void cMenuEditBouquet::Setup(void)
{
  int current = Current();

  Clear();
  strn0cpy(name, data.name, sizeof(name));
  Add(new cMenuEditStrItem( tr("Name"), name, sizeof(name), tr(FileNameChars)));
  Add(new cMenuEditCaItem( tr("CI-Slot for this Bouquet"), &bouquetCaId, true));//XXX
  Add(new cOsdItem(" ", osUnknown, false), false, NULL);
  Add(new cOsdItem(" ", osUnknown, false), false, NULL);
  Add(new cOsdItem(" ", osUnknown, false), false, NULL);
  Add(new cOsdItem(tr("Note:"), osUnknown, false), false, NULL);
  Add(new cOsdItem(tr("Select CI-Slot for current Bouquet."), osUnknown, false), false, NULL);
  Add(new cOsdItem(tr(""), osUnknown, false), false, NULL);
  SetCurrent(Get(current));
  Display();
}

eOSState cMenuEditBouquet::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
    switch (Key) {
      case kOk: {
        if (Channels.HasUniqueChannelID(&data, channel)) {
          data.name = strcpyrealloc(data.name, name);
          if (channel) {
            *channel = data;
            isyslog("edited bouquet %s", *data.ToText());
            state = osBack;
            }
          else {
            channel = new cChannel;
            *channel = data;
            Channels.Add(channel);
            isyslog("added bouquet %s", *data.ToText());
            state = osUser1;
            }
	  if( bouquetCaId != 0){
            cChannel *channelE;
	    for(channelE = (cChannel*)channel->Next(); channelE && !channelE->GroupSep(); channelE = (cChannel*) channelE->Next()){
              int caids[2] = {bouquetCaId, 0};
	      if(channelE){
		channelE->ForceCaIds((const int*)&caids);
		isyslog("editing complete bouquet: setting caid of channel %s to %i", channelE->Name(), bouquetCaId);
	      }
	    }
	  }
          Channels.SetModified(true);
          }
        else {
          Skins.Message(mtError, tr("Channel settings are not unique!"));
          state = osContinue;
          }
        }
        break;
      default:
        state = osContinue;
      }
    }
  return state;
}

// --- cMenuBouquetItem ---------------------------------------------------------

class cMenuBouquetItem : public cOsdItem {
private:
  cChannel *bouquet;
  cSchedulesLock schedulesLock;
  const cSchedules *schedules;
  const cEvent *event;
  char szProgressPart[12];
public:
  cMenuBouquetItem(cChannel *Channel);
  cChannel *Bouquet() { return bouquet; };
  void Set(void);
  };

cMenuBouquetItem::cMenuBouquetItem(cChannel *channel)
{
  bouquet = channel;
  event = NULL;
  szProgressPart[0] = '\0'; /* initialize to valid string */
}

void cMenuBouquetItem::Set()
{
  char *buffer = NULL;
/*  cChannel *channel = (cChannel*) bouquet->Next();

  if (channel && !channel->GroupSep()) {
     schedules = cSchedules::Schedules(schedulesLock);
     if (schedules) {
	const cSchedule *Schedule = schedules->GetSchedule(channel->GetChannelID());
	if (Schedule) {
	   event = Schedule->GetPresentEvent();
	   if (event) {
	      char szProgress[9] = "";
	      int frac = 0;
	      frac = (int)roundf( (float)(time(NULL) - event->StartTime()) / (float)(event->Duration()) * 8.0 );
	      frac = min(8,max(0, frac));
	      for(int i = 0; i < frac; i++)
		 szProgress[i] = '|';
	      szProgress[frac]=0;
	      sprintf(szProgressPart, "%c%-8s%c", '[', szProgress, ']');
	   }
	}
        asprintf(&buffer, "%s    -    %s   %s  %-.20s", bouquet->Name(), channel->Name(), szProgressPart, event?event->Title():" ");
     }
     else
        asprintf(&buffer, "%s    -    %s", bouquet->Name(), channel->Name());
  }
  else */
     asprintf(&buffer, "%s", bouquet->Name());
//  event = NULL;
  SetText(buffer, false);
}

// --- cMenuBouquetsList ---------------------------------------------------------

//#define CHANNELNUMBERTIMEOUT 1500 //ms

class cMenuBouquetsList : public cOsdMenu {
private:
  int bouquetMarked;
  void Setup(cChannel* channel);
  void Propagate(void);
  cChannel *GetBouquet(int Index);
protected:
  eOSState Switch(void);
  eOSState ViewChannels(void);
  eOSState EditBouquet(void);
  eOSState NewBouquet(void);
  void Mark(void);
  virtual void Move(int From, int To);
public:
  cMenuBouquetsList(cChannel* channel = NULL);
  virtual eOSState ProcessKey(eKeys Key);
  virtual void Display(void);
  };

cMenuBouquetsList::cMenuBouquetsList(cChannel* channel)
:cOsdMenu("Bouquets", CHNUMWIDTH)
{
  bouquetMarked = -1;
  Setup(channel);
}

void cMenuBouquetsList::Setup(cChannel* channel)
{
  cChannel *currentChannel =  channel ? channel : Channels.GetByNumber(cDevice::CurrentChannel());
  cMenuBouquetItem *currentItem = NULL;
  while(currentChannel && !currentChannel->GroupSep()) {
    if(currentChannel->Prev())
      currentChannel= (cChannel*) currentChannel->Prev();
    else
      break;
    }
  for( cChannel *channel = (cChannel*) Channels.First(); channel; channel = (cChannel*) channel->Next()) {
    if(channel->GroupSep()) {
      cMenuBouquetItem *item = new cMenuBouquetItem(channel);
      Add(item);
      if(!currentItem && item->Bouquet() == currentChannel)
        currentItem = item;
      }
    }

  if(!currentItem)
    currentItem = (cMenuBouquetItem*) First();
  SetCurrent(currentItem);
  SetHelp(tr("Edit"), tr("Move"), tr("Delete"), tr("New"));
  Display();
}

void cMenuBouquetsList::Propagate()
{
  Channels.ReNumber();
  for (cMenuBouquetItem *ci = (cMenuBouquetItem *)First(); ci; ci = (cMenuBouquetItem *)ci->Next())
     ci->Set();
  Display();
  Channels.SetModified(true);
}

cChannel *cMenuBouquetsList::GetBouquet(int Index)
{
  if(Count() <= Index) return NULL;
  cMenuBouquetItem *p = (cMenuBouquetItem *)Get(Index);
  return p ? (cChannel *)p->Bouquet() : NULL;
}

eOSState cMenuBouquetsList::Switch()
{
  if (HasSubMenu())
     return osContinue;
  cChannel *channel = GetBouquet(Current());
  while(channel && channel->GroupSep())
    channel = (cChannel*) channel->Next();
  if (channel)
     return cDevice::PrimaryDevice()->SwitchChannel(channel, true) ? osEnd : osContinue;
  return osEnd;
}

eOSState cMenuBouquetsList::ViewChannels()
{
  if (HasSubMenu())
     return osContinue;
  cChannel *channel = GetBouquet(Current());
  if(!channel) return osEnd;
  ::Setup.CurrentChannel = channel->Index();
  return osUser5;
}

eOSState cMenuBouquetsList::EditBouquet()
{
  cChannel *channel;
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  channel = GetBouquet(Current());
  if (channel)
     return AddSubMenu(new cMenuEditBouquet(channel, false));
  return osContinue;
}

eOSState cMenuBouquetsList::NewBouquet()
{
  cChannel *channel;
  if (HasSubMenu())
     return osContinue;
  channel = GetBouquet(Current());
  return AddSubMenu(new cMenuEditBouquet(channel, true));
}

void cMenuBouquetsList::Mark(void)
{

  if (Count() && bouquetMarked < 0) {
     bouquetMarked = Current();
     SetStatus(tr("Up/Dn for new location - OK to move"));
     }
}

void cMenuBouquetsList::Move(int From, int To)
{
  int CurrentChannelNr = cDevice::CurrentChannel();
  cChannel *CurrentChannel = Channels.GetByNumber(CurrentChannelNr);
  cChannel *FromChannel = GetBouquet(From);
  cChannel *ToChannel = GetBouquet(To);
  if(To > From)
    for(cChannel *channel = (cChannel*) ToChannel->Next(); channel && !channel->GroupSep(); channel = (cChannel*) channel->Next())
      ToChannel = channel;

  if (FromChannel && ToChannel && FromChannel != ToChannel) {
     int FromIndex = FromChannel->Index();
     int ToIndex = ToChannel->Index();
     cChannel *NextFromChannel = NULL;
     while (FromChannel && (!FromChannel->GroupSep() || !NextFromChannel)) {
       NextFromChannel = (cChannel*) FromChannel->Next();
       Channels.Move(FromChannel, ToChannel);
       if(To > From)
         ToChannel = FromChannel;
       FromChannel = NextFromChannel;
       }

     if (From != To)
     {
        cOsdMenu::Move(From, To);
        Propagate();
        isyslog("bouquet from %d moved to %d", FromIndex, ToIndex);
     }
     if (CurrentChannel && CurrentChannel->Number() != CurrentChannelNr) {
        isyslog("CurrentChannelNr = %d; CurrentChannel::number = %d", CurrentChannelNr, CurrentChannel->Number());
        Channels.SwitchTo(CurrentChannel->Number());
     }
   }
}

#define LOAD_RANGE 20

void cMenuBouquetsList::Display(void)
{
  int start = Current() - LOAD_RANGE;
  int end = Current() + LOAD_RANGE;
  for(int i = start; i<end; i++){
      cMenuBouquetItem *p = (cMenuBouquetItem *)Get(i);
      if(p && p->Bouquet()) {
        p->Set();
      }
  }
  cOsdMenu::Display();
}

eOSState cMenuBouquetsList::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(NORMALKEY(Key));

  switch (state) {
    case osUser1: { // add bouquet
         cChannel *channel = Channels.Last();
         if (channel) {
            cMenuBouquetItem *item = new cMenuBouquetItem(channel);
            item->Set();
            Add(item, true);
            if(HasSubMenu())
              return CloseSubMenu();
            }
         break;
         }
    case osBack:
         if(::Setup.UseBouquetList == 1)
           return ViewChannels();
         else
           return osEnd;
    default:
         if (state == osUnknown) {
            switch (Key) {
              case kOk:    if (bouquetMarked >= 0)
                           {
                             SetStatus(NULL);
                              if (bouquetMarked != Current() )
                                 Move(bouquetMarked , Current());
                              bouquetMarked = -1;
                            }
                            else
                              return ViewChannels();
                            break;
              case kRed:     return EditBouquet();
              case kGreen:   // Move Bouquet
                              Mark();
                              break;
              case kYellow:  // Delete Bouquet
                            if (Interface->Confirm(tr("Delete Bouquet?"))) {
                              Channels.Del(GetBouquet(Current()));
                              cOsdMenu::Del(Current());
                              Propagate();
                              if(Current() < -1) SetCurrent(0);
                              state = osContinue;
                              }
                              break;
              case kBlue:    return NewBouquet();
          case kGreater: return AddSubMenu(new cMenuChannels());
              default: break;
              }
            }
    }
  return state;
}

// --- cMenuBouquets ---------------------------------------------------------

//#define CHANNELNUMBERTIMEOUT 1000 //ms

cMenuBouquets::cMenuBouquets(int view)
:cOsdMenu("", CHNUMWIDTH)
{
  edit = false;
  favourite = false;
  number = 0;
  channelMarked = -1;
  startChannel = 0;
  SetCols(5, 18, 6);
  if (view == 1)
    AddSubMenu(new cMenuBouquetsList());
  else if (view == 2) {
    favourite = true;
    SetGroup(0);
    Display();
  }
  else if (view == 3) {
    favourite = true;
    SetGroup(0);
    Display();
    AddFavourite(true);
  }
  else
    Setup();
  Options();
  Channels.IncBeingEdited();
}

cMenuBouquets::~cMenuBouquets()
{
  Channels.DecBeingEdited();
}

void cMenuBouquets::Setup(void)
{
  int Index = Current();
  if(Index < 0 && Channels.GetByNumber(cDevice::CurrentChannel())) Index = Channels.GetByNumber(cDevice::CurrentChannel())->Index();
  if(Index > -1) SetGroup(Index);
  Display();
}

void cMenuBouquets::SetGroup(int Index)
{
  bool back = false;
  if(Index < 0) Index = 0;
  cChannel *currentChannel = Channels.Get(Index);
  cChannel *firstChannel = NULL;
  if(Channels.Count() == 0) return;
  if (Index == 0)
    currentChannel = Channels.First();
  else if (!currentChannel)
    currentChannel = Channels.GetByNumber(cDevice::CurrentChannel());
  else if (Current() > 0)
    back = true;
  cMenuChannelItem *currentItem = NULL;
  if (!currentChannel->GroupSep())
    startChannel = Channels.GetPrevGroup(currentChannel->Index());
  else
    startChannel = currentChannel->Index();
  if (startChannel < 0) {
    startChannel = 0;
    firstChannel = Channels.First();
    }
  else
    firstChannel = Channels.Get(startChannel + 1);
  if (!firstChannel || firstChannel->GroupSep()) {
    Clear();
    return;
    }
  if (back == true) {
    if (startChannel == 0 && !firstChannel->GroupSep())
      currentChannel = firstChannel;
    else
      currentChannel = (cChannel*) (Channels.Get(startChannel)->Next());
    }

  isyslog("start: %d name of first channel: %s", startChannel, firstChannel->Name());
  isyslog("current name: %s", currentChannel->Name());
  Clear();
  for (cChannel *channel = firstChannel; channel && !channel->GroupSep(); channel = Channels.Next(channel)) {
      if (/*cMenuChannelItem::SortMode() == cMenuChannelItem::csmNumber &&*/ *channel->Name()) {
         cMenuChannelItem *item = new cMenuChannelItem(channel);
         Add(item);
         if (channel == currentChannel)
            currentItem = item;
         }
      }
//  if (cMenuChannelItem::SortMode() != cMenuChannelItem::csmNumber)
//     Sort();

  if(currentItem)
    SetCurrent(currentItem);
}

cChannel *cMenuBouquets::GetChannel(int Index)
{
  cMenuChannelItem *p = (cMenuChannelItem *)Get(Index);
  return p ? (cChannel *)p->Channel() : NULL;
}

void cMenuBouquets::Mark()
{

  if (Count() && channelMarked < 0) {
     channelMarked = GetChannel(Current())->Index();
     edit = false;
     Options();
     //SetStatus(tr("1-9 for new location - OK to move"));
     SetStatus(tr("Up/Dn for new location - OK to move"));
     }
}

void cMenuBouquets::Propagate(void)
{
  Channels.ReNumber();
  for (cMenuChannelItem *ci = (cMenuChannelItem *)First(); ci; ci = (cMenuChannelItem *)ci->Next())
     ci->Set();
  Display();
  Channels.SetModified(true);
}

eOSState cMenuBouquets::Number(eKeys Key)
{

  if (HasSubMenu())
     return osContinue;
  if (numberTimer.TimedOut())
     number = 0;

  number = number * 10 + Key - k0;
  for (cMenuChannelItem *ci = (cMenuChannelItem *)First(); ci; ci = (cMenuChannelItem *)ci->Next()) {
       if (!ci->Channel()->GroupSep() && ci->Channel()->Number() == number) {
         SetCurrent(ci);
         Display();
         break;
       }
  }
  numberTimer.Set(CHANNELNUMBERTIMEOUT);

  return osContinue;
}

eOSState cMenuBouquets::Switch(void)
{
  if (HasSubMenu())
     return osContinue;
  cChannel *ch = GetChannel(Current());
  if (ch)
     return cDevice::PrimaryDevice()->SwitchChannel(ch, true) ? osEnd : osContinue;
  return osEnd;
}

eOSState cMenuBouquets::NewChannel(void)
{
  if (HasSubMenu())
     return osContinue;
  cChannel *ch = GetChannel(Current());
  if (!ch || favourite) {
    ch = Channels.GetByNumber(cDevice::CurrentChannel());
    favourite = false;
  }
  return AddSubMenu(new cMenuEditChannel(ch, true));
}

eOSState cMenuBouquets::EditChannel(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cChannel *ch = GetChannel(Current());
  if (ch)
     return AddSubMenu(new cMenuEditChannel(ch, false));
  return osContinue;
}

eOSState cMenuBouquets::DeleteChannel(void)
{
  if (!HasSubMenu() && Count() > 0) {
     int Index = Current();
     cChannel *channel = GetChannel(Current());
     int DeletedChannel = channel->Number();
     // Check if there is a timer using this channel:
     for (cTimer *ti = Timers.First(); ti; ti = Timers.Next(ti)) {
         if (ti->Channel() == channel) {
            Skins.Message(mtError, tr("Channel is being used by a timer!"));
            return osContinue;
            }
         }
     if (Interface->Confirm(tr("Delete channel?"))) {
        Channels.Del(channel);
        cOsdMenu::Del(Index);
        Propagate();
        isyslog("channel %d deleted", DeletedChannel);
        }
     }
  return osContinue;
}

eOSState cMenuBouquets::ListBouquets(void)
{
  if (HasSubMenu())
    return osContinue;
  return AddSubMenu(new cMenuBouquetsList(Channels.Get(startChannel)));
}

void cMenuBouquets::Move(int From, int To)
{
  int CurrentChannelNr = cDevice::CurrentChannel();
  cChannel *CurrentChannel = Channels.GetByNumber(CurrentChannelNr);
  cChannel *FromChannel = (cChannel*) Channels.Get(From);
  cChannel *ToChannel = (cChannel*) Channels.Get(To);
  if (FromChannel && ToChannel && (From != To)) {
     int FromNumber = FromChannel->Number();
     int ToNumber = ToChannel->Number();
     Channels.Move(FromChannel, ToChannel);
     Propagate();
     SetGroup(startChannel);
     Display();
     if(ToNumber)
     isyslog("channel %d moved to %d", FromNumber, ToNumber);
     else
       isyslog("channel %d moved to %s", FromNumber, ToChannel->Name());
     if (CurrentChannel && CurrentChannel->Number() != CurrentChannelNr)
        Channels.SwitchTo(CurrentChannel->Number());
     }
}

void cMenuBouquets::GetFavourite(void)
{
  cChannel *channel = Channels.First();
  if(!channel->GroupSep() || 0 != strcmp(channel->Name(), tr("Favourites"))) {
    cChannel *newChannel = new cChannel();
    newChannel->groupSep = true;
    newChannel->nid = 0;
    newChannel->tid = 0;
    newChannel->rid = 0;
    strcpyrealloc(newChannel->name, tr("Favourites"));
    Channels.Add(newChannel);
    Channels.Move(newChannel, Channels.First());
    Channels.ReNumber();
    Channels.SetModified(true);
  }
}

void cMenuBouquets::AddFavourite(bool active)
{
  cChannel *channel = active ? Channels.GetByNumber(cDevice::CurrentChannel()) : GetChannel(Current());
  if(channel && Interface->Confirm(tr("Add to Favourites?"))) {
    cChannel *newChannel = new cChannel();
    *newChannel = *channel;
    newChannel->rid = 100;
    Channels.Add(newChannel);
    GetFavourite();
    Channels.Move(newChannel, Channels.First()->Next());
    Channels.ReNumber();
    Channels.SetModified(true);
    SetGroup(0);
    Display();
    }
}

eOSState cMenuBouquets::PrevBouquet()
{
  if (HasSubMenu())
    return osContinue;
  if(startChannel > 0) {
    SetGroup(startChannel-1);
    Display();
  }
  else {
    SetGroup(Channels.Count()-1);
    Display();
    }
  return osContinue;
}

eOSState cMenuBouquets::NextBouquet()
{
  int next;
  if (HasSubMenu())
    return osContinue;
  next = Channels.GetNextGroup(startChannel);
  if(-1 < next && next < Channels.Count()) {
      SetGroup(next);
      Display();
    }
  else {
    SetGroup(0);
    Display();
    }
  return osContinue;
}

void cMenuBouquets::Options()
{
  if(edit)
    SetHelp(tr("Edit"), tr("Move"), tr("Delete"), tr("New"));
  else
    SetHelp(tr("Bouquets"), tr("Back"), tr("Next"), tr("Options"));
}

#define LOAD_RANGE 20

void cMenuBouquets::Display(void){
  int start = Current() - LOAD_RANGE;
  int end = Current() + LOAD_RANGE;
  cChannel *channel;
  if(Count() < end) end = Count();
  if(start < 0) start = 0;
  for(int i = start; i<end; i++){
      cMenuChannelItem *p = (cMenuChannelItem *)Get(i);
      if(p) {
    if(!p->IsSet())
          p->Set();
      }
  }
  channel = Channels.Get(startChannel);
  if(channel && channel->GroupSep()) {
    if(channel->Name() || strlen(channel->Name()) > 0)
      SetTitle(channel->Name());
    else
      SetTitle("");
  }
  else
    SetTitle("");
  cOsdMenu::Display();
}

eOSState cMenuBouquets::ProcessKey(eKeys Key)
{

  eOSState state = cOsdMenu::ProcessKey(NORMALKEY(Key));

  //  isyslog("ProcessKey ChannelKey: %d", Key);

  switch (state) {
    case osUser1: { // new channel
         cChannel *channel = Channels.Last();
         if (channel) {
            int current;
            cChannel *currentChannel = GetChannel(Current());
            if(currentChannel)
              current = currentChannel->Index() + 1;
            else
              current = Channels.GetNextGroup(startChannel);
            if( -1 < current && current < Channels.Count())
              Move(channel->Index() , current);
            else
              Add(new cMenuChannelItem(channel), true);
            return CloseSubMenu();
            }
         break;
         }
    case osUser5: // view bouquet channels
         if(HasSubMenu())
           CloseSubMenu();
         SetGroup(::Setup.CurrentChannel);
         Display();
         break;
    case osBack:
         if(edit) {
           edit = false;
           Options();
           state = osContinue;
           }
         else if(::Setup.UseBouquetList == 2)
           return ListBouquets();
         else
           return osEnd;
         break;
    default:
         if (state == osUnknown) {
            switch (Key) {
             case k0 ... k9:
                  {
                     dsyslog("ProcessKey GetKey: %d extractVal %d", Key, Key-k0);
                     Number(Key);
                  }
                            Setup();
                            break;
              case kOk:    if (channelMarked >= 0)
                           {
                             int current;
                             Current() > -1 ? current = GetChannel(Current())->Index(): current = startChannel;
                             SetStatus(NULL);
                             if (channelMarked != current)
                             {
                                if(current < channelMarked && Channels.Get(current)->GroupSep()) current++;
                                    Move(channelMarked , current);
                             }
                              channelMarked = -1;
                            }
                            else
                               return Switch();
                            break;
              case kRed:   if(edit)
                               return EditChannel();
                            else
                               return ListBouquets();
              case kGreen:  if(edit) // Move
                               Mark();
                             else
                               return PrevBouquet();
                             break;
              case kYellow: if(edit)  // Delete
                               DeleteChannel();
                             else
                               return NextBouquet();
                             break;
              case kBlue:   if(edit)
                               return NewChannel();
                             else {
                               edit = true;
                               Options();
                               }
                             break;
              case k2digit:  AddFavourite(false);
	                     break;
              case kGreater: return AddSubMenu(new cMenuChannels());
    	      default: break;
              }
            }
    }
  return state;
}


// --- cMenuText -------------------------------------------------------------

cMenuText::cMenuText(const char *Title, const char *Text, eDvbFont Font)
:cOsdMenu(Title)
{
  text = NULL;
  font = Font;
  SetText(Text);
}

cMenuText::~cMenuText()
{
  free(text);
}

void cMenuText::SetText(const char *Text)
{
  free(text);
  text = Text ? strdup(Text) : NULL;
}

void cMenuText::Display(void)
{
  cOsdMenu::Display();
  DisplayMenu()->SetText(text, font == fontFix); //XXX define control character in text to choose the font???
  if (text)
     cStatus::MsgOsdTextItem(text);
}

eOSState cMenuText::ProcessKey(eKeys Key)
{
  switch (Key) {
    case kUp|k_Repeat:
    case kUp:
    case kDown|k_Repeat:
    case kDown:
    case kLeft|k_Repeat:
    case kLeft:
    case kRight|k_Repeat:
    case kRight:
                  DisplayMenu()->Scroll(NORMALKEY(Key) == kUp || NORMALKEY(Key) == kLeft, NORMALKEY(Key) == kLeft || NORMALKEY(Key) == kRight);
                  cStatus::MsgOsdTextItem(NULL, NORMALKEY(Key) == kUp);
                  return osContinue;
    default: break;
    }

  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kOk: return osBack;
       default:  state = osContinue;
       }
     }
  return state;
}

// --- cMenuHelp --------------------------------------------------------

cMenuHelp::cMenuHelp(cHelpSection *Section, const char *Title)
:cOsdMenu(Title)
{

  text = NULL;
  helpPage = NULL;
  section = Section;
  char buffer[128];
  snprintf(buffer,128, "%s - %s",tr("Help"), Title);
  SetTitle(buffer);

  if (Section)
     helpPage = Section->GetHelpByTitle(Title);

  if (helpPage)
    SetText(helpPage->Text());

}

cMenuHelp::~cMenuHelp()
{
  if (text) free(text);
}

void cMenuHelp::SetText(const char *Text)
{
  if (text) free(text);
  text = Text ? strdup(Text) : NULL;
}
void cMenuHelp::SetNextHelp()
{

  SetStatus(NULL);
  cHelpPage *h =  static_cast<cHelpPage *>(helpPage->cListObject::Next());
  if (h) // aviod malloc/free!
  {
    helpPage = h;
    const char *myTitle = helpPage->Title();
    SetText(helpPage->Text());
    char buffer[128];
    snprintf(buffer,128,"%s - %s",tr("Help"), myTitle);
    SetTitle(buffer);
    Display();
  }
  else
  {
     SetStatus(tr("Already first help item"));
  }
}

void cMenuHelp::SetPrevHelp()
{
  SetStatus(NULL);
  cHelpPage *h = static_cast<cHelpPage *>(helpPage->cListObject::Prev());
  if (h) 
  {
    helpPage = h;
    const char *myTitle = helpPage->Title();
    SetText(helpPage->Text());

    char buffer[1024];
    snprintf(buffer,1024, "%s - %s",tr("Help"), myTitle);
    SetTitle(buffer);
    Display();
  }
  else
  {
     SetStatus(tr("Already last help item"));
  }
}

void cMenuHelp::Display(void)
{
  cOsdMenu::Display();
  DisplayMenu()->SetText(text, font == fontFix); //XXX define control character in text to choose the font???
  if (text)
     cStatus::MsgOsdTextItem(text);
}

eOSState cMenuHelp::ProcessKey(eKeys Key)
{
  switch (Key) {
    case kUp|k_Repeat:
    case kUp:

                  SetNextHelp();
                  break;
    case kDown|k_Repeat:
    case kDown:
                  SetPrevHelp();
                  break;
    case kLeft|k_Repeat:
    case kLeft:
    case kRight|k_Repeat:
    case kRight:
                  DisplayMenu()->Scroll(NORMALKEY(Key) == kUp || NORMALKEY(Key) == kLeft, NORMALKEY(Key) == kLeft || NORMALKEY(Key) == kRight);
                  cStatus::MsgOsdTextItem(NULL, NORMALKEY(Key) == kUp);
                  return osContinue;
    case kInfo: return osBack; // XXX TOTEST
    default: break;
    }

  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kInfo: return osBack;
       case kOk: return osBack;
       default:  state = osContinue;
       }
     }
  return state;
}

// --- cMenuEditTimer --------------------------------------------------------

cMenuEditTimer::cMenuEditTimer(cTimer *Timer, bool New)
:cOsdMenu(tr("Edit timer"), 15)
{
  firstday = NULL;
  timer = Timer;
  addIfConfirmed = New;
  PriorityTexts[0] = tr("low");
  PriorityTexts[1] = tr("normal");
  PriorityTexts[2] = tr("high");

  if (timer) {
     data = *timer;
     if (New)
        data.SetFlags(tfActive);
     channel = data.Channel()->Number();
     tmpprio = data.priority == 10 ? 0 : data.priority == 99 ? 2 : 1;
     Add(new cMenuEditBitItem( tr("Active"),       &data.flags, tfActive));
     Add(new cMenuEditChanItem(tr("Channel"),      &channel));
     Add(new cMenuEditDateItem(tr("Day"),          &data.day, &data.weekdays));
     Add(new cMenuEditTimeItem(tr("Start"),        &data.start));
     Add(new cMenuEditTimeItem(tr("Stop"),         &data.stop));
     Add(new cMenuEditBitItem( tr("VPS"),          &data.flags, tfVps));
     Add(new cMenuEditStraItem(tr("Priority"),     &tmpprio, 3, PriorityTexts));
     Add(new cMenuEditIntItem( tr("Lifetime"),     &data.lifetime, 0, MAXLIFETIME, NULL, tr("unlimited")));
     //Add(new cMenuEditBoolItem(tr("Child protection"), &data.fskProtection));  // PIN PATCH
     // PIN PATCH
     if (cOsd::pinValid)
        Add(new cMenuEditBoolItem(tr("Child protection"),&data.fskProtection));
     else {
        char buf[64];
        snprintf(buf,64, "%s\t%s", tr("Child protection"), data.fskProtection ? tr("yes") : tr("no"));
        Add(new cOsdItem(buf));
        }

     Add(new cMenuEditStrItem( tr("File"),          data.file, sizeof(data.file), tr(FileNameChars)));
     SetFirstDayItem();
     }
  Timers.IncBeingEdited();
}

cMenuEditTimer::~cMenuEditTimer()
{
  if (timer && addIfConfirmed)
     delete timer; // apparently it wasn't confirmed
  Timers.DecBeingEdited();
}

void cMenuEditTimer::SetFirstDayItem(void)
{
  if (!firstday && !data.IsSingleEvent()) {
     Add(firstday = new cMenuEditDateItem(tr("First day"), &data.day));
     Display();
     }
  else if (firstday && data.IsSingleEvent()) {
     Del(firstday->Index());
     firstday = NULL;
     Display();
     }
}

eOSState cMenuEditTimer::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kOk:     {
                       cChannel *ch = Channels.GetByNumber(channel);
                       if (ch)
                          data.channel = ch;
                       else {
                          Skins.Message(mtError, tr("*** Invalid Channel ***"));
                          break;
                          }
                       data.priority = tmpprio == 0 ? 10 : tmpprio == 1 ? 50 : 99;
                       if (!*data.file)
                          strcpy(data.file, data.Channel()->ShortName(true));
                       if (timer) {
                          if (memcmp(timer, &data, sizeof(data)) != 0)
                             *timer = data;
                          if (addIfConfirmed)
                             Timers.Add(timer);
                          timer->SetEventFromSchedule();
                          timer->Matches();
                          Timers.SetModified();
                          isyslog("timer %s %s (%s)", *timer->ToDescr(), addIfConfirmed ? "added" : "modified", timer->HasFlags(tfActive) ? "active" : "inactive");
                          addIfConfirmed = false;
                          }
                     }
                     return osBack;
       case kRed:
       case kGreen:
       case kYellow:
       case kBlue:   return osContinue;
       default: break;
       }
     }
  if (Key != kNone)
     SetFirstDayItem();
  return state;
}

// --- cMenuTimerItem --------------------------------------------------------

class cMenuTimerItem : public cOsdItem {
private:
  cTimer *timer;
public:
  cMenuTimerItem(cTimer *Timer);
  virtual int Compare(const cListObject &ListObject) const;
  virtual void Set(void);
  cTimer *Timer(void) { return timer; }
  };

cMenuTimerItem::cMenuTimerItem(cTimer *Timer)
{
  timer = Timer;
  Set();
}

int cMenuTimerItem::Compare(const cListObject &ListObject) const
{
  return timer->Compare(*((cMenuTimerItem *)&ListObject)->timer);
}

void cMenuTimerItem::Set(void)
{
  cString day, name("");
  if (timer->WeekDays())
     day = timer->PrintDay(0, timer->WeekDays());
  else if (timer->Day() - time(NULL) < 28 * SECSINDAY) {
     day = itoa(timer->GetMDay(timer->Day()));
     name = WeekDayName(timer->Day());
     }
  else {
     struct tm tm_r;
     time_t Day = timer->Day();
     localtime_r(&Day, &tm_r);
     char buffer[16];
     strftime(buffer, sizeof(buffer), "%Y%m%d", &tm_r);
     day = buffer;
     }
  char *buffer = NULL;
  asprintf(&buffer, "%c\t%d\t%s%s%s\t%02d:%02d\t%02d:%02d\t%s",
                    !(timer->HasFlags(tfActive)) ? ' ' : timer->FirstDay() ? '!' : timer->Recording() ? '#' : '>',
                    timer->Channel()->Number(),
                    *name,
                    *name && **name ? " " : "",
                    *day,
                    timer->Start() / 100,
                    timer->Start() % 100,
                    timer->Stop() / 100,
                    timer->Stop() % 100,
                    timer->File());
  SetText(buffer, false);
}

// --- cMenuTimers -----------------------------------------------------------

class cMenuTimers : public cOsdMenu {
private:
  int helpKeys;
  eOSState Edit(void);
  eOSState New(void);
  eOSState Delete(void);
  eOSState OnOff(void);
  eOSState Info(void);
  cTimer *CurrentTimer(void);
  void SetHelpKeys(void);
public:
  cMenuTimers(void);
  virtual ~cMenuTimers();
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuTimers::cMenuTimers(void)
:cOsdMenu(tr("Timers"), 2, CHNUMWIDTH, 10, 6, 6)
{
  helpKeys = -1;
  for (cTimer *timer = Timers.First(); timer; timer = Timers.Next(timer)) {
      timer->SetEventFromSchedule(); // make sure the event is current
      Add(new cMenuTimerItem(timer));
      }
  Sort();
  SetCurrent(First());
  SetHelpKeys();
  Timers.IncBeingEdited();
}

cMenuTimers::~cMenuTimers()
{
  Timers.DecBeingEdited();
}

cTimer *cMenuTimers::CurrentTimer(void)
{
  cMenuTimerItem *item = (cMenuTimerItem *)Get(Current());
  return item ? item->Timer() : NULL;
}

void cMenuTimers::SetHelpKeys(void)
{
  int NewHelpKeys = 0;
  cTimer *timer = CurrentTimer();
  if (timer) {
     if (timer->Event())
        NewHelpKeys = 2;
     else
        NewHelpKeys = 1;
     }
  if (NewHelpKeys != helpKeys) {
     helpKeys = NewHelpKeys;
     SetHelp(helpKeys > 0 ? tr("Button$On/Off") : NULL, tr("Button$New"), helpKeys > 0 ? tr("Button$Delete") : NULL, helpKeys == 2 ? tr("Button$Info") : NULL);
     }
}

eOSState cMenuTimers::OnOff(void)
{
  if (HasSubMenu())
     return osContinue;
  cTimer *timer = CurrentTimer();
  if (timer) {
     timer->OnOff();
     timer->SetEventFromSchedule();
     RefreshCurrent();
     DisplayCurrent(true);
     if (timer->FirstDay())
        isyslog("timer %s first day set to %s", *timer->ToDescr(), *timer->PrintFirstDay());
     else
        isyslog("timer %s %sactivated", *timer->ToDescr(), timer->HasFlags(tfActive) ? "" : "de");
     Timers.SetModified();
     }
  return osContinue;
}

eOSState cMenuTimers::Edit(void)
{
    struct Epgsearch_exttimeredit_v1_0
    {
     // in
        cTimer* timer;             // pointer to the timer to edit
        bool bNew;                 // flag that indicates, if this is a new timer or an existing one
        const cEvent* event;       // pointer to the event corresponding to this timer (may be NULL)
     // out
        cOsdMenu* pTimerMenu;      // pointer to the menu of results
    };


  if (HasSubMenu() || Count() == 0)
     return osContinue;
  isyslog("editing timer %s", *CurrentTimer()->ToDescr());

  /* RC: prepared for epgsearchs timeredit. currently has the prob that timers are only
         updated when menu ist closed
  cPlugin *p = cPluginManager::GetPlugin("epgsearch");
  if (p) {
      Epgsearch_exttimeredit_v1_0 serviceData;
      serviceData.timer = CurrentTimer();
      serviceData.bNew = false;
      serviceData.event = NULL;

      p->Service("Epgsearch-exttimeredit-v1.0", &serviceData);
      if (serviceData.pTimerMenu)
          return AddSubMenu(serviceData.pTimerMenu);
      else
          Skins.Message(mtError, tr("This version of EPGSearch does not support this service!"));
  }
  */
  return AddSubMenu(new cMenuEditTimer(CurrentTimer()));
}

eOSState cMenuTimers::New(void)
{
    struct Epgsearch_exttimeredit_v1_0
    {
        // in
        cTimer* timer;             // pointer to the timer to edit
        bool bNew;                 // flag that indicates, if this is a new timer or an existing one
        const cEvent* event;       // pointer to the event corresponding to this timer (may be NULL)
        // out
        cOsdMenu* pTimerMenu;      // pointer to the menu of results
    };

  if (HasSubMenu())
     return osContinue;

  /*
  cPlugin *p = cPluginManager::GetPlugin("epgsearch");
  if (p) {
      Epgsearch_exttimeredit_v1_0 serviceData;
      serviceData.timer = new cTimer;
      serviceData.bNew = true;
      serviceData.event = NULL;

      p->Service("Epgsearch-exttimeredit-v1.0", &serviceData);
      if (serviceData.pTimerMenu)
          return AddSubMenu(serviceData.pTimerMenu);
      else
          Skins.Message(mtError, tr("This version of EPGSearch does not support this service!"));
  }
  */
  return AddSubMenu(new cMenuEditTimer(new cTimer, true));
}

eOSState cMenuTimers::Delete(void)
{
  // Check if this timer is active:
  cTimer *ti = CurrentTimer();
  if (ti) {
     if (Interface->Confirm(tr("Delete timer?"))) {
        if (ti->Recording()) {
           if (Interface->Confirm(tr("Timer still recording - really delete?"))) {
              ti->Skip();
              cRecordControls::Process(time(NULL));
              }
           else
              return osContinue;
           }
        isyslog("deleting timer %s", *ti->ToDescr());
        Timers.Del(ti);
        cOsdMenu::Del(Current());
        Timers.SetModified();
        Display();
        }
     }
  return osContinue;
}

eOSState cMenuTimers::Info(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cTimer *ti = CurrentTimer();
  if (ti && ti->Event())
     return AddSubMenu(new cMenuEvent(ti->Event()));
  return osContinue;
}

eOSState cMenuTimers::ProcessKey(eKeys Key)
{
  int TimerNumber = HasSubMenu() ? Count() : -1;
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kOk:     return Edit();
       case kRed:    state = OnOff(); break; // must go through SetHelpKeys()!
       case kGreen:  return New();
       case kYellow: state = Delete(); break;
       case kBlue:   return Info();
                     break;
       default: break;
       }
     }
  if (TimerNumber >= 0 && !HasSubMenu() && Timers.Get(TimerNumber)) {
     // a newly created timer was confirmed with Ok
     Add(new cMenuTimerItem(Timers.Get(TimerNumber)), true);
     Display();
     }
  if (Key != kNone)
     SetHelpKeys();
  return state;
}

// --- cMenuEvent ------------------------------------------------------------

cMenuEvent::cMenuEvent(const cEvent *Event, bool CanSwitch, bool Buttons)
:cOsdMenu(tr("Event"))
{
  event = Event;
  if (event) {
     cChannel *channel = Channels.GetByChannelID(event->ChannelID(), true);
     if (channel) {
        SetTitle(channel->Name());
        int TimerMatch = tmNone;
        Timers.GetMatch(event, &TimerMatch);
        if (Buttons)
           SetHelp(TimerMatch == tmFull ? tr("Button$Timer") : tr("Button$Record"), NULL, NULL, CanSwitch ? tr("Button$Switch") : NULL);
        }
     }
}

void cMenuEvent::Display(void)
{
  cOsdMenu::Display();
  DisplayMenu()->SetEvent(event);
  if (event->Description())
     cStatus::MsgOsdTextItem(event->Description());
}

eOSState cMenuEvent::ProcessKey(eKeys Key)
{
  switch (Key) {
    case kUp|k_Repeat:
    case kUp:
    case kDown|k_Repeat:
    case kDown:
    case kLeft|k_Repeat:
    case kLeft:
    case kRight|k_Repeat:
    case kRight:
                  DisplayMenu()->Scroll(NORMALKEY(Key) == kUp || NORMALKEY(Key) == kLeft, NORMALKEY(Key) == kLeft || NORMALKEY(Key) == kRight);
                  cStatus::MsgOsdTextItem(NULL, NORMALKEY(Key) == kUp);
                  return osContinue;
    default: break;
    }

  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kGreen:
       case kYellow: return osContinue;
       case kOk:     return osBack;
       default: break;
       }
     }
  return state;
}
// --- cMenuActiveEvent ------------------------------------------------------------

cMenuActiveEvent::cMenuActiveEvent()
:cOsdMenu(tr("ActiveEvent"))
{
  event = NULL;
  cChannel *channel = Channels.GetByNumber(cDevice::CurrentChannel());
  if (channel) {
     SetTitle(channel->Name());
     cSchedulesLock schedulesLock;
     const cSchedules *schedules = cSchedules::Schedules(schedulesLock);
     if (schedules) {
        const cSchedule *schedule = schedules->GetSchedule(channel);
        if (schedule) {
           event = schedule->GetPresentEvent();
           }
        }
     SetHelpButtons();
     }
}

void cMenuActiveEvent::Display(void)
{
  cOsdMenu::Display();
  if (event) {
    DisplayMenu()->SetEvent(event);
    if (event->Description())
       cStatus::MsgOsdTextItem(event->Description());
    }
}

void cMenuActiveEvent::SetHelpButtons(void)
{
  int timerMatch = tmNone;
  Timers.GetMatch(event, &timerMatch);
  if (event)
     SetHelp(timerMatch == tmFull ? tr("Button$Timer") : tr("Button$Record"), NULL, NULL, NULL);
  else
     SetHelp(NULL);

}

eOSState cMenuActiveEvent::Record(void)
{
   int timerMatch = tmNone;
   Timers.GetMatch(event, &timerMatch);

   if (timerMatch == tmFull) {
     int tm = tmNone;
     cTimer *timer = Timers.GetMatch(event, &tm);
     if (timer)
        return AddSubMenu(new cMenuEditTimer(timer));
        }
     cTimer *timer = new cTimer(event);
     cTimer *t = Timers.GetTimer(timer);
     if (t) {
        delete timer;
        timer = t;
        return AddSubMenu(new cMenuEditTimer(timer));
        }
     else {
        Timers.Add(timer);
        Timers.SetModified();
        isyslog("timer %s added (active)", *timer->ToDescr());
        if (timer->Matches(0, false, NEWTIMERLIMIT))
           return AddSubMenu(new cMenuEditTimer(timer));
        if (HasSubMenu())
           CloseSubMenu();
        }

  return osContinue;
}

eOSState cMenuActiveEvent::ProcessKey(eKeys Key)
{
  bool hadSubMenu = HasSubMenu();

  if (!hadSubMenu) {
  switch (Key) {
    case kUp|k_Repeat:
    case kUp:
    case kDown|k_Repeat:
    case kDown:
    case kLeft|k_Repeat:
    case kLeft:
    case kRight|k_Repeat:
    case kRight:
                  DisplayMenu()->Scroll(NORMALKEY(Key) == kUp || NORMALKEY(Key) == kLeft, NORMALKEY(Key) == kLeft || NORMALKEY(Key) == kRight);
                  cStatus::MsgOsdTextItem(NULL, NORMALKEY(Key) == kUp);
                  return osContinue;
    case kBack:   return osEnd;
    default: break;
    }
  }

  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kRed: return Record();
       case kGreen:
       case kYellow: return osContinue;
       case kOk:     return osEnd;
       default: break;
       }
     }

  if (!hadSubMenu)
      SetHelpButtons();
  return state;

}

// --- cMenuScheduleItem -----------------------------------------------------

class cMenuScheduleItem : public cOsdItem {
public:
  enum eScheduleSortMode { ssmAllThis, ssmThisThis, ssmThisAll, ssmAllAll }; // "which event(s) on which channel(s)"
private:
  static eScheduleSortMode sortMode;
public:
  const cEvent *event;
  const cChannel *channel;
  bool withDate;
  int timerMatch;
  cMenuScheduleItem(const cEvent *Event, cChannel *Channel = NULL, bool WithDate = false);
  static void SetSortMode(eScheduleSortMode SortMode) { sortMode = SortMode; }
  static void IncSortMode(void) { sortMode = eScheduleSortMode((sortMode == ssmAllAll) ? ssmAllThis : sortMode + 1); }
  static eScheduleSortMode SortMode(void) { return sortMode; }
  virtual int Compare(const cListObject &ListObject) const;
  bool Update(bool Force = false);
  };

cMenuScheduleItem::eScheduleSortMode cMenuScheduleItem::sortMode = ssmAllThis;

cMenuScheduleItem::cMenuScheduleItem(const cEvent *Event, cChannel *Channel, bool WithDate)
{
  event = Event;
  channel = Channel;
  withDate = WithDate;
  timerMatch = tmNone;
  Update(true);
}

int cMenuScheduleItem::Compare(const cListObject &ListObject) const
{
  cMenuScheduleItem *p = (cMenuScheduleItem *)&ListObject;
  int r = -1;
  if (sortMode != ssmAllThis)
     r = strcoll(event->Title(), p->event->Title());
  if (sortMode == ssmAllThis || r == 0)
     r = event->StartTime() - p->event->StartTime();
  return r;
}

static char *TimerMatchChars = " tT";

bool cMenuScheduleItem::Update(bool Force)
{
  bool result = false;
  int OldTimerMatch = timerMatch;
  Timers.GetMatch(event, &timerMatch);
  if (Force || timerMatch != OldTimerMatch) {
     char *buffer = NULL;
     char t = TimerMatchChars[timerMatch];
     char v = event->Vps() && (event->Vps() - event->StartTime()) ? 'V' : ' ';
     char r = event->SeenWithin(30) && event->IsRunning() ? '*' : ' ';
     if (channel && withDate)
        asprintf(&buffer, "%d\t%.*s\t%.*s\t%s\t%c%c%c\t%s", channel->Number(), 6, channel->ShortName(true), 6, *event->GetDateString(), *event->GetTimeString(), t, v, r, event->Title());
     else if (channel)
        asprintf(&buffer, "%d\t%.*s\t%s\t%c%c%c\t%s", channel->Number(), 6, channel->ShortName(true), *event->GetTimeString(), t, v, r, event->Title());
     else
        asprintf(&buffer, "%.*s\t%s\t%c%c%c\t%s", 6, *event->GetDateString(), *event->GetTimeString(), t, v, r, event->Title());
     SetText(buffer, false);
     result = true;
     }
  return result;
}

// --- cMenuWhatsOn ----------------------------------------------------------

class cMenuWhatsOn : public cOsdMenu {
private:
  bool now;
  int helpKeys;
  int timerState;
  eOSState Record(void);
  eOSState Switch(void);
  static int currentChannel;
  static const cEvent *scheduleEvent;
  bool Update(void);
  void SetHelpKeys(void);
public:
  cMenuWhatsOn(const cSchedules *Schedules, bool Now, int CurrentChannelNr);
  static int CurrentChannel(void) { return currentChannel; }
  static void SetCurrentChannel(int ChannelNr) { currentChannel = ChannelNr; }
  static const cEvent *ScheduleEvent(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

int cMenuWhatsOn::currentChannel = 0;
const cEvent *cMenuWhatsOn::scheduleEvent = NULL;

cMenuWhatsOn::cMenuWhatsOn(const cSchedules *Schedules, bool Now, int CurrentChannelNr)
:cOsdMenu(Now ? tr("What's on now?") : tr("What's on next?"), CHNUMWIDTH, 7, 6, 4)
{
  now = Now;
  helpKeys = -1;
  timerState = 0;
  Timers.Modified(timerState);
  for (cChannel *Channel = Channels.First(); Channel; Channel = Channels.Next(Channel)) {
      if (!Channel->GroupSep()) {
         const cSchedule *Schedule = Schedules->GetSchedule(Channel);
         if (Schedule) {
            const cEvent *Event = Now ? Schedule->GetPresentEvent() : Schedule->GetFollowingEvent();
            if (Event)
               Add(new cMenuScheduleItem(Event, Channel), Channel->Number() == CurrentChannelNr);
            }
         }
      }
  currentChannel = CurrentChannelNr;
  Display();
  SetHelpKeys();
}

bool cMenuWhatsOn::Update(void)
{
  bool result = false;
  if (Timers.Modified(timerState)) {
     for (cOsdItem *item = First(); item; item = Next(item)) {
         if (((cMenuScheduleItem *)item)->Update())
            result = true;
         }
     }
  return result;
}

void cMenuWhatsOn::SetHelpKeys(void)
{
  cMenuScheduleItem *item = (cMenuScheduleItem *)Get(Current());
  int NewHelpKeys = 0;
  if (item) {
     if (item->timerMatch == tmFull)
        NewHelpKeys = 2;
     else
        NewHelpKeys = 1;
     }
  if (NewHelpKeys != helpKeys) {
     const char *Red[] = { NULL, tr("Button$Record"), tr("Button$Timer") };
     SetHelp(Red[NewHelpKeys], now ? tr("Button$Next") : tr("Button$Now"), tr("Button$Schedule"), tr("Button$Switch"));
     helpKeys = NewHelpKeys;
     }
}

const cEvent *cMenuWhatsOn::ScheduleEvent(void)
{
  const cEvent *ei = scheduleEvent;
  scheduleEvent = NULL;
  return ei;
}

eOSState cMenuWhatsOn::Switch(void)
{
  cMenuScheduleItem *item = (cMenuScheduleItem *)Get(Current());
  if (item) {
     cChannel *channel = Channels.GetByChannelID(item->event->ChannelID(), true);
     if (channel && cDevice::PrimaryDevice()->SwitchChannel(channel, true))
        return osEnd;
     }
  Skins.Message(mtError, tr("Can't switch channel!"));
  return osContinue;
}

eOSState cMenuWhatsOn::Record(void)
{
  cMenuScheduleItem *item = (cMenuScheduleItem *)Get(Current());
  if (item) {
     if (item->timerMatch == tmFull) {
        int tm = tmNone;
        cTimer *timer = Timers.GetMatch(item->event, &tm);
        if (timer)
           return AddSubMenu(new cMenuEditTimer(timer));
        }
     cTimer *timer = new cTimer(item->event);
     cTimer *t = Timers.GetTimer(timer);
     if (t) {
        delete timer;
        timer = t;
        return AddSubMenu(new cMenuEditTimer(timer));
        }
     else {
        Timers.Add(timer);
        Timers.SetModified();
        isyslog("timer %s added (active)", *timer->ToDescr());
        if (timer->Matches(0, false, NEWTIMERLIMIT))
           return AddSubMenu(new cMenuEditTimer(timer));
        if (HasSubMenu())
           CloseSubMenu();
        if (Update())
           Display();
        SetHelpKeys();
        }
     }
  return osContinue;
}

eOSState cMenuWhatsOn::ProcessKey(eKeys Key)
{
  bool HadSubMenu = HasSubMenu();
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kRecord:
       case kRed:    return Record();
       case kYellow: state = osBack;
                     // continue with kGreen
       case kGreen:  {
                       cMenuScheduleItem *mi = (cMenuScheduleItem *)Get(Current());
                       if (mi) {
                          scheduleEvent = mi->event;
                          currentChannel = mi->channel->Number();
                          }
                     }
                     break;
       case kBlue:   return Switch();
       case kOk:     if (Count())
                        return AddSubMenu(new cMenuEvent(((cMenuScheduleItem *)Get(Current()))->event, true, true));
                     break;
       default:      break;
       }
     }
  else if (!HasSubMenu()) {
     if (HadSubMenu && Update())
        Display();
     if (Key != kNone)
        SetHelpKeys();
     }
  return state;
}

// --- cMenuSchedule ---------------------------------------------------------

class cMenuSchedule : public cOsdMenu {
private:
  cSchedulesLock schedulesLock;
  const cSchedules *schedules;
  bool now, next;
  int otherChannel;
  int helpKeys;
  int timerState;
  eOSState Number(void);
  eOSState Record(void);
  eOSState Switch(void);
  void PrepareScheduleAllThis(const cEvent *Event, const cChannel *Channel);
  void PrepareScheduleThisThis(const cEvent *Event, const cChannel *Channel);
  void PrepareScheduleThisAll(const cEvent *Event, const cChannel *Channel);
  void PrepareScheduleAllAll(const cEvent *Event, const cChannel *Channel);
  bool Update(void);
  void SetHelpKeys(void);
public:
  cMenuSchedule(void);
  virtual ~cMenuSchedule();
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuSchedule::cMenuSchedule(void)
:cOsdMenu("")
{
  now = next = false;
  otherChannel = 0;
  helpKeys = -1;
  timerState = 0;
  Timers.Modified(timerState);
  cMenuScheduleItem::SetSortMode(cMenuScheduleItem::ssmAllThis);
  cChannel *channel = Channels.GetByNumber(cDevice::CurrentChannel());
  if (channel) {
     cMenuWhatsOn::SetCurrentChannel(channel->Number());
     schedules = cSchedules::Schedules(schedulesLock);
     PrepareScheduleAllThis(NULL, channel);
     SetHelpKeys();
     }
}

cMenuSchedule::~cMenuSchedule()
{
  cMenuWhatsOn::ScheduleEvent(); // makes sure any posted data is cleared
}

void cMenuSchedule::PrepareScheduleAllThis(const cEvent *Event, const cChannel *Channel)
{
  Clear();
  SetCols(7, 6, 4);
  char *buffer = NULL;
  asprintf(&buffer, tr("Schedule - %s"), Channel->Name());
  SetTitle(buffer);
  free(buffer);
  if (schedules && Channel) {
     const cSchedule *Schedule = schedules->GetSchedule(Channel);
     if (Schedule) {
        const cEvent *PresentEvent = Event ? Event : Schedule->GetPresentEvent();
        time_t now = time(NULL) - Setup.EPGLinger * 60;
        for (const cEvent *ev = Schedule->Events()->First(); ev; ev = Schedule->Events()->Next(ev)) {
            if (ev->EndTime() > now || ev == PresentEvent)
               Add(new cMenuScheduleItem(ev), ev == PresentEvent);
            }
        }
     }
}

void cMenuSchedule::PrepareScheduleThisThis(const cEvent *Event, const cChannel *Channel)
{
  Clear();
  SetCols(7, 6, 4);
  char *buffer = NULL;
  asprintf(&buffer, tr("This event - %s"), Channel->Name());
  SetTitle(buffer);
  free(buffer);
  if (schedules && Channel && Event) {
     const cSchedule *Schedule = schedules->GetSchedule(Channel);
     if (Schedule) {
        time_t now = time(NULL) - Setup.EPGLinger * 60;
        for (const cEvent *ev = Schedule->Events()->First(); ev; ev = Schedule->Events()->Next(ev)) {
            if ((ev->EndTime() > now || ev == Event) && !strcmp(ev->Title(), Event->Title()))
               Add(new cMenuScheduleItem(ev), ev == Event);
            }
        }
     }
}

void cMenuSchedule::PrepareScheduleThisAll(const cEvent *Event, const cChannel *Channel)
{
  Clear();
  SetCols(CHNUMWIDTH, 7, 7, 6, 4);
  SetTitle(tr("This event - all channels"));
  if (schedules && Event) {
     for (cChannel *ch = Channels.First(); ch; ch = Channels.Next(ch)) {
         const cSchedule *Schedule = schedules->GetSchedule(ch);
         if (Schedule) {
            time_t now = time(NULL) - Setup.EPGLinger * 60;
            for (const cEvent *ev = Schedule->Events()->First(); ev; ev = Schedule->Events()->Next(ev)) {
                if ((ev->EndTime() > now || ev == Event) && !strcmp(ev->Title(), Event->Title()))
                   Add(new cMenuScheduleItem(ev, ch, true), ev == Event && ch == Channel);
                }
            }
         }
     }
}

void cMenuSchedule::PrepareScheduleAllAll(const cEvent *Event, const cChannel *Channel)
{
  Clear();
  SetCols(CHNUMWIDTH, 7, 7, 6, 4);
  SetTitle(tr("All events - all channels"));
  if (schedules) {
     for (cChannel *ch = Channels.First(); ch; ch = Channels.Next(ch)) {
         const cSchedule *Schedule = schedules->GetSchedule(ch);
         if (Schedule) {
            time_t now = time(NULL) - Setup.EPGLinger * 60;
            for (const cEvent *ev = Schedule->Events()->First(); ev; ev = Schedule->Events()->Next(ev)) {
                if (ev->EndTime() > now || ev == Event)
                   Add(new cMenuScheduleItem(ev, ch, true), ev == Event && ch == Channel);
                }
            }
         }
     }
}

bool cMenuSchedule::Update(void)
{
  bool result = false;
  if (Timers.Modified(timerState)) {
     for (cOsdItem *item = First(); item; item = Next(item)) {
         if (((cMenuScheduleItem *)item)->Update())
            result = true;
         }
     }
  return result;
}

void cMenuSchedule::SetHelpKeys(void)
{
  cMenuScheduleItem *item = (cMenuScheduleItem *)Get(Current());
  int NewHelpKeys = 0;
  if (item) {
     if (item->timerMatch == tmFull)
        NewHelpKeys = 2;
     else
        NewHelpKeys = 1;
     }
  if (NewHelpKeys != helpKeys) {
     const char *Red[] = { NULL, tr("Button$Record"), tr("Button$Timer") };
     SetHelp(Red[NewHelpKeys], tr("Button$Now"), tr("Button$Next"));
     helpKeys = NewHelpKeys;
     }
}

eOSState cMenuSchedule::Number(void)
{
  cMenuScheduleItem::IncSortMode();
  cMenuScheduleItem *CurrentItem = (cMenuScheduleItem *)Get(Current());
  const cChannel *Channel = NULL;
  const cEvent *Event = NULL;
  if (CurrentItem) {
     Event = CurrentItem->event;
     Channel = Channels.GetByChannelID(Event->ChannelID(), true);
     }
  else
     Channel = Channels.GetByNumber(cDevice::CurrentChannel());
  switch (cMenuScheduleItem::SortMode()) {
    case cMenuScheduleItem::ssmAllThis:  PrepareScheduleAllThis(Event, Channel); break;
    case cMenuScheduleItem::ssmThisThis: PrepareScheduleThisThis(Event, Channel); break;
    case cMenuScheduleItem::ssmThisAll:  PrepareScheduleThisAll(Event, Channel); break;
    case cMenuScheduleItem::ssmAllAll:   PrepareScheduleAllAll(Event, Channel); break;
    }
  CurrentItem = (cMenuScheduleItem *)Get(Current());
  Sort();
  SetCurrent(CurrentItem);
  Display();
  return osContinue;
}

eOSState cMenuSchedule::Record(void)
{
  cMenuScheduleItem *item = (cMenuScheduleItem *)Get(Current());
  if (item) {
     if (item->timerMatch == tmFull) {
        int tm = tmNone;
        cTimer *timer = Timers.GetMatch(item->event, &tm);
        if (timer)
           return AddSubMenu(new cMenuEditTimer(timer));
        }
     cTimer *timer = new cTimer(item->event);
     cTimer *t = Timers.GetTimer(timer);
     if (t) {
        delete timer;
        timer = t;
        return AddSubMenu(new cMenuEditTimer(timer));
        }
     else {
        Timers.Add(timer);
        Timers.SetModified();
        isyslog("timer %s added (active)", *timer->ToDescr());
        if (timer->Matches(0, false, NEWTIMERLIMIT))
           return AddSubMenu(new cMenuEditTimer(timer));
        if (HasSubMenu())
           CloseSubMenu();
        if (Update())
           Display();
        SetHelpKeys();
        }
     }
  return osContinue;
}

eOSState cMenuSchedule::Switch(void)
{
  if (otherChannel) {
     if (Channels.SwitchTo(otherChannel))
        return osEnd;
     }
  Skins.Message(mtError, tr("Can't switch channel!"));
  return osContinue;
}

eOSState cMenuSchedule::ProcessKey(eKeys Key)
{
  bool HadSubMenu = HasSubMenu();
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case k0:      return Number();
       case kRecord:
       case kRed:    return Record();
       case kGreen:  if (schedules) {
                        if (!now && !next) {
                           int ChannelNr = 0;
                           if (Count()) {
                              cChannel *channel = Channels.GetByChannelID(((cMenuScheduleItem *)Get(Current()))->event->ChannelID(), true);
                              if (channel)
                                 ChannelNr = channel->Number();
                              }
                           now = true;
                           return AddSubMenu(new cMenuWhatsOn(schedules, true, ChannelNr));
                           }
                        now = !now;
                        next = !next;
                        return AddSubMenu(new cMenuWhatsOn(schedules, now, cMenuWhatsOn::CurrentChannel()));
                        }
       case kYellow: if (schedules)
                        return AddSubMenu(new cMenuWhatsOn(schedules, false, cMenuWhatsOn::CurrentChannel()));
                     break;
       case kBlue:   if (Count() && otherChannel)
                        return Switch();
                     break;
       case kOk:     if (Count())
                        return AddSubMenu(new cMenuEvent(((cMenuScheduleItem *)Get(Current()))->event, otherChannel, true));
                     break;
       default:      break;
       }
     }
  else if (!HasSubMenu()) {
     now = next = false;
     const cEvent *ei = cMenuWhatsOn::ScheduleEvent();
     if (ei) {
        cChannel *channel = Channels.GetByChannelID(ei->ChannelID(), true);
        if (channel) {
           cMenuScheduleItem::SetSortMode(cMenuScheduleItem::ssmAllThis);
           PrepareScheduleAllThis(NULL, channel);
           if (channel->Number() != cDevice::CurrentChannel()) {
              otherChannel = channel->Number();
              SetHelp(Count() ? tr("Button$Record") : NULL, tr("Button$Now"), tr("Button$Next"), tr("Button$Switch"));
              }
           Display();
           }
        }
     else if (HadSubMenu && Update())
        Display();
     if (Key != kNone)
        SetHelpKeys();
     }
  return state;
}

// --- cMenuCommands ---------------------------------------------------------

class cMenuCommands : public cOsdMenu {
private:
  cCommands *commands;
  char *parameters;
  eOSState Execute(void);
public:
  cMenuCommands(const char *Title, cCommands *Commands, const char *Parameters = NULL);
  virtual ~cMenuCommands();
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuCommands::cMenuCommands(const char *Title, cCommands *Commands, const char *Parameters)
:cOsdMenu(Title)
{
  SetHasHotkeys();
  commands = Commands;
  parameters = Parameters ? strdup(Parameters) : NULL;
  for (cCommand *command = commands->First(); command; command = commands->Next(command))
      Add(new cOsdItem(hk(command->Title())));
}

cMenuCommands::~cMenuCommands()
{
  free(parameters);
}

eOSState cMenuCommands::Execute(void)
{
  cCommand *command = commands->Get(Current());
  if (command) {
     char *buffer = NULL;
     bool confirmed = true;
     if (command->Confirm()) {
        asprintf(&buffer, "%s?", command->Title());
        confirmed = Interface->Confirm(buffer);
        free(buffer);
        }
     if (confirmed) {
        asprintf(&buffer, "%s...", command->Title());
        Skins.Message(mtStatus, buffer);
        free(buffer);
        const char *Result = command->Execute(parameters);
        Skins.Message(mtStatus, NULL);
        if (Result)
           return AddSubMenu(new cMenuText(command->Title(), Result, fontFix));
        return osEnd;
        }
     }
  return osContinue;
}

eOSState cMenuCommands::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kRed:
       case kGreen:
       case kYellow:
       case kBlue:   return osContinue;
       case kOk:     return Execute();
       default:      break;
       }
     }
  return state;
}

// --- cMenuCam --------------------------------------------------------------

cMenuCam::cMenuCam(cCiMenu *CiMenu)
:cOsdMenu("")
{
  dsyslog("CAM: Menu ------------------");
  ciMenu = CiMenu;
  selected = false;
  offset = 0;
  if (ciMenu->Selectable())
     SetHasHotkeys();
  SetTitle(*ciMenu->TitleText() ? ciMenu->TitleText() : "CAM");
  dsyslog("CAM: '%s'", ciMenu->TitleText());
  if (*ciMenu->SubTitleText()) {
     dsyslog("CAM: '%s'", ciMenu->SubTitleText());
     AddMultiLineItem(ciMenu->SubTitleText());
     offset = Count();
     }
  for (int i = 0; i < ciMenu->NumEntries(); i++) {
      Add(new cOsdItem(hk(ciMenu->Entry(i)), osUnknown, ciMenu->Selectable()));
      dsyslog("CAM: '%s'", ciMenu->Entry(i));
      }
  if (*ciMenu->BottomText()) {
     AddMultiLineItem(ciMenu->BottomText());
     dsyslog("CAM: '%s'", ciMenu->BottomText());
     }
  Display();
}

cMenuCam::~cMenuCam()
{
  if (!selected)
     ciMenu->Abort();
  delete ciMenu;
}

void cMenuCam::AddMultiLineItem(const char *s)
{
  while (s && *s) {
        const char *p = strchr(s, '\n');
        int l = p ? p - s : strlen(s);
        cOsdItem *item = new cOsdItem;
        item->SetSelectable(false);
        item->SetText(strndup(s, l), false);
        Add(item);
        s = p ? p + 1 : p;
        }
}

eOSState cMenuCam::Select(void)
{
  if (ciMenu->Selectable()) {
     ciMenu->Select(Current() - offset);
     dsyslog("CAM: select %d", Current() - offset);
     }
  else
     ciMenu->Cancel();
  selected = true;
  return osEnd;
}

eOSState cMenuCam::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kOk:     return Select();
       default: break;
       }
     }
  else if (state == osBack) {
     ciMenu->Cancel();
     selected = true;
     return osEnd;
     }
  if (ciMenu->HasUpdate()) {
     selected = true;
     return osEnd;
     }
  return state;
}

// --- cMenuCamEnquiry -------------------------------------------------------

cMenuCamEnquiry::cMenuCamEnquiry(cCiEnquiry *CiEnquiry)
:cOsdMenu("", 1)
{
  ciEnquiry = CiEnquiry;
  int Length = ciEnquiry->ExpectedLength();
  input = MALLOC(char, Length + 1);
  *input = 0;
  replied = false;
  SetTitle("CAM");
  Add(new cOsdItem(ciEnquiry->Text(), osUnknown, false));
  Add(new cOsdItem("", osUnknown, false));
  Add(new cMenuEditNumItem("", input, Length, ciEnquiry->Blind()));
  Display();
}

cMenuCamEnquiry::~cMenuCamEnquiry()
{
  if (!replied)
     ciEnquiry->Abort();
  free(input);
  delete ciEnquiry;
}

eOSState cMenuCamEnquiry::Reply(void)
{
  if (ciEnquiry->ExpectedLength() < 0xFF && int(strlen(input)) != ciEnquiry->ExpectedLength()) {
     char buffer[64];
     snprintf(buffer, sizeof(buffer), tr("Please enter %d digits!"), ciEnquiry->ExpectedLength());
     Skins.Message(mtError, buffer);
     return osContinue;
     }
  ciEnquiry->Reply(input);
  replied = true;
  return osEnd;
}

eOSState cMenuCamEnquiry::ProcessKey(eKeys Key)
{
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kOk:     return Reply();
       default: break;
       }
     }
  else if (state == osBack) {
     ciEnquiry->Cancel();
     replied = true;
     return osEnd;
     }
  return state;
}

// --- CamControl ------------------------------------------------------------

cOsdObject *CamControl(void)
{
  for (int d = 0; d < cDevice::NumDevices(); d++) {
      cDevice *Device = cDevice::GetDevice(d);
      if (Device) {
         cCiHandler *CiHandler = Device->CiHandler();
         if (CiHandler && CiHandler->HasUserIO()) {
            cCiMenu *CiMenu = CiHandler->GetMenu();
            if (CiMenu)
               return new cMenuCam(CiMenu);
            else {
               cCiEnquiry *CiEnquiry = CiHandler->GetEnquiry();
               if (CiEnquiry)
                  return new cMenuCamEnquiry(CiEnquiry);
               }
            }
         }
      }
  return NULL;
}

// --- cMenuRecording --------------------------------------------------------

class cMenuRecording : public cOsdMenu {
private:
  const cRecording *recording;
  bool withButtons;
public:
  cMenuRecording(const cRecording *Recording, bool WithButtons = false);
  virtual void Display(void);
  virtual eOSState ProcessKey(eKeys Key);
};

cMenuRecording::cMenuRecording(const cRecording *Recording, bool WithButtons)
:cOsdMenu(tr("Recording info"))
{
  recording = Recording;
  withButtons = WithButtons;
  if (withButtons)
     SetHelp(tr("Button$Play"), tr("Button$Rewind"));
}

void cMenuRecording::Display(void)
{
  cOsdMenu::Display();
  DisplayMenu()->SetRecording(recording);
  if (recording->Info()->Description())
     cStatus::MsgOsdTextItem(recording->Info()->Description());
}

eOSState cMenuRecording::ProcessKey(eKeys Key)
{
  switch (Key) {
    case kUp|k_Repeat:
    case kUp:
    case kDown|k_Repeat:
    case kDown:
    case kLeft|k_Repeat:
    case kLeft:
    case kRight|k_Repeat:
    case kRight:
                  DisplayMenu()->Scroll(NORMALKEY(Key) == kUp || NORMALKEY(Key) == kLeft, NORMALKEY(Key) == kLeft || NORMALKEY(Key) == kRight);
                  cStatus::MsgOsdTextItem(NULL, NORMALKEY(Key) == kUp);
                  return osContinue;
    default: break;
    }

  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kRed:    if (withButtons)
                        Key = kOk; // will play the recording, even if recording commands are defined
       case kGreen:  if (!withButtons)
                        break;
                     cRemote::Put(Key, true);
                     // continue with osBack to close the info menu and process the key
       case kOk:     return osBack;
       default: break;
       }
     }
  return state;
}

// --- cMenuRecordingItem ----------------------------------------------------

class cMenuRecordingItem : public cOsdItem {
private:
  char *fileName;
  char *name;
  int totalEntries, newEntries;
public:
  cMenuRecordingItem(cRecording *Recording, int Level);
  ~cMenuRecordingItem();
  void IncrementCounter(bool New);
  const char *Name(void) { return name; }
  const char *FileName(void) { return fileName; }
  bool IsDirectory(void) { return name != NULL; }
  };

cMenuRecordingItem::cMenuRecordingItem(cRecording *Recording, int Level)
{
  fileName = strdup(Recording->FileName());
  name = NULL;
  totalEntries = newEntries = 0;
  SetText(Recording->Title('\t', true, Level));
  if (*Text() == '\t')
     name = strdup(Text() + 2); // 'Text() + 2' to skip the two '\t'
}

cMenuRecordingItem::~cMenuRecordingItem()
{
  free(fileName);
  free(name);
}

void cMenuRecordingItem::IncrementCounter(bool New)
{
  totalEntries++;
  if (New)
     newEntries++;
  char *buffer = NULL;
  asprintf(&buffer, "%d\t%d\t%s", totalEntries, newEntries, name);
  SetText(buffer, false);
}

// --- cMenuRecordings -------------------------------------------------------

cMenuRecordings::cMenuRecordings(const char *Base, int Level, bool OpenSubMenus)
:cOsdMenu(Base ? Base : tr("Recordings"), 9, 7)
{
  base = Base ? strdup(Base) : NULL;
  level = Setup.RecordingDirs ? Level : -1;
  Recordings.StateChanged(recordingsState); // just to get the current state
  helpKeys = -1;
  Display(); // this keeps the higher level menus from showing up briefly when pressing 'Back' during replay
  Set();
  if (Current() < 0)
     SetCurrent(First());
  else if (OpenSubMenus && cReplayControl::LastReplayed() && Open(true))
     return;
  Display();
  SetHelpKeys();
}

cMenuRecordings::~cMenuRecordings()
{
  helpKeys = -1;
  free(base);
}

void cMenuRecordings::SetHelpKeys(void)
{
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  int NewHelpKeys = 0;
  if (ri) {
     if (ri->IsDirectory())
        NewHelpKeys = 1;
     else {
        NewHelpKeys = 2;
        cRecording *recording = GetRecording(ri);
        if (recording && recording->Info()->Title())
           NewHelpKeys = 3;
        }
     }
  if (NewHelpKeys != helpKeys) {
     switch (NewHelpKeys) {
       case 0: SetHelp(NULL); break;
       case 1: SetHelp(tr("Button$Open")); break;
       case 2:
       case 3: SetHelp(RecordingCommands.Count() ? tr("Commands") : tr("Button$Play"), tr("Button$Rewind"), tr("Button$Delete"), NewHelpKeys == 3 ? tr("Button$Info") : NULL);
       }
     helpKeys = NewHelpKeys;
     }
}

void cMenuRecordings::Set(bool Refresh)
{
  const char *CurrentRecording = cReplayControl::LastReplayed();
  cMenuRecordingItem *LastItem = NULL;
  char *LastItemText = NULL;
  cThreadLock RecordingsLock(&Recordings);
  if (Refresh) {
     cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
     if (ri) {
        cRecording *Recording = Recordings.GetByName(ri->FileName());
        if (Recording)
           CurrentRecording = Recording->FileName();
        }
     }
  Clear();
  Recordings.Sort();
  for (cRecording *recording = Recordings.First(); recording; recording = Recordings.Next(recording)) {
      if (!base || (strstr(recording->Name(), base) == recording->Name() && recording->Name()[strlen(base)] == '~')) {
         cMenuRecordingItem *Item = new cMenuRecordingItem(recording, level);
         if (*Item->Text() && (!LastItem || strcmp(Item->Text(), LastItemText) != 0)) {
            Add(Item);
            LastItem = Item;
            free(LastItemText);
            LastItemText = strdup(LastItem->Text()); // must use a copy because of the counters!
            }
         else
            delete Item;
         if (LastItem) {
            if (CurrentRecording && strcmp(CurrentRecording, recording->FileName()) == 0)
               SetCurrent(LastItem);
            if (LastItem->IsDirectory())
               LastItem->IncrementCounter(recording->IsNew());
            }
         }
      }
  free(LastItemText);
  if (Refresh)
     Display();
}

cRecording *cMenuRecordings::GetRecording(cMenuRecordingItem *Item)
{
  cRecording *recording = Recordings.GetByName(Item->FileName());
  if (!recording)
     Skins.Message(mtError, tr("Error while accessing recording!"));
  return recording;
}

bool cMenuRecordings::Open(bool OpenSubMenus)
{
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && ri->IsDirectory()) {
     const char *t = ri->Name();
     char *buffer = NULL;
     if (base) {
        asprintf(&buffer, "%s~%s", base, t);
        t = buffer;
        }
     AddSubMenu(new cMenuRecordings(t, level + 1, OpenSubMenus));
     free(buffer);
     return true;
     }
  return false;
}

eOSState cMenuRecordings::Play(void)
{
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri) {
     if (cStatus::MsgReplayProtected(GetRecording(ri), ri->Name(), base,
                                     ri->IsDirectory()) == true)    // PIN PATCH
        return osContinue;                                          // PIN PATCH
     if (ri->IsDirectory())
        Open();
     else {
        cRecording *recording = GetRecording(ri);
        if (recording) {
           cReplayControl::SetRecording(recording->FileName(), recording->Title());
           return osReplay;
           }
        }
     }
  return osContinue;
}

eOSState cMenuRecordings::Rewind(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && !ri->IsDirectory()) {
     cDevice::PrimaryDevice()->StopReplay(); // must do this first to be able to rewind the currently replayed recording
     cResumeFile ResumeFile(ri->FileName());
     ResumeFile.Delete();
     return Play();
     }
  return osContinue;
}

eOSState cMenuRecordings::Delete(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && !ri->IsDirectory()) {
     if (Interface->Confirm(tr("Delete recording?"))) {
        cRecordControl *rc = cRecordControls::GetRecordControl(ri->FileName());
        if (rc) {
           if (Interface->Confirm(tr("Timer still recording - really delete?"))) {
              cTimer *timer = rc->Timer();
              if (timer) {
                 timer->Skip();
                 cRecordControls::Process(time(NULL));
                 if (timer->IsSingleEvent()) {
                    isyslog("deleting timer %s", *timer->ToDescr());
                    Timers.Del(timer);
                    }
                 Timers.SetModified();
                 }
              }
           else
              return osContinue;
           }
        cRecording *recording = GetRecording(ri);
        if (recording) {
           if (recording->Delete()) {
              cReplayControl::ClearLastReplayed(ri->FileName());
              Recordings.DelByName(ri->FileName());
              cOsdMenu::Del(Current());
              SetHelpKeys();
              Display();
              if (!Count())
                 return osBack;
              }
           else
              Skins.Message(mtError, tr("Error while deleting recording!"));
           }
        }
     }
  return osContinue;
}

eOSState cMenuRecordings::Info(void)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && !ri->IsDirectory()) {
     cRecording *recording = GetRecording(ri);
     if (recording && recording->Info()->Title())
        return AddSubMenu(new cMenuRecording(recording, true));
     }
  return osContinue;
}

eOSState cMenuRecordings::Commands(eKeys Key)
{
  if (HasSubMenu() || Count() == 0)
     return osContinue;
  cMenuRecordingItem *ri = (cMenuRecordingItem *)Get(Current());
  if (ri && !ri->IsDirectory()) {
     cRecording *recording = GetRecording(ri);
     if (recording) {
        char *parameter = NULL;
        asprintf(&parameter, "\"%s\"", *strescape(recording->FileName(), "\"$"));
        cMenuCommands *menu;
        eOSState state = AddSubMenu(menu = new cMenuCommands(tr("Recording commands"), &RecordingCommands, parameter));
        free(parameter);
        if (Key != kNone)
           state = menu->ProcessKey(Key);
        return state;
        }
     }
  return osContinue;
}

eOSState cMenuRecordings::ProcessKey(eKeys Key)
{
  bool HadSubMenu = HasSubMenu();
  eOSState state = cOsdMenu::ProcessKey(Key);

  if (state == osUnknown) {
     switch (Key) {
       case kOk:     return Play();
       case kRed:    return (helpKeys > 1 && RecordingCommands.Count()) ? Commands() : Play();
       case kGreen:  return Rewind();
       case kYellow: return Delete();
       case kBlue:   return Info();
       case k1...k9: return Commands(Key);
       case kNone:   if (Recordings.StateChanged(recordingsState))
                        Set(true);
                     break;
       default: break;
       }
     }
  if (Key == kYellow && HadSubMenu && !HasSubMenu()) {
     // the last recording in a subdirectory was deleted, so let's go back up
     cOsdMenu::Del(Current());
     if (!Count())
        return osBack;
     Display();
     }
  if (!HasSubMenu() && Key != kNone)
     SetHelpKeys();
  return state;
}

// --- cMenuSetupBase --------------------------------------------------------

class cMenuSetupBase : public cMenuSetupPage {
protected:
  cSetup data;
  virtual void Store(void);
public:
  cMenuSetupBase(void);
  int tmpScrollBarWidth;      // hw ScrollBarWidth
};

cMenuSetupBase::cMenuSetupBase(void)
{
  data = Setup;
  switch (data.OSDScrollBarWidth) {              // hw move setup to tmpsrcollbarwidth
    case 5:  {tmpScrollBarWidth = 0;
      break;
    }
    case 7:   {tmpScrollBarWidth = 1;
              break;
    }
    case 9:   {tmpScrollBarWidth = 2;
              break;
    }
    case 11:  {tmpScrollBarWidth = 3;
              break;
    }
    case 13:  {tmpScrollBarWidth = 4;
              break;
    }
    case 15:  {tmpScrollBarWidth = 5;
              break;
    }
    default : tmpScrollBarWidth = 3;
  }
}

void cMenuSetupBase::Store(void)
{
  switch (tmpScrollBarWidth) {              // hw move tmpsrcollbarwidth to setup
    case 0:  {data.OSDScrollBarWidth = 5;
              break;
    }
    case 1:  {data.OSDScrollBarWidth = 7;
              break;
    }
    case 2:  {data.OSDScrollBarWidth = 9;
              break;
    }
    case 3:  {data.OSDScrollBarWidth = 11;
              break;
    }
    case 4:  {data.OSDScrollBarWidth = 13;
              break;
    }
    case 5:  {data.OSDScrollBarWidth = 15;
              break;
    }
  }
  Setup = data;
  Setup.Save();
}

// --- cMenuSetupOSD ---------------------------------------------------------

class cMenuSetupOSD : public cMenuSetupBase {
private:
  const char *useSmallFontTexts[3];
  const char *channelViewModeTexts[3];
  const char *ScrollBarWidthTexts[6];

  int numSkins;
  int originalSkinIndex;
  int skinIndex;
  const char **skinDescriptions;
  cThemes themes;
  int themeIndex;
  virtual void Set(void);
  void ExpertMenu(void);            // ExpertMenu for OSD
  void DrawExpertMenu(void);        // Draw ExpertMenu
public:
  cMenuSetupOSD(void);
  virtual ~cMenuSetupOSD();
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuSetupOSD::cMenuSetupOSD(void)
{

  numSkins = Skins.Count();
  skinIndex = originalSkinIndex = Skins.Current()->Index();
  skinDescriptions = new const char*[numSkins];
  themes.Load(Skins.Current()->Name());
  themeIndex = Skins.Current()->Theme() ? themes.GetThemeIndex(Skins.Current()->Theme()->Description()) : 0;
  Set();
}

cMenuSetupOSD::~cMenuSetupOSD()
{
  cFont::SetCode(I18nCharSets()[Setup.OSDLanguage]);
  delete[] skinDescriptions;
}

void cMenuSetupOSD::Set(void)
{
  int current = Current();
  
  for (cSkin *Skin = Skins.First(); Skin; Skin = Skins.Next(Skin))
       skinDescriptions[Skin->Index()] = Skin->Description();

  channelViewModeTexts[0] = tr("channellist");
  channelViewModeTexts[1] = tr("current bouquet");
  channelViewModeTexts[2] = tr("bouquet list");

  Clear();
  SetSection(tr("OSD"));
  
#ifndef RBLITE
    Add(new cMenuEditStraItem(tr("Setup.OSD$Skin"),               &skinIndex, numSkins, skinDescriptions));
#endif
    if (themes.NumThemes())
    Add(new cMenuEditStraItem(tr("Setup.OSD$Theme"),            &themeIndex, themes.NumThemes(), themes.Descriptions()));
    Add(new cMenuEditBoolItem(tr("Setup.OSD$Random"),                   &data.OSDRandom));
    Add(new cMenuEditBoolItem(tr("Setup.OSD$Scroll pages"),             &data.MenuScrollPage));
    Add(new cMenuEditBoolItem(tr("Setup.OSD$Scroll wraps"),             &data.MenuScrollWrap));
    Add(new cMenuEditStraItem(tr("Setup.OSD$Channellist starts with"),  &data.UseBouquetList, 3, channelViewModeTexts));

  //Add(new cMenuEditStraItem(tr("Setup.OSD$Language"),               &data.OSDLanguage, I18nNumLanguages, I18nLanguages()));
  //Add(new cMenuEditIntItem( tr("Setup.EPG$Preferred languages"),    &numLanguages, 1, I18nNumLanguages));
  //for (int i = 1; i < numLanguages; i++) {
  //Add(new cMenuEditStraItem(tr(" Setup.EPG$Preferred language"),     &data.EPGLanguages[i], I18nNumLanguages, I18nLanguages()));
  // }
  //Add(new cMenuEditBoolItem(tr("Setup.OSD$Timeout requested channel info"), &data.TimeoutRequChInfo));
  //Add(new cMenuEditBoolItem(tr("Setup.OSD$Menu button closes"),     &data.MenuButtonCloses));
  //Add(new cMenuEditStraItem(tr("Setup.OSD$Use small font"),         &data.UseSmallFont, 3, useSmallFontTexts));

  SetHelp(tr("Expertmenu"));  // hw
  SetCurrent(Get(current));
  Display();
}

eOSState cMenuSetupOSD::ProcessKey(eKeys Key)
{
  if (Key == kRed) {
      ExpertMenu();
  }
  if (Key == kOk) {
#ifdef RBLITE
    Skins.SetCurrent("Reel");
#else
    if (skinIndex != originalSkinIndex) {
      cSkin *Skin = Skins.Get(skinIndex);
      if (Skin) {
        strn0cpy(data.OSDSkin, Skin->Name(), sizeof(data.OSDSkin));
        Skins.SetCurrent(Skin->Name());
      }
    }
#endif
    if (themes.NumThemes() && Skins.Current()->Theme()) {
      // hw data.UseSmallFont=2;
      Skins.SetCurrent("Reel");
      Skins.Current()->Theme()->Load(themes.FileName(themeIndex));
      strn0cpy(data.OSDTheme, themes.Name(themeIndex), sizeof(data.OSDTheme));
    }
    data.OSDWidth &= ~0x07; // OSD width must be a multiple of 8
  }
  //eOSState state = cMenuSetupBase::ProcessKey(Key);

  //if (Key == kRed) Dump();
/*
  int osdLanguage = data.OSDLanguage;
  int oldSkinIndex = skinIndex;

    if (oldThemeIndex != themeIndex) {
    data.UseSmallFont=1;
    Skins.Current()->Theme()->Load(themes.FileName(themeIndex));
    strn0cpy(data.OSDTheme, themes.Name(themeIndex), sizeof(data.OSDTheme));
    }

  if (data.OSDLanguage != osdLanguage || skinIndex != oldSkinIndex) {
     int OriginalOSDLanguage = Setup.OSDLanguage;
     Setup.OSDLanguage = data.OSDLanguage;
     cFont::SetCode(I18nCharSets()[Setup.OSDLanguage]);

     cSkin *Skin = Skins.Get(skinIndex);
     if (Skin) {
        char *d = themes.NumThemes() ? strdup(themes.Descriptions()[themeIndex]) : NULL;
        themes.Load(Skin->Name());
        if (skinIndex != oldSkinIndex)
           themeIndex = d ? themes.GetThemeIndex(d) : 0;
        free(d);
        }

     Set();
     Setup.OSDLanguage = OriginalOSDLanguage;
     }
*/
  int oldSkinIndex = skinIndex;
  eOSState state = cMenuSetupBase::ProcessKey(Key);

  if (skinIndex != oldSkinIndex) {
    cSkin *Skin = Skins.Get(skinIndex);
    if (Skin) {
      char *d = themes.NumThemes() ? strdup(themes.Descriptions()[themeIndex]) : NULL;
      themes.Load(Skin->Name());
      if (skinIndex != oldSkinIndex)
        themeIndex = d ? themes.GetThemeIndex(d) : 0;
        free(d);
    }
    Set();
  }
  return state;
}
// --- OSD ExpertMenu Init ---------------------------------------------------------

void cMenuSetupOSD::ExpertMenu(void)
{
    SetHelp(NULL);                                      // clear HelpKey
    Clear();                                            // Clear OSD
    SetSection(tr("OSD - Expertmenu"));                 // Title OSD
    DrawExpertMenu();                                   // Draw New OSD
}

// --- OSD ExpertMenu Draw ---------------------------------------------------------

void cMenuSetupOSD::DrawExpertMenu(void)
{
    useSmallFontTexts[0] = tr("never");
    useSmallFontTexts[1] = tr("skin dependent");
    useSmallFontTexts[2] = tr("always");

    ScrollBarWidthTexts[0] = "5";
    ScrollBarWidthTexts[1] = "7";
    ScrollBarWidthTexts[2] = "9";
    ScrollBarWidthTexts[3] = "11";
    ScrollBarWidthTexts[4] = "13";
    ScrollBarWidthTexts[5] = "15";
    
    int current = Current();
    Add(new cMenuEditIntItem(tr("Setup.OSD$Left"),                &data.OSDLeft, 0, MAXOSDWIDTH));
    Add(new cMenuEditIntItem(tr("Setup.OSD$Top"),                 &data.OSDTop, 0, MAXOSDHEIGHT));
    Add(new cMenuEditIntItem(tr("Setup.OSD$Width"),               &data.OSDWidth, MINOSDWIDTH, MAXOSDWIDTH));
    Add(new cMenuEditIntItem(tr("Setup.OSD$Height"),              &data.OSDHeight, MINOSDHEIGHT, MAXOSDHEIGHT));
    Add(new cMenuEditIntItem( tr("Setup.OSD$Channel info time (s)"),    &data.ChannelInfoTime, 1, 60));
    Add(new cMenuEditBoolItem(tr("Setup.OSD$Ok shows"),                 &data.WantChListOnOk, tr("channelinfo"), tr("channellist")));
    Add(new cMenuEditBoolItem(tr("Setup.OSD$Remain Time"),              &data.OSDRemainTime));
    Add(new cMenuEditBoolItem(tr("Setup.OSD$Use Symbol"),         &data.OSDUseSymbol));
    Add(new cMenuEditStraItem(tr("Setup.OSD$ScrollBar Width"),    &tmpScrollBarWidth, 6, ScrollBarWidthTexts));
    Add(new cMenuEditStraItem(tr("Setup.OSD$Use small font"),     &data.UseSmallFont, 3, useSmallFontTexts));

    SetCurrent(Get(current));
    Display();
}

// --- cMenuSetupLang ---------------------------------------------------------

class cMenuSetupLang : public cMenuSetupBase {
private:
  int originalNumLanguages;
  //int numLanguages;
  int optLanguages;
  void DrawMenu(void);
public:
  cMenuSetupLang(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuSetupLang::cMenuSetupLang(void)
{
  for (originalNumLanguages = 0; originalNumLanguages < I18nNumLanguages && data.EPGLanguages[originalNumLanguages] >= 0; originalNumLanguages++)
      ;
  //originalNumLanguages = numLanguages;
  optLanguages = originalNumLanguages-1;

  //SetSection(tr("Setup.OSD$Language"));
  //SetHelp(tr("Button$Scan"));
  DrawMenu();
}

void cMenuSetupLang::DrawMenu(void)
{
  SetSection(tr("Setup.OSD$Language"));
  int current = Current();

  Clear();

  Add(new cMenuEditStraItem(tr("Setup.EPG$Preferred language"),     &data.EPGLanguages[0], I18nNumLanguages, I18nLanguages()));
  Add(new cMenuEditIntItem( tr("Setup.OSD$Optional languages"),     &optLanguages, 0, I18nNumLanguages-1, tr("none"), NULL));
  for (int i = 1; i <= optLanguages; i++)
     Add(new cMenuEditStraItem(tr("Setup.OSD$ Optional language"),   &data.EPGLanguages[i], I18nNumLanguages, I18nLanguages()));

  SetCurrent(Get(current));
  Display();
}

eOSState cMenuSetupLang::ProcessKey(eKeys Key)
{
  bool Modified = optLanguages+1 != originalNumLanguages;


  int oldnumLanguages = optLanguages+1;
  int oldPrefLanguage = data.EPGLanguages[0];

  eOSState state = cMenuSetupBase::ProcessKey(Key);

  if (Key != kNone) {
     if (optLanguages+1 != oldnumLanguages) {
        Modified=true;
        for (int i = oldnumLanguages; i <= optLanguages; i++) {
	    Modified = true;
            data.EPGLanguages[i] = 0;
            for (int l = 0; l < I18nNumLanguages; l++) {
                int k;
                for (k = 0; k < oldnumLanguages; k++) {
                    if (data.EPGLanguages[k] == l)
                       break;
                    }
                if (k >= oldnumLanguages) {
                   data.EPGLanguages[i] = l;
                   break;
                   }
                }
            }
        data.EPGLanguages[optLanguages+1] = -1;
     }
     if (oldPrefLanguage != data.EPGLanguages[0]) {
        Modified = true;
	data.OSDLanguage = data.EPGLanguages[0];
        Setup.OSDLanguage = data.EPGLanguages[0];
        cFont::SetCode(I18nCharSets()[Setup.OSDLanguage]);
     }

     if (!Modified) {
        for (int i = 0; i <= optLanguages; i++) {
            if (data.EPGLanguages[i] != ::Setup.EPGLanguages[i]) {
               Modified = true;
               break;
            }
        }
     }

     if (Modified) {
        for (int i = 0; i <= I18nNumLanguages ; i++) {
            data.AudioLanguages[i] = data.EPGLanguages[i];
	    if (data.EPGLanguages[i] == -1)
	        break;
	}
	if (Key == kOk)
           cSchedules::ResetVersions();
	else
           DrawMenu();
     }
  }
  return state;
}

// --- cMenuSetupEPG ---------------------------------------------------------

class cMenuSetupEPG : public cMenuSetupBase {
private:
  //int originalNumLanguages;
  //int numLanguages;
  void Setup(void);
public:
  cMenuSetupEPG(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuSetupEPG::cMenuSetupEPG(void)
{
  /*for (numLanguages = 0; numLanguages < I18nNumLanguages && data.EPGLanguages[numLanguages] >= 0; numLanguages++)
      ;
  originalNumLanguages = numLanguages;
  */
  SetSection(tr("EPG"));
  SetHelp(tr("Button$Scan"));
  Setup();
}

void cMenuSetupEPG::Setup(void)
{
  int current = Current();

  Clear();
  /*
  Add(new cMenuEditIntItem( tr("Setup.EPG$EPG scan timeout (h)"),      &data.EPGScanTimeout));
  Add(new cMenuEditIntItem( tr("Setup.EPG$EPG bugfix level"),          &data.EPGBugfixLevel, 0, MAXEPGBUGFIXLEVEL));
  Add(new cMenuEditIntItem( tr("Setup.EPG$EPG linger time (min)"),     &data.EPGLinger, 0));
  Add(new cMenuEditBoolItem(tr("Setup.EPG$Set system time"),           &data.SetSystemTime));
  if (data.SetSystemTime)
     Add(new cMenuEditTranItem(tr("Setup.EPG$Use time from transponder"), &data.TimeTransponder, &data.TimeSource));
  Add(new cMenuEditIntItem( tr("Setup.EPG$Preferred languages"),       &numLanguages, 0, I18nNumLanguages));
  for (int i = 0; i < numLanguages; i++)
     Add(new cMenuEditStraItem(tr("Setup.EPG$Preferred language"),     &data.EPGLanguages[i], I18nNumLanguages, I18nLanguages()));
  */
  SetCurrent(Get(current));
  Display();
}

eOSState cMenuSetupEPG::ProcessKey(eKeys Key)
{
  /*
  if (Key == kOk) {
     bool Modified = numLanguages != originalNumLanguages;
     if (!Modified) {
        for (int i = 0; i < numLanguages; i++) {
            if (data.EPGLanguages[i] != ::Setup.EPGLanguages[i]) {
               Modified = true;
               break;
               }
            }
        }
     if (Modified)
        cSchedules::ResetVersions();
     }

  int oldnumLanguages = numLanguages;
  */
  int oldSetSystemTime = data.SetSystemTime;

  eOSState state = cMenuSetupBase::ProcessKey(Key);
  if (Key != kNone) {
     if (data.SetSystemTime != oldSetSystemTime) {
        /*for (int i = oldnumLanguages; i < numLanguages; i++) {
            data.EPGLanguages[i] = 0;
            for (int l = 0; l < I18nNumLanguages; l++) {
                int k;
                for (k = 0; k < oldnumLanguages; k++) {
                    if (data.EPGLanguages[k] == l)
                       break;
                    }
                if (k >= oldnumLanguages) {
                   data.EPGLanguages[i] = l;
                   break;
                   }
                }
            }
        data.EPGLanguages[numLanguages] = -1;
	*/
        Setup();
        }
     if (Key == kRed) {
        EITScanner.ForceScan();
        return osEnd;
        }
     }
  return state;
}

// --- cMenuSetupDVB ---------------------------------------------------------

class cMenuSetupDVB : public cMenuSetupBase {
private:
  int originalNumAudioLanguages;
  int numAudioLanguages;
  void Setup(void);
/*
  const char *videoDisplayFormatTexts[3];
  const char *updateChannelsTexts[6];
*/

public:
  cMenuSetupDVB(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuSetupDVB::cMenuSetupDVB(void)
{

/*
  for (numAudioLanguages = 0; numAudioLanguages < I18nNumLanguages && data.AudioLanguages[numAudioLanguages] >= 0; numAudioLanguages++)
      ;
  originalNumAudioLanguages = numAudioLanguages;

  videoDisplayFormatTexts[0] = tr("pan&scan");
  videoDisplayFormatTexts[1] = tr("letterbox");
  videoDisplayFormatTexts[2] = tr("center cut out");
  updateChannelsTexts[0] = tr("no");
  updateChannelsTexts[1] = tr("names only");
  updateChannelsTexts[2] = tr("PIDs only");
  updateChannelsTexts[3] = tr("names and PIDs");
  updateChannelsTexts[4] = tr("add new channels");
  updateChannelsTexts[5] = tr("add new transponders");
  SetSection(tr("DVB"));
*/
  SetSection(tr("Audio"));
  Setup();
}

void cMenuSetupDVB::Setup(void)
{
  int current = Current();

  Clear();
/*
  Add(new cMenuEditIntItem( tr("Setup.DVB$Primary DVB interface"), &data.PrimaryDVB, 1, cDevice::NumDevices()));
  Add(new cMenuEditBoolItem(tr("Setup.DVB$Video format"),          &data.VideoFormat, "4:3", "16:9"));
  if (data.VideoFormat == 0)
     Add(new cMenuEditStraItem(tr("Setup.DVB$Video display format"), &data.VideoDisplayFormat, 3, videoDisplayFormatTexts));

  Add(new cMenuEditIntItem( tr("Setup.DVB$Audio languages"),       &numAudioLanguages, 0, I18nNumLanguages));
  for (int i = 0; i < numAudioLanguages; i++)
      Add(new cMenuEditStraItem(tr("Setup.DVB$Audio language"),    &data.AudioLanguages[i], I18nNumLanguages, I18nLanguages()));
*/
  Add(new cMenuEditBoolItem(tr("Setup.DVB$Use Dolby Digital"),     &data.UseDolbyDigital));
  if (data.UseDolbyDigital) {
     Add(new cMenuEditIntItem(tr(" Delay ac3 (10ms)"),               &data.ReplayDelay, 0, 80));
     Add(new cMenuEditBoolItem(tr(" Prefer ac3 over HDMI"),          &data.Ac3OverHdmi));
  }
  Add(new cMenuEditIntItem(tr("Delay Stereo (10ms)"),              &data.MP2Delay, 0, 80));
  SetCurrent(Get(current));
  Display();
}

eOSState cMenuSetupDVB::ProcessKey(eKeys Key)
{
  /*
  int oldPrimaryDVB = ::Setup.PrimaryDVB;
  int oldVideoDisplayFormat = ::Setup.VideoDisplayFormat;
  bool oldVideoFormat = ::Setup.VideoFormat;
  bool newVideoFormat = data.VideoFormat;
  int oldnumAudioLanguages = numAudioLanguages;
  */

  bool oldUseDD = data.UseDolbyDigital;

  eOSState state = cMenuSetupBase::ProcessKey(Key);

  if (Key != kNone) {
     if (oldUseDD != data.UseDolbyDigital) {
        Setup();
     }
  }
  return state;

  /*
  if (Key != kNone) {
     bool DoSetup = data.VideoFormat != newVideoFormat;
     if (numAudioLanguages != oldnumAudioLanguages) {
        for (int i = oldnumAudioLanguages; i < numAudioLanguages; i++) {
            data.AudioLanguages[i] = 0;
            for (int l = 0; l < I18nNumLanguages; l++) {
                int k;
                for (k = 0; k < oldnumAudioLanguages; k++) {
                    if (data.AudioLanguages[k] == l)
                       break;
                    }
                if (k >= oldnumAudioLanguages) {
                   data.AudioLanguages[i] = l;
                   break;
                   }
                }
            }
        data.AudioLanguages[numAudioLanguages] = -1;
        DoSetup = true;
        }
     if (DoSetup)
        Setup();
     }
  if (state == osBack && Key == kOk) {
     if (::Setup.PrimaryDVB != oldPrimaryDVB)
        state = osSwitchDvb;
     if (::Setup.VideoDisplayFormat != oldVideoDisplayFormat)
        cDevice::PrimaryDevice()->SetVideoDisplayFormat(eVideoDisplayFormat(::Setup.VideoDisplayFormat));
     if (::Setup.VideoFormat != oldVideoFormat)
     }

  return state;
  */
}

// --- cMenuSetupLNB ---------------------------------------------------------
// #define  DBG "DEBUG [diseqc]: "

class cMenuSetupLNB : public cMenuSetupBase {
private:
  void Setup(void);
  void SetHelpKeys(void);
  bool IsUnique(int Tuner = 0, int Source=0);
  void LoadActuall();
  void AddDefault();
  void ResetLnbs();
  void Init();

  // holds only unique Sources
  typedef struct tLnbType {
     int source;
     int lnbType;
     int flag;
     } /* keep this */ ;
  //tLnbType lnbTypesAtTuner[MAXTUNERS+1][MAXLNBS]; //XXX
  tLnbType lnbTypesAtTuner[MAXTUNERS+1][64];
  int lnbNumberAtTuner[MAXTUNERS+1];
  //int lnbNumber[MAXTUNERS+1];  // number of diffrent LNBs/sources


  int tuner;
  int DiSEqC[MAXTUNERS+1];
  int Diseqc[MAXTUNERS+1]; // DiSEqFlags
  int RotorLNBTuner[MAXTUNERS+1];
  int diffSetups;
  cOsdMenu *smenu;
  static int IntCmp(const void *a, const void *b);
  void LoadTmpSources();

  bool extended;
  const char *useDiSEqcTexts[8];
  const char *lofTexts[7];
  int oldLnbNumber;
  int currentChannel;
  bool circular; // XXX
  int waitMs[MAXTUNERS+1];
  int repeat[MAXTUNERS+1];
  void DumpDiseqcs(bool All = false);

public:
  cMenuSetupLNB(void);
  virtual eOSState ProcessKey(eKeys Key);
  eOSState Save();
  };

cMenuSetupLNB::cMenuSetupLNB(void)
{
  SetSection(tr("Dish settings"));
  //DLOG( " LNB DEBUG  cMenuSetupLNB Constr  data.DiSEqC: %d  ", data.DiSEqC );

  SetCols(19);
  extended = false;
  circular = 0;
  oldLnbNumber = 0;
  tuner = (::Setup.DiSEqC & DIFFSETUPS) == DIFFSETUPS;
  diffSetups = tuner;

  DLOG (DBG " Have Diff Tuner Setup? %s ", diffSetups?"YES":"NO");


  Init();


  Setup();
}

void cMenuSetupLNB::Setup(void)
{
  DLOG (DBG " cMenuSetupLNB::Setup()    ");
  int current = Current();
  Clear();

  useDiSEqcTexts[0] = tr("DiSEqC disabled");
  useDiSEqcTexts[1] = "mini DiSEqC";
  useDiSEqcTexts[2] = "DiSEqC 1.0";
  useDiSEqcTexts[3] = "DiSEqC 1.1";
  useDiSEqcTexts[4] = "DisiCon 4";
  useDiSEqcTexts[5] = "Rotor - DiSEqC 1.2";
  useDiSEqcTexts[6] = "Rotor - GotoX";
  useDiSEqcTexts[7] = tr("Rotor - shared LNB");

  lofTexts[0] = "9750/10600 MHz";
  lofTexts[1] = "10750/11250 MHz";
  lofTexts[2] = "5150 MHz";
  lofTexts[3] = "9750 MHz";
  lofTexts[4] = "10600 Mhz";
  lofTexts[5] = "11250 MHz";
  lofTexts[6] = "11475 MHz";

  char buffer[16];
  char LnbC = 'A';

  bool hasRotor = false;
  for (int i = diffSetups; i < diffSetups*MAXTUNERS + 1; i++) { // ???
     if (i!=tuner && ((DiSEqC[i] & ROTORMASK) == GOTOX ||
                 (DiSEqC[i] & ROTORMASK) == DISEQC12) &&
                 (!i || cDevice::GetDevice(i-1) &&
                  cDevice::GetDevice(i-1)->ProvidesSource(cSource::stSat)))
     {
        hasRotor = true;
     }
  }

  if (extended)
     Add(new cMenuEditBoolItem(tr("Different Setups"),  &diffSetups));
  if (tuner)
     Add(new cMenuEditSatTunItem(tr("Tuner"), &tuner));
  if ((Diseqc[tuner])==ROTOR_SHARED && !hasRotor)
     Diseqc[tuner]=DiSEqC[tuner]=NONE;

  Add(new cMenuEditStraItem(tr("DiSEqC Type"),     &Diseqc[tuner], hasRotor ? 8 : 7, useDiSEqcTexts));


  //if (!(Diseqc[tuner] == DISICON4  || extended) || Diseqc[tuner] >= 4)
  // Add(new cMenuEditStraItem(tr("LNB Type"), &lnbTypesAtTuner[tuner][0].lnbType, 7, lofTexts));

  DLOG (DBG " switch Diseqc[%X] @ tuner: %d ", Diseqc[tuner], tuner);
  switch (Diseqc[tuner]) {
     case NONE:
            lnbNumberAtTuner[tuner] = 1;
            if (tuner)
                    Add(new cMenuEditSrcItem(tr("Satellite"), &lnbTypesAtTuner[tuner][0].source));
           if (extended)
                   Add(new cMenuEditStraItem(tr("   LNB Type"), &lnbTypesAtTuner[tuner][0].lnbType, 7, lofTexts));
            break;
     case MINI :
            lnbNumberAtTuner[tuner] = 2;
            for (int i=0; i < lnbNumberAtTuner[tuner];i++) {
               snprintf(buffer, sizeof(buffer), "LNB %c",LnbC+i);
               Add(new cMenuEditSrcEItem(buffer, &lnbTypesAtTuner[tuner][i].source, DiSEqC, tuner));
               if (extended)
                     Add(new cMenuEditStraItem(tr("   LNB Type"), &lnbTypesAtTuner[tuner][i].lnbType, 7, lofTexts));
               }

             if (extended)
                Add(new cMenuEditIntItem(tr("Delay (ms)"), &waitMs[tuner], 15, 100));
             break;
     case FULL:
             Add(new cMenuEditIntItem(tr("Number of LNBs"), &lnbNumberAtTuner[tuner],MINLNBS,MAXLNBS));

             for (int i=0;i < lnbNumberAtTuner[tuner];i++) {
                 snprintf(buffer, sizeof(buffer), "LNB %c",LnbC+i);
                 Add(new cMenuEditSrcEItem(buffer, &lnbTypesAtTuner[tuner][i].source, DiSEqC, tuner));
                 if (extended)
                    Add(new cMenuEditStraItem(tr("   LNB Type"), &lnbTypesAtTuner[tuner][i].lnbType, 7, lofTexts));
               }

              if (extended) {
                 Add(new cMenuEditIntItem(tr("Delay (ms)"), &waitMs[tuner], 15, 100));
                 Add(new cMenuEditIntItem(tr("Repeat"), &repeat[tuner], 0, 3));
                 }
              else if (((data.DiSEqC & (SWITCHMASK << (tuner ? (tuner-1)*TUNERBITS : 0))) >> (tuner ? (tuner-1)*TUNERBITS : 0))==MINI)
                 waitMs[tuner] = 0;
              break;
     case FULL_11:
             Add(new cMenuEditIntItem(tr("Number of LNBs"), &lnbNumberAtTuner[tuner],MINLNBS,16));

             for (int i=0;i < lnbNumberAtTuner[tuner];i++) {
                 snprintf(buffer, sizeof(buffer), "LNB %c",LnbC+i);
                 Add(new cMenuEditSrcEItem(buffer, &lnbTypesAtTuner[tuner][i].source, DiSEqC, tuner));
                 if (extended)
                    Add(new cMenuEditStraItem(tr("   LNB Type"), &lnbTypesAtTuner[tuner][i].lnbType, 7, lofTexts));
               }

              if (extended) {
                 Add(new cMenuEditIntItem(tr("Delay (ms)"), &waitMs[tuner], 15, 100));
                 Add(new cMenuEditIntItem(tr("Repeat"), &repeat[tuner], 0, 3));
                 }
              else if (((data.DiSEqC & (SWITCHMASK << (tuner ? (tuner-1)*TUNERBITS : 0))) >> (tuner ? (tuner-1)*TUNERBITS : 0))==MINI)
                 waitMs[tuner] = 0;
              break;
     case DISICON4:
             lnbNumberAtTuner[tuner] = 1;
             Add(new cMenuEditSrcEItem(tr("Satellite"), &lnbTypesAtTuner[tuner][0].source, DiSEqC, tuner));
             break;
     case ROTOR_SHARED:
             lnbNumberAtTuner[tuner] = 7;
             Add(new cMenuEditRShItem(tr("Rotor on tuner"), &RotorLNBTuner[tuner], DiSEqC));
             break;
     default:
            if (extended)
                    Add(new cMenuEditStraItem(tr("   LNB Type"), &lnbTypesAtTuner[tuner][0].lnbType, 7, lofTexts));
             lnbNumberAtTuner[tuner] = 1;

     }

  SetHelp(extended? tr("Normal") : tr("Expert"), (DiSEqC[tuner] & ROTORMASK) ? tr("Rotor Settings") : NULL);
  SetCurrent(Get(current));
  Display();
}

int cMenuSetupLNB::IntCmp(const void *a, const void *b)
{
  return (* (int *)a - *(int *)b);
}

void cMenuSetupLNB::Init()
{

  // if settings changed we have to send diseqc commands by switching channel
  currentChannel = cDevice::CurrentChannel();
  if (currentChannel > Channels.Count())
     currentChannel = 1;

  if (tuner) {
     for (int t=0; t<MAXTUNERS; t++)
        DiSEqC[t+1]= (data.DiSEqC & (TUNERMASK << (TUNERBITS * t))) >> (TUNERBITS * t);
     }
  else
  {
     DLOG (DBG " for indiff Tuner setup set  DiSEqC[0] to %d ", data.DiSEqC);
     DiSEqC[0]= data.DiSEqC;
  }
  DLOG( " LNB DEBUG  cMenuSetupLNB Constr  DiSEqC[%d]: %d  ", tuner, DiSEqC[tuner]);

  for (int t=0; t<=MAXTUNERS; t++) {
     if (DiSEqC[t] & SWITCHMASK)
        Diseqc[t]=DiSEqC[t] & SWITCHMASK;
     else if (DiSEqC[t] & ROTORLNB)
        Diseqc[t] = 7;
     else if (DiSEqC[t] & GOTOX)
        Diseqc[t] = 6;
     else if (DiSEqC[t] & DISEQC12)
        Diseqc[t] = 5;
     else
        Diseqc[t] = 0;
     }

  for (int t=1; t<=MAXTUNERS; t++) {
     RotorLNBTuner[t]=(DiSEqC[t] & ROTORLNB) ? (DiSEqC[t] & 0x30) >> 4 : 0;
  }

  DLOG (DBG " Restet LNB Types & LnbNubers  ");

  for (int t=0;t<MAXTUNERS+1;t++) {
     lnbNumberAtTuner[t] = 0;
     waitMs[t] = Diseqcs.WaitMs(t);
     repeat[t] = Diseqcs.RepeatCmd(t);
     for (int lnb=0;lnb<64;lnb++) {  // MAXLNBs
        lnbTypesAtTuner[t][lnb].source = 0;
        lnbTypesAtTuner[t][lnb].lnbType = 0;
        }
    }

  DLOG (DBG " Load Actuall ");
  int index = 0;

  for (cDiseqc *d = Diseqcs.First(); d; d = Diseqcs.Next(d)) {
     int t = d->Tuner();
     DLOG (DBG " %d.) T: %d;Source: %d ",++i , t, d->Source());
     if (index == 0) {
        lnbNumberAtTuner[t] = 1;
        lnbTypesAtTuner[t][index].source = d->Source();
        lnbTypesAtTuner[t][index].lnbType = d->LnbType();
        index++;
        DLOG (DBG " Count Lnb %d @ Tuner %d add Source %d  ", lnbNumberAtTuner[t], t, d->Source());
     }
     else if (index > 0) {
        if (lnbTypesAtTuner[t][lnbNumberAtTuner[t]-1].source != d->Source()) {
          lnbTypesAtTuner[t][lnbNumberAtTuner[t]].source = d->Source();
          lnbTypesAtTuner[t][lnbNumberAtTuner[t]].lnbType = d->LnbType();
          lnbNumberAtTuner[t]++;
          index++;
          DLOG (DBG " Count Lnb %d @ Tuner %d add Source %d  ", lnbNumberAtTuner[t], t, d->Source());
          }
        }
     }   /// Tested OK  for indifferent Settings
  for (int k=0; k<=MAXTUNERS; k++)
    for (int i=0; i<lnbNumberAtTuner[k]; i++) {
       if (lnbTypesAtTuner[k][i].source == cSource::stSat)
          switch (DiSEqC[k] & ROTORMASK) {
             case DISEQC12: lnbTypesAtTuner[k][i].source=0;
                            break;
             case GOTOX:    lnbTypesAtTuner[k][i].source=1;
                            break;
             case ROTORLNB: lnbTypesAtTuner[k][i].source = 1 + ((DiSEqC[k] & 0x30) >> 4);
                            break;
             }
       }
   DumpDiseqcs(true);
   AddDefault();
   DumpDiseqcs(true);
}


void cMenuSetupLNB::DumpDiseqcs(bool all)
{
  dsyslog(DBG  " Dump Diseqc () D.Count() %d D.LnbCount() %d ALL %c", Diseqcs.Count(), Diseqcs.LnbCount(), all?'y':'n');
  //Loading already configured LnbTypes to LnbStruct
  int limit = 0;

  for (int t = 0;t<MAXTUNERS+1;t++)
  {
     if (all)
       limit = 20;  // vor save
     else
       limit = lnbNumberAtTuner[t];
      // Count Lnb 0 @ T: 1 S: 4

      DLOG(DBG " Count Lnb %d @ T: %d ",  lnbNumberAtTuner[t], t);
      for (int lnb = 0;lnb<limit;lnb++) {
         DLOG(DBG " LNB %d points at  %d   ",lnb, lnbTypesAtTuner[t][lnb].source );
      }
  }
}

void cMenuSetupLNB::AddDefault()
{
   DLOG (DBG " AddDefault() ");

  tLnbType initTypes[] = {
   /// TODO we need to know right LNB types for each satelite
     { 35008, 0, 0 },
     { 34946, 0, 0 },
     { 35031, 0, 0 },
     { 35076, 0, 0 },
     { 35051, 0, 0 },
     { 35098, 0, 0 },
     { 35121, 0, 0 },
     { 35129, 0, 0 },
     { 32838, 0, 0 },
     { 32878, 0, 0 },
     { 34916, 0, 0 },
     { 34866, 0, 0 },
     { 34976, 0, 0 },
     { 32778, 0, 0 },
     { 35176, 0, 0 },
     { 35236, 0, 0 }
   };
  // fill up with default values to avoid string "0"  in EditSrcItem
  for (int t=0; t<MAXTUNERS+1; t++) {
     int cnt = lnbNumberAtTuner[t];
     DLOG (DBG " Procceding  through tuner %d LNBs: %d ",t, cnt);
     for (int i=0;i<16;i++) {  // runs initTypes // XXX MAXLNBS
        for(int lnb=0;lnb<cnt;lnb++) { // loop though  actuall lnbs
           //DLOG (DBG " check %d. InitSource %d vs. lnb %d s:%d   ", i, initTypes[i].source, lnb, lnbTypesAtTuner[t][lnb].source);
           if (lnbTypesAtTuner[t][lnb].source == initTypes[i].source) {
             initTypes[i].flag = 1;
             DLOG (DBG " check %d. mark  s:%d found @ lnb %d", i, initTypes[i].source, lnb);
             break; //next lnb
             }
           }
        } /* end for  LNBs */
        DLOG (DBG " Tuner %d add defaults  ", t);
#if DEBUG_DISEQC
        for (int x = 0;x < 16;x++)
        {
           if (initTypes[x].flag == 1)
             DLOG(DBG " %d.) %d  marked as found   ",x, initTypes[x].source);
        }
#endif
        int lnb = lnbNumberAtTuner[t];
        for (int i=0;i<16;i++) { // loop though  actuall lnbs ///XXX MAXLNBS
            if (initTypes[i].flag == 0 || lnbNumberAtTuner[t] == 0) {
               lnbTypesAtTuner[t][lnb].source = initTypes[i].source;
               if (lnb>=16) // MAXLNB
                  break;
               lnb++;
               }
            }
    } /* end for MAXTUNERS */ //OK testet
}

bool cMenuSetupLNB::IsUnique(int Tuner, int Source)
{
  // actual, we do not call this function
  DLOG (DBG "IsUnique Source %d @ T: %d ",Source, Tuner);
  if(Source)  {
     for (int i=0;i<0;i++) {
        if (lnbTypesAtTuner[Tuner][i].source == Source)
           return false;
        }
     return true;
     }

  int tmp[MAXLNBS] = { 0 };

  for (int i=0;i<MAXLNBS;i++) // ?? +1
     tmp[0]=lnbTypesAtTuner[Tuner][i].source;

  qsort(tmp, MAXLNBS ,sizeof(int), IntCmp);

  for (int i= 1; i< MAXLNBS;i++) {
     if (tmp[i] == tmp[i-1]&& tmp[i]!= 0)
        return false;
     }

  return true;
}

eOSState cMenuSetupLNB::Save()
{

  DLOG (DBG " cMenuSetupLNB::Save()    ");
  eOSState state = osContinue; ///???

  bool isUnique = true;
  if (!tuner)
     isUnique = IsUnique();
  else {
     for (int k=1; k<=MAXTUNERS; k++) {
        if (!cDevice::GetDevice(k-1) || !(cDevice::GetDevice(k-1)->ProvidesSource(cSource::stSat)))
           continue;
        if (!IsUnique(k))
           isUnique = false;
        }
     }
  if (!isUnique) {
     Skins.Message(mtError, tr("Sat positions must be unique!"));
     return osContinue;
     }

  if (!extended) {
     DLOG(DBG " LNB DEBUG  Not Extended ");
     for (int k = 0; k<=MAXTUNERS; k++) {
        for (int i = 1; i<lnbNumberAtTuner[k];i++) {
           lnbTypesAtTuner[k][i].lnbType = lnbTypesAtTuner[k][0].lnbType;
           }
        }
     }

  // ask user to rewrite diseqc.conf
  if (Interface->Confirm(tr("Overwrite DiSEqC.conf?"))) {

  DLOG(DBG " LNB DEBUG  Not Extended ");
  for (int i=0; i<=MAXTUNERS; i++)
     if (Diseqc[i]>4)
        lnbTypesAtTuner[i][0].source = cSource::stSat;

  //XXX
  // Diseqcs.SetLnbType(lnbTypesAtTuner[0].lnbType);

  // dsyslog ("DBG LNB_TYPE: DiSEqC: %d", data.DiSEqC);
  dsyslog ("delete all Diseqs");
  Diseqcs.Clear();

  if (!tuner) {
     Diseqcs.SetRepeatCmd(repeat[0],0);
     Diseqcs.SetWaitMs(waitMs[0],0);

     for (int i=0;i<lnbNumberAtTuner[0];i++) {
        Diseqcs.NewLnb(DiSEqC[0] & SWITCHMASK, lnbTypesAtTuner[0][i].source, lnbTypesAtTuner[0][i].lnbType);
        }
     }
 else {
    for (int k=1; k<=MAXTUNERS; k++) {
       if (!cDevice::GetDevice(k-1) || !(cDevice::GetDevice(k-1)->ProvidesSource(cSource::stSat)))
          continue;
       Diseqcs.SetRepeatCmd(repeat[k],k);
       Diseqcs.SetWaitMs(waitMs[k],k);

       for (int i=0;i<lnbNumberAtTuner[k];i++) {
          //dsyslog ("for  lnbNumberAtTuner %d newLnb(dyseqType: %d, source: %d, lnbType %d", lnbNumberAtTuner, data.DiSEqC, lnbTypesAtTuner[i].source, lnbTypesAtTuner[i].lnbType);
          Diseqcs.NewLnb(DiSEqC[k] & SWITCHMASK, lnbTypesAtTuner[k][i].source, lnbTypesAtTuner[k][i].lnbType, k);
          }
       }
    }
  // update current Setup  Object
     data.DiSEqC= tuner ? (DIFFSETUPS | (DiSEqC[4]<<(TUNERBITS*3)) | (DiSEqC[3]<<(TUNERBITS*2)) | (DiSEqC[2]<<TUNERBITS) | (DiSEqC[1])): DiSEqC[0];

    Store();
    if (data.DiSEqC) {
       Diseqcs.Save();

       // workaround to trigger diseqc codes
       Channels.SwitchTo(currentChannel+2);
       Channels.SwitchTo(currentChannel);
       }
    Skins.Message(mtInfo, tr("Changes done"),1);
    state = osUnknown;

    }
  return state;
}

eOSState cMenuSetupLNB::ProcessKey(eKeys Key)
{

  oldLnbNumber = lnbNumberAtTuner[tuner];
  int oldDiSEqC = Diseqc[tuner];
  int oldDiffSetups = diffSetups;

  if (HasSubMenu()) {
    eOSState state = smenu->ProcessKey(Key);
    if (state == osBack)
       return CloseSubMenu();
    return state;
    }

  if (Key == kOk) {
     Key = kNone;
     SetStatus(NULL);
     return Save();
  }

  eOSState state = cMenuSetupBase::ProcessKey(Key);

  //dsyslog (" lnbNumberAtTuner < oldLnbNumber  %d < %d", lnbNumberAtTuner, oldLnbNumber);

  if (Key == kGreen && (DiSEqC[tuner] & ROTORMASK)) {
     cPlugin *p = cPluginManager::GetPlugin("rotor");
     if (p) {
        int oldDiSEqC = data.DiSEqC;
        ::Setup.DiSEqC= tuner ? (DIFFSETUPS | (DiSEqC[4]<<(TUNERBITS*3)) | (DiSEqC[3]<<(TUNERBITS*2)) | (DiSEqC[2]<<TUNERBITS) | (DiSEqC[1])) : DiSEqC[0];
        AddSubMenu(smenu = (cOsdMenu *) p->MainMenuAction());
        ::Setup.DiSEqC = oldDiSEqC;
        }
     }

  if ((Key != kNone)) {
     if (Diseqc[tuner] != oldDiSEqC || lnbNumberAtTuner[tuner] != oldLnbNumber) {
        switch (Diseqc[tuner]) {
           case 5: DiSEqC[tuner] = DISEQC12;
                   break;
           case 6: DiSEqC[tuner] = GOTOX;
                   break;
           case 7: DiSEqC[tuner] = ROTORLNB + (RotorLNBTuner[tuner] << 4);
                   break;
          default: DiSEqC[tuner] = Diseqc[tuner];
                   break;
           }
        }
     if (Diseqc[tuner]<4)
        for (int i=0; i<lnbNumberAtTuner[tuner]; i++) {
            if ((lnbTypesAtTuner[tuner][i].source & cSource::st_Mask) == cSource::stNone) {
               switch (lnbTypesAtTuner[tuner][i].source) {
                  case 0: DiSEqC[tuner] = DISEQC12 | Diseqc[tuner];
                          break;
                  case 1: DiSEqC[tuner] = GOTOX | Diseqc[tuner];
                          break;
                 default: DiSEqC[tuner] = (ROTORLNB + ((lnbTypesAtTuner[tuner][i].source - 1) << 4))  | Diseqc[tuner];
                          break;
                  }
               }
            else
               DiSEqC[tuner] = Diseqc[tuner];
         }
      if (oldDiffSetups != diffSetups) {
         if (diffSetups)
            tuner=1;
         else
            tuner=0;
         }
      if (Key == kRed)
         extended = extended?false:true;

     Setup();
      } // endif other key as kOk

  return state;
}
/*
void cMenuSetupLNB::LoadActuall()
{
  dsyslog(" LoadActuall() D.Count() %d D.LnbCount() %d", Diseqcs.Count(), Diseqcs.LnbCount());
  //Loading already configured LnbTypes to LnbStruct
  for (int t=0; t<=MAXTUNERS; t++) {
     lnbNumberAtTuner[t] = 0;
     }

  if (Diseqcs.Count() == 0) {
     return;
     }

  DLOG (" LoadActuall()  ");

  /// FIXME
  for (cDiseqc *diseqc = Diseqcs.First(); diseqc; diseqc = Diseqcs.Next(diseqc)) {
     DLOG (DBG " Source: %d ", diseqc->Source());
     bool found=false;

     for (int k=0; k<lnbNumberAtTuner[diseqc->tuner()]; k++)
        if (lnbTypesAtTuner[diseqc->Tuner()][k].source == diseqc->Source())
           found=true;
     if (!found) {
        lnbTypesAtTuner[diseqc->Tuner()][lnbNumberAtTuner[diseqc->Tuner()]].source = diseqc->Source();
        lnbTypesAtTuner[diseqc->Tuner()][lnbNumberAtTuner[diseqc->Tuner()]].lnbType = diseqc->LnbType();
        lnbNumberAtTuner[diseqc->Tuner()]++;
        if (!diseqc->Tuner()) {
           for (int i=1; i<=MAXTUNERS; i++) {
              lnbTypesAtTuner[i][lnbNumberAtTuner[i]].source = diseqc->Source();
              lnbTypesAtTuner[i][lnbNumberAtTuner[i]].lnbType = diseqc->LnbType();
              lnbNumberAtTuner[i]++;
              }
           }
        }
    }

  for (int t=0; t<=MAXTUNERS; t++) {
     for (int lnb=0; lnb<lnbNumberAtTuner[t]; lnb++) {
        if (lnbTypesAtTuner[t][lnb].source == cSource::stSat) {
           switch (DiSEqC[t] & ROTORMASK) {
              case DISEQC12: lnbTypesAtTuner[t][lnb].source=0;
                             break;
              case GOTOX:    lnbTypesAtTuner[t][lnb].source=1;
                             break;
              default:       lnbTypesAtTuner[t][lnb].source = 1 + ((DiSEqC[t] & 0x30) >> 4);
              }
           }
        }
     }

  if (Tuner) {
     for (int t=1; t<=MAXTUNERS; t++) {
        for (int lnb=1; lnb<lnbNumberAtTuner[t]; lnb++) {
           if (lnbTypesAtTuner[t][lnb].lnbType!=lnbTypesAtTuner[t][0].lnbType) {
              extended=true;
              }
           }
       }
     }
  else {
     for (int lnb=1; lnb<lnbNumberAtTuner[0]; lnb++) {
        if (lnbTypesAtTuner[0][lnb].lnbType!=lnbTypesAtTuner[0][0].lnbType)
           extended=true;
        }
  }

#if 1
      dsyslog ("load actuall ");
     for (int t=1; t<=MAXTUNERS; t++) {
        for (int lnb=0;lnb<16;lnb++) { // MAXXXLNBS
          dsyslog ("found  %d LNBs @Tuner %d", lnbNumberAtTuner[t],t);
          dsyslog ("lnbTypesAtTuner[%d].source %d", lnb, lnbTypesAtTuner[t][lnb].source);
          }
       }
#endif

} */

// --- cMenuSetupCICAM -------------------------------------------------------

bool camFound[3]; //TB: has the CAM been found successfully?!
time_t firstTimeChecked[3]; //TB: first time we did check - used for deminining when we assume an error

class cMenuSetupCICAMItem : public cOsdItem {
private:
  cCiHandler *ciHandler;
  int slot;
  int device;
public:
  cMenuSetupCICAMItem(int Device, cCiHandler *CiHandler, int Slot, int state, int enabled);
  cCiHandler *CiHandler(void) { return ciHandler; }
  int Slot(void) { return slot; }
  int Device(void) { return device; }
  };

cMenuSetupCICAMItem::cMenuSetupCICAMItem(int Device, cCiHandler *CiHandler, int Slot, int state, int enabled)
{
  int slot_tmp=Slot;
  ciHandler = CiHandler;
  slot = Slot;
  char buffer[64];
  const char *CamName = CiHandler->GetCamName(slot);
#ifndef RBLITE
  slot=Device;
#endif
  device=Device;
  if (!CamName) {
    if (state&(3<<(16+2*slot))){
#ifdef RBLITE
      if((firstTimeChecked[slot] == 0) || ((time(NULL) - firstTimeChecked[slot]) < 10)){
#else
      if((firstTimeChecked[slot] == 0) || ((time(NULL) - firstTimeChecked[slot]) < 25)){
#endif
	CamName = tr("Init");
	if(!firstTimeChecked[slot])
	  firstTimeChecked[slot] = time(NULL);
      } else {
	CamName = tr("Error");
      }
    } else {
      CamName = "-";
      camFound[slot] = 1;
      firstTimeChecked[slot] = 0;
    }
  } else {
    camFound[slot] = 1;
    firstTimeChecked[slot] = 0;
  }

  // GA: CAM enable/disable
  if (!(enabled&(1<<slot))) {
    if (state&(1<<slot))
      CamName = tr("Disabled");
    else
      CamName = tr("--OFF--");
  }

  switch(slot){
    case 0:
            snprintf(buffer, sizeof(buffer), "%s:\t%s", tr("lower slot"), CamName);
	    break;
    case 1:
            snprintf(buffer, sizeof(buffer), "%s:\t%s", tr("upper slot"), CamName);
	    break;
    case 2:
	    snprintf(buffer, sizeof(buffer), "%s:\t%s", tr("internal CAM"), CamName);
	    break;
    default:
	    break;
  }
  slot=slot_tmp;
  SetText(buffer);
}

class cMenuSetupCICAM : public cMenuSetupBase {
private:
  eOSState Menu(void);
  eOSState Reset(void);
  eOSState Switch(void);
  void Update(int);
public:
  cMenuSetupCICAM(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

#define IOCTL_REEL_CI_GET_STATE _IOR('d', 0x45, int)

void cMenuSetupCICAM::Update(int cur) {
#ifdef RBLITE
  int numDevices=1;       
  SetCols(20);
  int fd,state=0;
  fd = open("/dev/reelfpga0", O_RDWR);
  if (fd) {
    state=ioctl(fd,IOCTL_REEL_CI_GET_STATE,0);
    close(fd);
  }
#else
  int numDevices=cDevice::NumDevices();
  int state=(5<<16)|3;
#endif
  SetSection(tr("Common Interface"));
  for (int d = 0; d < numDevices; d++) {
    cDevice *Device = cDevice::GetDevice(d);
    if (Device) {
      cCiHandler *CiHandler = Device->CiHandler();
      if (CiHandler) {
	for (int Slot = CiHandler->NumSlots()-1; Slot >= 0; Slot--)
	  Add(new cMenuSetupCICAMItem(Device->CardIndex(), CiHandler, Slot, state, data.CAMEnabled));
      }
    }
  }
  // GA: Best way without knowing object
  for(int i=0;i<=cur;i++)
    CursorDown();
#ifdef RBLITE
  SetHelp(tr("Reset"), NULL, NULL, tr("On/Off"));
#else
  SetHelp(tr("Reset"), NULL, NULL, NULL);
#endif
}

cMenuSetupCICAM::cMenuSetupCICAM(void)
{
  Update(0);
}

eOSState cMenuSetupCICAM::Menu(void)
{
  cMenuSetupCICAMItem *item = (cMenuSetupCICAMItem *)Get(Current());
  if (item) {
     if (item->CiHandler()->EnterMenu(item->Slot())) {
        Skins.Message(mtWarning, tr("Opening CAM menu..."));
        time_t t = time(NULL);
        while (time(NULL) - t < MAXWAITFORCAMMENU && !item->CiHandler()->HasUserIO())
              item->CiHandler()->Process();
        return osEnd; // the CAM menu will be executed explicitly from the main loop
        }
     else
        Skins.Message(mtError, tr("Can't open CAM menu!"));
     }
  return osContinue;
}

eOSState cMenuSetupCICAM::Reset(void)
{
  cMenuSetupCICAMItem *item = (cMenuSetupCICAMItem *)Get(Current());
  if (item) {
     Skins.Message(mtWarning, tr("Resetting CAM..."));
     int slot = item->Slot();
     int device = item->Device();
     if (item->CiHandler()->Reset(slot)) {
        Skins.Message(mtInfo, tr("CAM has been reset"));
#ifdef RBLITE
        camFound[slot] = 0;
#else
	camFound[device] = 0;
#endif
        return osContinue;
        }
     else
        Skins.Message(mtError, tr("Can't reset CAM!"));
     }
  return osContinue;
}

eOSState cMenuSetupCICAM::Switch(void)  // GA
{
  cMenuSetupCICAMItem *item = (cMenuSetupCICAMItem *)Get(Current());
  if (item){
    int slot = item->Slot();
    data.CAMEnabled^=(1<<slot);

    if(!(data.CAMEnabled&(1<<slot))){
      camFound[slot] = 0;
      firstTimeChecked[slot] = 0;
    }
  }
  return osContinue;
}

time_t timeLastRefresh = 0;

eOSState cMenuSetupCICAM::ProcessKey(eKeys Key)
{
  if(Key == kOk)
    return Menu();

  eOSState state = cMenuSetupBase::ProcessKey(Key);
  int cur;

  if (state == osUnknown) {
     switch (Key) {
       case kRed:    return Reset();
#ifdef RBLITE
       case kBlue:
		     cur=Current();
		     Switch();
		     Clear();
		     Update(cur);
		     Display();
		     Store();
		     return osContinue;
#endif
       default: break;
       }
     }

  if((timeLastRefresh == 0) || ((time(NULL) - timeLastRefresh) >= 1)){ //TB: refresh every second
    cur=Current();
    Clear();
    Update(cur);
    Display();
    timeLastRefresh = time(NULL);
  }

  return state;
}

// --- cMenuSetupRecord ------------------------------------------------------

class cMenuSetupRecord : public cMenuSetupBase {
private:
    const char *PriorityTexts[3];
    int tmpprio, tmppauseprio;
    virtual void Store(void);
public:
    cMenuSetupRecord(void);
};

cMenuSetupRecord::cMenuSetupRecord(void)
{
  PriorityTexts[0] = tr("low");
  PriorityTexts[1] = tr("normal");
  PriorityTexts[2] = tr("high");

  tmpprio = data.DefaultPriority == 10 ? 0 : data.DefaultPriority == 99 ? 2 : 1;
  tmppauseprio = data.PausePriority == 10 ? 0 : data.PausePriority == 99 ? 2 : 1;

  SetSection(tr("Recording"));
  Add(new cMenuEditBoolItem(tr("Setup.Recording$Record digital audio"),      &data.UseDolbyInRecordings));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Margin at start (min)"),     &data.MarginStart));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Margin at stop (min)"),      &data.MarginStop));
  Add(new cMenuEditStraItem(tr("Setup.Recording$Default priority"),          &tmpprio, 3, PriorityTexts));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Default lifetime (d)"),      &data.DefaultLifetime, 0, MAXLIFETIME, NULL, tr("unlimited")));
  Add(new cMenuEditBoolItem(tr("Setup.Recording$Use VPS"),                   &data.UseVps));
  Add(new cMenuEditBoolItem(tr("Setup.Recording$Mark instant recording"),    &data.MarkInstantRecord));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Instant rec. time (min)"),   &data.InstantRecordTime, 1, MAXINSTANTRECTIME));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Pause lifetime (d)"),        &data.PauseLifetime, 0, MAXLIFETIME, NULL, tr("unlimited")));
  Add(new cMenuEditStraItem( tr("Setup.Recording$Pause priority"),           &tmppauseprio, 3, PriorityTexts));
  //Add(new cMenuEditBoolItem(tr("Setup.OSD$Recording directories"),           &data.RecordingDirs));
  Add(new cMenuEditBoolItem(tr("Setup.Recording$Split edited files"),        &data.SplitEditedFiles));
  };

/*
  SetSection(tr("Recording"));
  Add(new cMenuEditBoolItem( tr("Setup.Recording$Record Dolby"),	     &data.UseDolbyInRecordings));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Margin at start (min)"),     &data.MarginStart));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Margin at stop (min)"),      &data.MarginStop));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Primary limit"),             &data.PrimaryLimit, 0, MAXPRIORITY));
  Add(new cMenuEditStraItem(tr("Setup.Recording$Default priority"),          &tmpprio, 3, PriorityTexts));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Default lifetime (d)"),      &data.DefaultLifetime, 0, MAXLIFETIME));
  Add(new cMenuEditStraItem( tr("Setup.Recording$Pause priority"),           &tmppauseprio, 3, PriorityTexts));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Pause lifetime (d)"),        &data.PauseLifetime, 0, MAXLIFETIME));
  Add(new cMenuEditBoolItem(tr("Setup.Recording$Use episode name"),          &data.UseSubtitle));
  Add(new cMenuEditBoolItem(tr("Setup.Recording$Use VPS"),                   &data.UseVps));
  Add(new cMenuEditIntItem( tr("Setup.Recording$VPS margin (s)"),            &data.VpsMargin, 0));
  Add(new cMenuEditBoolItem(tr("Setup.Recording$Mark instant recording"),    &data.MarkInstantRecord));
  Add(new cMenuEditStrItem( tr("Setup.Recording$Name instant recording"),     data.NameInstantRecord, sizeof(data.NameInstantRecord), tr(FileNameChars)));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Instant rec. time (min)"),   &data.InstantRecordTime, 1, MAXINSTANTRECTIME));
  Add(new cMenuEditIntItem( tr("Setup.Recording$Max. video file size (MB)"), &data.MaxVideoFileSize, MINVIDEOFILESIZE, MAXVIDEOFILESIZE));
  Add(new cMenuEditBoolItem(tr("Setup.Recording$Split edited files"),        &data.SplitEditedFiles));
}
*/

void cMenuSetupRecord::Store(void)
{
    data.DefaultPriority = tmpprio == 0 ? 10 : tmpprio == 1 ? 50 : 99;
    data.PausePriority   = tmppauseprio == 0 ? 10 : tmppauseprio == 1 ? 50 : 99;
    cMenuSetupBase::Store();
};

// --- cMenuSetupReplay ------------------------------------------------------

class cMenuSetupReplay : public cMenuSetupBase {
protected:
  virtual void Store(void);
public:
  cMenuSetupReplay(void);
  };

cMenuSetupReplay::cMenuSetupReplay(void)
{
  SetSection(tr("Replay"));
  Add(new cMenuEditBoolItem(tr("Setup.Replay$Multi speed mode"), &data.MultiSpeedMode));
  Add(new cMenuEditBoolItem(tr("Setup.Replay$Show replay mode"), &data.ShowReplayMode));
  //Add(new cMenuEditIntItem(tr("Setup.Replay$Resume ID"), &data.ResumeID, 0, 99));
  Add(new cMenuEditBoolItem(tr("Setup.Replay$Play&Jump"), &data.PlayJump));
  Add(new cMenuEditBoolItem(tr("Setup.Replay$Jump&Play"), &data.JumpPlay));
  //Add(new cMenuEditBoolItem(tr("Setup.Replay$Pause at last mark"), &data.PauseLastMark));
  //Add(new cMenuEditBoolItem(tr("Setup.Replay$Reload marks"), &data.ReloadMarks));
}

void cMenuSetupReplay::Store(void)
{
  if (Setup.ResumeID != data.ResumeID)
     Recordings.ResetResume();
  cMenuSetupBase::Store();
}

// --- cMenuSetupMisc --------------------------------------------------------

class cMenuSetupMisc : public cMenuSetupBase {
private:
  const char *updateChannelsTexts[3];
  int tmpUpdateChannels;
  virtual void Store(void);
public:
  cMenuSetupMisc(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuSetupMisc::cMenuSetupMisc(void)
{
  updateChannelsTexts[0] = tr("off");
  updateChannelsTexts[1] = tr("names and PIDs"); // 3
  updateChannelsTexts[2] = tr("add new transponders"); // 5

  tmpUpdateChannels = (int) data.UpdateChannels / 2;

  SetSection(tr("Background activity"));
  SetHelp(tr("Button$Scan EPG"));
  Add(new cMenuEditIntItem( tr("Setup.EPG$EPG scan timeout (h)"),      &data.EPGScanTimeout, 0, INT_MAX, tr("off")));
  Add(new cMenuEditIntItem( tr("Setup.Miscellaneous$Min. event timeout (min)"),   &data.MinEventTimeout, 0, INT_MAX, tr("off")));
  Add(new cMenuEditIntItem( tr("Setup.Miscellaneous$Min. user inactivity (min)"), &data.MinUserInactivity, 0, INT_MAX, tr("off")));
  //Add(new cMenuEditIntItem( tr("Setup.Miscellaneous$SVDRP timeout (s)"),          &data.SVDRPTimeout));
  Add(new cMenuEditIntItem( tr("Setup.Miscellaneous$Zap timeout (s)"),            &data.ZapTimeout, 0, INT_MAX, tr("off")));
  Add(new cMenuEditStraItem(tr("Setup.DVB$Update channels"),        &tmpUpdateChannels, 3, updateChannelsTexts));
  Add(new cMenuEditChanItem(tr("Setup.Miscellaneous$Initial channel"),            &data.InitialChannel, tr("Setup.Miscellaneous$as before")));
  Add(new cMenuEditIntItem( tr("Setup.Miscellaneous$Initial volume"),             &data.InitialVolume, -1, 255, tr("Setup.Miscellaneous$as before")));
}

eOSState cMenuSetupMisc::ProcessKey(eKeys Key)
{
  eOSState state = cMenuSetupBase::ProcessKey(Key);
     if (Key == kRed) {
        EITScanner.ForceScan();
        return osEnd;
     }
  return state;
}

void cMenuSetupMisc::Store(void)
{
  if ( tmpUpdateChannels == 0 )
	 data.UpdateChannels = 0;
  else if  ( tmpUpdateChannels == 1 )
	 data.UpdateChannels = 3;
  else if  ( tmpUpdateChannels == 2 )
	 data.UpdateChannels = 5;
  cMenuSetupBase::Store();
}


// --- cMenuSetupLiveBuffer --------------------------------------------------

class cMenuSetupLiveBuffer : public cMenuSetupBase {
private:
  void Setup();
public:
  eOSState ProcessKey(eKeys Key);
  cMenuSetupLiveBuffer(void);
  };

cMenuSetupLiveBuffer::cMenuSetupLiveBuffer(void)
{
  SetSection(tr("Permanent Timeshift"));
  Setup();
}

void cMenuSetupLiveBuffer::Setup(void)
{
  int current=Current();
 Clear();
  Add(new cMenuEditBoolItem(tr("Permanent Timeshift"),                                &data.LiveBuffer));
  if (data.LiveBuffer) {
     Add(new cMenuEditIntItem(tr("Setup.LiveBuffer$Buffer (min)"),          &data.LiveBufferSize, 1, 60));
        }
  SetCurrent(Get(current));
  Display();
}

eOSState cMenuSetupLiveBuffer::ProcessKey(eKeys Key)
{
  int oldLiveBuffer = data.LiveBuffer;
  eOSState state = cMenuSetupBase::ProcessKey(Key);

  if (Key != kNone && (data.LiveBuffer != oldLiveBuffer))
     Setup();
  return state;
}


// --- cMenuSetupPluginItem --------------------------------------------------

class cMenuSetupPluginItem : public cOsdItem {
private:
  int pluginIndex;
public:
  cMenuSetupPluginItem(const char *Name, int Index);
  int PluginIndex(void) { return pluginIndex; }
  };

cMenuSetupPluginItem::cMenuSetupPluginItem(const char *Name, int Index)
:cOsdItem(Name)
{
  pluginIndex = Index;
}

// --- cMenuSetupPlugins -----------------------------------------------------

class cMenuSetupPlugins : public cMenuSetupBase {
public:
  cMenuSetupPlugins(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuSetupPlugins::cMenuSetupPlugins(void)
{
  SetSection(tr("System settings"));
  SetHasHotkeys();
  for (int i = 0; ; i++) {
      cPlugin *p = cPluginManager::GetPlugin(i);
      if (p) {
	 if(p->HasSetupOptions()){
         char *buffer = NULL;
         //asprintf(&buffer, "%s (%s) - %s", p->Name(), p->Version(), p->Description());
         asprintf(&buffer, "%s", p->MainMenuEntry());
         Add(new cMenuSetupPluginItem(hk(buffer), i));
         free(buffer);
         }
      }
      else
         break;
      }
}

eOSState cMenuSetupPlugins::ProcessKey(eKeys Key)
{
  eOSState state = HasSubMenu() ? cMenuSetupBase::ProcessKey(Key) : cOsdMenu::ProcessKey(Key);

  if (Key == kOk) {
     if (state == osUnknown) {
        cMenuSetupPluginItem *item = (cMenuSetupPluginItem *)Get(Current());
        if (item) {
           cPlugin *p = cPluginManager::GetPlugin(item->PluginIndex());
           if (p) {
              cMenuSetupPage *menu = p->SetupMenu();
              if (menu) {
                 menu->SetPlugin(p);
                 return AddSubMenu(menu);
                 }
              Skins.Message(mtInfo, tr("This plugin has no setup parameters!"));
              }
           }
        }
     else if (state == osContinue)
        Store();
     }
  return state;
}

// --- cMenuSetup ------------------------------------------------------------

class cMenuSetup : public cOsdMenu {
private:
  virtual void Set(void);
  eOSState Restart(void);
public:
  cMenuSetup(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

cMenuSetup::cMenuSetup(void)
:cOsdMenu("")
{
  Set();
}

void cMenuSetup::Set(void)
{
  Clear();
  char buffer[64];
  snprintf(buffer, sizeof(buffer), "%s - VDR %s", tr("Setup"), VDRVERSION);
  SetTitle(buffer);
  SetHasHotkeys();
  Add(new cOsdItem(hk(tr("LNB")),           osUser4));
  Add(new cOsdItem(hk(tr("OSD")),           osUser1));
  Add(new cOsdItem(hk(tr("EPG")),           osUser2));
  Add(new cOsdItem(hk(tr("DVB")),           osUser3));
  Add(new cOsdItem(hk(tr("CICAM")),         osUser5));
  Add(new cOsdItem(hk(tr("Recording settings")),     osUser6));
  Add(new cOsdItem(hk(tr("Replay settings")),        osUser7));
  Add(new cOsdItem(hk(tr("Miscellaneous")), osUser8));
  Add(new cOsdItem(hk(tr("Permanent Timeshift")),osLiveBuffer));
  if (cPluginManager::HasPlugins())
  Add(new cOsdItem(hk(tr("Plugins")),       osUser9));
  Add(new cOsdItem(hk(tr("Restart")),       osUser10));
}

eOSState cMenuSetup::Restart(void)
{
  if (Interface->Confirm(tr("Really restart?"))
     && (!cRecordControls::Active() || Interface->Confirm(tr("Recording - restart anyway?")))
     && !cPluginManager::Active(tr("restart anyway?"))) {
     cThread::EmergencyExit(true);
     return osEnd;
     }
  return osContinue;
}

eOSState cMenuSetup::ProcessKey(eKeys Key)
{
  int osdLanguage = Setup.OSDLanguage;
  eOSState state = cOsdMenu::ProcessKey(Key);

  switch (state) {
    case osUser1: return AddSubMenu(new cMenuSetupOSD);
    case osUser2: return AddSubMenu(new cMenuSetupEPG);
    case osUser3: return AddSubMenu(new cMenuSetupDVB);
    case osUser4: return AddSubMenu(new cMenuSetupLNB);
    case osUser5: return AddSubMenu(new cMenuSetupCICAM);
    case osUser6: return AddSubMenu(new cMenuSetupRecord);
    case osUser7: return AddSubMenu(new cMenuSetupReplay);
    case osUser8: return AddSubMenu(new cMenuSetupMisc);
    case osUser9: return AddSubMenu(new cMenuSetupPlugins);
    case osUser10: return Restart();
    case osLiveBuffer: return AddSubMenu(new cMenuSetupLiveBuffer);
    default: ;
    }
  if (Setup.OSDLanguage != osdLanguage) {
     Set();
     if (!HasSubMenu())
        Display();
     }
  return state;
}

// --- cMenuPluginItem -------------------------------------------------------

class cMenuPluginItem : public cOsdItem {
private:
  int pluginIndex;
public:
  cMenuPluginItem(const char *Name, int Index);
  int PluginIndex(void) { return pluginIndex; }
  };

cMenuPluginItem::cMenuPluginItem(const char *Name, int Index)
:cOsdItem(Name, osPlugin)
{
  pluginIndex = Index;
}

// --- cMenuMain -------------------------------------------------------------

#define STOP_RECORDING tr(" Stop recording ")

cOsdObject *cMenuMain::pluginOsdObject = NULL;

cMenuMain::cMenuMain(eOSState State)
:cOsdMenu("")
{
  // Load Menu Configuration
  HelpMenus.Load();

  char *menuXML = NULL;
  asprintf(&menuXML, "%s/setup/vdr-menu.xml", cPlugin::ConfigDirectory());
  subMenu.LoadXml(menuXML);
  free(menuXML);

  nrDynamicMenuEntries=0;

  lastDiskSpaceCheck = 0;
  lastFreeMB = 0;
  replaying = false;
  stopReplayItem = NULL;
  cancelEditingItem = NULL;
  stopRecordingItem = NULL;
  recordControlsState = 0;
  Set();

  // Initial submenus:

  switch (State) {
    case osSchedule:   AddSubMenu(new cMenuSchedule); break;
    case osChannels:   if(Setup.UseBouquetList==1)
			 AddSubMenu(new cMenuBouquets(0));
		       else if(Setup.UseBouquetList==2)
			 AddSubMenu(new cMenuBouquets(1));
		       else
		         AddSubMenu(new cMenuChannels); break;
    case osTimers:     AddSubMenu(new cMenuTimers); break;
    case osRecordings: AddSubMenu(new cMenuRecordings(NULL, 0, true)); break;
    case osSetup:      AddSubMenu(new cMenuSetup); break;
    case osCommands:   AddSubMenu(new cMenuCommands(tr("Commands"), &Commands)); break;
    case osActiveEvent:AddSubMenu(new cMenuActiveEvent); break;
    default: break;
    }
}

cOsdObject *cMenuMain::PluginOsdObject(void)
{
  cOsdObject *o = pluginOsdObject;
  pluginOsdObject = NULL;
  return o;
}

void cMenuMain::Set(int current)
{
  Clear();
  SetTitle("VDR");
  SetHasHotkeys();

// *** START PATCH SETUP
  stopReplayItem = NULL;
  cancelEditingItem = NULL;
  stopRecordingItem = NULL;

  // remember initial dynamic MenuEntries added
  int index = 0;
  nrDynamicMenuEntries = Count();
  for (cSubMenuNode *node = subMenu.GetMenuTree()->First(); node;
       node = subMenu.GetMenuTree()->Next(node))
  {
    cSubMenuNode::Type type = node->GetType();
    if(type != cSubMenuNode::UNDEFINED)
    {
      if (!HasSubMenu() && current == index)
      {
         SetStatus(node->GetInfo());
      }
    }

    if(type==cSubMenuNode::PLUGIN)
    {
      const char *item = node->GetPluginMainMenuEntry();

      if(item)
        Add(new cMenuPluginItem(hk(item), node->GetPluginIndex()));
    }
    else if(type==cSubMenuNode::MENU)
    {
      char *item = NULL;
      asprintf(&item, "%s%s", tr(node->GetName()), subMenu.GetMenuSuffix());
      Add(new cOsdItem(hk(item)));
      free(item);
    }
    else
      if(type==cSubMenuNode::COMMAND)
    {
      Add(new cOsdItem(hk(tr(node->GetName()))));
    }
    else
      if(type==cSubMenuNode::SYSTEM)
    {
      const char *item = node->GetName();
      if(strcmp(item, "Schedule") == 0)
        Add(new cOsdItem(hk(tr("Schedule")),   osSchedule));
      else
        if(strcmp(item, "Channels") == 0)
          Add(new cOsdItem(hk(tr("Channels")),   osChannels));
      else
        if(strcmp(item, "Timers") == 0)
          Add(new cOsdItem(hk(tr("Timers")),   osTimers));
      else
        if(strcmp(item, "Recordings") == 0)
          Add(new cOsdItem(hk(tr("Recordings")),   osRecordings));
      else
        if(strcmp(item, "Setup") == 0)
          Add(new cOsdItem(hk(tr("Setup")),   osSetup));
      else
        if(strcmp(item, "Commands") == 0 && Commands.Count()>0)
          Add(new cOsdItem(hk(tr("Commands")),   osCommands));
    }
    index++;
  }
  if(current >=0 && current<Count())
  {
    SetCurrent(Get(current));
  }


// *** END PATCH SETUP

/* original
  // Basic menu items:

  Add(new cOsdItem(hk(tr("Schedule")),   osSchedule));
  Add(new cOsdItem(hk(tr("Channels")),   osChannels));
  Add(new cOsdItem(hk(tr("Timers")),     osTimers));
  Add(new cOsdItem(hk(tr("Recordings")), osRecordings));

  // Plugins:

  for (int i = 0; ; i++) {
      cPlugin *p = cPluginManager::GetPlugin(i);
      if (p) {
         const char *item = p->MainMenuEntry();
         if (item)
            Add(new cMenuPluginItem(hk(item), i));
         }
      else
         break;
      }

  // More basic menu items:

  Add(new cOsdItem(hk(tr("Setup")),      osSetup));
  if (Commands.Count())
     Add(new cOsdItem(hk(tr("Commands")),  osCommands));

----- end origiginal */

  Update(true);

  Display();
}

#define MB_PER_MINUTE 25.75 // this is just an estimate!

bool cMenuMain::Update(bool Force)
{
  bool result = false;
  cOsdItem *fMenu = NULL;
  if( Force && subMenu.isTopMenu())
  {
    fMenu = First();
    nrDynamicMenuEntries = 0;
  }

  if( subMenu.isTopMenu())
  {
  // Title with disk usage:
  if (Force || time(NULL) - lastDiskSpaceCheck > DISKSPACECHEK) {
     int FreeMB;
     int Percent = VideoDiskSpace(&FreeMB);
     if (Force || FreeMB != lastFreeMB) {
        int Minutes = int(double(FreeMB) / MB_PER_MINUTE);
        int Hours = Minutes / 60;
        Minutes %= 60;
        char buffer[40];
        snprintf(buffer, sizeof(buffer), "%s - %s %d%% %2d:%02dh %s", tr("Menu"), tr("Disk"), Percent, Hours, Minutes, tr("free"));
        //XXX -> skin function!!!
        SetTitle(buffer);
        result = true;
        }
     lastDiskSpaceCheck = time(NULL);
     }
   }
   else
   {
     SetTitle(tr(subMenu.GetParentMenuTitel()));
     return(true);
   }

  bool NewReplaying = cControl::Control() != NULL;
  if (Force || NewReplaying != replaying) {
     replaying = NewReplaying;
     // Replay control:
     if (replaying && !stopReplayItem)
        Add(stopReplayItem = new cOsdItem(tr(" Stop replaying"), osStopReplay));
     else if (stopReplayItem && !replaying) {
        Del(stopReplayItem->Index());
        stopReplayItem = NULL;
        }
     // Color buttons:
     SetHelp(!replaying ? tr("Button$Record") : NULL, tr("Button$Audio"), replaying ? NULL : tr("Button$Pause"), replaying ? tr("Button$Stop") : cReplayControl::LastReplayed() ? tr("Button$Resume") : NULL);
     result = true;
     }

  // Editing control:
  bool CutterActive = cCutter::Active();
  if (CutterActive && !cancelEditingItem) {
     Add(cancelEditingItem = new cOsdItem(tr(" Cancel editing"), osCancelEdit));
     result = true;
     }
  else if (cancelEditingItem && !CutterActive) {
     Del(cancelEditingItem->Index());
     cancelEditingItem = NULL;
     result = true;
     }

  // Record control:
  if (cRecordControls::StateChanged(recordControlsState)) {
     while (stopRecordingItem) {
           cOsdItem *it = Next(stopRecordingItem);
           Del(stopRecordingItem->Index());
           stopRecordingItem = it;
           }
     const char *s = NULL;
     while ((s = cRecordControls::GetInstantId(s)) != NULL) {
           char *buffer = NULL;
           asprintf(&buffer, "%s%s", STOP_RECORDING, s);
           cOsdItem *item = new cOsdItem(osStopRecord);
           item->SetText(buffer, false);
           Add(item);
           if (!stopRecordingItem)
              stopRecordingItem = item;
           }
     result = true;
     }

  // adjust nrDynamicMenuEntries
  if( fMenu != NULL)
     nrDynamicMenuEntries = fMenu->Index();

  return result;
}


eOSState cMenuMain::ProcessKey(eKeys Key)
{
  bool HadSubMenu = HasSubMenu();
  int osdLanguage = Setup.OSDLanguage;
  eOSState state = cOsdMenu::ProcessKey(Key);
  HadSubMenu |= HasSubMenu();

  switch (state) {
    case osSchedule:   return AddSubMenu(new cMenuSchedule);
    case osChannels:   return AddSubMenu(new cMenuChannels);
    case osTimers:     return AddSubMenu(new cMenuTimers);
    case osRecordings: return AddSubMenu(new cMenuRecordings);
    case osSetup:      return AddSubMenu(new cMenuSetup);
    case osCommands:   return AddSubMenu(new cMenuCommands(tr("Commands"), &Commands));
    case osStopRecord: if (Interface->Confirm(tr("Stop recording?"))) {
                          cOsdItem *item = Get(Current());
                          if (item) {
                             cRecordControls::Stop(item->Text() + strlen(STOP_RECORDING));
                             return osEnd;
                             }
                          }
                       break;
    case osCancelEdit: if (Interface->Confirm(tr("Cancel editing?"))) {
                          cCutter::Stop();
                          return osEnd;
                          }
                       break;
    case osPlugin:     {
                         cMenuPluginItem *item = (cMenuPluginItem *)Get(Current());
                         if (item) {
                            cPlugin *p = cPluginManager::GetPlugin(item->PluginIndex());
                            if (p) {
                               if ( strcmp(p->Name(), "setup") == 0)
                               {
                                   char *tmp = strrchr(item->Text(),' ');
                                   p->Service("link", (void *)++tmp);
                               }
                               if (!cStatus::MsgPluginProtected(p)) {  // PIN PATCH
                               cOsdObject *menu = p->MainMenuAction();
                               if (menu) {
                                  if (menu->IsMenu())
                                     return AddSubMenu((cOsdMenu *)menu);
                                  else {
                                     pluginOsdObject = menu;
                                     return osPlugin;
                                     }
                                  }
                               }
                            }
                         }
                         state = osEnd;
                       }
                       break;
    case osBack:   {
                    int newCurrent =0;
                    if (subMenu.Up(&newCurrent) )
                    {
                     Set(newCurrent);
                     return osContinue;
                    }
                    else
                        return osEnd;
                    }
                    break;
    default: switch (Key) {

               //case kInfo: if (!HadSubMenu) return DisplayHelp((Current() - nrDynamicMenuEntries));
               case kRecord:
               case kRed:    if (!HadSubMenu)
                                state = replaying ? osContinue : osRecord;
                             break;
               case kGreen:  if (!HadSubMenu) {
                                cRemote::Put(kAudio, true);
                                state = osEnd;
                                }
                             break;
               case kYellow: if (!HadSubMenu)
                                state = replaying ? osContinue : osPause;
                             break;
               case kBlue:   if (!HadSubMenu)
                                state = replaying ? osStopReplay : cReplayControl::LastReplayed() ? osReplay : osContinue;
                             break;
               case kOk:    if(state == osUnknown)
                            {
                              int index = Current()-nrDynamicMenuEntries;
                              cSubMenuNode *node = subMenu.GetNode(index);

                              if ( node != NULL)
                              {
                                if(node->GetType() == cSubMenuNode::MENU)
                                {
                                  subMenu.Down(index);
                                }
                                else
                                  if(node->GetType() == cSubMenuNode::COMMAND)
                                {
                                  char *buffer = NULL;
                                  bool confirmed = true;
                                  if( node->CommandConfirm())
                                  {
                                    asprintf(&buffer, "%s?", node->GetName());
                                    confirmed = Interface->Confirm(buffer);
                                    free(buffer);
                                  }
                                  if (confirmed)
                                  {
                                    asprintf(&buffer, "%s...", node->GetName());
                                    Skins.Message(mtStatus, buffer);
                                    free(buffer);
                                    const char *Result = subMenu.ExecuteCommand(node->GetCommand());
                                    Skins.Message(mtStatus, NULL);
                                    if (Result)
                                      return AddSubMenu(new cMenuText(node->GetName(), Result, fontFix));

                                    return osEnd;
                                  }
                                }
                              }

                              Set();
                              return state;
                            }
               break;
               default:  break;
        }
    }
  if (!HasSubMenu() && Update(HadSubMenu))
     Display();
  if (Key != kNone) {

      int index = Current() - nrDynamicMenuEntries;
      cSubMenuNode *node = subMenu.GetNode(index);

      if (!HasSubMenu() && node)
      {
         if (node->GetInfo())
              SetStatus(node->GetInfo());
         else
           SetStatus(NULL);
      }
     if (Setup.OSDLanguage != osdLanguage) {
        Set();
        if (!HasSubMenu())
           Display();
        }
     }
  return state;
}

// --- SetTrackDescriptions --------------------------------------------------

static void SetTrackDescriptions(int LiveChannel)
{
  cDevice::PrimaryDevice()->ClrAvailableTracks(true);
  const cComponents *Components = NULL;
  cSchedulesLock SchedulesLock;
  if (LiveChannel) {
     cChannel *Channel = Channels.GetByNumber(LiveChannel);
     if (Channel) {
        const cSchedules *Schedules = cSchedules::Schedules(SchedulesLock);
        if (Schedules) {
           const cSchedule *Schedule = Schedules->GetSchedule(Channel);
           if (Schedule) {
              const cEvent *Present = Schedule->GetPresentEvent();
              if (Present)
                 Components = Present->Components();
              }
           }
        }
     }
  else if (cReplayControl::NowReplaying()) {
     cThreadLock RecordingsLock(&Recordings);
     cRecording *Recording = Recordings.GetByName(cReplayControl::NowReplaying());
     if (Recording)
        Components = Recording->Info()->Components();
     }
  if (Components) {
     int indexAudio = 0;
     int indexDolby = 0;
     for (int i = 0; i < Components->NumComponents(); i++) {
         const tComponent *p = Components->Component(i);
         if (p->stream == 2) {
            if (p->type == 0x05)
               cDevice::PrimaryDevice()->SetAvailableTrack(ttDolby, indexDolby++, 0, LiveChannel ? NULL : p->language, p->description);
            else
               cDevice::PrimaryDevice()->SetAvailableTrack(ttAudio, indexAudio++, 0, LiveChannel ? NULL : p->language, p->description);
            }
         }
     }
}

// --- cDisplayChannel -------------------------------------------------------

#define DIRECTCHANNELTIMEOUT 1500 //ms

cDisplayChannel *cDisplayChannel::currentDisplayChannel = NULL;

cDisplayChannel::cDisplayChannel(int Number, bool Switched)
:cOsdObject(true)
{
  currentDisplayChannel = this;
  group = -1;
  withInfo = !Switched || Setup.ShowInfoOnChSwitch;
  displayChannel = Skins.Current()->DisplayChannel(withInfo);
  number = 0;
  timeout = Switched || Setup.TimeoutRequChInfo;
  channel = Channels.GetByNumber(Number);
  lastPresent = lastFollowing = NULL;
  if (channel) {
     DisplayChannel();
     DisplayInfo();
     displayChannel->Flush();
     }
  lastTime.Set();
}

cDisplayChannel::cDisplayChannel(eKeys FirstKey)
:cOsdObject(true)
{
  currentDisplayChannel = this;
  group = -1;
  number = 0;
  timeout = true;
  lastPresent = lastFollowing = NULL;
  lastTime.Set();
  withInfo = Setup.ShowInfoOnChSwitch;
  displayChannel = Skins.Current()->DisplayChannel(withInfo);
  channel = Channels.GetByNumber(cDevice::CurrentChannel());
  ProcessKey(FirstKey);
}

cDisplayChannel::~cDisplayChannel()
{
  delete displayChannel;
  cStatus::MsgOsdClear();
  currentDisplayChannel = NULL;
}

void cDisplayChannel::DisplayChannel(void)
{
  displayChannel->SetChannel(channel, number);
  cStatus::MsgOsdChannel(ChannelString(channel, number));
  lastPresent = lastFollowing = NULL;
}

void cDisplayChannel::DisplayInfo(void)
{
  if (withInfo && channel) {
     cSchedulesLock SchedulesLock;
     const cSchedules *Schedules = cSchedules::Schedules(SchedulesLock);
     if (Schedules) {
        const cSchedule *Schedule = Schedules->GetSchedule(channel);
        if (Schedule) {
           const cEvent *Present = Schedule->GetPresentEvent();
           const cEvent *Following = Schedule->GetFollowingEvent();
           if (Present != lastPresent || Following != lastFollowing) {
              SetTrackDescriptions(channel->Number());
              displayChannel->SetEvents(Present, Following);
              cStatus::MsgOsdProgramme(Present ? Present->StartTime() : 0, Present ? Present->Title() : NULL, Present ? Present->ShortText() : NULL, Following ? Following->StartTime() : 0, Following ? Following->Title() : NULL, Following ? Following->ShortText() : NULL);
              lastPresent = Present;
              lastFollowing = Following;
              }
           }
        }
     }
}

void cDisplayChannel::Refresh(void)
{
  DisplayChannel();
  displayChannel->SetEvents(NULL, NULL);
}

cChannel *cDisplayChannel::NextAvailableChannel(cChannel *Channel, int Direction)
{
  if (Direction) {
     while (Channel) {
           Channel = Direction > 0 ? Channels.Next(Channel) : Channels.Prev(Channel);
	if (cStatus::MsgChannelProtected(0, Channel) == false)                     // PIN PATCH
           if (Channel && !Channel->GroupSep() && (cDevice::PrimaryDevice()->ProvidesChannel(Channel, Setup.PrimaryLimit) || cDevice::GetDevice(Channel, 0)))
              return Channel;
           }
     }
  return NULL;
}

eOSState cDisplayChannel::ProcessKey(eKeys Key)
{
  cChannel *NewChannel = NULL;
  if (Key != kNone)
     lastTime.Set();
  switch (Key) {
    case k0:
         if (number == 0) {
            // keep the "Toggle channels" function working
            cRemote::Put(Key);
            return osEnd;
            }
    case k1 ... k9:
         group = -1;
         if (number >= 0) {
            if (number > Channels.MaxNumber())
               number = Key - k0;
            else
               number = number * 10 + Key - k0;
            channel = Channels.GetByNumber(number);
            Refresh();
            withInfo = false;
            // Lets see if there can be any useful further input:
            int n = channel ? number * 10 : 0;
            int m = 10;
            cChannel *ch = channel;
            while (ch && (ch = Channels.Next(ch)) != NULL) {
                  if (!ch->GroupSep()) {
                     if (n <= ch->Number() && ch->Number() < n + m) {
                        n = 0;
                        break;
                        }
                     if (ch->Number() > n) {
                        n *= 10;
                        m *= 10;
                        }
                     }
                  }
            if (n > 0) {
               // This channel is the only one that fits the input, so let's take it right away:
               NewChannel = channel;
               withInfo = true;
               number = 0;
               Refresh();
               }
            }
         break;
    case kLeft|k_Repeat:
    case kLeft:
    case kRight|k_Repeat:
    case kRight:
    case kNext|k_Repeat:
    case kNext:
    case kPrev|k_Repeat:
    case kPrev:
         withInfo = false;
         number = 0;
         if (group < 0) {
            cChannel *channel = Channels.GetByNumber(cDevice::CurrentChannel());
            if (channel)
               group = channel->Index();
            }
         if (group >= 0) {
            int SaveGroup = group;
            if (NORMALKEY(Key) == kRight || NORMALKEY(Key) == kNext)
               group = Channels.GetNextGroup(group) ;
            else
               group = Channels.GetPrevGroup(group < 1 ? 1 : group);
            if (group < 0)
               group = SaveGroup;
            channel = Channels.Get(group);
            if (channel) {
               Refresh();
               if (!channel->GroupSep())
                  group = -1;
               }
            }
         break;
    case kUp|k_Repeat:
    case kUp:
    case kDown|k_Repeat:
    case kDown:
    case kChanUp|k_Repeat:
    case kChanUp:
    case kChanDn|k_Repeat:
    case kChanDn: {
         eKeys k = NORMALKEY(Key);
         cChannel *ch = NextAvailableChannel(channel, (k == kUp || k == kChanUp) ? 1 : -1);
         if (ch)
            channel = ch;
         else if (channel && channel->Number() != cDevice::CurrentChannel())
            Key = k; // immediately switches channel when hitting the beginning/end of the channel list with k_Repeat
         }
         // no break here
    case kUp|k_Release:
    case kDown|k_Release:
    case kChanUp|k_Release:
    case kChanDn|k_Release:
    case kNext|k_Release:
    case kPrev|k_Release:
         if (!(Key & k_Repeat) && channel && channel->Number() != cDevice::CurrentChannel())
            NewChannel = channel;
         withInfo = true;
         group = -1;
         number = 0;
         Refresh();
         break;
    case kNone:
         if (number && lastTime.Elapsed() > DIRECTCHANNELTIMEOUT) {
            channel = Channels.GetByNumber(number);
            if (channel)
               NewChannel = channel;
            withInfo = true;
            number = 0;
            Refresh();
            lastTime.Set();
            }
         break;
    //TODO
    //XXX case kGreen:  return osEventNow;
    //XXX case kYellow: return osEventNext;
    case kOk:
         if (group >= 0) {
            channel = Channels.Get(Channels.GetNextNormal(group));
            if (channel)
               NewChannel = channel;
            withInfo = true;
            group = -1;
            Refresh();
            }
         else if (number > 0) {
            channel = Channels.GetByNumber(number);
            if (channel)
               NewChannel = channel;
            withInfo = true;
            number = 0;
            Refresh();
            }
         else
            return osEnd;
         break;
    default:
         if ((Key & (k_Repeat | k_Release)) == 0) {
            cRemote::Put(Key);
            return osEnd;
            }
    };
  if (!timeout || lastTime.Elapsed() < (uint64_t)(Setup.ChannelInfoTime * 1000)) {
     if (Key == kNone && !number && group < 0 && !NewChannel && channel && channel->Number() != cDevice::CurrentChannel()) {
        // makes sure a channel switch through the SVDRP CHAN command is displayed
        channel = Channels.GetByNumber(cDevice::CurrentChannel());
        Refresh();
        lastTime.Set();
        }
     DisplayInfo();
     displayChannel->Flush();
     if (NewChannel) {
        SetTrackDescriptions(NewChannel->Number()); // to make them immediately visible in the channel display
        Channels.SwitchTo(NewChannel->Number());
        SetTrackDescriptions(NewChannel->Number()); // switching the channel has cleared them
        channel = NewChannel;
        }
     return osContinue;
     }
  return osEnd;
}

// --- cDisplayVolume --------------------------------------------------------

#define VOLUMETIMEOUT 1000 //ms
#define MUTETIMEOUT   5000 //ms

cDisplayVolume *cDisplayVolume::currentDisplayVolume = NULL;

cDisplayVolume::cDisplayVolume(void)
:cOsdObject(true)
{
  currentDisplayVolume = this;
  timeout.Set(cDevice::PrimaryDevice()->IsMute() ? MUTETIMEOUT : VOLUMETIMEOUT);
  displayVolume = Skins.Current()->DisplayVolume();
  Show();
}

cDisplayVolume::~cDisplayVolume()
{
  delete displayVolume;
  currentDisplayVolume = NULL;
}

void cDisplayVolume::Show(void)
{
  displayVolume->SetVolume(cDevice::CurrentVolume(), MAXVOLUME, cDevice::PrimaryDevice()->IsMute());
}

cDisplayVolume *cDisplayVolume::Create(void)
{
  if (!currentDisplayVolume)
     new cDisplayVolume;
  return currentDisplayVolume;
}

void cDisplayVolume::Process(eKeys Key)
{
  if (currentDisplayVolume)
     currentDisplayVolume->ProcessKey(Key);
}

eOSState cDisplayVolume::ProcessKey(eKeys Key)
{
  switch (Key) {
    case kVolUp|k_Repeat:
    case kVolUp:
    case kVolDn|k_Repeat:
    case kVolDn:
         Show();
         timeout.Set(VOLUMETIMEOUT);
         break;
    case kMute:
         if (cDevice::PrimaryDevice()->IsMute()) {
            Show();
            timeout.Set(MUTETIMEOUT);
            }
         else
            timeout.Set();
         break;
    case kNone: break;
    default: if ((Key & k_Release) == 0) {
                cRemote::Put(Key);
                return osEnd;
                }
    }
  return timeout.TimedOut() ? osEnd : osContinue;
}

// --- cDisplayTracks --------------------------------------------------------

#define TRACKTIMEOUT 5000 //ms

cDisplayTracks *cDisplayTracks::currentDisplayTracks = NULL;

cDisplayTracks::cDisplayTracks(void)
:cOsdObject(true)
{
  cDevice::PrimaryDevice()->EnsureAudioTrack();
  SetTrackDescriptions(!cDevice::PrimaryDevice()->Replaying() || cDevice::PrimaryDevice()->Transferring() ? cDevice::CurrentChannel() : 0);
  currentDisplayTracks = this;
  numTracks = track = 0;
  audioChannel = cDevice::PrimaryDevice()->GetAudioChannel();
  eTrackType CurrentAudioTrack = cDevice::PrimaryDevice()->GetCurrentAudioTrack();
  for (int i = ttAudioFirst; i <= ttDolbyLast; i++) {
      const tTrackId *TrackId = cDevice::PrimaryDevice()->GetTrack(eTrackType(i));
      if (TrackId && TrackId->id) {
         types[numTracks] = eTrackType(i);
         descriptions[numTracks] = strdup(*TrackId->description ? TrackId->description : *TrackId->language ? TrackId->language : *itoa(i));
         if (i == CurrentAudioTrack)
            track = numTracks;
         numTracks++;
         }
      }
  timeout.Set(TRACKTIMEOUT);
  displayTracks = Skins.Current()->DisplayTracks(tr("Button$Audio"), numTracks, descriptions);
  Show();
}

cDisplayTracks::~cDisplayTracks()
{
  delete displayTracks;
  currentDisplayTracks = NULL;
  for (int i = 0; i < numTracks; i++)
      free(descriptions[i]);
  cStatus::MsgOsdClear();
}

void cDisplayTracks::Show(void)
{
  int ac = IS_AUDIO_TRACK(types[track]) ? audioChannel : -1;
  displayTracks->SetTrack(track, descriptions);
  displayTracks->SetAudioChannel(ac);
  displayTracks->Flush();
  cStatus::MsgSetAudioTrack(track, descriptions);
  cStatus::MsgSetAudioChannel(ac);
}

cDisplayTracks *cDisplayTracks::Create(void)
{
  if (cDevice::PrimaryDevice()->NumAudioTracks() > 0) {
     if (!currentDisplayTracks)
        new cDisplayTracks;
     return currentDisplayTracks;
     }
  Skins.Message(mtWarning, tr("No audio available!"));
  return NULL;
}

void cDisplayTracks::Process(eKeys Key)
{
  if (currentDisplayTracks)
     currentDisplayTracks->ProcessKey(Key);
}

eOSState cDisplayTracks::ProcessKey(eKeys Key)
{
  int oldTrack = track;
  int oldAudioChannel = audioChannel;
  switch (Key) {
    case kUp|k_Repeat:
    case kUp:
    case kDown|k_Repeat:
    case kDown:
         if (NORMALKEY(Key) == kUp && track > 0)
            track--;
         else if (NORMALKEY(Key) == kDown && track < numTracks - 1)
            track++;
         timeout.Set(TRACKTIMEOUT);
         break;
    case kLeft|k_Repeat:
    case kLeft:
    case kRight|k_Repeat:
    case kRight: if (IS_AUDIO_TRACK(types[track])) {
                    static int ac[] = { 1, 0, 2 };
                    audioChannel = ac[cDevice::PrimaryDevice()->GetAudioChannel()];
                    if (NORMALKEY(Key) == kLeft && audioChannel > 0)
                       audioChannel--;
                    else if (NORMALKEY(Key) == kRight && audioChannel < 2)
                       audioChannel++;
                    audioChannel = ac[audioChannel];
                    timeout.Set(TRACKTIMEOUT);
                    }
         break;
    case kAudio|k_Repeat:
    case kAudio:
         if (++track >= numTracks)
            track = 0;
         timeout.Set(TRACKTIMEOUT);
         break;
    case kOk:
         if (track != cDevice::PrimaryDevice()->GetCurrentAudioTrack())
            oldTrack = -1; // make sure we explicitly switch to that track
         timeout.Set();
         break;
    case kNone: break;
    default: if ((Key & k_Release) == 0)
                return osEnd;
    }
  if (track != oldTrack || audioChannel != oldAudioChannel)
     Show();
  if (track != oldTrack) {
     cDevice::PrimaryDevice()->SetCurrentAudioTrack(types[track]);
     Setup.CurrentDolby = IS_DOLBY_TRACK(types[track]);
     }
  if (audioChannel != oldAudioChannel)
     cDevice::PrimaryDevice()->SetAudioChannel(audioChannel);
  return timeout.TimedOut() ? osEnd : osContinue;
}

// --- cRecordControl --------------------------------------------------------

cRecordControl::cRecordControl(cDevice *Device, cTimer *Timer, bool Pause)
{
  // We're going to manipulate an event here, so we need to prevent
  // others from modifying any EPG data:
  cSchedulesLock SchedulesLock;
  cSchedules::Schedules(SchedulesLock);

  event = NULL;
  instantId = NULL;
  fileName = NULL;
  recorder = NULL;
  device = Device;
  if (!device) device = cDevice::PrimaryDevice();//XXX
  timer = Timer;
  if (!timer) {
     timer = new cTimer(true, Pause);
     Timers.Add(timer);
     Timers.SetModified();
     asprintf(&instantId, cDevice::NumDevices() > 1 ? "%s - %d" : "%s", timer->Channel()->Name(), device->CardIndex() + 1);
     }
  else if (timer->HasFlags(tfInstant))
     asprintf(&instantId, cDevice::NumDevices() > 1 ? "%s - %d" : "%s", timer->Channel()->Name(), device->CardIndex() + 1);
  timer->SetPending(true);
  timer->SetRecording(true);
  event = timer->Event();

  if (event || GetEvent())
     dsyslog("Title: '%s' Subtitle: '%s'", event->Title(), event->ShortText());
  cRecording Recording(timer, event);
  fileName = strdup(Recording.FileName());

  // crude attempt to avoid duplicate recordings:
  if (cRecordControls::GetRecordControl(fileName)) {
     isyslog("already recording: '%s'", fileName);
     if (Timer) {
        timer->SetPending(false);
        timer->SetRecording(false);
        timer->OnOff();
        }
     else {
        Timers.Del(timer);
        Timers.SetModified();
        if (!cReplayControl::LastReplayed()) // an instant recording, maybe from cRecordControls::PauseLiveVideo()
           cReplayControl::SetRecording(fileName, Recording.Name());
        }
     timer = NULL;
     return;
     }

  cRecordingUserCommand::InvokeCommand(RUC_BEFORERECORDING, fileName);
  isyslog("record %s", fileName);
  if (MakeDirs(fileName, true)) {
     const cChannel *ch = timer->Channel();
     int startFrame=-1;
     int endFrame=0;
     cLiveBuffer *liveBuffer = cLiveBufferManager::InLiveBuffer(timer, &startFrame, &endFrame);
     if (liveBuffer) {
        timer->SetFlags(tfhasLiveBuf);
        liveBuffer->SetStartFrame(startFrame);
        if (endFrame) {
           liveBuffer->CreateIndexFile(fileName, 0, endFrame);
           Timers.Del(timer);
           Timers.SetModified();
           timer = NULL;
           Recording.WriteInfo();
           Recordings.AddByName(fileName);
           return;
           }
     }
     recorder = new cRecorder(fileName, ch->Ca(), timer->Priority(), ch->Vpid(), ch->Apids(), ch->Dpids(), ch->Spids(), liveBuffer);
     if (device->AttachReceiver(recorder)) {
        time_t start_t=time(0);
        while(recorder->GetRemux()->SFmode()==SF_UNKNOWN && (time(0)-start_t)<2) 
           usleep(50*1000); //TB: give recorder's remux a chance to detect mode
        if(recorder->GetRemux()->SFmode()==SF_H264){
          Recording.SetIsHD(true);
        } 
        if(recorder->GetRemux()->TSmode()==rTS){
          Recording.SetIsTS(true);
        }
        Recording.WriteInfo();
        cStatus::MsgRecording(device, Recording.Name(), Recording.FileName(), true, ch->Number());
        if (!Timer || Timer->HasFlags(tfInstant) && !cReplayControl::LastReplayed()) // an instant recording, maybe from cRecordControls::PauseLiveVideo()
           cReplayControl::SetRecording(fileName, Recording.Name());
        Recordings.AddByName(fileName);
        return;
        }
     else
        DELETENULL(recorder);
     }
  if (!Timer) {
     Timers.Del(timer);
     Timers.SetModified();
     timer = NULL;
     }
}

cRecordControl::~cRecordControl()
{
  Stop();
  free(instantId);
  free(fileName);
}

#define INSTANT_REC_EPG_LOOKAHEAD 300 // seconds to look into the EPG data for an instant recording

bool cRecordControl::GetEvent(void)
{
  const cChannel *channel = timer->Channel();
  time_t Time = timer->HasFlags(tfInstant) ? timer->StartTime() + INSTANT_REC_EPG_LOOKAHEAD : timer->StartTime() + (timer->StopTime() - timer->StartTime()) / 2;
  for (int seconds = 0; seconds <= MAXWAIT4EPGINFO; seconds++) {
      {
        cSchedulesLock SchedulesLock;
        const cSchedules *Schedules = cSchedules::Schedules(SchedulesLock);
        if (Schedules) {
           const cSchedule *Schedule = Schedules->GetSchedule(channel);
           if (Schedule) {
              event = Schedule->GetEventAround(Time);
              if (event) {
                 if (seconds > 0)
                    dsyslog("got EPG info after %d seconds", seconds);
                 return true;
                 }
              }
           }
      }
      if (seconds == 0)
         dsyslog("waiting for EPG info...");
      sleep(1);
      }
  dsyslog("no EPG info available");
  return false;
}

void cRecordControl::Stop(void)
{
  if (timer) {
     DELETENULL(recorder);
     timer->SetRecording(false);
     cStatus::MsgRecording(device, NULL, fileName, false, timer->Channel()->Number());
     timer = NULL;
     cRecordingUserCommand::InvokeCommand(RUC_AFTERRECORDING, fileName);
     }
}

bool cRecordControl::Process(time_t t)
{
  if (!recorder || !timer || !timer->Matches(t))
     return false;
  AssertFreeDiskSpace(timer->Priority());
  return true;
}

// --- cRecordControls -------------------------------------------------------

cRecordControl *cRecordControls::RecordControls[MAXRECORDCONTROLS] = { NULL };
int cRecordControls::state = 0;

bool cRecordControls::Start(cTimer *Timer, bool Pause)
{
  static time_t LastNoDiskSpaceMessage = 0;
  int FreeMB = 0;
  if (Timer) {
     AssertFreeDiskSpace(Timer->Priority(), !Timer->Pending());
     Timer->SetPending(true);
     }
  VideoDiskSpace(&FreeMB);
  if (FreeMB < MINFREEDISK) {
     if (!Timer || time(NULL) - LastNoDiskSpaceMessage > NODISKSPACEDELTA) {
        isyslog("not enough disk space to start recording%s%s", Timer ? " timer " : "", Timer ? *Timer->ToDescr() : "");
        Skins.Message(mtWarning, tr("Not enough disk space to start recording!"));
        LastNoDiskSpaceMessage = time(NULL);
        }
     return false;
     }
  LastNoDiskSpaceMessage = 0;

  ChangeState();
  int ch = Timer ? Timer->Channel()->Number() : cDevice::CurrentChannel();
  cChannel *channel = Channels.GetByNumber(ch);

  if (channel) {
     bool NeedsDetachReceivers = false;
     int Priority = Timer ? Timer->Priority() : Pause ? Setup.PausePriority : Setup.DefaultPriority;
     cDevice *device = cDevice::GetDevice(channel, Priority, &NeedsDetachReceivers);
     if (device) {
        if (NeedsDetachReceivers) {
           Stop(device);
           if (device == cTransferControl::ReceiverDevice())
              cControl::Shutdown(); // in case this device was used for Transfer Mode
           }
        dsyslog("switching device %d to channel %d", device->DeviceNumber() + 1, channel->Number());
        if (!device->SwitchChannel(channel, false)) {
           cThread::EmergencyExit(true);
           return false;
           }
        if (!Timer || Timer->Matches() || cLiveBufferManager::InLiveBuffer(Timer)) {
           for (int i = 0; i < MAXRECORDCONTROLS; i++) {
               if (!RecordControls[i]) {
                  RecordControls[i] = new cRecordControl(device, Timer, Pause);
                  cStatus::MsgRecordingFile(RecordControls[i]->FileName());  // PIN PATCH
                  return RecordControls[i]->Process(time(NULL));
                  }
               }
           }
        }
     else if (!Timer || (Timer->Priority() >= Setup.PrimaryLimit && !Timer->Pending())) {
        isyslog("no free DVB device to record channel %d!", ch);
        Skins.Message(mtError, tr("No free DVB device to record!"));
        }
     }
  else
     esyslog("ERROR: channel %d not defined!", ch);
  return false;
}

void cRecordControls::Stop(const char *InstantId)
{
  ChangeState();
  for (int i = 0; i < MAXRECORDCONTROLS; i++) {
      if (RecordControls[i]) {
         const char *id = RecordControls[i]->InstantId();
         if (id && strcmp(id, InstantId) == 0) {
            cTimer *timer = RecordControls[i]->Timer();
            RecordControls[i]->Stop();
            if (timer) {
               isyslog("deleting timer %s", *timer->ToDescr());
               Timers.Del(timer);
               Timers.SetModified();
               }
            break;
            }
         }
      }
}

void cRecordControls::Stop(cDevice *Device)
{
  ChangeState();
  for (int i = 0; i < MAXRECORDCONTROLS; i++) {
      if (RecordControls[i]) {
         if (RecordControls[i]->Device() == Device) {
            isyslog("stopping recording on DVB device %d due to higher priority", Device->CardIndex() + 1);
            RecordControls[i]->Stop();
            }
         }
      }
}

bool cRecordControls::PauseLiveVideo(bool fromLiveBuffer)
{
  Skins.Message(mtStatus, tr("Pausing live video..."));
  cReplayControl::SetRecording(NULL, NULL); // make sure the new cRecordControl will set cReplayControl::LastReplayed()
  cTimer *timer = NULL;
  if (fromLiveBuffer)
     timer = cLiveBufferManager::Timer(-1);
  if (Start(timer, true)) {
     sleep(2); // allow recorded file to fill up enough to start replaying
     cReplayControl *rc = new cReplayControl;
     cControl::Launch(rc);
     cControl::Attach();
     sleep(1); // allow device to replay some frames, so we have a picture
     Skins.Message(mtStatus, NULL);
     rc->ProcessKey(kPause); // pause, allowing replay mode display
     return true;
     }
  Skins.Message(mtStatus, NULL);
  return false;
}

const char *cRecordControls::GetInstantId(const char *LastInstantId)
{
  for (int i = 0; i < MAXRECORDCONTROLS; i++) {
      if (RecordControls[i]) {
         if (!LastInstantId && RecordControls[i]->InstantId())
            return RecordControls[i]->InstantId();
         if (LastInstantId && LastInstantId == RecordControls[i]->InstantId())
            LastInstantId = NULL;
         }
      }
  return NULL;
}

cRecordControl *cRecordControls::GetRecordControl(const char *FileName)
{
  for (int i = 0; i < MAXRECORDCONTROLS; i++) {
      if (RecordControls[i] && strcmp(RecordControls[i]->FileName(), FileName) == 0)
         return RecordControls[i];
      }
  return NULL;
}

void cRecordControls::Process(time_t t)
{
  for (int i = 0; i < MAXRECORDCONTROLS; i++) {
      if (RecordControls[i]) {
         if (!RecordControls[i]->Process(t)) {
            DELETENULL(RecordControls[i]);
            ChangeState();
            }
         }
      }
}

void cRecordControls::ChannelDataModified(cChannel *Channel)
{
  for (int i = 0; i < MAXRECORDCONTROLS; i++) {
      if (RecordControls[i]) {
         if (RecordControls[i]->Timer() && RecordControls[i]->Timer()->Channel() == Channel) {
            if (RecordControls[i]->Device()->ProvidesTransponder(Channel)) { // avoids retune on devices that don't really access the transponder
               isyslog("stopping recording due to modification of channel %d", Channel->Number());
               RecordControls[i]->Stop();
               // This will restart the recording, maybe even from a different
               // device in case conditional access has changed.
               ChangeState();
               }
            }
         }
      }
}

bool cRecordControls::Active(void)
{
  for (int i = 0; i < MAXRECORDCONTROLS; i++) {
      if (RecordControls[i])
         return true;
      }
  return false;
}

void cRecordControls::Shutdown(void)
{
  for (int i = 0; i < MAXRECORDCONTROLS; i++)
      DELETENULL(RecordControls[i]);
  ChangeState();
}

bool cRecordControls::StateChanged(int &State)
{
  int NewState = state;
  bool Result = State != NewState;
  State = state;
  return Result;
}

// --- cReplayControl --------------------------------------------------------

cReplayControl *cReplayControl::currentReplayControl = NULL;
char *cReplayControl::fileName = NULL;
char *cReplayControl::title = NULL;

cReplayControl::cReplayControl(void)
:cDvbPlayerControl(fileName), marks(fileName)
{
  currentReplayControl = this;
  displayReplay = NULL;
  visible = modeOnly = shown = displayFrames = false;
  lastCurrent = lastTotal = -1;
  lastPlay = lastForward = false;
  lastSpeed = -2;
  timeoutShow = 0;
  timeSearchActive = false;
  cRecording Recording(fileName);
  cStatus::MsgReplaying(this, Recording.Name(), Recording.FileName(), true);
  SetTrackDescriptions(false);
}

cReplayControl::~cReplayControl()
{
  Hide();
  cStatus::MsgReplaying(this, NULL, fileName, false);
  Stop();
  if (currentReplayControl == this)
     currentReplayControl = NULL;
}

void cReplayControl::SetRecording(const char *FileName, const char *Title)
{
  free(fileName);
  free(title);
  fileName = FileName ? strdup(FileName) : NULL;
  title = Title ? strdup(Title) : NULL;
}

const char *cReplayControl::NowReplaying(void)
{
  return currentReplayControl ? fileName : NULL;
}

const char *cReplayControl::LastReplayed(void)
{
  return fileName;
}

void cReplayControl::ClearLastReplayed(const char *FileName)
{
  if (fileName && FileName && strcmp(fileName, FileName) == 0) {
     free(fileName);
     fileName = NULL;
     }
}

void cReplayControl::ShowTimed(int Seconds)
{
  if (modeOnly)
     Hide();
  if (!visible) {
     shown = ShowProgress(true);
     timeoutShow = (shown && Seconds > 0) ? time(NULL) + Seconds : 0;
     }
}

void cReplayControl::Show(void)
{
  ShowTimed();
}

void cReplayControl::Hide(void)
{
  if (visible) {
     delete displayReplay;
     displayReplay = NULL;
     needsFastResponse = visible = false;
     modeOnly = false;
     lastPlay = lastForward = false;
     lastSpeed = -2; // an invalid value
     timeSearchActive = false;
     }
}

void cReplayControl::ShowMode(void)
{
  if (visible || Setup.ShowReplayMode && !cOsd::IsOpen()) {
     bool Play, Forward;
     int Speed;
     if (GetReplayMode(Play, Forward, Speed) && (!visible || Play != lastPlay || Forward != lastForward || Speed != lastSpeed)) {
        bool NormalPlay = (Play && Speed == -1);

        if (!visible) {
           if (NormalPlay)
              return; // no need to do indicate ">" unless there was a different mode displayed before
           visible = modeOnly = true;
           displayReplay = Skins.Current()->DisplayReplay(modeOnly);
           }

        if (modeOnly && !timeoutShow && NormalPlay)
           timeoutShow = time(NULL) + MODETIMEOUT;
        displayReplay->SetMode(Play, Forward, Speed);
        lastPlay = Play;
        lastForward = Forward;
        lastSpeed = Speed;
        }
     }
}

bool cReplayControl::ShowProgress(bool Initial)
{
  int Current, Total;

  if (GetIndex(Current, Total) && Total > 0) {
     if (!visible) {
        displayReplay = Skins.Current()->DisplayReplay(modeOnly);
        displayReplay->SetMarks(&marks);
        needsFastResponse = visible = true;
        }
     if (Initial) {
        if (title)
           displayReplay->SetTitle(title);
        lastCurrent = lastTotal = -1;
        }
     if (Total != lastTotal) {
        displayReplay->SetTotal(IndexToHMSF(Total));
        if (!Initial)
           displayReplay->Flush();
        }
     if (Current != lastCurrent || Total != lastTotal) {
        displayReplay->SetProgress(Current, Total);
        if (!Initial)
           displayReplay->Flush();
        displayReplay->SetCurrent(IndexToHMSF(Current, displayFrames));
        displayReplay->Flush();
        lastCurrent = Current;
        }
     lastTotal = Total;
     ShowMode();
     return true;
     }
  return false;
}

void cReplayControl::TimeSearchDisplay(void)
{
  char buf[64];
  strcpy(buf, tr("Jump: "));
  int len = strlen(buf);
  char h10 = '0' + (timeSearchTime >> 24);
  char h1  = '0' + ((timeSearchTime & 0x00FF0000) >> 16);
  char m10 = '0' + ((timeSearchTime & 0x0000FF00) >> 8);
  char m1  = '0' + (timeSearchTime & 0x000000FF);
  char ch10 = timeSearchPos > 3 ? h10 : '-';
  char ch1  = timeSearchPos > 2 ? h1  : '-';
  char cm10 = timeSearchPos > 1 ? m10 : '-';
  char cm1  = timeSearchPos > 0 ? m1  : '-';
  sprintf(buf + len, "%c%c:%c%c", ch10, ch1, cm10, cm1);
  displayReplay->SetJump(buf);
}

void cReplayControl::TimeSearchProcess(eKeys Key)
{
#define STAY_SECONDS_OFF_END 10
  int Seconds = (timeSearchTime >> 24) * 36000 + ((timeSearchTime & 0x00FF0000) >> 16) * 3600 + ((timeSearchTime & 0x0000FF00) >> 8) * 600 + (timeSearchTime & 0x000000FF) * 60;
  int Current = (lastCurrent / FRAMESPERSEC);
  int Total = (lastTotal / FRAMESPERSEC);
  switch (Key) {
    case k0 ... k9:
         if (timeSearchPos < 4) {
            timeSearchTime <<= 8;
            timeSearchTime |= Key - k0;
            timeSearchPos++;
            TimeSearchDisplay();
            }
         break;
    case kFastRew:
    case kLeft:
    case kFastFwd:
    case kRight: {
         int dir = ((Key == kRight || Key == kFastFwd) ? 1 : -1);
         if (dir > 0)
            Seconds = min(Total - Current - STAY_SECONDS_OFF_END, Seconds);
         SkipSeconds(Seconds * dir);
         timeSearchActive = false;
         }
         break;
    case kPlay:
    case kUp:
    case kPause:
    case kDown:
    case kOk:
         Seconds = min(Total - STAY_SECONDS_OFF_END, Seconds);
         Goto(Seconds * FRAMESPERSEC, Key == kDown || Key == kPause || Key == kOk);
         timeSearchActive = false;
         break;
    default:
         timeSearchActive = false;
         break;
    }

  if (!timeSearchActive) {
     if (timeSearchHide)
        Hide();
     else
        displayReplay->SetJump(NULL);
     ShowMode();
     }
}

void cReplayControl::TimeSearch(void)
{
  timeSearchTime = timeSearchPos = 0;
  timeSearchHide = false;
  if (modeOnly)
     Hide();
  if (!visible) {
     Show();
     if (visible)
        timeSearchHide = true;
     else
        return;
     }
  timeoutShow = 0;
  TimeSearchDisplay();
  timeSearchActive = true;
}

void cReplayControl::MarkToggle(void)
{
  int Current, Total;
  if (GetIndex(Current, Total, true)) {
     cMark *m = marks.Get(Current);
     lastCurrent = -1; // triggers redisplay
     if (m)
        marks.Del(m);
     else {
        marks.Add(Current);
        ShowTimed(2);
        bool Play, Forward;
        int Speed;
        if (GetReplayMode(Play, Forward, Speed) && !Play) {
           Goto(Current, true);
           displayFrames = true;
           }
        }
     marks.Save();
     }
}

void cReplayControl::MarkJump(bool Forward)
{
  if (marks.Count()) {
     int Current, Total;
     if (GetIndex(Current, Total)) {
        cMark *m = Forward ? marks.GetNext(Current) : marks.GetPrev(Current);
        if (m) {
           bool Play2, Forward2;
           int Speed;
           if (Setup.JumpPlay && GetReplayMode(Play2, Forward2, Speed) &&
               Play2 && Forward && m->position < Total - SecondsToFrames(3)) {
              Goto(m->position);
              Play();
              }
           else {
              Goto(m->position, true);
              displayFrames = true;
              }
           }
        }
     }
}

void cReplayControl::MarkMove(bool Forward)
{
  int Current, Total;
  if (GetIndex(Current, Total)) {
     cMark *m = marks.Get(Current);
     if (m) {
        displayFrames = true;
        int p = SkipFrames(Forward ? 1 : -1);
        cMark *m2;
        if (Forward) {
           if ((m2 = marks.Next(m)) != NULL && m2->position <= p)
              return;
           }
        else {
           if ((m2 = marks.Prev(m)) != NULL && m2->position >= p)
              return;
           }
        Goto(m->position = p, true);
        marks.Save();
        }
     }
}

void cReplayControl::EditCut(void)
{
  if (fileName) {
     Hide();
     if (!cCutter::Active()) {
        if (!marks.Count())
           Skins.Message(mtError, tr("No editing marks defined!"));
        else if (!cCutter::Start(fileName))
           Skins.Message(mtError, tr("Can't start editing process!"));
        else
           Skins.Message(mtInfo, tr("Editing process started"));
        }
     else
        Skins.Message(mtError, tr("Editing process already active!"));
     ShowMode();
     }
}

void cReplayControl::EditTest(void)
{
  int Current, Total;
  if (GetIndex(Current, Total)) {
     cMark *m = marks.Get(Current);
     if (!m)
        m = marks.GetNext(Current);
     if (m) {
        if ((m->Index() & 0x01) != 0 && !Setup.PlayJump)
           m = marks.Next(m);
        if (m) {
           Goto(m->position - SecondsToFrames(3));
           Play();
           }
        }
     }
}

cOsdObject *cReplayControl::GetInfo(void)
{
  cRecording *Recording = Recordings.GetByName(cReplayControl::LastReplayed());
  if (Recording)
     return new cMenuRecording(Recording, false);
  return NULL;
}

eOSState cReplayControl::ProcessKey(eKeys Key)
{
  if (!Active())
     return osEnd;
  marks.Reload();
  if (visible) {
     if (timeoutShow && time(NULL) > timeoutShow) {
        Hide();
        ShowMode();
        timeoutShow = 0;
        }
     else if (modeOnly)
        ShowMode();
     else
        shown = ShowProgress(!shown) || shown;
     }
  bool DisplayedFrames = displayFrames;
  displayFrames = false;
  if (timeSearchActive && Key != kNone) {
     TimeSearchProcess(Key);
     return osContinue;
     }
  bool DoShowMode = true;
  switch (Key) {
    // Positioning:
    case kPlay:
                   Play(); break;
    case kPause:
                   Pause(); break;
    case kLeft:
    case kLeft|k_Repeat:
                   SkipSeconds( -5); break;
    case kFastRew|k_Release:
                   if (Setup.MultiSpeedMode) break;
    case kFastRew:
                   Backward(); break;
    case kRight:   bool playing, forwarding;
                   int speed;
                   if (GetReplayMode(playing, forwarding, speed) && !playing) {
                       Play();
                       break;
                   }
    case kRight|k_Repeat:
                   SkipSeconds( 5); break;

    case kFastFwd|k_Release:
                   if (Setup.MultiSpeedMode) break;
    case kFastFwd:
                   Forward(); break;
    case kRed:     TimeSearch(); break;
    case k1|k_Repeat:
    case k1:       SkipSeconds(-20); break;
    case k3|k_Repeat:
    case k3:       SkipSeconds( 20); break;
    case kGreen|k_Repeat:
    case kGreen:   SkipSeconds(-60); break;
    case kYellow|k_Repeat:
    case kYellow:  SkipSeconds( 60); break;
    case kStop:
    case kBlue:    Hide();
                   Stop();
                   return osEnd;
    default: {
      DoShowMode = false;
      switch (Key) {
        // Editing:
        case kMarkToggle:      MarkToggle(); break;
        case kPrev|k_Repeat:
        case kPrev:
        case kDown:
        case kMarkJumpBack|k_Repeat:
        case kMarkJumpBack:    MarkJump(false); break;
        case kNext|k_Repeat:
        case kNext:
        case kUp:
        case kMarkJumpForward|k_Repeat:
        case kMarkJumpForward: MarkJump(true); break;
        case kMarkMoveBack|k_Repeat:
        case kMarkMoveBack:    MarkMove(false); break;
        case kMarkMoveForward|k_Repeat:
        case kMarkMoveForward: MarkMove(true); break;
        case kEditCut:         EditCut(); break;
        case kEditTest:        EditTest(); break;
        default: {
          displayFrames = DisplayedFrames;
          switch (Key) {
            // Menu control:
            case kOk:      if (visible && !modeOnly) {
                              Hide();
                              DoShowMode = true;
                              }
                           else
                              Show();
                           break;
            case kBack:    if (visible && !modeOnly) {
                              Hide();
                              DoShowMode = true;
                              }
                           else {
                              //return osRecordings;
                              cRemote::CallPlugin("extrecmenu");
                              return osEnd;
                           }
                           break;
            default:       return osUnknown;
            }
          }
        }
      }
    }
  if (DoShowMode)
     ShowMode();
  return osContinue;
}
