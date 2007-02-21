/*
 * menu.h: The actual menu implementations
 *
 * See the main source file 'vdr.c' for copyright information and
 * how to reach the author.
 *
 * $Id: menu.h 1.86 2006/10/20 13:09:57 kls Exp $
 */

#ifndef __MENU_H
#define __MENU_H

#include "ci.h"
#include "device.h"
#include "epg.h"
#include "osdbase.h"
#include "dvbplayer.h"
#include "menuitems.h"
#include "recorder.h"
#include "skins.h"
#include "submenu.h"
#include "help.h"

class cMenuText : public cOsdMenu {
private:
  char *text;
  eDvbFont font;
public:
  cMenuText(const char *Title, const char *Text, eDvbFont Font = fontOsd);
  virtual ~cMenuText();
  void SetText(const char *Text);
  virtual void Display(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

class cMenuHelp : public cOsdMenu {
private:
  char *text;
  cHelpSection *section;
  cHelpPage *helpPage;
  eDvbFont font;
public:
  cMenuHelp(cHelpSection *Section, const char *Title);
  virtual ~cMenuHelp();
  void SetText(const char *Text);
  void SetNextHelp();
  void SetPrevHelp();
  virtual void Display(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

class cMenuEditTimer : public cOsdMenu {
private:
  cTimer *timer;
  cTimer data;
  int channel;
  bool addIfConfirmed;
  cMenuEditDateItem *firstday;
  void SetFirstDayItem(void);
  int tmpprio;
  const char *PriorityTexts[3];
public:
  cMenuEditTimer(cTimer *Timer, bool New = false);
  virtual ~cMenuEditTimer();
  virtual eOSState ProcessKey(eKeys Key);
  };

class cMenuEvent : public cOsdMenu {
private:
  const cEvent *event;
public:
  cMenuEvent(const cEvent *Event, bool CanSwitch = false, bool Buttons = false);
  virtual void Display(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

class cMenuActiveEvent : public cOsdMenu {
private:
  const cEvent *event;
public:
  cMenuActiveEvent();
  virtual void Display(void);
  eOSState Record(void);
  void SetHelpButtons(void);
  virtual eOSState ProcessKey(eKeys Key);
  };

class cMenuMain : public cOsdMenu {
private:
  int    nrDynamicMenuEntries;
  time_t lastDiskSpaceCheck;
  cSubMenu subMenu;
  int lastFreeMB;
  bool replaying;
  cOsdItem *stopReplayItem;
  cOsdItem *cancelEditingItem;
  cOsdItem *stopRecordingItem;
  int recordControlsState;
  static cOsdObject *pluginOsdObject;
  void Set(int current=0);
  bool Update(bool Force = false);
  eOSState DisplayHelp(int Index);
public:
  cMenuMain(eOSState State = osUnknown);
  virtual eOSState ProcessKey(eKeys Key);
  static cOsdObject *PluginOsdObject(void);
  static void SetPluginOsdObject(cOsdObject *PluginOsdObject) 
      { pluginOsdObject = PluginOsdObject ; }
  };

class cDisplayChannel : public cOsdObject {
private:
  cSkinDisplayChannel *displayChannel;
  int group;
  bool withInfo;
  cTimeMs lastTime;
  int number;
  bool timeout;
  cChannel *channel;
  const cEvent *lastPresent;
  const cEvent *lastFollowing;
  static cDisplayChannel *currentDisplayChannel;
  void DisplayChannel(void);
  void DisplayInfo(void);
  void Refresh(void);
  cChannel *NextAvailableChannel(cChannel *Channel, int Direction);
public:
  cDisplayChannel(int Number, bool Switched);
  cDisplayChannel(eKeys FirstKey);
  virtual ~cDisplayChannel();
  virtual eOSState ProcessKey(eKeys Key);
  static bool IsOpen(void) { return currentDisplayChannel != NULL; }
  };

class cDisplayVolume : public cOsdObject {
private:
  cSkinDisplayVolume *displayVolume;
  cTimeMs timeout;
  static cDisplayVolume *currentDisplayVolume;
  virtual void Show(void);
  cDisplayVolume(void);
public:
  virtual ~cDisplayVolume();
  static cDisplayVolume *Create(void);
  static void Process(eKeys Key);
  eOSState ProcessKey(eKeys Key);
  };

class cDisplayTracks : public cOsdObject {
private:
  cSkinDisplayTracks *displayTracks;
  cTimeMs timeout;
  eTrackType types[ttMaxTrackTypes];
  char *descriptions[ttMaxTrackTypes];
  int numTracks, track, audioChannel;
  static cDisplayTracks *currentDisplayTracks;
  virtual void Show(void);
  cDisplayTracks(void);
public:
  virtual ~cDisplayTracks();
  static bool IsOpen(void) { return currentDisplayTracks != NULL; }
  static cDisplayTracks *Create(void);
  static void Process(eKeys Key);
  eOSState ProcessKey(eKeys Key);
  };

class cMenuCam : public cOsdMenu {
private:
  cCiMenu *ciMenu;
  bool selected;
  int offset;
  void AddMultiLineItem(const char *s);
  eOSState Select(void);
public:
  cMenuCam(cCiMenu *CiMenu);
  virtual ~cMenuCam();
  virtual eOSState ProcessKey(eKeys Key);
  };

class cMenuCamEnquiry : public cOsdMenu {
private:
  cCiEnquiry *ciEnquiry;
  char *input;
  bool replied;
  eOSState Reply(void);
public:
  cMenuCamEnquiry(cCiEnquiry *CiEnquiry);
  virtual ~cMenuCamEnquiry();
  virtual eOSState ProcessKey(eKeys Key);
  };

cOsdObject *CamControl(void);

class cMenuRecordingItem;

class cMenuRecordings : public cOsdMenu {
private:
  char *base;
  int level;
  int recordingsState;
  int helpKeys;
  void SetHelpKeys(void);
  void Set(bool Refresh = false);
  bool Open(bool OpenSubMenus = false);
  eOSState Play(void);
  eOSState Rewind(void);
  eOSState Delete(void);
  eOSState Info(void);
  eOSState Commands(eKeys Key = kNone);
protected:
  cRecording *GetRecording(cMenuRecordingItem *Item);
public:
  cMenuRecordings(const char *Base = NULL, int Level = 0, bool OpenSubMenus = false);
  ~cMenuRecordings();
  virtual eOSState ProcessKey(eKeys Key);
  };

class cMenuEditSrcItem : public cMenuEditIntItem {
private:
  const cSource *source;
protected:
  virtual void Set(void);
public:
  cMenuEditSrcItem(const char *Name, int *Value);
  eOSState ProcessKey(eKeys Key);
  };

class cRecordControl {
private:
  cDevice *device;
  cTimer *timer;
  cRecorder *recorder;
  const cEvent *event;
  char *instantId;
  char *fileName;
  bool GetEvent(void);
public:
  cRecordControl(cDevice *Device, cTimer *Timer = NULL, bool Pause = false);
  virtual ~cRecordControl();
  bool Process(time_t t);
  cDevice *Device(void) { return device; }
  void Stop(void);
  const char *InstantId(void) { return instantId; }
  const char *FileName(void) { return fileName; }
  cTimer *Timer(void) { return timer; }
  };

class cRecordControls {
private:
  static cRecordControl *RecordControls[];
  static int state;
public:
  static bool Start(cTimer *Timer = NULL, bool Pause = false);
  static void Stop(const char *InstantId);
  static void Stop(cDevice *Device);
  static bool PauseLiveVideo(void);
  static const char *GetInstantId(const char *LastInstantId);
  static cRecordControl *GetRecordControl(const char *FileName);
  static void Process(time_t t);
  static void ChannelDataModified(cChannel *Channel);
  static bool Active(void);
  static void Shutdown(void);
  static void ChangeState(void) { state++; }
  static bool StateChanged(int &State);
  };

class cReplayControl : public cDvbPlayerControl {
private:
  cSkinDisplayReplay *displayReplay;
  cMarksReload marks;
  bool visible, modeOnly, shown, displayFrames;
  int lastCurrent, lastTotal;
  bool lastPlay, lastForward;
  int lastSpeed;
  time_t timeoutShow;
  bool timeSearchActive, timeSearchHide;
  int timeSearchTime, timeSearchPos;
  void TimeSearchDisplay(void);
  void TimeSearchProcess(eKeys Key);
  void TimeSearch(void);
  void ShowTimed(int Seconds = 0);
  static cReplayControl *currentReplayControl;
  static char *fileName;
  static char *title;
  void ShowMode(void);
  bool ShowProgress(bool Initial);
  void MarkToggle(void);
  void MarkJump(bool Forward);
  void MarkMove(bool Forward);
  void EditCut(void);
  void EditTest(void);
public:
  cReplayControl(void);
  virtual ~cReplayControl();
  virtual cOsdObject *GetInfo(void);
  virtual eOSState ProcessKey(eKeys Key);
  virtual void Show(void);
  virtual void Hide(void);
  bool Visible(void) { return visible; }
  static void SetRecording(const char *FileName, const char *Title);
  static const char *NowReplaying(void);
  static const char *LastReplayed(void);
  static void ClearLastReplayed(const char *FileName);
  };

class cMenuBouquets : public cOsdMenu {
private:
  bool edit;
  bool favourite;
  int startChannel;
  int number;
  int channelMarked;
  cTimeMs numberTimer;
  void Setup(void);
  void SetGroup(int Index);
  cChannel *GetChannel(int Index);
  void Propagate(void);
  void GetFavourite(void);
  eOSState PrevBouquet(void);
  eOSState NextBouquet(void);
protected:
  void Options(void);
  eOSState Switch(void);
  eOSState NewChannel(void);
  eOSState EditChannel(void);
  eOSState DeleteChannel(void);
  eOSState ListBouquets(void);
  eOSState Number(eKeys Key);
  void Mark(void);
  virtual void Move(int From, int To);
public:
  cMenuBouquets(int view);
  ~cMenuBouquets(void);
  void AddFavourite(bool active);
  virtual eOSState ProcessKey(eKeys Key);
  virtual void Display(void);
  };

#endif //__MENU_H
