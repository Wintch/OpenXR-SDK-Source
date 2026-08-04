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
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libavutil/spherical.h>
#include <libavutil/stereo3d.h>
}

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
        if (thread.joinable()) thread.join();
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
            int ret = av_read_frame(fmt, packet);

            if (ret == AVERROR_EOF) {
                // Flush whatever the decoder is still holding, then either rewind and keep
                // going or stop so the caller can move on to the next file in a playlist.
                avcodec_send_packet(dec, nullptr);
                DrainDecoder(frame, swFrame, loopOffset, lastPts);
                if (!loop) break;
                avcodec_flush_buffers(dec);
                loopOffset = lastPts + frameDuration;
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
    return true;
}

void Video360::SetLoop(bool loop) { m_impl->loop = loop; }
void Video360::SetRate(double rate) { m_impl->rate = (rate > 0.0) ? rate : 0.0; }

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
