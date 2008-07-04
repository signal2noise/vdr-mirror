/*
 * dvbplayer.c: The DVB player
 *
 * See the main source file 'vdr.c' for copyright information and
 * how to reach the author.
 *
 * $Id: dvbplayer.c 1.46 2007/04/28 14:55:22 kls Exp $
 */

#include "dvbplayer.h"
#include <stdlib.h>
#include "recording.h"
#include "remux.h"
#include "ringbuffer.h"
#include "thread.h"
#include "tools.h"
#include "status.h"

// --- cBackTrace ------------------------------------------------------------

#define AVG_FRAME_SIZE 15000         // an assumption about the average frame size
#define DVB_BUF_SIZE   (256 * 1024)  // an assumption about the dvb firmware buffer size
#define BACKTRACE_ENTRIES (DVB_BUF_SIZE / AVG_FRAME_SIZE + 20) // how many entries are needed to backtrace buffer contents

#define DELAYED_FAST_TRICKMODE

class cBackTrace
{
	private:
		int index[BACKTRACE_ENTRIES];
		int length[BACKTRACE_ENTRIES];
		int pos, num;
	public:
		cBackTrace ( void );
		void Clear ( void );
		void Add ( int Index, int Length );
		int Get ( bool Forward );
};

cBackTrace::cBackTrace ( void )
{
	Clear();
}

void cBackTrace::Clear ( void )
{
	pos = num = 0;
}

void cBackTrace::Add ( int Index, int Length )
{
	index[pos] = Index;
	length[pos] = Length;
	if ( ++pos >= BACKTRACE_ENTRIES )
		pos = 0;
	if ( num < BACKTRACE_ENTRIES )
		num++;
}

int cBackTrace::Get ( bool Forward )
{
	int p = pos;
	int n = num;
	int l = DVB_BUF_SIZE + ( Forward ? 0 : 256 * 1024 ); //XXX (256 * 1024) == DVB_BUF_SIZE ???
	int i = -1;

	while ( n && l > 0 )
	{
		if ( --p < 0 )
			p = BACKTRACE_ENTRIES - 1;
		i = index[p] - 1;
		l -= length[p];
		n--;
	}
	return i;
}

// --- cNonBlockingFileReader ------------------------------------------------

class cNonBlockingFileReader : public cThread
{
	private:
		cUnbufferedFile *f;
		uchar *buffer;
		int wanted;
		int length;
		bool hasData;
		cCondWait newSet;
		cCondVar newDataCond;
		cMutex newDataMutex;
	protected:
		void Action ( void );
	public:
		cNonBlockingFileReader ( void );
		~cNonBlockingFileReader();
		void Clear ( void );
		int Read ( cUnbufferedFile *File, uchar *Buffer, int Length );
		bool Reading ( void ) { return buffer; }
		bool WaitForDataMs ( int msToWait );
};

cNonBlockingFileReader::cNonBlockingFileReader ( void )
		:cThread ( "non blocking file reader" )
{
	f = NULL;
	buffer = NULL;
	wanted = length = 0;
	hasData = false;
	Start();
}

cNonBlockingFileReader::~cNonBlockingFileReader()
{
	newSet.Signal();
	Cancel ( 3 );
	free ( buffer );
}

void cNonBlockingFileReader::Clear ( void )
{
	Lock();
	f = NULL;
	free ( buffer );
	buffer = NULL;
	wanted = length = 0;
	hasData = false;
	Unlock();
	newSet.Signal();
}

int cNonBlockingFileReader::Read ( cUnbufferedFile *File, uchar *Buffer, int Length )
{
	if ( hasData && buffer )
	{
		if ( buffer != Buffer )
		{
			esyslog ( "ERROR: cNonBlockingFileReader::Read() called with different buffer!" );
			errno = EINVAL;
			return -1;
		}
		buffer = NULL;
		return length;
	}
	if ( !buffer )
	{
		f = File;
		buffer = Buffer;
		wanted = Length;
		length = 0;
		hasData = false;
		newSet.Signal();
	}
	errno = EAGAIN;
	return -1;
}

void cNonBlockingFileReader::Action ( void )
{
	while ( Running() )
	{
		Lock();
		if ( !hasData && f && buffer )
		{
			int r = f->Read ( buffer + length, wanted - length );
			if ( r >= 0 )
			{
				length += r;
				if ( !r || length == wanted ) // r == 0 means EOF
				{
					cMutexLock NewDataLock ( &newDataMutex );
					hasData = true;
					newDataCond.Broadcast();
				}
			}
			else if ( r < 0 && FATALERRNO )
			{
				LOG_ERROR;
				length = r; // this will forward the error status to the caller
				hasData = true;
			}
		}
		Unlock();
		newSet.Wait ( 1000 );
	}
}

bool cNonBlockingFileReader::WaitForDataMs ( int msToWait )
{
	cMutexLock NewDataLock ( &newDataMutex );
	if ( hasData )
		return true;
	return newDataCond.TimedWait ( newDataMutex, msToWait );
}

// --- cDvbPlayer ------------------------------------------------------------

#define PLAYERBUFSIZE  MEGABYTE(1)

// The number of frames to back up when resuming an interrupted replay session:
#define RESUMEBACKUP (10 * FRAMESPERSEC)

class cDvbPlayer : public cPlayer, cThread
{
	private:
		enum ePlayModes { pmPlay, pmPause, pmSlow, pmFast, pmStill };
		enum ePlayDirs { pdForward, pdBackward };
		static int Speeds[];
		cNonBlockingFileReader *nonBlockingFileReader;
		cRingBufferFrame *ringBuffer;
		cBackTrace *backTrace;
		cMarksReload marks;
		cFileName *fileName;
		cIndexFile *index;
		cUnbufferedFile *replayFile;
		bool eof;
		bool firstPacket;
		ePlayModes playMode;
		ePlayDirs playDir;
		int trickSpeed;
		int readIndex, writeIndex;
		uchar *PATPMT;
		cFrame *readFrame;
		cFrame *playFrame;
		void TrickSpeed ( int Increment );
		void Empty ( void );
		bool NextFile ( uchar FileNumber = 0, int FileOffset = -1 );
		int Resume ( void );
		bool Save ( void );
		void CheckTS ( void );
		// Check for TS File
	protected:
		virtual void Activate ( bool On );
		virtual void Action ( void );
	public:
		cDvbPlayer ( const char *FileName );
		virtual ~cDvbPlayer();
		bool Active ( void ) { return cThread::Running(); }
		void Pause ( void );
		void Play ( void );
		void Forward ( void );
		void Backward ( void );
		int SkipFrames ( int Frames );
		void SkipSeconds ( int Seconds );
		void Goto ( int Position, bool Still = false );
		virtual bool GetIndex ( int &Current, int &Total, bool SnapToIFrame = false );
		virtual bool GetReplayMode ( bool &Play, bool &Forward, int &Speed );
};

#define MAX_VIDEO_SLOWMOTION 63 // max. arg to pass to VIDEO_SLOWMOTION // TODO is this value correct?
#define NORMAL_SPEED  4 // the index of the '1' entry in the following array
#define MAX_SPEEDS    3 // the offset of the maximum speed from normal speed in either direction
#define SPEED_MULT   12 // the speed multiplier
int cDvbPlayer::Speeds[] = { 0, -2, -4, -8, 1, 2, 4, 12, 0 };

cDvbPlayer::cDvbPlayer ( const char *FileName )
		:cThread ( "dvbplayer" ), marks ( FileName )
{
	nonBlockingFileReader = NULL;
	ringBuffer = NULL;
	backTrace = NULL;
	index = NULL;
	eof = false;
	firstPacket = true;
	playMode = pmPlay;
	playDir = pdForward;
	trickSpeed = NORMAL_SPEED;
	readIndex = writeIndex = -1;
	readFrame = NULL;
	playFrame = NULL;
	PATPMT = NULL;
	isyslog ( "replay %s", FileName );
	fileName = new cFileName ( FileName, false );
	replayFile = fileName->Open();
	if ( !replayFile )
		return;
	ringBuffer = new cRingBufferFrame ( PLAYERBUFSIZE );
	// Create the index file:
	index = new cIndexFile ( FileName, false );
	if ( !index )
		esyslog ( "ERROR: can't allocate index" );
	else if ( !index->Ok() )
	{
		delete index;
		index = NULL;
	}
	// Check for TS Data
	CheckTS();
	backTrace = new cBackTrace;
}

cDvbPlayer::~cDvbPlayer()
{
	Detach();
	Save();

	if ( PATPMT != NULL )
		free ( PATPMT );
	delete readFrame; // might not have been stored in the buffer in Action()
	delete index;
	delete fileName;
	delete backTrace;
	delete ringBuffer;
}

void cDvbPlayer::TrickSpeed ( int Increment )
{
	int nts = trickSpeed + Increment;
	if ( Speeds[nts] == 1 )
	{
		trickSpeed = nts;
		if ( playMode == pmFast )
			Play();
		else
			Pause();
	}
	else if ( Speeds[nts] )
	{
		trickSpeed = nts;
		int Mult = ( playMode == pmSlow && playDir == pdForward ) ? 1 : SPEED_MULT;
		int sp = ( Speeds[nts] > 0 ) ? Mult / Speeds[nts] : -Speeds[nts] * Mult;
		if ( sp > MAX_VIDEO_SLOWMOTION )
			sp = MAX_VIDEO_SLOWMOTION;
		DeviceTrickSpeed ( sp );
	}
}

void cDvbPlayer::Empty ( void )
{
	LOCK_THREAD;
	if ( nonBlockingFileReader )
		nonBlockingFileReader->Clear();
	if ( ( readIndex = backTrace->Get ( playDir == pdForward ) ) < 0 )
		readIndex = writeIndex;
	delete readFrame; // might not have been stored in the buffer in Action()
	readFrame = NULL;
	playFrame = NULL;
	ringBuffer->Clear();
	backTrace->Clear();
	DeviceClear();
	firstPacket = true;
}

bool cDvbPlayer::NextFile ( uchar FileNumber, int FileOffset )
{
	if ( FileNumber > 0 )
		replayFile = fileName->SetOffset ( FileNumber, FileOffset );
	else if ( replayFile && eof )
		replayFile = fileName->NextFile();
	eof = false;
	return replayFile != NULL;
}

int cDvbPlayer::Resume ( void )
{
	if ( index )
	{
		int Index = index->GetResume();
		if ( Index >= 0 )
		{
			uchar FileNumber;
			int FileOffset;
			if ( index->Get ( Index, &FileNumber, &FileOffset ) && NextFile ( FileNumber, FileOffset ) )
				return Index;
		}
	}
	return -1;
}

bool cDvbPlayer::Save ( void )
{
	if ( index )
	{
		int Index = writeIndex;
		if ( Index >= 0 )
		{
			Index -= RESUMEBACKUP;
			if ( Index > 0 )
				Index = index->GetNextIFrame ( Index, false );
			else
				Index = 0;
			if ( Index >= 0 )
				return index->StoreResume ( Index );
		}
	}
	return false;
}

void cDvbPlayer::Activate ( bool On )
{
	if ( On )
	{
		if ( replayFile )
			Start();
	}
	else
		Cancel ( 9 );
}
static int init_repeat = 0;
void cDvbPlayer::Action ( void )
{
	uchar *b = NULL;
	uchar *p = NULL;
	int pc = 0;
	bool cutIn = false;
	int total = -1;
	int last_index = -1;

	readIndex = Resume();
	if ( readIndex >= 0 )
		isyslog ( "resuming replay at index %d (%s)", readIndex, *IndexToHMSF ( readIndex, true ) );

	if ( Setup.PlayJump && readIndex <= 0 && marks.First() && index )
	{
		int Index = marks.First()->position;
		uchar FileNumber;
		int FileOffset;
		if ( index->Get ( Index, &FileNumber, &FileOffset ) &&
		        NextFile ( FileNumber, FileOffset ) )
		{
			isyslog ( "PlayJump: start replay at first mark %d (%s)",
			          Index, *IndexToHMSF ( Index, true ) );
			readIndex = Index;
		}
	}

	bool LastMarkPause = false;
	nonBlockingFileReader = new cNonBlockingFileReader;
	int Length = 0;
	bool Sleep = false;
	bool WaitingForData = false;

	while ( Running() && ( NextFile() || readIndex >= 0 || ringBuffer->Available() || !DeviceFlush ( 100 ) ) )
	{
		if ( Sleep )
		{
			if ( WaitingForData )
				nonBlockingFileReader->WaitForDataMs ( 3 ); // this keeps the CPU load low, but reacts immediately on new data
			else
				cCondWait::SleepMs ( 3 ); // this keeps the CPU load low
			Sleep = false;
		}
		cPoller Poller;
		if ( DevicePoll ( Poller, 100 ) )
		{

			LOCK_THREAD;

			// Read the next frame from the file:

			if ( playMode != pmStill && playMode != pmPause && !LastMarkPause )
			{
				if ( !readFrame && ( replayFile || readIndex >= 0 ) )
				{
					if ( !nonBlockingFileReader->Reading() )
					{
						if ( playMode == pmFast || ( playMode == pmSlow && playDir == pdBackward ))
						{
							uchar FileNumber;
							int FileOffset;
							bool TimeShiftMode = index->IsStillRecording();
							int Index = index->GetNextIFrame ( readIndex, playDir == pdForward, &FileNumber, &FileOffset, &Length, TimeShiftMode );
							if ( Index >= 0 )
							{
								if ( !NextFile ( FileNumber, FileOffset ) )
								{
									readIndex = Index;
									continue;
								}
							}
							else
							{
								if ( !TimeShiftMode && playDir == pdForward )
								{
									// hit end of recording: signal end of file but don't change playMode
									readIndex = -1;
									eof = true;
									continue;
								}
								// hit begin of recording: wait for device buffers to drain
								// before changing play mode:
								if ( !DeviceFlush ( 100 ) )
									continue;
								// can't call Play() here, because those functions may only be
								// called from the foreground thread - and we also don't need
								// to empty the buffer here
								DevicePlay();
								playMode = pmPlay;
								playDir = pdForward;
								continue;
							}
							readIndex = Index;
						}
						else if ( index )
						{
							uchar FileNumber;
							int FileOffset;
							readIndex++;
							if ( Setup.PlayJump || Setup.PauseLastMark )
							{
								// check for end mark - jump to next mark or pause
								marks.Reload();
								cMark *m = marks.Get ( readIndex );
								if ( m && ( m->Index() & 0x01 ) != 0 )
								{
									m = marks.Next ( m );
									int Index;
									if ( m )
										Index = m->position;
									else if ( Setup.PauseLastMark )
									{
										// pause at last mark
										isyslog ( "PauseLastMark: pause at position %d (%s)",
										          readIndex, *IndexToHMSF ( readIndex, true ) );
										LastMarkPause = true;
										Index = -1;
									}
									else if ( total == index->Last() )
										// at last mark jump to end of recording
										Index = index->Last() - 1;
									else
										// jump but stay off end of live-recordings
										Index = index->GetNextIFrame ( index->Last() - 150, true );
									// don't jump in edited recordings
									if ( Setup.PlayJump && Index > readIndex &&
									        Index > index->GetNextIFrame ( readIndex, true ) )
									{
										isyslog ( "PlayJump: %d frames to %d (%s)",
										          Index - readIndex, Index,
										          *IndexToHMSF ( Index, true ) );
										readIndex = Index;
										cutIn = true;
									}
								}
							}
							// for detecting growing length of live-recordings
							uchar PictureType;
							if ( index->Get ( readIndex, &FileNumber, &FileOffset, &PictureType ) &&
							        PictureType == I_FRAME )
								total = index->Last();
							if ( ! ( index->Get ( readIndex, &FileNumber, &FileOffset, NULL, &Length ) && NextFile ( FileNumber, FileOffset ) ) )
							{
								readIndex = -1;
								eof = true;
								continue;
							}
						}
						else // allows replay even if the index file is missing
							Length = MAXFRAMESIZE;
						if ( Length == -1 )
							Length = MAXFRAMESIZE; // this means we read up to EOF (see cIndex)
						else if ( Length > MAXFRAMESIZE )
						{
							esyslog ( "ERROR: frame larger than buffer (%d > %d)", Length, MAXFRAMESIZE );
							Length = MAXFRAMESIZE;
						}
						b = MALLOC ( uchar, Length );
					}
					int r = nonBlockingFileReader->Read ( replayFile, b, Length );
					if ( r > 0 )
					{
						WaitingForData = false;
						readFrame = new cFrame ( b, -r, ftUnknown, readIndex ); // hands over b to the ringBuffer
						b = NULL;
					}
					else if ( r == 0 )
						eof = true;
					else if ( r < 0 && errno == EAGAIN )
						WaitingForData = true;
					else if ( r < 0 && FATALERRNO )
					{
						LOG_ERROR;
						break;
					}
				}

				// Store the frame in the buffer:

				if ( readFrame )
				{
					if ( cutIn )
					{
						cRemux::SetBrokenLink ( readFrame->Data(), readFrame->Count() );
						cutIn = false;
					}
					if ( ringBuffer->Put ( readFrame ) )
						readFrame = NULL;
				}
			}
			else
				Sleep = true;

			// Get the next frame from the buffer:

			if ( !playFrame )
			{
				playFrame = ringBuffer->Get();
				p = NULL;
				pc = 0;
			}

			// Play the frame:

			if ( playFrame )
			{
				if ( !p )
				{
					p = playFrame->Data();
					pc = playFrame->Count();
					if ( p )
					{
						if ( firstPacket )
						{
							if ( PATPMT != NULL )
								PlayTS ( NULL,0, false, PATPMT );
							else
								PlayPes ( NULL, 0 );

							cRemux::SetBrokenLink ( p, pc );
							firstPacket = false;
						}
					}
				}
				if ( p )
				{
					int w = 0;
					if ( PATPMT != NULL )
						w += PlayTS ( p, pc, playMode != pmPlay );
					else
						w += PlayPes ( p, pc, playMode != pmPlay );

					if ( w > 0 )
					{
						while(init_repeat && playMode != pmPlay) {
							if ( PATPMT != NULL )
								PlayTS ( p, pc, playMode != pmPlay );
							else
								PlayPes ( p, pc, playMode != pmPlay );
							init_repeat--;
						} // while
						p += w;
						pc -= w;
					}
					else if ( w < 0 && FATALERRNO )
					{
						LOG_ERROR;
						break;
					}
				}
#ifdef DELAYED_FAST_TRICKMODE
				if((playMode == pmFast) && (last_index >= 0)) {
					int i = playFrame->Index()-last_index;
					if (playDir == pdBackward) i = -i;
					if(!PATPMT) i=1;
					int delay = 1000000/(25*Speeds[trickSpeed]*2);
					printf("Mode %d Dir %d Trick %d Speed %d (%d * (%d = %d - %d))\n", playMode, playDir, trickSpeed, Speeds[trickSpeed], delay, i, playFrame->Index(), last_index);
					while(i-- > 0 && delay > 0 && delay <= 1000000)
						usleep(delay); 
				} else if ( playMode == pmSlow && playDir == pdBackward ) {
					int delay = (2*Speeds[trickSpeed]*1000000)/-8;
					printf("SlowBack %d %d %d\n", delay, trickSpeed, Speeds[trickSpeed]);
					if(delay > 0 && delay <= 2000000)
						usleep(delay);
				} // if
				last_index = playFrame->Index();
#endif
				if ( pc == 0 )
				{
					writeIndex = playFrame->Index();
					backTrace->Add ( playFrame->Index(), playFrame->Count() );
					ringBuffer->Drop ( playFrame );
					playFrame = NULL;
					p = NULL;
				}
			}
			else
			{
				if ( LastMarkPause )
				{
					LastMarkPause = false;
					playMode = pmPause;
					writeIndex = readIndex;
				}
				Sleep = true;
			}
		}
	}

	cNonBlockingFileReader *nbfr = nonBlockingFileReader;
	nonBlockingFileReader = NULL;
	delete nbfr;
}

void cDvbPlayer::Pause ( void )
{
	if ( playMode == pmPause || playMode == pmStill )
		Play();
	else
	{
		LOCK_THREAD;
		if ( playMode == pmFast || ( playMode == pmSlow && playDir == pdBackward ) )
			Empty();
		DeviceFreeze();
		playMode = pmPause;
	}
}

void cDvbPlayer::Play ( void )
{
	if ( playMode != pmPlay )
	{
		LOCK_THREAD;
		if ( playMode == pmStill || playMode == pmFast || ( playMode == pmSlow && playDir == pdBackward ) )
			Empty();
		DevicePlay();
		playMode = pmPlay;
		playDir = pdForward;
	}
}

void cDvbPlayer::Forward ( void )
{
	if ( index )
	{
		switch ( playMode )
		{
			case pmFast:
				if ( Setup.MultiSpeedMode )
				{
					TrickSpeed ( playDir == pdForward ? 1 : -1 );
					break;
				}
				else if ( playDir == pdForward )
				{
					Play();
					break;
				}
				// run into pmPlay
			case pmPlay:
			{
				LOCK_THREAD;
				Empty();
				DeviceMute();
				playMode = pmFast;
				playDir = pdForward;
				trickSpeed = NORMAL_SPEED;
				TrickSpeed ( Setup.MultiSpeedMode ? 1 : MAX_SPEEDS );
				init_repeat=5;
			}
			break;
			case pmSlow:
				if ( Setup.MultiSpeedMode )
				{
					TrickSpeed ( playDir == pdForward ? -1 : 1 );
					break;
				}
				else if ( playDir == pdForward )
				{
					Pause();
					break;
				}
				// run into pmPause
			case pmStill:
			case pmPause:
				DeviceMute();
				playMode = pmSlow;
				playDir = pdForward;
				trickSpeed = NORMAL_SPEED;
				TrickSpeed ( Setup.MultiSpeedMode ? -1 : -MAX_SPEEDS );
				break;
		}
	}
}

void cDvbPlayer::Backward ( void )
{
	if ( index )
	{
		switch ( playMode )
		{
			case pmFast:
				if ( Setup.MultiSpeedMode )
				{
					TrickSpeed ( playDir == pdBackward ? 1 : -1 );
					break;
				}
				else if ( playDir == pdBackward )
				{
					Play();
					break;
				}
				// run into pmPlay
			case pmPlay:
			{
				LOCK_THREAD;
				Empty();
				DeviceMute();
				playMode = pmFast;
				playDir = pdBackward;
				trickSpeed = NORMAL_SPEED;
				TrickSpeed ( Setup.MultiSpeedMode ? 1 : MAX_SPEEDS );
				init_repeat=5;
			}
			break;
			case pmSlow:
				if ( Setup.MultiSpeedMode )
				{
					TrickSpeed ( playDir == pdBackward ? -1 : 1 );
					break;
				}
				else if ( playDir == pdBackward )
				{
					Pause();
					break;
				}
				// run into pmPause
			case pmStill:
			case pmPause:
			{
				LOCK_THREAD;
				Empty();
				DeviceMute();
				playMode = pmSlow;
				playDir = pdBackward;
				trickSpeed = NORMAL_SPEED;
				TrickSpeed ( Setup.MultiSpeedMode ? -1 : -MAX_SPEEDS );
				init_repeat=5;
			}
			break;
		}
	}
}

int cDvbPlayer::SkipFrames ( int Frames )
{
	if ( index && Frames )
	{
		int Current, Total;
		GetIndex ( Current, Total, true );
		int OldCurrent = Current;
		// As GetNextIFrame() increments/decrements at least once, the
		// destination frame (= Current + Frames) must be adjusted by
		// -1/+1 respectively.
		Current = index->GetNextIFrame ( Current + Frames + ( Frames > 0 ? -1 : 1 ), Frames > 0 );
		return Current >= 0 ? Current : OldCurrent;
	}
	return -1;
}

void cDvbPlayer::SkipSeconds ( int Seconds )
{
	if ( index && Seconds )
	{
		LOCK_THREAD;
		Empty();
		int Index = writeIndex;
		if ( Index >= 0 )
		{
			Index = max ( Index + Seconds * FRAMESPERSEC, 0 );
			if ( Index > 0 )
				Index = index->GetNextIFrame ( Index, false, NULL, NULL, NULL, true );
			if ( Index >= 0 )
				readIndex = writeIndex = Index - 1; // Action() will first increment it!
		}
		Play();
	}
}

void cDvbPlayer::Goto ( int Index, bool Still )
{
	printf("goto %d %d\n", Index, Still);
	if ( index )
	{
		LOCK_THREAD;
		Empty();
		if ( ++Index <= 0 )
			Index = 1; // not '0', to allow GetNextIFrame() below to work!
		uchar FileNumber;
		int FileOffset, Length;
		Index = index->GetNextIFrame ( Index, false, &FileNumber, &FileOffset, &Length );
		if ( Index >= 0 && NextFile ( FileNumber, FileOffset ) && Still )
		{
			uchar b[MAXFRAMESIZE + 4 + 5 + 4];
			int r = ReadFrame ( replayFile, b, Length, sizeof ( b ) );
			if ( r > 0 )
			{
				if ( playMode == pmPause )
					DevicePlay();
				// append sequence end code to get the image shown immediately with softdevices
				if ( r > 6 && ( b[3] & 0xF0 ) == 0xE0 )   // make sure to append it only to a video packet
				{
					b[r++] = 0x00;
					b[r++] = 0x00;
					b[r++] = 0x01;
					b[r++] = b[3];
					if ( b[6] & 0x80 ) // MPEG 2
					{
						b[r++] = 0x00;
						b[r++] = 0x07;
						b[r++] = 0x80;
						b[r++] = 0x00;
						b[r++] = 0x00;
					}
					else   // MPEG 1
					{
						b[r++] = 0x00;
						b[r++] = 0x05;
						b[r++] = 0x0F;
					}
					b[r++] = 0x00;
					b[r++] = 0x00;
					b[r++] = 0x01;
					b[r++] = 0xB7;
				}

				int tsstartcode = 0x00000047;
				if ( r >= 4 && ( * ( ( int* ) b ) & 0x000000FF ) == tsstartcode ) //TS-Packet
				{
					//printf("--------------cDvbPlayer::Goto: TS-------------\n");
					if ( PATPMT )
					{
						PlayTS ( NULL, 0, false, PATPMT );
						for ( uint i = 0; i < 10; ++i )
						{
							PlayTS ( b, Length, false );
						}
					}
				}
				else
				{
					DeviceStillPicture ( b, r );
				}
			}
			playMode = pmStill;
		}
		readIndex = writeIndex = Index;
	}
}

bool cDvbPlayer::GetIndex ( int &Current, int &Total, bool SnapToIFrame )
{
	if ( index )
	{
		if ( playMode == pmStill )
			Current = max ( readIndex, 0 );
		else
		{
			Current = max ( writeIndex, 0 );
			if ( SnapToIFrame )
			{
				int i1 = index->GetNextIFrame ( Current + 1, false );
				int i2 = index->GetNextIFrame ( Current, true );
				Current = ( abs ( Current - i1 ) <= abs ( Current - i2 ) ) ? i1 : i2;
			}
		}
		Total = index->Last();
		return true;
	}
	Current = Total = -1;
	return false;
}

bool cDvbPlayer::GetReplayMode ( bool &Play, bool &Forward, int &Speed )
{
	Play = ( playMode == pmPlay || playMode == pmFast );
	Forward = ( playDir == pdForward );
	if ( playMode == pmFast || playMode == pmSlow )
		Speed = Setup.MultiSpeedMode ? abs ( trickSpeed - NORMAL_SPEED ) : 0;
	else
		Speed = -1;
	return true;
}


void cDvbPlayer::CheckTS()
{
	uchar * pp = NULL;
	pp = MALLOC ( uchar, 2*TS_SIZE );
	if ( pp != NULL )
	{
		if ( replayFile->Read ( pp, 2*TS_SIZE ) == 2*TS_SIZE )
		{
			if ( ( *pp == 0x47 && * ( pp+1 ) == 0x40 ) && ( * ( pp+TS_SIZE ) == 0x47 && * ( pp+TS_SIZE+1 ) == 0x40 ) )
			{
				printf ( "CheckTS(): TS recognized\n" );
				PATPMT = pp;
			}
			else
			{
				printf ( "CheckTS(): PES recognized\n" );
				PATPMT = NULL;
				free ( pp );
			}
		}
		if ( PATPMT == NULL )
			replayFile->Seek ( 0,SEEK_SET );
	}
}

// --- cDvbPlayerControl -----------------------------------------------------

cDvbPlayerControl::cDvbPlayerControl ( const char *FileName )
		:cControl ( player = new cDvbPlayer ( FileName ) )
{
}

cDvbPlayerControl::~cDvbPlayerControl()
{
	Stop();
}

bool cDvbPlayerControl::Active ( void )
{
	return player && player->Active();
}

void cDvbPlayerControl::Stop ( void )
{
	cStatus::MsgReplaying ( this, NULL, NULL, false ); // GT: otherwise race condition (GraphLCD).
	delete player;
	player = NULL;
}

void cDvbPlayerControl::Pause ( void )
{
	if ( player )
		player->Pause();
}

void cDvbPlayerControl::Play ( void )
{
	if ( player )
		player->Play();
}

void cDvbPlayerControl::Forward ( void )
{
	if ( player )
		player->Forward();
}

void cDvbPlayerControl::Backward ( void )
{
	if ( player )
		player->Backward();
}

void cDvbPlayerControl::SkipSeconds ( int Seconds )
{
	if ( player )
		player->SkipSeconds ( Seconds );
}

int cDvbPlayerControl::SkipFrames ( int Frames )
{
	if ( player )
		return player->SkipFrames ( Frames );
	return -1;
}

bool cDvbPlayerControl::GetIndex ( int &Current, int &Total, bool SnapToIFrame )
{
	if ( player )
	{
		player->GetIndex ( Current, Total, SnapToIFrame );
		return true;
	}
	return false;
}

bool cDvbPlayerControl::GetReplayMode ( bool &Play, bool &Forward, int &Speed )
{
	return player && player->GetReplayMode ( Play, Forward, Speed );
}

void cDvbPlayerControl::Goto ( int Position, bool Still )
{
	if ( player )
		player->Goto ( Position, Still );
}
