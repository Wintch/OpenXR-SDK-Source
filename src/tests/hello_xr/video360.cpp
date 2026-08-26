#include "pch.h"
#include "common.h"
#include "logger.h"
#include "video360.h"

#ifndef HAVE_FFMPEG

// Built without ffmpeg: keep the API so graphicsplugin_vulkan.cpp needs no #ifdefs, and let
// Open() fail so the caller falls back to the still-photo skybox.
struct Video360::Impl {};
Video360::Video360() : m_impl(new Impl()) {}
Video360::~Video360() = default;
bool Video360::Open(const std::string&) {
    Log::Write(Log::Level::Error, "video360: built without ffmpeg, cannot play video");
    return false;
}
void Video360::SetFrameBuffers(std::vector<Video360Buffer>) {}
bool Video360::Start() { return false; }
void Video360::SetLoop(bool) {}
void Video360::SetRate(double) {}
bool Video360::Finished() const { return true; }
bool Video360::IsOpen() const { return false; }
int Video360::Width() const { return 0; }
int Video360::Height() const { return 0; }
size_t Video360::YBytes() const { return 0; }
size_t Video360::UVBytes() const { return 0; }
double Video360::FrameRate() const { return 0.0; }
double Video360::Duration() const { return 0.0; }
double Video360::PlaybackPosition() const { return 0.0; }
void Video360::Seek(double) {}
std::string Video360::CodecName() const { return "none"; }
PanoLayout Video360::DetectedLayout() const { return PanoLayout{}; }
bool Video360::FullRange() const { return false; }
bool Video360::Bt709() const { return true; }
bool Video360::HwDecodeActive() const { return false; }
int Video360::AcquireCurrentSlot() { return -1; }

#else

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/spherical.h>
#include <libavutil/stereo3d.h>
#include <libswresample/swresample.h>
}

// Stereo audio playback for the player, gated at runtime by HELLO_XR_AUDIO=1 (see the
// "Stereo audio" section of docs/02-player-360.md). Head-locked only: the content this
// player is built around (get360.sh's android_vr downloads) is plain stereo, never
// ambisonic, so there is no spatialization/HRTF/head-rotation to do - the two channels are
// just decoded, resampled to a fixed format, and handed to the sound card. HAVE_PULSE_SIMPLE
// is defined by CMakeLists.txt only when libpulse-simple was found at configure time; when
// it's absent, HELLO_XR_AUDIO is accepted but logs once and has no effect (video-only,
// exactly today's behavior) - see the guarded call sites below.
#if defined(HAVE_PULSE_SIMPLE)
#include <pulse/error.h>
#include <pulse/simple.h>
#endif

namespace {
std::string AvErr(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

// The decoder offers its list of output formats; take CUDA (NVDEC) when present, otherwise
// let libavcodec pick its default software format.
AVPixelFormat PickHwFormat(AVCodecContext* ctx, const AVPixelFormat* fmts) {
    for (const AVPixelFormat* p = fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_CUDA) return *p;
    }
    return avcodec_default_get_format(ctx, fmts);
}

// av_hwframe_transfer_data only honours a caller-provided destination when the frame looks
// referenced (dst->buf[0] != NULL); otherwise it quietly allocates its own and our pointers
// are ignored. Wrap the mapped staging memory in a buffer whose free callback does nothing.
void NoopFree(void*, uint8_t*) {}

bool EnvOff(const char* name) {
    const char* v = getenv(name);
    return v != nullptr && v[0] == '0';
}

bool EnvOn(const char* name) {
    const char* v = getenv(name);
    return v != nullptr && v[0] == '1';
}

bool DecoderCanUseCuda(const AVCodec* codec) {
    for (int i = 0;; i++) {
        const AVCodecHWConfig* cfg = avcodec_get_hw_config(codec, i);
        if (cfg == nullptr) return false;
        if (cfg->pix_fmt == AV_PIX_FMT_CUDA && (cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) != 0) return true;
    }
}

// av_find_best_stream hands back ffmpeg's *default* decoder for the codec, and for AV1 that is
// libdav1d - which has no hwaccel at all. The result is that a card with a perfectly good AV1
// NVDEC block silently decodes 8K AV1 on the CPU: measured here at 25 fps on a 60 fps file,
// with no error anywhere to explain it. So when hardware decode is wanted, go looking for a
// decoder that can actually reach it.
const AVCodec* PickHwCapableDecoder(AVCodecID id, const AVCodec* fallback) {
    void* iter = nullptr;
    while (const AVCodec* c = av_codec_iterate(&iter)) {
        if (c->id != id || !av_codec_is_decoder(c)) continue;
        if (DecoderCanUseCuda(c)) return c;
    }
    return fallback;
}
}  // namespace

struct Video360::Impl {
    // A frame in flight, identified by which caller-provided buffer holds it.
    struct QueuedFrame {
        int slot{-1};
        double pts{0.0};  // seconds since playback start, already loop-adjusted
    };

    AVFormatContext* fmt{nullptr};
    AVCodecContext* dec{nullptr};
    AVBufferRef* hwDevice{nullptr};
    int streamIndex{-1};
    int width{0};
    int height{0};
    double timeBase{0.0};
    double frameDuration{1.0 / 30.0};  // fallback when a frame carries no usable pts
    double durationSeconds{0.0};       // 0 if the container did not say

    // Seek request from Seek(), consumed at the top of DecodeLoop()'s while loop. Guarded by
    // mutex, same as the other cross-thread state below.
    bool seekPending{false};
    int64_t seekTargetPts{0};  // stream timeBase units
    std::string codecName{"unknown"};
    PanoLayout layout;  // whatever the container declared; Unknown fields mean "it did not say"

    std::atomic<bool> hwActive{false};
    std::atomic<bool> fullRange{false};
    std::atomic<bool> bt709{true};
    bool warnedFormat{false};

    std::vector<Video360Buffer> buffers;
    std::vector<uint8_t> rowScratch;  // see NormalizeToNv12

    std::thread thread;
    std::mutex mutex;
    std::condition_variable spaceAvailable;
    std::deque<QueuedFrame> queue;
    std::deque<int> freeSlots;
    int currentSlot{-1};  // held by the renderer, off-limits to the decoder
    std::atomic<bool> quit{false};
    bool opened{false};
    bool started{false};

    // Playlist support: stop at the end of the file instead of rewinding, and let the caller
    // see when the last frame has had its time on screen.
    bool loop{true};
    std::atomic<bool> decodeDone{false};
    double finalPts{0.0};  // pts of the last frame queued; guarded by mutex

    // Playback clock. Wall time is accumulated into playbackTime scaled by the current rate,
    // rather than being read as (now - start): that way pausing (rate 0) or slowing down
    // mid-playback just changes how fast the clock runs from here on, with no discontinuity
    // and nothing to recompute.
    std::chrono::steady_clock::time_point lastTick;
    double playbackTime{0.0};
    std::atomic<double> rate{1.0};
    bool clockStarted{false};

    // ---- Stereo audio (HELLO_XR_AUDIO=1) ----
    // Fully optional, entirely self-contained here: nothing outside this block is touched
    // when audioActive is false (the default), so the no-audio path is bit-for-bit what it
    // was before this feature existed. audioActive is decided once in Open() (env var set,
    // an audio stream exists, the codec/resampler/pa_simple all opened clean) and never
    // changes after Start() - plain bool, no atomics needed, same convention as `loop` and
    // `directXfer` above.
    static constexpr int kAudioSampleRate = 48000;
    static constexpr int kAudioChannels = 2;

    bool audioActive{false};
    int audioStreamIndex{-1};
    AVCodecContext* audioDec{nullptr};
    SwrContext* swr{nullptr};
    double audioTimeBase{0.0};
#if defined(HAVE_PULSE_SIMPLE)
    pa_simple* pa{nullptr};
#endif
    std::thread audioThread;

    // One demuxed-but-undecoded audio packet, handed from DecodeLoop's single
    // av_read_frame call to the dedicated audio thread. loopOffsetAtRead is DecodeLoop's
    // own `loopOffset` local, copied by value at the moment the packet was read, so the
    // audio thread can put this packet's pts on the exact same absolute (loop-adjusted)
    // timeline the video queue's pts already use, without the two threads sharing that
    // variable directly.
    struct QueuedAudioPacket {
        AVPacket* pkt;
        double loopOffsetAtRead;
    };
    // Separate mutex/cv from the video queue's `mutex`/`spaceAvailable` on purpose: the
    // audio thread only ever touches this pair, so a busy audio thread can never add
    // latency to AcquireCurrentSlot(), which runs on the render thread at up to 90Hz.
    std::mutex audioMutex;
    std::condition_variable audioPacketAvailable;
    std::deque<QueuedAudioPacket> audioQueue;
    bool audioResetPending{false};  // set at a loop seam or a successful manual seek;
                                     // consumed by the audio thread, which does the actual
                                     // pa_simple_flush/avcodec_flush_buffers - pa_simple and
                                     // AVCodecContext are not safe to touch from two threads

    // Audio playback clock. anchorPts is the absolute (loop-adjusted) pts of the sample at
    // framesWritten == 0 for the current "epoch"; reset (audioAnchorSet = false,
    // audioFramesWritten = 0) on every loop restart and manual seek, mirroring what
    // clockStarted/loopOffset do for the video queue. AudioClockSeconds() below is then
    // anchorPts + framesWritten/rate, corrected by however many of those written frames
    // pa_simple/PipeWire is still holding, not yet audible (its own reported latency).
    // audioClockSeconds/audioClockValid are the only fields of this whole block the render
    // thread reads (AcquireCurrentSlot, via plain atomics, no lock) - everything else here
    // belongs exclusively to the audio thread.
    bool audioAnchorSet{false};
    double audioAnchorPts{0.0};
    int64_t audioFramesWritten{0};  // stereo sample pairs written to pa_simple since anchor
    std::atomic<double> audioClockSeconds{0.0};
    std::atomic<bool> audioClockValid{false};

    // HELLO_XR_VIDEO_STATS: what the decode thread costs, and whether it is the bottleneck.
    bool wantStats{false};
    bool directXfer{true};   // HELLO_XR_VIDEO_DIRECT=0 disables, for A/B measurement
    bool directWorks{true};  // cleared on the first failure, never retried
    uint64_t statFrames{0};
    double statDecodeMs{0.0};
    double statWriteMs{0.0};
    uint64_t statStalls{0};    // decoder had to wait for the renderer to free a buffer
    uint64_t statStarved{0};   // renderer asked for a frame and the queue was empty
    std::chrono::steady_clock::time_point statWindow;

    // Destination frame for the NVDEC VRAM->RAM transfer, pointed at the current staging slot.
    AVFrame* xferFrame{nullptr};

    ~Impl() {
        quit = true;
        spaceAvailable.notify_all();
        audioPacketAvailable.notify_all();
        if (thread.joinable()) thread.join();
#if defined(HAVE_PULSE_SIMPLE)
        if (audioThread.joinable()) audioThread.join();
        for (auto& qp : audioQueue) av_packet_free(&qp.pkt);
        audioQueue.clear();
        if (pa) pa_simple_free(pa);
        if (swr) swr_free(&swr);
        if (audioDec) avcodec_free_context(&audioDec);
#endif
        if (xferFrame) av_frame_free(&xferFrame);
        if (dec) avcodec_free_context(&dec);
        if (hwDevice) av_buffer_unref(&hwDevice);
        if (fmt) avformat_close_input(&fmt);
    }

    size_t YBytes() const { return (size_t)width * height; }
    size_t UVBytes() const { return (size_t)width * height / 2; }

    // Points a hand-built AVFrame at a staging slot so NVDEC copies VRAM straight into memory
    // the GPU can already read - one PCIe crossing instead of "VRAM -> system RAM -> memcpy
    // -> staging". Returns false if the transfer refuses the layout, in which case the caller
    // falls back to transferring into an ffmpeg-owned frame and normalizing from it.
    bool TransferDirect(const AVFrame* hw, const Video360Buffer& dst) {
        av_frame_unref(xferFrame);
        xferFrame->format = AV_PIX_FMT_NV12;
        xferFrame->width = width;
        xferFrame->height = height;
        xferFrame->data[0] = dst.y;
        xferFrame->linesize[0] = width;
        xferFrame->data[1] = dst.uv;
        xferFrame->linesize[1] = width;  // interleaved CbCr at half height, so same pitch as Y
        xferFrame->buf[0] = av_buffer_create(dst.y, YBytes(), NoopFree, nullptr, AV_BUFFER_FLAG_READONLY);
        if (xferFrame->buf[0] == nullptr) return false;

        const int ret = av_hwframe_transfer_data(xferFrame, hw, 0);
        if (ret < 0) {
            Log::Write(Log::Level::Info,
                       Fmt("video360: direct NVDEC transfer into staging not accepted (%s), using the copy path",
                           AvErr(ret).c_str()));
            return false;
        }
        // The transfer may have replaced our pointers if it decided to reallocate after all.
        return xferFrame->data[0] == dst.y;
    }

    // Copies an arbitrary decoded frame into tightly-packed 8-bit NV12 in a caller-provided
    // buffer. Handles the formats this pipeline actually meets: NV12 (NVDEC transfer output),
    // YUV420P/YUVJ420P (software h264/hevc), and the 10-bit variants P010/YUV420P10
    // (downshifted to 8 bit - HDR grading is out of scope for a VR skybox).
    //
    // The destination is very likely write-combined memory (Vulkan HOST_VISIBLE), where a
    // partial cache-line write forces the write-combine buffer to flush - so the byte-strided
    // interleave paths build each row in a small cached scratch and blit it out whole. The
    // NV12 path needs no such trick: it is already a full-row memcpy.
    bool NormalizeToNv12(const AVFrame* f, const Video360Buffer& out) {
        const int w = width, h = height;
        const int cw = w / 2, ch = h / 2;

        switch (f->format) {
            case AV_PIX_FMT_NV12:
                for (int r = 0; r < h; r++) memcpy(out.y + (size_t)r * w, f->data[0] + (size_t)r * f->linesize[0], w);
                for (int r = 0; r < ch; r++)
                    memcpy(out.uv + (size_t)r * cw * 2, f->data[1] + (size_t)r * f->linesize[1], (size_t)cw * 2);
                return true;
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUVJ420P:
                for (int r = 0; r < h; r++) memcpy(out.y + (size_t)r * w, f->data[0] + (size_t)r * f->linesize[0], w);
                for (int r = 0; r < ch; r++) {
                    const uint8_t* u = f->data[1] + (size_t)r * f->linesize[1];
                    const uint8_t* v = f->data[2] + (size_t)r * f->linesize[2];
                    uint8_t* dst = rowScratch.data();
                    for (int c = 0; c < cw; c++) {
                        dst[c * 2 + 0] = u[c];
                        dst[c * 2 + 1] = v[c];
                    }
                    memcpy(out.uv + (size_t)r * cw * 2, dst, (size_t)cw * 2);
                }
                return true;
            case AV_PIX_FMT_P010LE:  // data in the high 10 of 16 bits -> high byte is the 8-bit value
                for (int r = 0; r < h; r++) {
                    const uint8_t* src = f->data[0] + (size_t)r * f->linesize[0];
                    uint8_t* dst = rowScratch.data();
                    for (int c = 0; c < w; c++) dst[c] = src[c * 2 + 1];
                    memcpy(out.y + (size_t)r * w, dst, (size_t)w);
                }
                for (int r = 0; r < ch; r++) {
                    const uint8_t* src = f->data[1] + (size_t)r * f->linesize[1];
                    uint8_t* dst = rowScratch.data();
                    for (int c = 0; c < cw * 2; c++) dst[c] = src[c * 2 + 1];
                    memcpy(out.uv + (size_t)r * cw * 2, dst, (size_t)cw * 2);
                }
                return true;
            case AV_PIX_FMT_YUV420P10LE:  // data in the low 10 bits -> >>2
                for (int r = 0; r < h; r++) {
                    const uint16_t* src = (const uint16_t*)(f->data[0] + (size_t)r * f->linesize[0]);
                    uint8_t* dst = rowScratch.data();
                    for (int c = 0; c < w; c++) dst[c] = (uint8_t)(src[c] >> 2);
                    memcpy(out.y + (size_t)r * w, dst, (size_t)w);
                }
                for (int r = 0; r < ch; r++) {
                    const uint16_t* u = (const uint16_t*)(f->data[1] + (size_t)r * f->linesize[1]);
                    const uint16_t* v = (const uint16_t*)(f->data[2] + (size_t)r * f->linesize[2]);
                    uint8_t* dst = rowScratch.data();
                    for (int c = 0; c < cw; c++) {
                        dst[c * 2 + 0] = (uint8_t)(u[c] >> 2);
                        dst[c * 2 + 1] = (uint8_t)(v[c] >> 2);
                    }
                    memcpy(out.uv + (size_t)r * cw * 2, dst, (size_t)cw * 2);
                }
                return true;
            default:
                if (!warnedFormat) {
                    warnedFormat = true;
                    Log::Write(Log::Level::Error,
                               Fmt("video360: unsupported decoded pixel format %s", av_get_pix_fmt_name((AVPixelFormat)f->format)));
                }
                return false;
        }
    }

    void CaptureColorInfo(const AVFrame* f) {
        fullRange = (f->color_range == AVCOL_RANGE_JPEG) || f->format == AV_PIX_FMT_YUVJ420P;
        switch (f->colorspace) {
            case AVCOL_SPC_BT709:
                bt709 = true;
                break;
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
            case AVCOL_SPC_SMPTE240M:
                bt709 = false;
                break;
            default:  // unspecified: HD content is overwhelmingly BT.709
                bt709 = (height >= 720);
                break;
        }
    }

    // Takes a buffer the renderer is not using. Blocks while every buffer is spoken for,
    // which is the natural back-pressure that keeps the decoder from running ahead.
    int TakeFreeSlot() {
        std::unique_lock<std::mutex> lock(mutex);
        if (freeSlots.empty()) statStalls++;
        spaceAvailable.wait(lock, [this] { return quit || !freeSlots.empty(); });
        if (quit) return -1;
        const int slot = freeSlots.front();
        freeSlots.pop_front();
        return slot;
    }

    void ReturnSlot(int slot) {
        std::lock_guard<std::mutex> lock(mutex);
        freeSlots.push_back(slot);
    }

    void ReportStats() {
        const auto now = std::chrono::steady_clock::now();
        const double secs = std::chrono::duration<double>(now - statWindow).count();
        uint64_t starved;
        {
            std::lock_guard<std::mutex> lock(mutex);
            starved = statStarved;
            statStarved = 0;
        }
        Log::Write(Log::Level::Info,
                   Fmt("video decode: %.1f frames/s produced | decode %.2f ms, write-to-staging %.2f ms "
                       "| %llu waits for a free buffer, %llu renderer starves%s",
                       statFrames / secs, statDecodeMs / statFrames, statWriteMs / statFrames,
                       (unsigned long long)statStalls, (unsigned long long)starved,
                       (hwActive ? (directWorks ? " | NVDEC direct-to-staging" : " | NVDEC + copy") : " | software decode")));
        statFrames = 0;
        statDecodeMs = statWriteMs = 0.0;
        statStalls = 0;
        statWindow = now;
    }

    void DecodeLoop() {
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        AVFrame* swFrame = av_frame_alloc();  // fallback destination for NVDEC VRAM->RAM transfers
        // Every loop of the video adds the full playback duration to this, so the pts handed
        // to the consumer keeps increasing monotonically and the wall clock never rewinds.
        double loopOffset = 0.0;
        double lastPts = 0.0;

        while (!quit) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (seekPending) {
                    const int64_t targetPts = seekTargetPts;
                    seekPending = false;
                    lock.unlock();

                    avcodec_flush_buffers(dec);
                    if (av_seek_frame(fmt, streamIndex, targetPts, AVSEEK_FLAG_BACKWARD) < 0) {
                        Log::Write(Log::Level::Warning, "video360: seek failed");
                    } else {
                        std::lock_guard<std::mutex> lock2(mutex);
                        for (const auto& qf : queue) freeSlots.push_back(qf.slot);
                        queue.clear();
                        clockStarted = false;  // re-anchor playbackTime to the first post-seek frame
                        loopOffset = 0.0;      // this is an absolute jump, not a loop-around
                        spaceAvailable.notify_all();
#if defined(HAVE_PULSE_SIMPLE)
                        // Tell the audio thread to flush and re-anchor to the new position -
                        // see AudioThread()'s reset handling. Only on a SUCCESSFUL seek: on
                        // failure nothing above changed either, so there is nothing to undo.
                        if (audioActive) {
                            std::lock_guard<std::mutex> audioLock(audioMutex);
                            audioResetPending = true;
                        }
                        audioPacketAvailable.notify_all();
#endif
                    }
                    continue;
                }
            }

            int ret = av_read_frame(fmt, packet);

            if (ret == AVERROR_EOF) {
                // Flush whatever the decoder is still holding, then either rewind and keep
                // going or stop so the caller can move on to the next file in a playlist.
                avcodec_send_packet(dec, nullptr);
                DrainDecoder(frame, swFrame, loopOffset, lastPts);
                if (!loop) break;
                avcodec_flush_buffers(dec);
                // BUG (found 2026-08-09, see BUG_player_loop_speedup_2026-08-09.md in the
                // stereo3d-pack repo): this used to be a plain assignment, `loopOffset =
                // lastPts + frameDuration`. lastPts is always the file's own RAW, 0-based
                // last timestamp (it naturally restarts near 0 every loop, since the file is
                // re-read from position 0 each time) - so the assignment silently threw away
                // every earlier loop's accumulated offset and reused the SAME one-loop-width
                // value forever after the second loop. From loop 3 onward every loop's frames
                // were queued under the identical pts window loop 2 used, which was already
                // "in the past" relative to playbackTime by the time loop 2 finished - so the
                // very next EOF/seek/flush cycle would fire almost immediately (its own
                // frames were stale the instant they were queued), then the next one even
                // faster, compounding into dozens of loop-restarts per real second. That is
                // the sustained ~3x speed-up: not slow motion, but the file being re-decoded
                // and re-discarded far faster than real time, over and over, in the same
                // narrow pts window. Confirmed live via temporary instrumentation logging
                // loopOffset/queue state at every EOF - it stayed pinned at one loop's width
                // instead of growing, and loop-restart log lines went from ~7.5s apart to
                // sub-second apart within a couple of loops. Must accumulate, not overwrite:
                loopOffset += lastPts + frameDuration;
                {
                    // Secondary, smaller effect, worth keeping regardless of the fix above:
                    // re-anchor playbackTime to whatever's next in the queue instead of
                    // trusting the wall-clock accumulator across the loop seam. Without this,
                    // any real time spent in this seek/flush (however small) is wall-clock
                    // time AcquireCurrentSlot() still counts as elapsed but that produced no
                    // frames. Unlike the manual-seek path above, the queue is NOT cleared here
                    // - whatever's still queued is the tail end of the loop that just
                    // finished, still correctly paced and still due to be shown.
                    std::lock_guard<std::mutex> lock2(mutex);
                    clockStarted = false;
                }
#if defined(HAVE_PULSE_SIMPLE)
                // Same reset the manual-seek path signals above, for the same reason: the
                // audio thread must flush pa_simple and re-anchor to loopOffset's new value
                // rather than let stale end-of-file audio bleed into the new loop.
                if (audioActive) {
                    std::lock_guard<std::mutex> audioLock(audioMutex);
                    audioResetPending = true;
                }
                audioPacketAvailable.notify_all();
#endif
                if (av_seek_frame(fmt, streamIndex, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                    Log::Write(Log::Level::Warning, "video360: seek to start failed, stopping playback");
                    break;
                }
                continue;
            }
            if (ret < 0) {
                Log::Write(Log::Level::Error, Fmt("video360: read error: %s", AvErr(ret).c_str()));
                break;
            }

            if (packet->stream_index == streamIndex) {
                ret = avcodec_send_packet(dec, packet);
                if (ret < 0 && ret != AVERROR(EAGAIN)) {
                    Log::Write(Log::Level::Warning, Fmt("video360: send_packet: %s", AvErr(ret).c_str()));
                } else {
                    DrainDecoder(frame, swFrame, loopOffset, lastPts);
                }
            }
#if defined(HAVE_PULSE_SIMPLE)
            else if (audioActive && packet->stream_index == audioStreamIndex) {
                // Hand off a clone to the audio thread - `packet` itself gets reused (via
                // av_packet_unref below) on the very next av_read_frame call, so anything
                // crossing to another thread needs its own copy. loopOffset is captured by
                // value now, not read later, so the audio thread never needs to touch this
                // thread's local variable.
                if (AVPacket* clone = av_packet_clone(packet)) {
                    {
                        std::lock_guard<std::mutex> audioLock(audioMutex);
                        audioQueue.push_back(QueuedAudioPacket{clone, loopOffset});
                    }
                    audioPacketAvailable.notify_all();
                }
            }
#endif
            av_packet_unref(packet);
        }

        av_frame_free(&swFrame);
        av_frame_free(&frame);
        av_packet_free(&packet);

        // Set last: AcquireCurrentSlot only calls it "finished" once the queue has drained too,
        // so the final frames still get their time on screen.
        decodeDone = true;
    }

    // Pulls every frame the decoder can currently produce, writes it as NV12 into a free
    // staging slot and queues that slot.
    void DrainDecoder(AVFrame* frame, AVFrame* swFrame, double loopOffset, double& lastPts) {
        while (!quit) {
            const auto tDecodeStart = std::chrono::steady_clock::now();
            int ret = avcodec_receive_frame(dec, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
            if (ret < 0) {
                Log::Write(Log::Level::Warning, Fmt("video360: receive_frame: %s", AvErr(ret).c_str()));
                return;
            }
            const auto tDecodeEnd = std::chrono::steady_clock::now();

            int64_t ts = frame->best_effort_timestamp;
            double pts = (ts == AV_NOPTS_VALUE) ? lastPts + frameDuration : ts * timeBase;
            lastPts = pts;

            const int slot = TakeFreeSlot();
            if (slot < 0) {
                av_frame_unref(frame);
                return;
            }

            const auto tWriteStart = std::chrono::steady_clock::now();
            bool ok;
            if (frame->format == AV_PIX_FMT_CUDA) {
                hwActive = true;
                // Preferred: NVDEC writes the NV12 surface straight into the mapped staging
                // slot. Falls back to an ffmpeg-owned system-memory frame plus a normalize
                // pass if the driver will not take our layout.
                ok = directXfer && directWorks && TransferDirect(frame, buffers[slot]);
                if (ok) {
                    CaptureColorInfo(frame);
                } else {
                    if (directXfer && directWorks) directWorks = false;
                    av_frame_unref(swFrame);
                    if ((ret = av_hwframe_transfer_data(swFrame, frame, 0)) < 0) {
                        Log::Write(Log::Level::Warning, Fmt("video360: hwframe transfer: %s", AvErr(ret).c_str()));
                        av_frame_unref(frame);
                        ReturnSlot(slot);
                        continue;
                    }
                    av_frame_copy_props(swFrame, frame);
                    CaptureColorInfo(swFrame);
                    ok = NormalizeToNv12(swFrame, buffers[slot]);
                }
            } else {
                CaptureColorInfo(frame);
                ok = NormalizeToNv12(frame, buffers[slot]);
            }
            const auto tWriteEnd = std::chrono::steady_clock::now();

            av_frame_unref(frame);
            if (!ok) {
                ReturnSlot(slot);
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(mutex);
                queue.push_back(QueuedFrame{slot, pts + loopOffset});
                finalPts = pts + loopOffset;
            }

            if (wantStats) {
                statFrames++;
                statDecodeMs += std::chrono::duration<double, std::milli>(tDecodeEnd - tDecodeStart).count();
                statWriteMs += std::chrono::duration<double, std::milli>(tWriteEnd - tWriteStart).count();
                if (statFrames >= 60) ReportStats();
            }
        }
    }

#if defined(HAVE_PULSE_SIMPLE)
    // Opens the audio decoder, the resampler (to fixed S16/48000/stereo), and a pa_simple
    // playback stream. Called from Open() only when HELLO_XR_AUDIO=1 and the file has an
    // audio stream. Returns false having logged exactly one line on any failure - the
    // caller (Open()) falls back to video-only rather than letting a bad audio track or a
    // sound server hiccup take the whole player down.
    bool OpenAudio(const AVStream* stream, const AVCodec* codec) {
        audioDec = avcodec_alloc_context3(codec);
        if (!audioDec) return false;
        int ret = avcodec_parameters_to_context(audioDec, stream->codecpar);
        if (ret < 0) {
            Log::Write(Log::Level::Warning,
                       Fmt("video360: HELLO_XR_AUDIO: parameters_to_context: %s, playing video-only", AvErr(ret).c_str()));
            return false;
        }
        if ((ret = avcodec_open2(audioDec, codec, nullptr)) < 0) {
            Log::Write(Log::Level::Warning, Fmt("video360: HELLO_XR_AUDIO: cannot open audio codec '%s': %s, playing video-only",
                                                codec->name, AvErr(ret).c_str()));
            return false;
        }
        audioTimeBase = av_q2d(stream->time_base);

        // Plain stereo output regardless of what the file carries (mono gets upmixed, 5.1
        // gets downmixed, etc. by swresample) - this player has no spatialization path, so
        // there is nothing to gain from preserving more channels than a headset has ears.
        AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
        ret = swr_alloc_set_opts2(&swr, &outLayout, AV_SAMPLE_FMT_S16, kAudioSampleRate, &audioDec->ch_layout,
                                  audioDec->sample_fmt, audioDec->sample_rate, 0, nullptr);
        if (ret < 0 || !swr || swr_init(swr) < 0) {
            Log::Write(Log::Level::Warning, "video360: HELLO_XR_AUDIO: cannot set up the resampler, playing video-only");
            return false;
        }

        pa_sample_spec spec{};
        spec.format = PA_SAMPLE_S16LE;
        spec.channels = (uint8_t)kAudioChannels;
        spec.rate = (uint32_t)kAudioSampleRate;
        int paErr = 0;
        pa = pa_simple_new(nullptr, "hello_xr", PA_STREAM_PLAYBACK, nullptr, "360 player", &spec, nullptr, nullptr, &paErr);
        if (!pa) {
            Log::Write(Log::Level::Warning, Fmt("video360: HELLO_XR_AUDIO: cannot connect to the PipeWire/Pulse sink (%s), playing video-only",
                                                pa_strerror(paErr)));
            return false;
        }

        Log::Write(Log::Level::Info,
                   Fmt("video360: HELLO_XR_AUDIO on - '%s' %d Hz -> %d Hz stereo S16, head-locked (no spatialization)",
                       codec->name, audioDec->sample_rate, kAudioSampleRate));
        return true;
    }

    // Blocks while playback is paused (rate <= 0), flushing pa_simple exactly once on the
    // transition into pause so whatever's already buffered goes silent immediately - the
    // grip-to-pause gesture (playercontrol.cpp, patch 0005) is meant to be instant, and
    // without this the last ~buffer's worth of audio would otherwise keep audibly playing
    // out on its own after the video visibly stopped. Returns false if the caller should
    // abandon the chunk it was about to write, because shutdown or a loop/seek reset
    // arrived while waiting - in that case control belongs back at the top of AudioThread's
    // main loop, not here.
    bool WaitWhilePaused() {
        if (rate.load(std::memory_order_relaxed) > 0.0) return true;
        int flushErr = 0;
        pa_simple_flush(pa, &flushErr);
        std::unique_lock<std::mutex> lock(audioMutex);
        audioPacketAvailable.wait(lock, [this] {
            return quit.load() || audioResetPending || rate.load(std::memory_order_relaxed) > 0.0;
        });
        return !quit.load() && !audioResetPending;
    }

    // Dedicated audio thread: pulls packets DecodeLoop routed into audioQueue, decodes and
    // resamples them, and blocks on pa_simple_write - that blocking write IS the pacing
    // (PipeWire only accepts more once the hardware has consumed what's already queued),
    // exactly like DecodeLoop's own TakeFreeSlot() blocking is what paces video decode
    // against the renderer. Runs for the lifetime of the Video360 once started; only exists
    // at all when audioActive was set true in Open().
    void AudioThread() {
        AVFrame* frame = av_frame_alloc();
        std::vector<uint8_t> outBuf;  // resample scratch, grown on demand, never shrunk

        while (!quit.load()) {
            // Service a pending loop-seam or manual-seek reset before pulling more packets.
            // Done here, not in DecodeLoop, because pa_simple and the audio AVCodecContext
            // are single-threaded APIs and this is the only thread that ever touches them.
            {
                std::unique_lock<std::mutex> lock(audioMutex);
                if (audioResetPending) {
                    for (auto& qp : audioQueue) av_packet_free(&qp.pkt);
                    audioQueue.clear();
                    audioResetPending = false;
                    lock.unlock();

                    avcodec_flush_buffers(audioDec);
                    // Deliberately NOT recreating swr here: whatever partial samples are
                    // still sitting in its internal resampling delay line are, at typical
                    // source rates (44.1/48 kHz), a sub-millisecond discontinuity - not
                    // worth a full close/reinit at every loop restart.
                    int flushErr = 0;
                    pa_simple_flush(pa, &flushErr);
                    audioAnchorSet = false;
                    audioFramesWritten = 0;
                    audioClockValid.store(false);
                    continue;
                }
            }

            QueuedAudioPacket item{nullptr, 0.0};
            {
                std::unique_lock<std::mutex> lock(audioMutex);
                audioPacketAvailable.wait(lock,
                                          [this] { return quit.load() || audioResetPending || !audioQueue.empty(); });
                if (quit.load() || audioResetPending) continue;
                item = audioQueue.front();
                audioQueue.pop_front();
            }

            int ret = avcodec_send_packet(audioDec, item.pkt);
            av_packet_free(&item.pkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                Log::Write(Log::Level::Warning, Fmt("video360: audio send_packet: %s", AvErr(ret).c_str()));
                continue;
            }

            while (!quit.load()) {
                ret = avcodec_receive_frame(audioDec, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                if (ret < 0) {
                    Log::Write(Log::Level::Warning, Fmt("video360: audio receive_frame: %s", AvErr(ret).c_str()));
                    break;
                }

                const int64_t ts = frame->best_effort_timestamp;
                const double framePts = (ts == AV_NOPTS_VALUE) ? -1.0 : ts * audioTimeBase + item.loopOffsetAtRead;

                const int maxOut = swr_get_out_samples(swr, frame->nb_samples);
                const size_t neededBytes = (size_t)maxOut * kAudioChannels * sizeof(int16_t);
                if (outBuf.size() < neededBytes) outBuf.resize(neededBytes);
                uint8_t* outPtr = outBuf.data();
                const int converted =
                    swr_convert(swr, &outPtr, maxOut, (const uint8_t**)frame->extended_data, frame->nb_samples);
                av_frame_unref(frame);
                if (converted <= 0) continue;

                // Holds here (not writing, not pulling the next packet) until unpaused or a
                // reset arrives - see WaitWhilePaused(). A reset means this chunk predates
                // the seek/loop target, so it's simply dropped; the reset itself is handled
                // at the top of the outer loop.
                if (!WaitWhilePaused()) break;

                if (!audioAnchorSet && framePts >= 0.0) {
                    audioAnchorPts = framePts;
                    audioAnchorSet = true;
                }

                int writeErr = 0;
                pa_simple_write(pa, outBuf.data(), (size_t)converted * kAudioChannels * sizeof(int16_t), &writeErr);
                audioFramesWritten += converted;

                if (audioAnchorSet) {
                    double clock = audioAnchorPts + (double)audioFramesWritten / kAudioSampleRate;
                    const pa_usec_t latency = pa_simple_get_latency(pa, nullptr);
                    // (pa_usec_t)-1 signals "unknown" (see pa_simple_get_latency(3)) - skip
                    // the correction that one time rather than let the unsigned -1 wrap the
                    // clock into the distant past.
                    if (latency != (pa_usec_t)-1) clock -= (double)latency / 1000000.0;
                    audioClockSeconds.store(clock, std::memory_order_relaxed);
                    audioClockValid.store(true, std::memory_order_relaxed);
                }
            }
        }

        av_frame_free(&frame);
    }
#endif  // HAVE_PULSE_SIMPLE
};

Video360::Video360() : m_impl(new Impl()) {}
Video360::~Video360() = default;

bool Video360::Open(const std::string& path) {
    Impl& impl = *m_impl;

    int ret = avformat_open_input(&impl.fmt, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        Log::Write(Log::Level::Error, Fmt("video360: cannot open '%s': %s", path.c_str(), AvErr(ret).c_str()));
        return false;
    }
    if ((ret = avformat_find_stream_info(impl.fmt, nullptr)) < 0) {
        Log::Write(Log::Level::Error, Fmt("video360: no stream info: %s", AvErr(ret).c_str()));
        return false;
    }

    const AVCodec* codec = nullptr;
    impl.streamIndex = av_find_best_stream(impl.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (impl.streamIndex < 0 || !codec) {
        Log::Write(Log::Level::Error, Fmt("video360: no video stream in '%s'", path.c_str()));
        return false;
    }

    AVStream* stream = impl.fmt->streams[impl.streamIndex];
    const bool wantHw = !EnvOff("HELLO_XR_VIDEO_HW");
    if (wantHw && !DecoderCanUseCuda(codec)) {
        const AVCodec* better = PickHwCapableDecoder(stream->codecpar->codec_id, codec);
        if (better != codec) {
            Log::Write(Log::Level::Info, Fmt("video360: '%s' cannot use NVDEC, switching to '%s'", codec->name, better->name));
            codec = better;
        }
    }
    impl.dec = avcodec_alloc_context3(codec);
    if (!impl.dec) return false;
    if ((ret = avcodec_parameters_to_context(impl.dec, stream->codecpar)) < 0) {
        Log::Write(Log::Level::Error, Fmt("video360: parameters_to_context: %s", AvErr(ret).c_str()));
        return false;
    }
    // 0 = let libavcodec pick a thread count from the CPU. Only matters on the software
    // fallback path (hwaccel decode serializes on the GPU anyway).
    impl.dec->thread_count = 0;
    impl.dec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;

    // NVDEC via ffmpeg's CUDA hwaccel. Uses the dlopened driver libraries (libcuda/libnvcuvid),
    // no CUDA toolkit involved. HELLO_XR_VIDEO_HW=0 forces the software decoder.
    if (wantHw) {
        ret = av_hwdevice_ctx_create(&impl.hwDevice, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
        if (ret < 0) {
            Log::Write(Log::Level::Warning,
                       Fmt("video360: no CUDA/NVDEC device (%s), falling back to software decode", AvErr(ret).c_str()));
        } else {
            impl.dec->hw_device_ctx = av_buffer_ref(impl.hwDevice);
            impl.dec->get_format = PickHwFormat;
        }
    } else {
        Log::Write(Log::Level::Info, "video360: HELLO_XR_VIDEO_HW=0, software decode forced");
    }

    if ((ret = avcodec_open2(impl.dec, codec, nullptr)) < 0) {
        Log::Write(Log::Level::Error, Fmt("video360: cannot open codec: %s", AvErr(ret).c_str()));
        return false;
    }

    impl.width = impl.dec->width;
    impl.height = impl.dec->height;
    impl.timeBase = av_q2d(stream->time_base);
    if (stream->avg_frame_rate.num > 0 && stream->avg_frame_rate.den > 0) {
        impl.frameDuration = av_q2d(AVRational{stream->avg_frame_rate.den, stream->avg_frame_rate.num});
    }
    // Prefer the container-level duration (AV_TIME_BASE units, i.e. microseconds) over the
    // stream's own - some files only set one or the other.
    if (impl.fmt->duration != AV_NOPTS_VALUE) {
        impl.durationSeconds = (double)impl.fmt->duration / AV_TIME_BASE;
    } else if (stream->duration != AV_NOPTS_VALUE) {
        impl.durationSeconds = stream->duration * impl.timeBase;
    }
    if (impl.width <= 0 || impl.height <= 0 || (impl.width % 2) != 0 || (impl.height % 2) != 0) {
        Log::Write(Log::Level::Error, Fmt("video360: %dx%d is not a usable even-sized frame", impl.width, impl.height));
        return false;
    }

    impl.codecName = codec->name ? codec->name : "unknown";

    // What the file says about itself. VR180 cameras and YouTube's VR streams both write these
    // (MP4 `st3d` for the stereo packing, `sv3d` for the projection); plenty of re-encodes drop
    // them, which is what the filename and aspect-ratio fallbacks in ResolvePanoLayout are for.
    const AVPacketSideData* sd = av_packet_side_data_get(stream->codecpar->coded_side_data,
                                                         stream->codecpar->nb_coded_side_data, AV_PKT_DATA_STEREO3D);
    if (sd != nullptr && sd->size >= sizeof(AVStereo3D)) {
        const AVStereo3D* s3d = (const AVStereo3D*)sd->data;
        switch (s3d->type) {
            case AV_STEREO3D_2D:
                impl.layout.stereo = PanoStereo::Mono;
                break;
            case AV_STEREO3D_SIDEBYSIDE:
                impl.layout.stereo = PanoStereo::SideBySide;
                break;
            case AV_STEREO3D_TOPBOTTOM:
                impl.layout.stereo = PanoStereo::TopBottom;
                break;
            default:
                Log::Write(Log::Level::Warning,
                           Fmt("video360: stereo layout '%s' is not supported, treating the frame as mono",
                               av_stereo3d_type_name(s3d->type)));
                impl.layout.stereo = PanoStereo::Mono;
                break;
        }
        // AV_STEREO3D_FLAG_INVERT means the right eye is packed first.
        impl.layout.swapEyes = (s3d->flags & AV_STEREO3D_FLAG_INVERT) != 0;
    }

    sd = av_packet_side_data_get(stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
                                 AV_PKT_DATA_SPHERICAL);
    if (sd != nullptr && sd->size >= sizeof(AVSphericalMapping)) {
        const AVSphericalMapping* sph = (const AVSphericalMapping*)sd->data;
        switch (sph->projection) {
            case AV_SPHERICAL_EQUIRECTANGULAR:
                impl.layout.projection = PanoProjection::Equirect360;
                break;
            case AV_SPHERICAL_EQUIRECTANGULAR_TILE:
                // A cropped equirect. Only the full-frame case maps cleanly onto our shader,
                // and a half-width tile is exactly how some tools write VR180.
                impl.layout.projection = PanoProjection::HalfEquirect180;
                break;
            case AV_SPHERICAL_HALF_EQUIRECTANGULAR:
                impl.layout.projection = PanoProjection::HalfEquirect180;
                break;
            default:
                Log::Write(Log::Level::Warning,
                           Fmt("video360: projection '%s' is not supported", av_spherical_projection_name(sph->projection)));
                break;
        }
    }

    impl.wantStats = getenv("HELLO_XR_VIDEO_STATS") != nullptr;
    impl.directXfer = !EnvOff("HELLO_XR_VIDEO_DIRECT");
    impl.xferFrame = av_frame_alloc();
    if (!impl.xferFrame) return false;
    impl.rowScratch.resize((size_t)impl.width + 64);

    // Optional stereo audio. Never allowed to fail Open() itself: any problem here (no
    // audio stream, a codec/resampler that won't open, no sound server to talk to) is
    // logged once and playback continues video-only, silent exactly as it always was - a
    // demo must not die because its audio track or output device didn't cooperate.
    if (EnvOn("HELLO_XR_AUDIO")) {
#if defined(HAVE_PULSE_SIMPLE)
        const AVCodec* audioCodec = nullptr;
        impl.audioStreamIndex = av_find_best_stream(impl.fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &audioCodec, 0);
        if (impl.audioStreamIndex < 0 || !audioCodec) {
            Log::Write(Log::Level::Info, "video360: HELLO_XR_AUDIO=1 but the file has no audio stream, playing video-only");
        } else {
            impl.audioActive = impl.OpenAudio(impl.fmt->streams[impl.audioStreamIndex], audioCodec);
        }
#else
        Log::Write(Log::Level::Info, "video360: HELLO_XR_AUDIO=1 but this build has no libpulse-simple, playing video-only");
#endif
    }

    Log::Write(Log::Level::Info, Fmt("video360: '%s' %dx%d %s @ %.2f fps (decode: %s)", path.c_str(), impl.width,
                                     impl.height, codec->name, 1.0 / impl.frameDuration,
                                     impl.dec->hw_device_ctx ? "NVDEC requested" : "software"));

    impl.opened = true;
    return true;
}

void Video360::SetFrameBuffers(std::vector<Video360Buffer> buffers) {
    m_impl->buffers = std::move(buffers);
    m_impl->freeSlots.clear();
    for (int i = 0; i < (int)m_impl->buffers.size(); i++) m_impl->freeSlots.push_back(i);
}

bool Video360::Start() {
    Impl& impl = *m_impl;
    if (!impl.opened || impl.started) return false;
    if (impl.buffers.size() < 2) {
        Log::Write(Log::Level::Error, "video360: Start() before SetFrameBuffers(), or fewer than two buffers");
        return false;
    }
    impl.statWindow = std::chrono::steady_clock::now();
    impl.started = true;
    impl.thread = std::thread([&impl] { impl.DecodeLoop(); });
#if defined(HAVE_PULSE_SIMPLE)
    if (impl.audioActive) impl.audioThread = std::thread([&impl] { impl.AudioThread(); });
#endif
    return true;
}

void Video360::SetLoop(bool loop) { m_impl->loop = loop; }
void Video360::SetRate(double rate) {
    m_impl->rate = (rate > 0.0) ? rate : 0.0;
    // Wakes a paused audio thread immediately (see Impl::WaitWhilePaused) instead of
    // leaving it to notice the new rate whenever its next spurious wakeup happens. Harmless
    // to call when audio is inactive - notifying a condition_variable with no waiters is a
    // no-op.
    m_impl->audioPacketAvailable.notify_all();
}

bool Video360::Finished() const {
    Impl& impl = *m_impl;
    if (impl.loop || !impl.started) return false;
    if (!impl.decodeDone) return false;

    std::lock_guard<std::mutex> lock(impl.mutex);
    if (!impl.queue.empty()) return false;
    // The decoder never produced anything at all - a broken file. Say finished so a playlist
    // moves on instead of sitting on a black skybox forever.
    if (!impl.clockStarted) return true;
    return impl.playbackTime >= impl.finalPts + impl.frameDuration;
}

bool Video360::IsOpen() const { return m_impl->opened; }
int Video360::Width() const { return m_impl->width; }
int Video360::Height() const { return m_impl->height; }
size_t Video360::YBytes() const { return m_impl->YBytes(); }
size_t Video360::UVBytes() const { return m_impl->UVBytes(); }
double Video360::FrameRate() const { return m_impl->frameDuration > 0.0 ? 1.0 / m_impl->frameDuration : 0.0; }
double Video360::Duration() const { return m_impl->durationSeconds; }
double Video360::PlaybackPosition() const {
    Impl& impl = *m_impl;
    std::lock_guard<std::mutex> lock(impl.mutex);
    return impl.playbackTime;
}
void Video360::Seek(double deltaSeconds) {
    Impl& impl = *m_impl;
    if (!impl.started || impl.decodeDone) return;
    std::lock_guard<std::mutex> lock(impl.mutex);
    double target = impl.playbackTime + deltaSeconds;
    if (target < 0.0) target = 0.0;
    if (impl.durationSeconds > 0.0 && target > impl.durationSeconds) target = impl.durationSeconds;
    impl.seekTargetPts = (int64_t)(target / impl.timeBase);
    impl.seekPending = true;
}
std::string Video360::CodecName() const { return m_impl->codecName; }
PanoLayout Video360::DetectedLayout() const { return m_impl->layout; }
bool Video360::FullRange() const { return m_impl->fullRange; }
bool Video360::Bt709() const { return m_impl->bt709; }
bool Video360::HwDecodeActive() const { return m_impl->hwActive; }

int Video360::AcquireCurrentSlot() {
    Impl& impl = *m_impl;
    if (!impl.started) return -1;

    std::unique_lock<std::mutex> lock(impl.mutex);

    // Start the clock on the first frame rather than at Start(), so however long the decoder
    // took to spin up does not count as playback time already elapsed. Starting it AT the
    // first frame's pts (not at zero) also means a file whose timestamps do not begin at zero
    // does not have its opening seconds skipped.
    const auto now = std::chrono::steady_clock::now();
    if (!impl.clockStarted) {
        if (impl.queue.empty()) return -1;
        impl.playbackTime = impl.queue.front().pts;
        impl.lastTick = now;
        impl.clockStarted = true;
    } else if (impl.audioActive && impl.audioClockValid.load(std::memory_order_relaxed)) {
        // Audio is the master clock once it's active: pa_simple's blocking writes are paced
        // by the real hardware/PipeWire clock, which holds sync over minutes far better than
        // a wall-clock accumulator running in parallel with it ever could. The audio thread
        // updates audioClockSeconds after every chunk it writes (Impl::AudioThread).
        // audioClockValid goes false for the brief startup window before the first chunk
        // lands, and again for one beat right after a loop seam or manual seek while the
        // audio thread is mid-flush - both cases fall through to the wall-clock accumulator
        // below instead of stalling video on an audio clock that isn't ready yet.
        impl.playbackTime = impl.audioClockSeconds.load(std::memory_order_relaxed);
        impl.lastTick = now;
    } else {
        impl.playbackTime += std::chrono::duration<double>(now - impl.lastTick).count() * impl.rate.load();
        impl.lastTick = now;
    }
    const double elapsed = impl.playbackTime;

    // Advance to the newest frame whose presentation time has passed. The loop (rather than a
    // single pop) means that if rendering stalls we skip stale frames instead of playing them
    // back in slow motion. Every buffer we move past goes straight back to the decoder.
    bool advanced = false;
    while (!impl.queue.empty() && impl.queue.front().pts <= elapsed) {
        if (impl.currentSlot >= 0) impl.freeSlots.push_back(impl.currentSlot);
        impl.currentSlot = impl.queue.front().slot;
        impl.queue.pop_front();
        advanced = true;
    }
    if (advanced) {
        impl.spaceAvailable.notify_all();
        return impl.currentSlot;
    }
    // Nothing due. If the queue is also empty the decoder is behind, which is worth counting
    // separately from "the renderer is simply faster than the frame rate".
    if (impl.queue.empty() && impl.wantStats) impl.statStarved++;
    return -1;
}

#endif  // HAVE_FFMPEG
