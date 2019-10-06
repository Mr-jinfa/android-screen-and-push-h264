/*
 * Copyright 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <termios.h>
#include <unistd.h>

#define LOG_TAG "ScreenRecord"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS
#define LOG_NDEBUG 0
#include <utils/Log.h>

#include <binder/IPCThreadState.h>
#include <utils/Errors.h>
#include <utils/Timers.h>
#include <utils/Trace.h>

#include <gui/Surface.h>
#include <gui/SurfaceComposerClient.h>
#include <gui/ISurfaceComposer.h>
#include <ui/DisplayInfo.h>
#include <media/openmax/OMX_IVCommon.h>
#include <media/stagefright/foundation/ABuffer.h>
#include <media/stagefright/foundation/AMessage.h>
#include <media/stagefright/MediaCodec.h>
#include <media/stagefright/MediaErrors.h>
#include <media/stagefright/MediaMuxer.h>
#include <media/ICrypto.h>
#include <utils/Log.h>

#include "screenrecord.h"
#include "Overlay.h"
#include "FrameOutput.h"
#include "pushRaw.h"

using namespace android;

#define LOG_TAG "ScreenRecord"
	
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG,LOG_TAG ,__VA_ARGS__) // 定义LOGD类型   
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,LOG_TAG ,__VA_ARGS__) // 定义LOGI类型   
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,LOG_TAG ,__VA_ARGS__) // 定义LOGW类型   
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,LOG_TAG ,__VA_ARGS__) // 定义LOGE类型   
#define LOGF(...) __android_log_print(ANDROID_LOG_FATAL,LOG_TAG ,__VA_ARGS__) // 定义LOGF类型 

//定义一个nalu用于发送
padBuffer h264Nalu;
cmdBuffer streamCmd;


static const uint32_t kMinBitRate = 100000;         // 0.1Mbps
static const uint32_t kMaxBitRate = 200 * 1000000;  // 200Mbps
static const uint32_t kMaxTimeLimitSec = 3000;       // 10s
static const uint32_t kFallbackWidth = 1080;        // 1080p
static const uint32_t kFallbackHeight = 1920;
static const char* kMimeTypeAvc = "video/avc";

// Command-line parameters.
static bool gVerbose = true;           // chatty on stdout
static bool gRotate = false;            // rotate 90 degrees
static enum {
    FORMAT_MP4, FORMAT_H264, FORMAT_FRAMES, FORMAT_RAW_FRAMES
} gOutputFormat = FORMAT_H264;           // data format for output
static bool gSizeSpecified = false;     // was size explicitly requested?
static bool gWantInfoScreen = false;    // do we want initial info screen?
static bool gWantFrameTime = false;     // do we want times on each frame?
static uint32_t gVideoWidth = 0;        // default width+height
static uint32_t gVideoHeight = 0;
static uint32_t gBitRate = BITRATE;     // 4Mbps
static uint32_t gTimeLimitSec = kMaxTimeLimitSec;
#define DISPLAY_FPS 25
struct sockaddr_in client_addr[2];	//音频、视频各一个通道

//定义横屏命令00 00 00 01:头 0101:横竖屏切换标识 01:一字节长 0:横屏 (10为十个字节长,十进制)
char across_screen[] = {0x00,0x00,0x00,0x01, 0x01,0x01, 0x01, 0x0, };
//定义竖屏命令00 00 00 01:头 0101:横竖屏切换标识 01:一字节长 1:竖屏
char vertical_screen[] = {0x00,0x00,0x00,0x01, 0x01,0x01, 0x01, 0x01, };

// Set by signal handler to stop recording.
static volatile bool gStopRequested = false;

// Previous signal handler state, restored after first hit.
static struct sigaction gOrigSigactionINT;
static struct sigaction gOrigSigactionHUP;

int onDataSync=0;
int rtpStop=0;
pthread_mutex_t m;
pthread_cond_t v;


typedef enum {
    FRAME_ORIENTATION_0      = 0,	//竖屏
    FRAME_ORIENTATION_90     = 1,	//横屏
    FRAME_ORIENTATION_180    = 2,
    FRAME_ORIENTATION_270    = 3
} gFrmFOrientationAngle;

enum H264NALTYPE
	{
		H264NT_NAL = 0,
		H264NT_SLICE, //P 帧
		H264NT_SLICE_DPA,
		H264NT_SLICE_DPB,
		H264NT_SLICE_DPC,
		H264NT_SLICE_IDR, // I 帧
		H264NT_SEI,
		H264NT_SPS,
		H264NT_PPS,
   };


/*
*检查帧的类型
*/
int frame_struct(uint8_t *data, int flag)
{
	// 5bits, 7.3.1 NAL unit syntax,
	// ISO_IEC_14496-10-AVC-2003.pdf, page 44.
	//	7: SPS, 8: PPS, 5: I Frame, 1: P Frame, 9: AUD, 6: SEI	
	unsigned char nut = (char)data[4] & 0x1f;
	printf("%s %#x(%s)\n",flag==0?"from encode":"to decode", (char)data[4],
	(nut == H264NT_SPS? "SPS":(nut == H264NT_PPS? "PPS":
	(nut == H264NT_SLICE_IDR? "I":(nut == H264NT_SLICE? "P":(nut == 9? "AUD":(nut == H264NT_SEI? "SEI":"user-defined ")))))));
	return nut;

}

/* The display orientation when start scrrenrecord, used to
   calculate offset after rotation */
static int gOrigOrientation = -1;

/*
 * The rotation Angle relative to the initial 'gOrigOrientation'.
 *
 * Note: 'gFrmOrientation' will be 90° if 'gOrigOrientation' initialize
 *        with 90° and get current mainDpyInfo.orientation 180°
 */
static int gFrmOrientation = 0;

/*
 * Catch keyboard interrupt signals.  On receipt, the "stop requested"
 * flag is raised, and the original handler is restored (so that, if
 * we get stuck finishing, a second Ctrl-C will kill the process).
 */
static void signalCatcher(int signum)
{
    gStopRequested = true;
    switch (signum) {
    case SIGINT:
    case SIGHUP:
        sigaction(SIGINT, &gOrigSigactionINT, NULL);
        sigaction(SIGHUP, &gOrigSigactionHUP, NULL);
        break;
    default:
        abort();
        break;
    }
}

/*
 * Configures signal handlers.  The previous handlers are saved.
 *
 * If the command is run from an interactive adb shell, we get SIGINT
 * when Ctrl-C is hit.  If we're run from the host, the local adb process
 * gets the signal, and we get a SIGHUP when the terminal disconnects.
 */
static status_t configureSignals() {
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = signalCatcher;
    if (sigaction(SIGINT, &act, &gOrigSigactionINT) != 0) {
        status_t err = -errno;
        fprintf(stderr, "Unable to configure SIGINT handler: %s\n",
                strerror(errno));
        return err;
    }
    if (sigaction(SIGHUP, &act, &gOrigSigactionHUP) != 0) {
        status_t err = -errno;
        fprintf(stderr, "Unable to configure SIGHUP handler: %s\n",
                strerror(errno));
        return err;
    }
    return NO_ERROR;
}

/*
 * Returns "true" if the device is rotated 90 degrees.
 */
static bool isDeviceRotated(int orientation) {
    return orientation != DISPLAY_ORIENTATION_0 &&
            orientation != DISPLAY_ORIENTATION_180;
}

static void updateFrmOrientation(int curOrientation) {
    // Initialize when the program executes first time
    if (gOrigOrientation == -1) {
        gOrigOrientation = curOrientation;
        gFrmOrientation = FRAME_ORIENTATION_0;
        return;
    }

    gFrmOrientation = curOrientation - gOrigOrientation;

    // Negative numbers expression that counterclockwise rotation
    if (gFrmOrientation == -1)
        gFrmOrientation = FRAME_ORIENTATION_270;
    else if (gFrmOrientation == -2)
        gFrmOrientation = FRAME_ORIENTATION_180;
    else if (gFrmOrientation == -3)
        gFrmOrientation = FRAME_ORIENTATION_90;
}

static status_t configureEncoder(float displayFps, sp<MediaCodec>* codec) {
	sp<AMessage> format = new AMessage;
    int codec_width = gVideoWidth > gVideoHeight ? gVideoHeight : gVideoWidth;
    int codec_height = gVideoWidth > gVideoHeight ? gVideoWidth : gVideoHeight;

	format->setInt32("width", codec_width);
	format->setInt32("height", codec_height);
	format->setString("mime", kMimeTypeAvc);
	format->setInt32("color-format", OMX_COLOR_FormatAndroidOpaque);
	format->setInt32("bitrate", gBitRate);
	format->setFloat("frame-rate", displayFps);
	format->setInt32("i-frame-interval", 1);
	return (*codec)->configure(format, NULL, NULL,
			   MediaCodec::CONFIGURE_FLAG_ENCODE);
}


static status_t setEncoder(float displayFps, sp<MediaCodec>* pCodec) {

	status_t err;

	sp<ALooper> looper = new ALooper;
	looper->setName("screenrecord_looper");
	looper->start();
	LOGD("Creating codec");
	sp<MediaCodec> codec = MediaCodec::CreateByType(looper, kMimeTypeAvc, true);
	if (codec == NULL) {
	   fprintf(stderr, "ERROR: unable to create %s codec instance\n",
			   kMimeTypeAvc);
	   return UNKNOWN_ERROR;
	}

	err = configureEncoder(displayFps, &codec);
	if (err != NO_ERROR) {
	   fprintf(stderr, "ERROR: unable to configure %s codec at %dx%d (err=%d)\n",
			   kMimeTypeAvc, gVideoWidth, gVideoHeight, err);
	   codec->release();
	   return err;
	}
	*pCodec = codec;
	return NO_ERROR;
}


/*
 * Configures and starts the MediaCodec encoder.  Obtains an input surface
 * from the codec.
 */
static status_t prepareEncoder(float displayFps, sp<MediaCodec>* pCodec,
        sp<IGraphicBufferProducer>* pBufferProducer) {
    status_t err;

    if (gVerbose) {
        printf("Configuring recorder for %dx%d %s at %.2fMbps\n",
                gVideoWidth, gVideoHeight, kMimeTypeAvc, gBitRate / 1000000.0);
        printf("send to %s %d bitrate:%.2fMbps\n",h264Nalu.client_ip[0], h264Nalu.client_port[0], gBitRate / 1000000.0);
    }
	setEncoder(displayFps, pCodec);		//设置编码器参数

    LOGD("Creating encoder for input surface");
    sp<IGraphicBufferProducer> bufferProducer;
    err = (*pCodec)->createInputSurface(&bufferProducer);	//在编码器对象中创建Surface buff消费者
    if (err != NO_ERROR) {
        fprintf(stderr,
            "ERROR: unable to create encoder input surface (err=%d)\n", err);
        (*pCodec)->release();
        return err;
    }

    LOGD("Starting codec");
    err = (*pCodec)->start();
    if (err != NO_ERROR) {
        fprintf(stderr, "ERROR: unable to start codec (err=%d)\n", err);
        (*pCodec)->release();
        return err;
    }

    LOGD("Codec prepared");
    *pBufferProducer = bufferProducer;
    return 0;
}

/*
 * Sets the display projection, based on the display dimensions, video size,
 * and device orientation.
 */
static status_t setDisplayProjection(const sp<IBinder>& dpy,
        const DisplayInfo& mainDpyInfo) {
    status_t err;

    // Set the region of the layer stack we're interested in, which in our
    // case is "all of it".  If the app is rotated (so that the width of the
    // app is based on the height of the display), reverse width/height.
    bool deviceRotated = isDeviceRotated(mainDpyInfo.orientation);
    uint32_t sourceWidth, sourceHeight;
    if (!deviceRotated) {
        sourceWidth = mainDpyInfo.w;
        sourceHeight = mainDpyInfo.h;
    } else {
        ALOGV("using rotated width/height");
        sourceHeight = mainDpyInfo.w;
        sourceWidth = mainDpyInfo.h;
    }
    Rect layerStackRect(sourceWidth, sourceHeight);

    // We need to preserve the aspect ratio of the display.
    float displayAspect = (float) sourceHeight / (float) sourceWidth;


    // Set the way we map the output onto the display surface (which will
    // be e.g. 1280x720 for a 720p video).  The rect is interpreted
    // post-rotation, so if the display is rotated 90 degrees we need to
    // "pre-rotate" it by flipping width/height, so that the orientation
    // adjustment changes it back.
    //
    // We might want to encode a portrait display as landscape to use more
    // of the screen real estate.  (If players respect a 90-degree rotation
    // hint, we can essentially get a 720x1280 video instead of 1280x720.)
    // In that case, we swap the configured video width/height and then
    // supply a rotation value to the display projection.
    uint32_t videoWidth, videoHeight;
    uint32_t outWidth, outHeight;
    if (!gRotate) {
        videoWidth = gVideoWidth;
        videoHeight = gVideoHeight;
    } else {
        videoWidth = gVideoHeight;
        videoHeight = gVideoWidth;
    }
    if (videoHeight > (uint32_t)(videoWidth * displayAspect)) {
        // limited by narrow width; reduce height
        outWidth = videoWidth;
        outHeight = (uint32_t)(videoWidth * displayAspect);
    } else {
        // limited by short height; restrict width
        outHeight = videoHeight;
        outWidth = (uint32_t)(videoHeight / displayAspect);
    }
    uint32_t offX, offY;
    offX = (videoWidth - outWidth) / 2;
    offY = (videoHeight - outHeight) / 2;

    updateFrmOrientation(mainDpyInfo.orientation);
    if (gFrmOrientation == 1 || gFrmOrientation == 3) {
        outWidth = gVideoHeight;
        outHeight = gVideoWidth;
    } else {
        outWidth = gVideoWidth;
        outHeight = gVideoHeight;
    }

    Rect displayRect(0, 0, outWidth, outHeight);

    if (gVerbose) {
        if (gRotate) {
            printf("Rotated content area is %ux%u at offset x=%d y=%d\n",
                    outHeight, outWidth, offY, offX);
        } else {
            printf("Content area is %ux%u at offset x=%d y=%d\n",
                    outWidth, outHeight, offX, offY);
        }
    }

    ALOGD("surface set DiaplayRect - %dx%d & get gFrmOrientation - %d",
            outWidth, outHeight, mainDpyInfo.orientation);

    SurfaceComposerClient::setDisplayProjection(dpy,
            mainDpyInfo.orientation,
            layerStackRect, displayRect);
    return NO_ERROR;
}

/*
 * Configures the virtual display.  When this completes, virtual display
 * frames will start arriving from the buffer producer.
 */
static status_t prepareVirtualDisplay(const DisplayInfo& mainDpyInfo,
        const sp<IGraphicBufferProducer>& bufferProducer,
        sp<IBinder>* pDisplayHandle) {
    sp<IBinder> dpy = SurfaceComposerClient::createDisplay(
            String8("ScreenRecorder"), false /*secure*/);

    SurfaceComposerClient::openGlobalTransaction();
    SurfaceComposerClient::setDisplaySurface(dpy, bufferProducer);
    setDisplayProjection(dpy, mainDpyInfo);
    SurfaceComposerClient::setDisplayLayerStack(dpy, 0);    // default stack
    SurfaceComposerClient::closeGlobalTransaction();

    *pDisplayHandle = dpy;

    return NO_ERROR;
}

/*
*通过横竖屏来填充cmd_buffer
*/
inline void func_setOrientation(unsigned char orientation)
{
	bcopy(orientation==FRAME_ORIENTATION_90 ?across_screen:vertical_screen, 
				streamCmd.cmd_buffer, 15);
    streamCmd.buffer_len = 15;
    if(orientation==FRAME_ORIENTATION_90)
    	printf("across screen\n");
    else if(orientation==FRAME_ORIENTATION_0)
    	printf("vertical screen\n");
}

/*
 * Runs the MediaCodec encoder, sending the output to the MediaMuxer.  The
 * input frames are coming from the virtual display as fast as SurfaceFlinger
 * wants to send them.
 *
 * Exactly one of muxer or rawFp must be non-null.
 *
 * The muxer must *not* have been started before calling.
 */
static status_t runEncoder(const sp<MediaCodec>& encoder,
        const sp<MediaMuxer>& muxer, FILE* rawFp, const sp<IBinder>& mainDpy,
        const sp<IBinder>& virtualDpy, unsigned char orientation) {
    static int kTimeout = 250000;   // be responsive on signal
    status_t err;
    ssize_t trackIdx = -1;
    uint32_t debugNumFrames = 0;
    int64_t startWhenNsec = systemTime(CLOCK_MONOTONIC);
    int64_t endWhenNsec = startWhenNsec + seconds_to_nanoseconds(gTimeLimitSec);
    DisplayInfo mainDpyInfo;

    assert((rawFp == NULL && muxer != NULL) || (rawFp != NULL && muxer == NULL));

    Vector<sp<ABuffer> > buffers;
    err = encoder->getOutputBuffers(&buffers);
    if (err != NO_ERROR) {
        fprintf(stderr, "Unable to get output buffers (err=%d)\n", err);
        return err;
    }

    // Run until we're signaled.
    while (!gStopRequested) {
        size_t bufIndex, offset, size;
        int64_t ptsUsec;
        uint32_t flags;

        if (systemTime(CLOCK_MONOTONIC) > endWhenNsec) {
            if (gVerbose) {
                printf("Time limit reached\n");
            }
            break;
        }

        LOGD("Calling dequeueOutputBuffer");
        err = encoder->dequeueOutputBuffer(&bufIndex, &offset, &size, &ptsUsec,
                &flags, kTimeout);
        LOGD("dequeueOutputBuffer returned %d", err);
        switch (err) {
        case NO_ERROR:
            // got a buffer
            if ((flags & MediaCodec::BUFFER_FLAG_CODECCONFIG) != 0) {
                LOGD("Got codec config buffer (%zu bytes)", size);
                printf("Got codec config buffer (%zu bytes)\n", size);
                if (muxer != NULL) {
                	printf("ignore this -- we passed the CSD into MediaMuxer\n");
                    // ignore this -- we passed the CSD into MediaMuxer when
                    // we got the format change notification
                    size = 0;
                }
            }
            if (size != 0) {
                //LOGD("Got data in buffer %zu, size=%zu, pts=%" PRId64,
                        //bufIndex, size, ptsUsec);

                { // scope
                    ATRACE_NAME("orientation");
                    // Check orientation, update if it has changed.
                    //
                    // Polling for changes is inefficient and wrong, but the
                    // useful stuff is hard to get at without a Dalvik VM.
                    err = SurfaceComposerClient::getDisplayInfo(mainDpy,
                            &mainDpyInfo);
                    if (err != NO_ERROR) {
                        LOGW("getDisplayInfo(main) failed: %d", err);
                    } else if (orientation != mainDpyInfo.orientation) {
                        //LOGD("orientation changed, now %d", mainDpyInfo.orientation);
                        LOGD("get orientation-changed, old: %d - now: %d",
                                orientation, mainDpyInfo.orientation);
                        SurfaceComposerClient::openGlobalTransaction();
                        setDisplayProjection(virtualDpy, mainDpyInfo);
                        SurfaceComposerClient::closeGlobalTransaction();
                        orientation = mainDpyInfo.orientation;
						//针对后面的翻转事件
                        func_setOrientation(mainDpyInfo.orientation);
                        //直接通过命令通道给客户端发更新横竖屏事件
                        streamCmd.buffer_len = 15;
						send(h264Nalu.connfd[cmd_pipe], streamCmd.cmd_buffer, streamCmd.buffer_len, 0);
		              }
                }

                // If the virtual display isn't providing us with timestamps,
                // use the current time.  This isn't great -- we could get
                // decoded data in clusters -- but we're not expecting
                // to hit this anyway.
                if (ptsUsec == 0) {
                    ptsUsec = systemTime(SYSTEM_TIME_MONOTONIC) / 1000;
                }

                if (muxer == NULL) {
#if 0
                    fwrite(buffers[bufIndex]->data(), 1, size, rawFp);
                    // Flush the data immediately in case we're streaming.
                    // We don't want to do this if all we've written is
                    // the SPS/PPS data because mplayer gets confused.
                    if ((flags & MediaCodec::BUFFER_FLAG_CODECCONFIG) == 0) {
                    	printf("flush raw buffer\n");
                        fflush(rawFp);
                    }
#endif
#if 1
					frame_struct(buffers[bufIndex]->data(), 0);
					//printf("-------------------------------\n");
	                bcopy(buffers[bufIndex]->data(), h264Nalu.pad_buffer, size);
	                h264Nalu.buffer_len = size;
					onDataSync = 1;
					//printf("onDataSync:%d\n", onDataSync);
					pthread_cond_broadcast(&v);
#endif
                } else {
                    // The MediaMuxer docs are unclear, but it appears that we
                    // need to pass either the full set of BufferInfo flags, or
                    // (flags & BUFFER_FLAG_SYNCFRAME).
                    //
                    // If this blocks for too long we could drop frames.  We may
                    // want to queue these up and do them on a different thread.
                    ATRACE_NAME("write sample");
                    assert(trackIdx != -1);
                    err = muxer->writeSampleData(buffers[bufIndex], trackIdx,
                            ptsUsec, flags);
                    if (err != NO_ERROR) {
                        fprintf(stderr,
                            "Failed writing data to muxer (err=%d)\n", err);
                        return err;
                    }
                }
                debugNumFrames++;
            }
            err = encoder->releaseOutputBuffer(bufIndex);
            if (err != NO_ERROR) {
                fprintf(stderr, "Unable to release output buffer (err=%d)\n",
                        err);
                return err;
            }
            if ((flags & MediaCodec::BUFFER_FLAG_EOS) != 0) {
                // Not expecting EOS from SurfaceFlinger.  Go with it.
                ALOGI("Received end-of-stream");
                gStopRequested = true;
            }
            break;
        case -EAGAIN:                       // INFO_TRY_AGAIN_LATER
            LOGD("Got -EAGAIN, looping");
            break;
        case INFO_FORMAT_CHANGED:           // INFO_OUTPUT_FORMAT_CHANGED
            {
                // Format includes CSD, which we must provide to muxer.
                LOGD("Encoder format changed");
                sp<AMessage> newFormat;
                encoder->getOutputFormat(&newFormat);
                if (muxer != NULL) {
                    trackIdx = muxer->addTrack(newFormat);
                    LOGD("Starting muxer");
                    err = muxer->start();
                    if (err != NO_ERROR) {
                        fprintf(stderr, "Unable to start muxer (err=%d)\n", err);
                        return err;
                    }
                }
            }
            break;
        case INFO_OUTPUT_BUFFERS_CHANGED:   // INFO_OUTPUT_BUFFERS_CHANGED
            // Not expected for an encoder; handle it anyway.
            LOGD("Encoder buffers changed");
            err = encoder->getOutputBuffers(&buffers);
            if (err != NO_ERROR) {
                fprintf(stderr,
                        "Unable to get new output buffers (err=%d)\n", err);
                return err;
            }
            break;
        case INVALID_OPERATION:
            LOGW("dequeueOutputBuffer returned INVALID_OPERATION");
            return err;
        default:
            fprintf(stderr,
                    "Got weird result %d from dequeueOutputBuffer\n", err);
            return err;
        }
    }

    LOGD("Encoder stopping (req=%d)", gStopRequested);
    if (gVerbose) {
        printf("Encoder stopping; recorded %u frames in %" PRId64 " seconds\n",
                debugNumFrames, nanoseconds_to_seconds(
                        systemTime(CLOCK_MONOTONIC) - startWhenNsec));
    }
    return NO_ERROR;
}

/*
 * Raw H.264 byte stream output requested.  Send the output to stdout
 * if desired.  If the output is a tty, reconfigure it to avoid the
 * CRLF line termination that we see with "adb shell" commands.
 */
static FILE* prepareRawOutput(const char* fileName) {
    FILE* rawFp = NULL;

    if (strcmp(fileName, "-") == 0) {
        if (gVerbose) {
            fprintf(stderr, "ERROR: verbose output and '-' not compatible");
            return NULL;
        }
        rawFp = stdout;
    } else {
        rawFp = fopen(fileName, "w");
        if (rawFp == NULL) {
            fprintf(stderr, "fopen raw failed: %s\n", strerror(errno));
            return NULL;
        }
    }

    int fd = fileno(rawFp);
    if (isatty(fd)) {
        // best effort -- reconfigure tty for "raw"
        LOGD("raw video output to tty (fd=%d)", fd);
        struct termios term;
        if (tcgetattr(fd, &term) == 0) {
            cfmakeraw(&term);
            if (tcsetattr(fd, TCSANOW, &term) == 0) {
                LOGD("tty successfully configured for raw");
            }
        }
    }

    return rawFp;
}

/*
 * Main "do work" start point.
 *
 * Configures codec, muxer, and virtual display, then starts moving bits
 * around.
 */
static status_t recordScreen(const char* fileName) {
    status_t err;

    // Configure signal handler.
    err = configureSignals();
    if (err != NO_ERROR) return err;

    // Start Binder thread pool.  MediaCodec needs to be able to receive
    // messages from mediaserver.
    sp<ProcessState> self = ProcessState::self();
    self->startThreadPool();

    // Get main display parameters.
    sp<IBinder> mainDpy = SurfaceComposerClient::getBuiltInDisplay(
            ISurfaceComposer::eDisplayIdMain);
    DisplayInfo mainDpyInfo;
    err = SurfaceComposerClient::getDisplayInfo(mainDpy, &mainDpyInfo);
    if (err != NO_ERROR) {
        fprintf(stderr, "ERROR: unable to get display characteristics\n");
        return err;
    }

    mainDpyInfo.fps = DISPLAY_FPS;
    h264Nalu.fps = mainDpyInfo.fps;
    if (gVerbose) {
        printf("Main display is %dx%d @%.2ffps (orientation=%u) capture:%ds\n",
                mainDpyInfo.w, mainDpyInfo.h, mainDpyInfo.fps,
                mainDpyInfo.orientation, kMaxTimeLimitSec);
	}

    bool rotated = isDeviceRotated(mainDpyInfo.orientation);
    if (gVideoWidth == 0) {
    gVideoWidth = rotated ? mainDpyInfo.h : mainDpyInfo.w;
    }
    if (gVideoHeight == 0) {
    gVideoHeight = rotated ? mainDpyInfo.w : mainDpyInfo.h;
    }
    //直接给命令通道发送横竖屏消息
	func_setOrientation(mainDpyInfo.orientation);
	streamCmd.buffer_len = 15;
	send(h264Nalu.connfd[cmd_pipe], streamCmd.cmd_buffer, streamCmd.buffer_len, 0);

    // Configure and start the encoder.
    sp<MediaCodec> encoder;
    sp<FrameOutput> frameOutput;
    sp<IGraphicBufferProducer> encoderInputSurface;
    if (gOutputFormat != FORMAT_FRAMES && gOutputFormat != FORMAT_RAW_FRAMES) {
        err = prepareEncoder(mainDpyInfo.fps, &encoder, &encoderInputSurface);

        if (err != NO_ERROR && !gSizeSpecified) {
            // fallback is defined for landscape; swap if we're in portrait
            bool needSwap = gVideoWidth < gVideoHeight;
            uint32_t newWidth = needSwap ? kFallbackHeight : kFallbackWidth;
            uint32_t newHeight = needSwap ? kFallbackWidth : kFallbackHeight;
            if (gVideoWidth != newWidth && gVideoHeight != newHeight) {
                LOGD("Retrying with 1080p");
                fprintf(stderr, "WARNING: failed at %dx%d, retrying at %dx%d\n",
                        gVideoWidth, gVideoHeight, newWidth, newHeight);
                gVideoWidth = newWidth;
                gVideoHeight = newHeight;
                err = prepareEncoder(mainDpyInfo.fps, &encoder,
                        &encoderInputSurface);
            }
        }
        if (err != NO_ERROR) return err;

        // From here on, we must explicitly release() the encoder before it goes
        // out of scope, or we will get an assertion failure from stagefright
        // later on in a different thread.
    } else {
        // We're not using an encoder at all.  The "encoder input surface" we hand to
        // SurfaceFlinger will just feed directly to us.
        frameOutput = new FrameOutput();
        //将刚才拿到的消费者传入,创建虚拟屏.
        err = frameOutput->createInputSurface(gVideoWidth, gVideoHeight, &encoderInputSurface);
        if (err != NO_ERROR) {
            return err;
        }
    }

    // Draw the "info" page by rendering a frame with GLES and sending
    // it directly to the encoder.
    // TODO: consider displaying this as a regular layer to avoid b/11697754
    if (gWantInfoScreen) {
        Overlay::drawInfoPage(encoderInputSurface);
    }

    // Configure optional overlay.
    sp<IGraphicBufferProducer> bufferProducer;
    sp<Overlay> overlay;
    if (gWantFrameTime) {
        // Send virtual display frames to an external texture.
        overlay = new Overlay();
        err = overlay->start(encoderInputSurface, &bufferProducer);
        if (err != NO_ERROR) {
            if (encoder != NULL) encoder->release();
            return err;
        }
        if (gVerbose) {
            printf("Bugreport overlay created\n");
        }
    } else {
        // Use the encoder's input surface as the virtual display surface.
        bufferProducer = encoderInputSurface;
    }

    // Configure virtual display.
    sp<IBinder> dpy;
    err = prepareVirtualDisplay(mainDpyInfo, bufferProducer, &dpy);
    if (err != NO_ERROR) {
        if (encoder != NULL) encoder->release();
        return err;
    }

    sp<MediaMuxer> muxer = NULL;
    FILE* rawFp = NULL;
    printf("goto capture h264\n");
    rawFp = prepareRawOutput(fileName);
    if (rawFp == NULL) {
        if (encoder != NULL) encoder->release();
        return -1;
    }
    // Main encoder loop.
    err = runEncoder(encoder, muxer, rawFp, mainDpy, dpy,
            mainDpyInfo.orientation);
    if (err != NO_ERROR) {
        fprintf(stderr, "Encoder failed (err=%d)\n", err);
        // fall through to cleanup
    }

    if (gVerbose) {
        printf("Stopping encoder and muxer\n");
    }

    // Shut everything down, starting with the producer side.
    encoderInputSurface = NULL;
    SurfaceComposerClient::destroyDisplay(dpy);
    if (overlay != NULL) overlay->stop();
    if (encoder != NULL) encoder->stop();
    if (muxer != NULL) {
        // If we don't stop muxer explicitly, i.e. let the destructor run,
        // it may hang (b/11050628).
        err = muxer->stop();
    } 
    else if (rawFp != stdout) {
        fclose(rawFp);
    }
    if (encoder != NULL) encoder->release();

    return err;
}

/*
 * Sends a broadcast to the media scanner to tell it about the new video.
 *
 * This is optional, but nice to have.
 */
#if 1
static status_t notifyMediaScanner(const char* fileName) {
    // need to do allocations before the fork()
    String8 fileUrl("file://");
    fileUrl.append(fileName);

    const char* kCommand = "/system/bin/am";
    const char* const argv[] = {
            kCommand,
            "broadcast",
            "-a",
            "android.intent.action.MEDIA_SCANNER_SCAN_FILE",
            "-d",
            fileUrl.string(),
            NULL
    };
    if (gVerbose) {
        printf("Executing:");
        for (int i = 0; argv[i] != NULL; i++) {
            printf(" %s", argv[i]);
        }
        putchar('\n');
    }

    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        LOGW("fork() failed: %s", strerror(err));
        return -err;
    } else if (pid > 0) {
        // parent; wait for the child, mostly to make the verbose-mode output
        // look right, but also to check for and log failures
        int status;
        pid_t actualPid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
        if (actualPid != pid) {
            LOGW("waitpid(%d) returned %d (errno=%d)", pid, actualPid, errno);
        } else if (status != 0) {
            LOGW("'am broadcast' exited with status=%d", status);
        } else {
            LOGD("'am broadcast' exited successfully");
        }
    } else {
        if (!gVerbose) {
            // non-verbose, suppress 'am' output
            LOGD("closing stdout/stderr in child");
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }
        }
        execv(kCommand, const_cast<char* const*>(argv));
        ALOGE("execv(%s) failed: %s\n", kCommand, strerror(errno));
        exit(1);
    }
    return NO_ERROR;
}

/*
 * Parses a string of the form "1280x720".
 *
 * Returns true on success.
 */
static bool parseWidthHeight(const char* widthHeight, uint32_t* pWidth,
        uint32_t* pHeight) {
    long width, height;
    char* end;

    // Must specify base 10, or "0x0" gets parsed differently.
    width = strtol(widthHeight, &end, 10);
    if (end == widthHeight || *end != 'x' || *(end+1) == '\0') {
        // invalid chars in width, or missing 'x', or missing height
        return false;
    }
    height = strtol(end + 1, &end, 10);
    if (*end != '\0') {
        // invalid chars in height
        return false;
    }

    *pWidth = width;
    *pHeight = height;
    return true;
}

/*
 * Accepts a string with a bare number ("4000000") or with a single-character
 * unit ("4m").
 *
 * Returns an error if parsing fails.
 */
static status_t parseValueWithUnit(const char* str, uint32_t* pValue) {
    long value;
    char* endptr;

    value = strtol(str, &endptr, 10);
    if (*endptr == '\0') {
        // bare number
        *pValue = value;
        return NO_ERROR;
    } else if (toupper(*endptr) == 'M' && *(endptr+1) == '\0') {
        *pValue = value * 1000000;  // check for overflow?
        return NO_ERROR;
    } else {
        fprintf(stderr, "Unrecognized value: %s\n", str);
        return UNKNOWN_ERROR;
    }
}
#endif

int wait_client_connect(int hope_num)
{
	unsigned short port = 8000;	
	int sockfd,err_log, client_num=0;
	socklen_t cliaddr_len;
	int reuseaddr = 1;
	
	sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if(sockfd < 0)
    {
        perror("socket");
        return -1;
    }
    setsockopt(sockfd,SOL_SOCKET,SO_REUSEADDR,&reuseaddr,sizeof(reuseaddr));
    struct sockaddr_in my_addr;
    bzero(&my_addr, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port   = htons(port);
    my_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    err_log = bind(sockfd, (struct sockaddr*)&my_addr, sizeof(my_addr));
    if(err_log != 0)
    {
        perror("binding");
        close(sockfd);
        return -1;
    }
    err_log = listen(sockfd, hope_num);    // 等待队列为num
    if(err_log != 0)
    {
        perror("listen");
        close(sockfd);
        return -1;
    }
	client_num=0;
    while(client_num <= hope_num-1)
	{
        cliaddr_len = sizeof(sockaddr_in);
        h264Nalu.connfd[client_num] = accept(sockfd, (struct sockaddr*)&client_addr[client_num], &cliaddr_len);
        if(h264Nalu.connfd[client_num] < 0)
        {
            perror("accept");
            continue;
        }
        inet_ntop(AF_INET, &client_addr[client_num].sin_addr, h264Nalu.client_ip[client_num], INET_ADDRSTRLEN);
        printf("-----------%d------\n", client_num);
        printf("client ip=%s,port=%d\n", h264Nalu.client_ip[client_num],ntohs(client_addr[client_num].sin_port));
        h264Nalu.client_port[client_num] = ntohs(client_addr[client_num].sin_port);
        ++client_num;
   }
   close(sockfd);
   return 0;
}

/*
 * 主线程作为网络通信服务端,负责监听客户端的连接请求
 *子线程用于同步发送编码器出来的buff(裸推)
 */
int main(int argc, char* const argv[])
{

//	gWantInfoScreen = true;	// do we want initial info screen?
	gWantFrameTime = true; 	// do we want times on each frame?
	gOutputFormat = FORMAT_H264;
    //创建tcp服务端,用于监听客户端连接
    printf("goto wait client connect\n");
    streamCmd.cmd_buffer = (unsigned char *)malloc(cmdSize);
retry:
    if(wait_client_connect(2) !=0) {
    	printf("retry to wait client connect\n");
    	sleep(3);
    	goto retry;
    }
    printf("wait client connect ok\n");

	// 创建一个锁
	pthread_mutex_init(&m, NULL);
	// 创建一个条件变量
	pthread_cond_init(&v, NULL);
	//创建子线程,用于接收onData回调的数据
	//start loop report raw h264 data.
	pthread_t h264_report;
	pthread_create(&h264_report, NULL, raw_send, (void *)NULL);

    const char* fileName = (argv[1] == NULL? "test.h264":argv[1]);
    h264Nalu.pad_buffer = (unsigned char *)malloc(NAL_MBUF_SIZE);

    status_t err = recordScreen(fileName);
#if 1
    if (err == NO_ERROR) {
        // Try to notify the media scanner.  Not fatal if this fails.
        notifyMediaScanner(fileName);
    }
#endif
    LOGD(err == NO_ERROR ? "success" : "failed");
	//唤醒所有阻塞在子线程的执行序列
	rtpStop = 1;
	onDataSync = 100;
	pthread_cond_broadcast(&v);
	pthread_mutex_unlock(&m);
	close(h264Nalu.connfd[cmd_pipe]);
	close(h264Nalu.connfd[video_pipe]);
	pthread_join(h264_report, NULL);
    return (int) err;
}
