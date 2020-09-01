/* Copyright (c) 2014-2018 Kernel Labs Inc. All Rights Reserved. */

/* Objectives:
   Allow for the playout of previously captured klvanc_capture mux files
   Allow for output of PRBS in selected audio channel(s)
   Allow for output of certain VANC test sequences
*/

/* FIXME: setup an autoconf rule for this */
#define USE_KLBARS 1

#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#include <assert.h>
#include <libgen.h>
#include <signal.h>
#include <libklvanc/vanc.h>
#ifdef USE_KLBARS
#include <libklbars/klbars.h>
#endif
#include "frame-writer.h"
#include "klburnin.h"

#include "hexdump.h"
#include "version.h"
#include "DeckLinkAPI.h"

static struct fwr_session_s *session;

#ifdef USE_KLBARS
struct kl_colorbar_context klbars_ctx;
#endif

// Keep track of the number of scheduled frames
uint32_t gTotalFramesScheduled = 0;

/* Initial output mode */
const BMDVideoOutputFlags kOutputFlag = bmdVideoOutputVANC;

/* Values get populated once the video mode is selected */
BMDTimeValue kFrameDuration = 0;
BMDTimeScale kTimeScale = 0;
uint32_t kFrameWidth = 0;
uint32_t kFrameHeight = 0;
uint32_t kRowBytes = 0;

// 10-bit YUV colour pixels
const uint32_t kBlueData[] = { 0x40aa298, 0x2a8a62a8, 0x298aa040, 0x2a8102a8 };

static struct fwr_header_timing_s ftlast;

/* Decklink portability macros */
#ifdef _WIN32
static char *dup_wchar_to_utf8(wchar_t *w)
{
    char *s = NULL;
    int l = WideCharToMultiByte(CP_UTF8, 0, w, -1, 0, 0, 0, 0);
    s = (char *) av_malloc(l);
    if (s)
        WideCharToMultiByte(CP_UTF8, 0, w, -1, s, l, 0, 0);
    return s;
}
#define DECKLINK_STR    OLECHAR *
#define DECKLINK_STRDUP dup_wchar_to_utf8
#define DECKLINK_FREE(s) SysFreeString(s)
#elif defined(__APPLE__)
static char *dup_cfstring_to_utf8(CFStringRef w)
{
    char s[256];
    CFStringGetCString(w, s, 255, kCFStringEncodingUTF8);
    return strdup(s);
}
#define DECKLINK_STR    const __CFString *
#define DECKLINK_STRDUP dup_cfstring_to_utf8
#define DECKLINK_FREE(s) CFRelease(s)
#else
#define DECKLINK_STR    const char *
#define DECKLINK_STRDUP strdup
/* free() is needed for a string returned by the DeckLink SDL. */
#define DECKLINK_FREE(s) free((void *) s)
#endif

#define RELEASE_IF_NOT_NULL(obj) \
        if (obj != NULL) { \
                obj->Release(); \
                obj = NULL; \
        }

static BMDDisplayMode selectedDisplayMode = bmdModeNTSC;
static struct klvanc_context_s *vanchdl;
static pthread_mutex_t sleepMutex;
static pthread_cond_t sleepCond;

static int g_verbose = 0;
static uint64_t lastGoodKLFrameCounter = 0;

static IDeckLink *deckLink;
static IDeckLinkDisplayModeIterator *displayModeIterator;

static uint32_t g_audioChannels = 16;
static uint32_t g_audioSampleDepth = 32;
static int g_muxedOutputExcludeVideo = 0;
static int g_muxedOutputExcludeAudio = 0;
static int g_muxedOutputExcludeData = 0;
static const char *g_muxedInputFilename = NULL;
static int g_shutdown = 0;
#ifdef USE_KLBARS
static enum kl_colorbar_pattern g_barFormat = KL_COLORBAR_SMPTE_RP_219_1;
#endif
static BMDDisplayMode g_detected_mode_id = 0;
static BMDDisplayMode g_requested_mode_id = 0;
static BMDVideoInputFlags g_inputFlags = bmdVideoInputEnableFormatDetection;
static BMDPixelFormat g_pixelFormat = bmdFormat10BitYUV;
static bool wantKlCounters = false;

static unsigned long audioFrameCount = 0;
static struct frameTime_s {
	unsigned long long lastTime;
	unsigned long long frameCount;
	unsigned long long remoteFrameCount;
} frameTimes[2];

static void signal_handler(int signum)
{
	g_shutdown = 1;
	pthread_cond_signal(&sleepCond);
}

static void listDisplayModes()
{
	int displayModeCount = 0;
	IDeckLinkDisplayMode *displayMode;
	while (displayModeIterator->Next(&displayMode) == S_OK) {

		char * displayModeString = NULL;
		DECKLINK_STR displayModeStringTmp = NULL;
		HRESULT result = displayMode->GetName(&displayModeStringTmp);
		if (result == S_OK) {
			BMDTimeValue frameRateDuration, frameRateScale;
			displayMode->GetFrameRate(&frameRateDuration, &frameRateScale);
			displayModeString = DECKLINK_STRDUP(displayModeStringTmp);
			DECKLINK_FREE(displayModeStringTmp);
			fprintf(stderr, "        %c%c%c%c : %-20s \t %li x %li \t %7g FPS\n",
				displayMode->GetDisplayMode() >> 24,
				displayMode->GetDisplayMode() >> 16,
				displayMode->GetDisplayMode() >>  8,
				displayMode->GetDisplayMode(),
				displayModeString,
				displayMode->GetWidth(),
				displayMode->GetHeight(),
				(double)frameRateScale /
				(double)frameRateDuration);

			free(displayModeString);
			displayModeCount++;
		}

		displayMode->Release();
	}
}

static int usage(const char *progname, int status)
{
	fprintf(stderr, COPYRIGHT "\n");
	fprintf(stderr, "Playout to decklink SDI port, including video and VANC\n");
	fprintf(stderr, "Version: " GIT_VERSION "\n");
	fprintf(stderr, "Usage: %s [OPTIONS]\n", basename((char *)progname));
	fprintf(stderr,
		"    -o <number>     Output to device <number> (def: 0)\n"
		"    -L              List available display modes\n"
		"    -m <mode>       Force to output in specified mode\n"
		"                    Eg. Hi59 (1080i59), hp60 (1280x720p60) Hp60 (1080p60) (def: ntsc):\n"
		"                    See -L for a complete list of which modes are supported by the output hardware.\n"
		"                    If not specified and a mux file is specified, that mode will be used.\n"
		"    -b <pattern>    Select pattern to output (def: 0 [SMPTE RP-219-1 HD colorbars])\n"
		"    -k              Burn in kl counters into video\n"
		"    -c <channels>   Audio Channels (2, 8 or 16 - def: %d)\n"
		"    -s <depth>      Audio Sample Depth (16 or 32 - def: %d)\n"
		"    -X <filename>   Analyze a muxed audio+video+vanc input file.\n"
		"    -ev             Exclude output of video from muxed file.\n"
		"    -ea             Exclude output of audio from muxed file.\n"
		"    -ed             Exclude output of data (vanc) from muxed file.\n"
		"    -v              Increase level of verbosity (def: 0)\n"
		"\n"
		);

	fprintf(stderr, "Example use cases:\n"
		"1) Output a mux file previously captured via klvanc_capture:\n"
		"\tklvanc_player -X inputfile.mux\n"
		"2) Output HD color bars to the second SDI port:\n"
		"\tklvanc_player -o 1 -b 0\n"
		);

	exit(status);
}
static char g_mode[5];		/* Racey */
static const char *display_mode_to_string(BMDDisplayMode m)
{
	g_mode[4] = 0;
	g_mode[3] = m;
	g_mode[2] = m >> 8;
	g_mode[1] = m >> 16;
	g_mode[0] = m >> 24;

	return &g_mode[0];
}

static int demux_single_frame(IDeckLinkVideoFrame* videoFrame, IDeckLinkVideoFrameAncillary *ancillaryData)
{
	struct fwr_header_audio_s *fa;
	struct fwr_header_video_s *fv;
	struct fwr_header_timing_s ft;
	struct fwr_header_vanc_s *fd;
	uint32_t header;
	HRESULT	result = S_OK;
	uint32_t *buffer;
	struct timeval tv1, tv2;
	int eos = 0;

	gettimeofday(&tv1, NULL);

	if (ftlast.counter == -1) {
		/* We never found a timing frame, so we cannot demux */
		return -1;
	}

	while (1) {
		fa = NULL;
		fv = NULL;
		if (fwr_session_frame_gettype(session, &header) < 0) {
			eos = 1;
			break;
		}

		if (header == timing_v1_header) {
			if (fwr_timing_frame_read(session, &ft) < 0) {
				eos = 1;
				break;
			}
			ftlast = ft;
			printf("timing: counter %" PRIu64 "  mode:%s  ts:%d.%06d\n",
			       ft.counter,
			       display_mode_to_string(ft.decklinkCaptureMode),
			       ft.ts1.tv_sec,
			       ft.ts1.tv_usec);

			if (ft.decklinkCaptureMode != selectedDisplayMode) {
				selectedDisplayMode = ft.decklinkCaptureMode;
				fprintf(stderr, "Change in frame format detected!\n");
				g_shutdown = 3;
				pthread_cond_signal(&sleepCond);
			}

			/* We hit the next timing header, so stop processing */
			break;
		} else if (header == video_v1_header) {
			if (fwr_video_frame_read(session, &fv) < 0) {
				fprintf(stderr, "No more video?\n");
				eos = 1;
				break;
			}
			printf("\tvideo: %d x %d  strideBytes: %d  bufferLengthBytes: %d\n",
				fv->width, fv->height, fv->strideBytes, fv->bufferLengthBytes);

			uint8_t *nextWord;
			uint8_t *cur = fv->ptr;

			/* Deal with the possibility that the video frame in the mux file
			   doesn't match the actual output video mode */
			size_t num_copy = fv->strideBytes;
			size_t num_rows = fv->height;
			if (num_copy > kRowBytes)
				num_copy = kRowBytes;
			if (num_rows > kFrameHeight)
				num_rows = kFrameHeight;

			videoFrame->GetBytes((void**)&nextWord);
			for (int i = 0; i < num_rows; i++) {
				memcpy(nextWord, cur, num_copy);
				nextWord += num_copy;
				cur += fv->strideBytes;
			}
		} else if (header == VANC_SOL_INDICATOR) {
			if (fwr_vanc_frame_read(session, &fd) < 0) {
				fprintf(stderr, "No more vanc?\n");
				eos = 1;
				break;
			}
#if 0
			printf("\t\tvanc: line: %4d -- ", fd->line);
			for (int i = 0; i < 32; i++)
				printf("%02x ", *(fd->ptr + i));
			printf("\n");
#endif
			// DJH TESTING
			result = ancillaryData->GetBufferForVerticalBlankingLine(fd->line, (void **)&buffer);
			if (result != S_OK)
			{
				fprintf(stderr, "Could not get buffer for Vertical blanking line %d - result = %08x\n",
					fd->line, result);
				continue;
			}
			memcpy(buffer, fd->ptr, fd->strideBytes);


		} else if (header == audio_v1_header) {
			if (fwr_pcm_frame_read(session, &fa) < 0) {
				eos = 1;
				break;
			}
			printf("\taudio: channels: %d  depth: %d  frameCount: %d  bufferLengthBytes: %d\n",
				fa->channelCount,
				fa->sampleDepth,
				fa->frameCount,
				fa->bufferLengthBytes);
		}

		if (fa) {
			fwr_pcm_frame_free(session, fa);
			fa = NULL;
		}
		if (fv) {
			fwr_video_frame_free(session, fv);
			fv = NULL;
		}
	}

	gettimeofday(&tv2, NULL);
	fprintf(stderr, "In: %d.%06d Out: %d.%06d\n", tv1.tv_sec, tv1.tv_usec,
		tv2.tv_sec, tv2.tv_usec);

	return eos;
}

static void fillVideo(IDeckLinkMutableVideoFrame* theFrame)
{
	uint32_t* nextWord;
	theFrame->GetBytes((void**)&nextWord);

#ifdef USE_KLBARS
	kl_colorbar_fill_pattern(&klbars_ctx, g_barFormat);

	char buf[64];
	snprintf(buf, sizeof(buf), "%d", gTotalFramesScheduled);
	kl_colorbar_render_string(&klbars_ctx, buf, strlen(buf), 1, 1);
	kl_colorbar_finalize(&klbars_ctx, (unsigned char *) nextWord, KL_COLORBAR_10BIT, kRowBytes);
#else
	/* Without klbars support, just fill the field with blue video */
	uint32_t wordsRemaining = (kRowBytes * kFrameHeight) / 4;
	while (wordsRemaining > 0)
	{
		*(nextWord++) = kBlueData[0];
		*(nextWord++) = kBlueData[1];
		*(nextWord++) = kBlueData[2];
		*(nextWord++) = kBlueData[3];
		wordsRemaining = wordsRemaining - 4;
	}
#endif
	/* Burn counter into video if desired */
	if (wantKlCounters) {
		theFrame->GetBytes((void**)&nextWord);
		klburnin_V210_write_32bit_value(nextWord, kRowBytes, gTotalFramesScheduled, 0);
	}
}

class OutputCallback: public IDeckLinkVideoOutputCallback
{
private:
	int32_t				m_refCount;
	IDeckLinkOutput*	m_deckLinkOutput;
public:
	OutputCallback(IDeckLinkOutput* deckLinkOutput)
	: m_refCount(1), m_deckLinkOutput(deckLinkOutput)
	{
		m_deckLinkOutput->AddRef();
	}
	virtual ~OutputCallback(void)
	{
		m_deckLinkOutput->Release();
	}
	
	HRESULT ScheduleNextFrame(IDeckLinkVideoFrame* videoFrame)
	{
		HRESULT	result = S_OK;
		IDeckLinkVideoFrameAncillary *ancillaryData = NULL;

		if (g_shutdown > 0)
			return result;

		result = videoFrame->GetAncillaryData(&ancillaryData);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not get ancillary data = %08x\n", result);
			goto bail;
		}

		if (session) {
			int ret = demux_single_frame(videoFrame, ancillaryData);
			if (ret != 0) {
				/* Don't schedule any more frames */
				if (ret == 1) {
					g_shutdown = 1;
					pthread_cond_signal(&sleepCond);
				}
				goto bail;
			}
		} else {
			fillVideo((IDeckLinkMutableVideoFrame *) videoFrame);
		}

		// When a video frame completes,reschedule another frame
		result = m_deckLinkOutput->ScheduleVideoFrame(videoFrame, gTotalFramesScheduled*kFrameDuration, kFrameDuration, kTimeScale);
		if (result != S_OK)
		{
			fprintf(stderr, "Could not schedule video frame = %08x\n", result);
			goto bail;
		}
		
		gTotalFramesScheduled++;
		
	bail:
#if 0		
		if (ancillaryData != NULL)
		{
			ancillaryData->Release();
			ancillaryData = NULL;
		}
#endif	
		return result;
	}
	
	HRESULT	STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult)
	{
		HRESULT result = ScheduleNextFrame(completedFrame);
		
		if (result != S_OK)
		{
			completedFrame->Release();
			completedFrame = NULL;
		}
		
		return result;
	}
	
	HRESULT	STDMETHODCALLTYPE ScheduledPlaybackHasStopped(void)
	{
		if (g_shutdown == 1)
			g_shutdown = 2;
		return S_OK;
	}
	
	// IUnknown
	HRESULT	STDMETHODCALLTYPE QueryInterface (REFIID iid, LPVOID *ppv)
	{
		*ppv = NULL;
		return E_NOINTERFACE;
	}
	
	ULONG STDMETHODCALLTYPE AddRef()
	{
		// gcc atomic operation builtin
		return __sync_add_and_fetch(&m_refCount, 1);
	}
	
	ULONG STDMETHODCALLTYPE Release()
	{
		// gcc atomic operation builtin
		int32_t newRefValue = __sync_sub_and_fetch(&m_refCount, 1);
		
		if (newRefValue == 0)
			delete this;
		
		return newRefValue;
	}
};


static IDeckLinkMutableVideoFrame* CreateFrame(IDeckLinkOutput* deckLinkOutput)
{
	HRESULT                         result;
	IDeckLinkMutableVideoFrame*     frame = NULL;
	IDeckLinkVideoFrameAncillary*	ancillaryData = NULL;
	
	result = deckLinkOutput->CreateVideoFrame(kFrameWidth, kFrameHeight, kRowBytes, g_pixelFormat, bmdFrameFlagDefault, &frame);
	if (result != S_OK)
	{
		fprintf(stderr, "Could not create a video frame - result = %08x\n", result);
		goto bail;
	}
	
	result = deckLinkOutput->CreateAncillaryData(g_pixelFormat, &ancillaryData);
	if (result != S_OK)
	{
		fprintf(stderr, "Could not create Ancillary data - result = %08x\n", result);
		goto bail;
	}

	// Ancillary data filled in callback
	result = frame->SetAncillaryData(ancillaryData);
	if (result != S_OK)
	{
		fprintf(stderr, "Fail to set ancillary data to the frame - result = %08x\n", result);
		goto bail;
	}
	
bail:
	// Release the Ancillary object
	if (ancillaryData != NULL)
		ancillaryData->Release();
	
	return frame;
}


static int startupVideo(IDeckLinkOutput *deckLinkOutput, OutputCallback *outputCallback)
{
	IDeckLinkDisplayMode *displayMode;
	HRESULT result;
	bool active;

	result = deckLinkOutput->IsScheduledPlaybackRunning(&active);
	fprintf(stderr, "playback running result=%d active=%d!\n", result, active);
	if (result == S_OK && active) {
		/* Stop video if already running */
		result = deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
		if (result != S_OK) {
			fprintf(stderr, "Failed to stop playback\n");
			return -1;
		}

		fprintf(stderr, "Disabling video output\n");
		deckLinkOutput->DisableVideoOutput();
		fprintf(stderr, "Done waiting for disable\n");
	}

	/* Confirm the user-requested display mode and other settings are valid for this device. */
	BMDDisplayModeSupport dm;
	deckLinkOutput->DoesSupportVideoMode(selectedDisplayMode, g_pixelFormat,
					     g_inputFlags, &dm, &displayMode);
	if (dm == bmdDisplayModeNotSupported) {
		fprintf(stderr, "The requested display mode [%s] is not supported with the selected pixel format\n", display_mode_to_string(selectedDisplayMode));
		return -1;
	}

	BMDTimeValue frameRateDuration, frameRateScale;
	displayMode->GetFrameRate(&kFrameDuration, &kTimeScale);
	kFrameWidth = displayMode->GetWidth();
	kFrameHeight = displayMode->GetHeight();
	/* 10-bit YUV row bytes, ref. SDK Manual "2.7.4 Pixel Formats" / bmdFormat10BitYUV */
	kRowBytes = ((kFrameWidth + 47) / 48) * 128;

	// Enable video output
	result = deckLinkOutput->EnableVideoOutput(selectedDisplayMode, kOutputFlag);
	if (result != S_OK)
	{
		fprintf(stderr, "Could not enable video output - result = %08x\n", result);
		return -1;
	}
	gTotalFramesScheduled = 0;

#ifdef USE_KLBARS
	kl_colorbar_init(&klbars_ctx, kFrameWidth, kFrameHeight, KL_COLORBAR_10BIT);
	if (g_muxedInputFilename == NULL) {
		const char *barPatternName = kl_colorbar_get_pattern_name(&klbars_ctx, g_barFormat);
		if (barPatternName != NULL)
			printf("Pattern selected: %s\n", barPatternName);
	}
#endif
	for (int i = 0; i < 3; i++)
	{
		IDeckLinkMutableVideoFrame *videoFrame = NULL;
		
		// Create a frame with defined format
		videoFrame = CreateFrame(deckLinkOutput);
		if (!videoFrame)
			return -1;

		fillVideo(videoFrame);
		
		result = outputCallback->ScheduleNextFrame(videoFrame);
		if (result != S_OK)
		{
			videoFrame->Release();
			
			fprintf(stderr, "Could not schedule video frame - result = %08x\n", result);
			return -1;
		}
		gTotalFramesScheduled++;
	}

	// Start
	result = deckLinkOutput->StartScheduledPlayback(0, kTimeScale, 1.0);
	if (result != S_OK)
	{
		fprintf(stderr, "Could not start - result = %08x\n", result);
		return -1;
	}

	return 0;
}

static int _main(int argc, char *argv[])
{
	IDeckLinkIterator *deckLinkIterator = CreateDeckLinkIteratorInstance();
	IDeckLinkOutput *deckLinkOutput = NULL;
	OutputCallback *outputCallback = NULL;

	int displayModeCount = 0;
	int exitStatus = 1;
	int ch;
	int portnr = 0;
	bool wantHelp = false;
	bool wantDisplayModes = false;
	HRESULT result;

	pthread_mutex_init(&sleepMutex, NULL);
	pthread_cond_init(&sleepCond, NULL);

	while ((ch = getopt(argc, argv, "?h3kc:s:m:p:vV:o:LX:R:e:b:")) != -1) {
		switch (ch) {
		case 'm':
			selectedDisplayMode  = *(optarg + 0) << 24;
			selectedDisplayMode |= *(optarg + 1) << 16;
			selectedDisplayMode |= *(optarg + 2) <<  8;
			selectedDisplayMode |= *(optarg + 3);
			g_requested_mode_id = selectedDisplayMode;
			g_detected_mode_id = selectedDisplayMode;
			break;
		case 'X':
			g_muxedInputFilename = optarg;
			break;
		case 'e':
			switch (optarg[0]) {
			case 'v':
				g_muxedOutputExcludeVideo = 1;
				break;
			case 'a':
				g_muxedOutputExcludeAudio = 1;
				break;
			case 'd':
				g_muxedOutputExcludeData = 1;
				break;
			default:
				fprintf(stderr, "Only valid types to exclude are video/audio/data\n");
				goto bail;
			}
			break;
#ifdef USE_KLBARS
		case 'b':
			g_barFormat = (kl_colorbar_pattern) atoi(optarg);
			break;
#endif
		case 'c':
			g_audioChannels = atoi(optarg);
			if (g_audioChannels != 2 && g_audioChannels != 8 && g_audioChannels != 16) {
				fprintf(stderr, "Invalid argument: Audio Channels must be either 2, 8 or 16\n");
				goto bail;
			}
			break;
		case 's':
			g_audioSampleDepth = atoi(optarg);
			if (g_audioSampleDepth != 16 && g_audioSampleDepth != 32) {
				fprintf(stderr, "Invalid argument: Audio Sample Depth must be either 16 bits or 32 bits\n");
				goto bail;
			}
			break;
		case 'o':
			portnr = atoi(optarg);
			break;
		case 'L':
			wantDisplayModes = true;
			break;
		case 'v':
			g_verbose++;
			break;
		case 'k':
			wantKlCounters = true;
			break;
		case '3':
			g_inputFlags |= bmdVideoInputDualStream3D;
			break;
		case 'p':
			switch (atoi(optarg)) {
			case 0:
				g_pixelFormat = bmdFormat8BitYUV;
				break;
			case 1:
				g_pixelFormat = bmdFormat10BitYUV;
				break;
			case 2:
				g_pixelFormat = bmdFormat10BitRGB;
				break;
			default:
				fprintf(stderr, "Invalid argument: Pixel format %d is not valid", atoi(optarg));
				goto bail;
			}
			break;
		case '?':
		case 'h':
			wantHelp = true;
		}
	}

	if (wantHelp) {
		usage(argv[0], 0);
		goto bail;
	}

	if (g_muxedInputFilename != NULL) {
		ftlast.counter = -1;
		if (fwr_session_file_open(g_muxedInputFilename, 0, &session) < 0) {
			fprintf(stderr, "Error opening %s\n", g_muxedInputFilename);
			return -1;
		}

		/* We need to search until we find a timing frame before we can start */
		struct fwr_header_audio_s *fa;
		struct fwr_header_video_s *fv;
		struct fwr_header_timing_s ft;
		struct fwr_header_vanc_s *fd;
		uint32_t header;
		while (1) {
			if (fwr_session_frame_gettype(session, &header) < 0) {
				break;
			}
			if (header == timing_v1_header) {
				if (fwr_timing_frame_read(session, &ft) < 0) {
					break;
				}
				/* Ok, we have enough to proceed with */
				ftlast = ft;
				selectedDisplayMode = ft.decklinkCaptureMode;
				break;
			} else if (header == video_v1_header) {
				if (fwr_video_frame_read(session, &fv) < 0) {
					fprintf(stderr, "No more video?\n");
					break;
				}
				fwr_video_frame_free(session, fv);
			} else if (header == VANC_SOL_INDICATOR) {
				if (fwr_vanc_frame_read(session, &fd) < 0) {
					fprintf(stderr, "No more vanc?\n");
					break;
				}
			} else if (header == audio_v1_header) {
				if (fwr_pcm_frame_read(session, &fa) < 0) {
					break;
				}
				fwr_pcm_frame_free(session, fa);
			}
		}
	}

        if (klvanc_context_create(&vanchdl) < 0) {
                fprintf(stderr, "Error initializing library context\n");
                exit(1);
        }

	/* We specifically want to see packets that have bad checksums. */
	vanchdl->allow_bad_checksums = 1;
	vanchdl->warn_on_decode_failure = 1;
	vanchdl->verbose = g_verbose;

	if (!deckLinkIterator) {
		fprintf(stderr, "This application requires the DeckLink drivers installed.\n");
		goto bail;
	}

	for (int i = 0; i <= portnr; i++) {
		/* Connect to the nth DeckLink instance */
		result = deckLinkIterator->Next(&deckLink);
		if (result != S_OK) {
			fprintf(stderr, "No output devices found.\n");
			goto bail;
		}
	}

	if (deckLink->QueryInterface(IID_IDeckLinkOutput, (void **)&deckLinkOutput) != S_OK) {
		fprintf(stderr, "No output devices found.\n");
		goto bail;
	}

	// Create an instance of output callback
	outputCallback = new OutputCallback(deckLinkOutput);
	if (outputCallback == NULL)
	{
		fprintf(stderr, "Could not create output callback object\n");
		goto bail;
	}

	result = deckLinkOutput->SetScheduledFrameCompletionCallback(outputCallback);
	if (result != S_OK)
	{
		fprintf(stderr, "Could not set output callback\n");
		goto bail;
	}

	/* Obtain an IDeckLinkDisplayModeIterator to enumerate the display
	   mode is supported on output */
	result = deckLinkOutput->GetDisplayModeIterator(&displayModeIterator);
	if (result != S_OK) {
		fprintf(stderr, "Could not obtain the video output display mode iterator - result = %08x\n", result);
		goto bail;
	}

	if (wantDisplayModes) {
		listDisplayModes();
		goto bail;
	}

	startupVideo(deckLinkOutput, outputCallback);

	signal(SIGINT, signal_handler);
	signal(SIGUSR1, signal_handler);
	signal(SIGUSR2, signal_handler);

	/* All Okay. */
	exitStatus = 0;

	/* Block main thread until signal occurs or format changes */
	while (1) {
		pthread_mutex_lock(&sleepMutex);
		while (g_shutdown == 0)
			pthread_cond_wait(&sleepCond, &sleepMutex);
		pthread_mutex_unlock(&sleepMutex);
		
		if (g_shutdown == 1) {
			result = deckLinkOutput->StopScheduledPlayback(0, NULL, 0);
			if (result != S_OK) {
				fprintf(stderr, "Failed to stop playback\n");
				goto bail;
			}
			
			fprintf(stderr, "Waiting for playback to stop...\n");
			while (g_shutdown != 2) {
				usleep(50 * 1000);
			}
			break;
		} else if (g_shutdown == 3) {
			/* Restart with different video mode */
			g_shutdown = 0;
			startupVideo(deckLinkOutput, outputCallback);
		}
	}

        klvanc_context_destroy(vanchdl);

bail:

	if (session)
		fwr_session_file_close(session);

	RELEASE_IF_NOT_NULL(displayModeIterator);
	RELEASE_IF_NOT_NULL(deckLinkOutput);
	RELEASE_IF_NOT_NULL(deckLink);
	RELEASE_IF_NOT_NULL(deckLinkIterator);

	return exitStatus;
}

extern "C" int player_main(int argc, char *argv[])
{
	return _main(argc, argv);
}
