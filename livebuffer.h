#ifndef __LIVEBUFFER_H
#define __LIVEBUFFER_H

#include "player.h"
#include "receiver.h"
#include "remux.h"
#include "ringbuffer.h"
#include "thread.h"

#define LIVEBUFSIZE MEGABYTE(5)

#define INDEXBLOCKSIZE 20000

class cUnbufferedFile64 {
private:
  int fd;
  off64_t curpos;
  off64_t cachedstart;
  off64_t cachedend;
  off64_t begin;
  off64_t lastpos;
  off64_t ahead;
  size_t readahead;
  size_t written;
  size_t totwritten;
  int FadviseDrop(off64_t Offset, off64_t Len);
public:
  cUnbufferedFile64(void);
  ~cUnbufferedFile64();
  int Open(const char *FileName, int Flags, mode_t Mode = DEFFILEMODE);
  int Close(void);
  off64_t Seek(off64_t Offset, int Whence);
  ssize_t Read(void *Data, size_t Size);
  ssize_t Write(const void *Data, size_t Size);
  static cUnbufferedFile64 *Create(const char *FileName, int Flags, mode_t Mode = DEFFILEMODE);
  };

class cFileName64 {
private:
  cUnbufferedFile64 *file;
  int fileNumber;
  char *fileName, *pFileNumber;
  bool record;
  bool blocking;
public:
  cFileName64(const char *FileName, bool Record, bool Blocking = false);
  ~cFileName64();
  const char *Name(void) { return fileName; }
  int Number(void) { return fileNumber; }
  void SetNumber(int Number);
  cUnbufferedFile64 *Open(void);
  void Close(void);
  void CloseAndRemove(void);
  void Remove(void);
  };

class cLiveIndex {
friend class cLiveCutterThread;
friend class cLiveBuffer;
private:
  struct frame {
    off64_t offset  ;  //:36;
    bool  written   ;  //:1 ;
    uchar pt        ;  //:3 ;
    };
  struct block {
    frame Frame[INDEXBLOCKSIZE];
    block *previous, *next;
    };
  block *headblock, *tailblock;
  int blockCount;
  int head, tail;
  int blank;
  int start;
  int writtenCount, delCount;
  int DPos, DCount;
  int lastWrittenSize, lastCount;
  off64_t fileSize;
  int lastFrame;
  int GetFrame(block** Block, int Number);
  cRwLock *RwLock;
public:
  cLiveIndex();
  ~cLiveIndex();
  void Add(uchar pt, int pos);
  void AddData(int pos);
  void Delete(off64_t FilePos);
  void Clear(void);
  void SetBlank(int Blank) { RwLock->Lock(true); blank = Blank; RwLock->Unlock(); }
  int NextWrite();
  void WrittenTo(off64_t FilePos, int Count);
  bool HasFrame(int Number) { return lastFrame > Number && Number >=delCount; }
  bool IsWritten(int Number) { return writtenCount > Number; }
  off64_t GetOffset(int Number);
  int Size(int Number, uchar *PictureType = NULL);
  int GetNextIFrame(int Index, bool Forward);
  int FindIFrame(int64_t PTS, uchar *Buffer);
  void Switched();
  int Last(void) { return lastFrame+1; }
  int First(void) { return start > delCount ? start : delCount; }
};

class cLiveFileReader : public cThread {
private:
  cFileName64 *fileName;
  cUnbufferedFile64 *readFile;
  cCondWait newSet;
  off64_t filePos;
  int hasData, length, wanted;
  uchar *buffer;
protected:
  virtual void Action(void);
public:
  cLiveFileReader(const char *FileName, int Number);
  ~cLiveFileReader();
  int Read(uchar **Buffer, off64_t FilePos, int Size);
  void Clear(void);
};

class cLiveFileWriter : public cThread {
private:
  cFileName64 *fileName;
  cUnbufferedFile64 *writeFile;
  cCondWait newSet,written;
  off64_t filePos,fileSize;
  int hasWritten, length, wantWrite;
  uchar *buffer;
  time_t lastCheck;
  bool full;
  bool LowDiskSpace(void);
protected:
  virtual void Action(void);
public:
  cLiveFileWriter(const char *FileName);
  ~cLiveFileWriter();
  off64_t Write(uchar *Buffer, int Size, bool Wait);
  int FileNumber(void) { return fileName->Number(); }
  void Clear(void);
  bool Full(void) { return full; }
};

class cLiveBuffer;

class cLiveCutterThread : public cThread {
private:
  cFileName64 *readFileName, *writeFileName;
  cUnbufferedFile64 *readFile, *writeFile;
  cLiveBuffer *liveBuffer;
  int startFrame, endFrame;
protected:
  virtual void Action(void);
public:
  cLiveCutterThread(const char *FileName, int FileNumber, cLiveBuffer *LiveBuffer, int StartFrame, int EndFrame);
  ~cLiveCutterThread();
};

class cLiveBuffer : public cThread {
friend class cLiveCutterThread;
private:
  uchar buffer[LIVEBUFSIZE];  // evtl. dynamisch anlegen, um bei Speichermangel Fehlermeldung
  int head, tail, blank;
  cLiveIndex *index;
  cRemux *remux;
  cLiveFileWriter *fileWriter;
  char *filestring;
  uchar pictureType;
  cLiveFileReader *fileReader;
  int startFrame;
  cLiveCutterThread *liveCutter;
  void Write(bool Wait);
  void Store(uchar *Data, int Count);
protected:
  virtual void Action(void);
public:
  cLiveBuffer(const char *FileName, cRemux *Remux);
  virtual ~cLiveBuffer();
  void SetNewRemux(cRemux *Remux, bool Clear = false);
  int GetFrame(uchar **Buffer, int Number, int Off = -1);
  int GetNextIFrame(int Index, bool Forward) { return index->GetNextIFrame(Index,Forward); }
  int LastIndex(void) { return index->Last(); }
  int FirstIndex(void) { return index->First(); }
  void CreateIndexFile(const char *FileName, int64_t PTS, int EndFrame = 0);
  void SetStartFrame(int StartFrame) { startFrame = StartFrame; }
  bool LiveCutterActive(void) { return liveCutter && liveCutter->Active(); }
};


class cLiveBackTrace;

class cLivePlayer : public cPlayer, public cThread {
private:
  enum ePlayModes { pmPlay, pmPause, pmSlow, pmFast, pmStill };
  enum ePlayDirs { pdForward, pdBackward };
  static int Speeds[];
  ePlayModes playMode;
  ePlayDirs playDir;
  int trickSpeed;
  cLiveBuffer *liveBuffer;
  cRingBufferFrame *ringBuffer;
  cLiveBackTrace *backTrace;
  int readIndex, writeIndex;
  cFrame *readFrame;
  cFrame *playFrame;
  int Off;
  bool firstPacket;
  void TrickSpeed(int Increment);
  void Empty(void);
protected:
  virtual void Activate(bool On);
  virtual void Action(void);
public:
  cLivePlayer(cLiveBuffer *LiveBuffer);
  virtual ~cLivePlayer();
  void Pause(void);
  void Play(void);
  void Forward(void);
  void Backward(void);
  bool Stop(void);
  void SkipSeconds(int Seconds);
  virtual bool GetIndex(int &Current, int &Total, bool SnapToIFrame = false);
  virtual bool GetReplayMode(bool &Play, bool &Forward, int &Speed);
};

class cLiveReceiver : public cReceiver, cThread {
friend class cLiveBufferManager;
friend class cLiveBufferControl;
private:
//  cRingBufferLinear *ringBuffer;
  cRemux *remux;
  const cChannel *channel;
protected:
  virtual void Activate(bool On);
  virtual void Receive(uchar *Data, int Length);
  virtual void Action(void);
public:
  cLiveReceiver(const cChannel *Channel);
  virtual ~cLiveReceiver();
  const cChannel *GetChannel() { return channel; }
  };

class cLiveBufferControl : public cControl {
private:
  cSkinDisplayReplay *displayReplay;
  cLivePlayer *player;
  bool visible, modeOnly, shown;
  int lastCurrent, lastTotal;
  bool lastPlay, lastForward;
  int lastSpeed;
  time_t timeoutShow;
  void ShowTimed(int Seconds = 0);
  void ShowMode(void);
  bool ShowProgress(bool Initial);
public:
  cLiveBufferControl(cLivePlayer *Player);
  ~cLiveBufferControl();
  bool GetIndex(int &Current, int &Total, bool SnapToIFrame = false);
  bool GetReplayMode(bool &Play, bool &Forward, int &Speed); 
  virtual eOSState ProcessKey(eKeys Key);
  virtual void Show(void);
  virtual void Hide(void);
  bool Visible(void) { return visible; }
  };

class cLiveBufferManager {
friend class cLiveBufferControl;
private:
  static cLiveReceiver* liveReceiver;
  static cLiveBuffer* liveBuffer;
  static cLivePlayer* livePlayer;
  static cLiveBufferControl* liveControl;
  static const cChannel *channel;
public:
  static void ChannelSwitch(cDevice *ReceiverDevice, const cChannel *Channel); 
  static void Shutdown(void);
  static cLiveBufferControl *GetLiveBufferControl(void) { return liveControl; }
  static cLiveBuffer *InLiveBuffer(cTimer *timer, int *StartFrame = NULL, int *EndFrame = NULL);
  static bool AllowsChannelSwitch(void);
};

#endif //__LIVEBUFFER_H
