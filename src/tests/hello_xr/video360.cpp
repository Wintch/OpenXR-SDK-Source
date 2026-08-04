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
bool Video360::IsOpen() const { return false; }
int Video360::Width() const { return 0; }
int Video360::Height() const { return 0; }
size_t Video360::YBytes() const { return 0; }
size_t Video360::UVBytes() const { return 0; }
bool Video360::FullRange() const { return false; }
bool Video360::Bt709() const { return true; }
bool Video360::HwDecodeActive() const { return false; }
const Video360Frame* Video360::AcquireCurrentFrame() { return nullptr; }

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
}

namespace {
// How many decoded frames to keep ahead of playback. Three is enough to absorb decode-time
// spikes (keyframes are much more expensive than P-frames) without adding real latency;
// each one costs Width*Height*1.5 bytes of host memory, ~12MB at 4096x2048.
constexpr size_t kQueueDepth = 3;

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
}  // namespace

struct Video360::Impl {
    struct Frame {
        std::vector<uint8_t> y;   // tightly packed, width*height
        std::vector<uint8_t> uv;  // interleaved CbCr, (width/2)*(height/2)*2
        double pts{0.0};          // seconds since playback start, already loop-adjusted
    };

    AVFormatContext* fmt{nullptr};
    AVCodecContext* dec{nullptr};
    AVBufferRef* hwDevice{nullptr};
    int streamIndex{-1};
    int width{0};
    int height{0};
    double timeBase{0.0};
    double frameDuration{1.0 / 30.0};  // fallback when a frame carries no usable pts

    std::atomic<bool> hwActive{false};
    std::atomic<bool> fullRange{false};
    std::atomic<bool> bt709{true};
    bool warnedFormat{false};

    std::thread thread;
    std::mutex mutex;
    std::condition_variable spaceAvailable;
    std::condition_variable frameAvailable;
    std::deque<Frame> queue;
    std::atomic<bool> quit{false};
    bool opened{false};

    // The frame currently uploaded to the GPU. Held so AcquireCurrentFrame can return a
    // stable pointer, and so we can tell "nothing new" from "here is a new frame".
    Frame current;
    Video360Frame currentView;
    bool haveCurrent{false};

    std::chrono::steady_clock::time_point clockStart;
    bool clockStarted{false};

    ~Impl() {
        quit = true;
        spaceAvailable.notify_all();
        frameAvailable.notify_all();
        if (thread.joinable()) thread.join();
        if (dec) avcodec_free_context(&dec);
        if (hwDevice) av_buffer_unref(&hwDevice);
        if (fmt) avformat_close_input(&fmt);
    }

    // Copies an arbitrary decoded frame into tightly-packed 8-bit NV12. Handles the formats
    // this pipeline actually meets: NV12 (NVDEC transfer output), YUV420P/YUVJ420P (software
    // h264/hevc), and the 10-bit variants P010/YUV420P10 (downshifted to 8 bit - HDR grading
    // is out of scope for a VR skybox).
    bool NormalizeToNv12(const AVFrame* f, Frame& out) {
        const int w = width, h = height;
        const int cw = w / 2, ch = h / 2;
        out.y.resize((size_t)w * h);
        out.uv.resize((size_t)cw * ch * 2);

        switch (f->format) {
            case AV_PIX_FMT_NV12:
                for (int r = 0; r < h; r++) memcpy(out.y.data() + (size_t)r * w, f->data[0] + (size_t)r * f->linesize[0], w);
                for (int r = 0; r < ch; r++)
                    memcpy(out.uv.data() + (size_t)r * cw * 2, f->data[1] + (size_t)r * f->linesize[1], (size_t)cw * 2);
                return true;
            case AV_PIX_FMT_YUV420P:
            case AV_PIX_FMT_YUVJ420P:
                for (int r = 0; r < h; r++) memcpy(out.y.data() + (size_t)r * w, f->data[0] + (size_t)r * f->linesize[0], w);
                for (int r = 0; r < ch; r++) {
                    const uint8_t* u = f->data[1] + (size_t)r * f->linesize[1];
                    const uint8_t* v = f->data[2] + (size_t)r * f->linesize[2];
                    uint8_t* dst = out.uv.data() + (size_t)r * cw * 2;
                    for (int c = 0; c < cw; c++) {
                        dst[c * 2 + 0] = u[c];
                        dst[c * 2 + 1] = v[c];
                    }
                }
                return true;
            case AV_PIX_FMT_P010LE:  // data in the high 10 of 16 bits -> high byte is the 8-bit value
                for (int r = 0; r < h; r++) {
                    const uint8_t* src = f->data[0] + (size_t)r * f->linesize[0];
                    uint8_t* dst = out.y.data() + (size_t)r * w;
                    for (int c = 0; c < w; c++) dst[c] = src[c * 2 + 1];
                }
                for (int r = 0; r < ch; r++) {
                    const uint8_t* src = f->data[1] + (size_t)r * f->linesize[1];
                    uint8_t* dst = out.uv.data() + (size_t)r * cw * 2;
                    for (int c = 0; c < cw * 2; c++) dst[c] = src[c * 2 + 1];
                }
                return true;
            case AV_PIX_FMT_YUV420P10LE:  // data in the low 10 bits -> >>2
                for (int r = 0; r < h; r++) {
                    const uint16_t* src = (const uint16_t*)(f->data[0] + (size_t)r * f->linesize[0]);
                    uint8_t* dst = out.y.data() + (size_t)r * w;
                    for (int c = 0; c < w; c++) dst[c] = (uint8_t)(src[c] >> 2);
                }
                for (int r = 0; r < ch; r++) {
                    const uint16_t* u = (const uint16_t*)(f->data[1] + (size_t)r * f->linesize[1]);
                    const uint16_t* v = (const uint16_t*)(f->data[2] + (size_t)r * f->linesize[2]);
                    uint8_t* dst = out.uv.data() + (size_t)r * cw * 2;
                    for (int c = 0; c < cw; c++) {
                        dst[c * 2 + 0] = (uint8_t)(u[c] >> 2);
                        dst[c * 2 + 1] = (uint8_t)(v[c] >> 2);
                    }
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

    void DecodeLoop() {
        AVPacket* packet = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        AVFrame* swFrame = av_frame_alloc();  // destination for NVDEC VRAM->RAM transfers
        // Every loop of the video adds the full playback duration to this, so the pts handed
        // to the consumer keeps increasing monotonically and the wall clock never rewinds.
        double loopOffset = 0.0;
        double lastPts = 0.0;

        while (!quit) {
            int ret = av_read_frame(fmt, packet);

            if (ret == AVERROR_EOF) {
                // Flush whatever the decoder is still holding, then rewind and keep going.
                avcodec_send_packet(dec, nullptr);
                DrainDecoder(frame, swFrame, loopOffset, lastPts);
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
    }

    // Pulls every frame the decoder can currently produce, normalizes it to NV12 and queues it.
    void DrainDecoder(AVFrame* frame, AVFrame* swFrame, double loopOffset, double& lastPts) {
        while (!quit) {
            int ret = avcodec_receive_frame(dec, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return;
            if (ret < 0) {
                Log::Write(Log::Level::Warning, Fmt("video360: receive_frame: %s", AvErr(ret).c_str()));
                return;
            }

            int64_t ts = frame->best_effort_timestamp;
            double pts = (ts == AV_NOPTS_VALUE) ? lastPts + frameDuration : ts * timeBase;
            lastPts = pts;

            const AVFrame* src = frame;
            if (frame->format == AV_PIX_FMT_CUDA) {
                // NVDEC decoded into VRAM; pull the NV12 surface back to system memory. (The
                // upload side puts it straight back on the GPU - a zero-copy Vulkan interop
                // exists but is far more code; at ~720MB/s for 4K30 this round trip is cheap.)
                av_frame_unref(swFrame);
                if ((ret = av_hwframe_transfer_data(swFrame, frame, 0)) < 0) {
                    Log::Write(Log::Level::Warning, Fmt("video360: hwframe transfer: %s", AvErr(ret).c_str()));
                    av_frame_unref(frame);
                    continue;
                }
                av_frame_copy_props(swFrame, frame);
                hwActive = true;
                src = swFrame;
            }

            Frame out;
            out.pts = pts + loopOffset;
            CaptureColorInfo(src);
            const bool ok = NormalizeToNv12(src, out);
            av_frame_unref(frame);
            if (!ok) continue;

            std::unique_lock<std::mutex> lock(mutex);
            spaceAvailable.wait(lock, [this] { return quit || queue.size() < kQueueDepth; });
            if (quit) return;
            queue.push_back(std::move(out));
            frameAvailable.notify_one();
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
    const char* hwEnv = getenv("HELLO_XR_VIDEO_HW");
    const bool wantHw = !(hwEnv && hwEnv[0] == '0');
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

    Log::Write(Log::Level::Info, Fmt("video360: '%s' %dx%d %s @ %.2f fps (decode: %s)", path.c_str(), impl.width,
                                     impl.height, codec->name, 1.0 / impl.frameDuration,
                                     impl.dec->hw_device_ctx ? "NVDEC requested" : "software"));
    if (impl.width != impl.height * 2) {
        Log::Write(Log::Level::Warning,
                   Fmt("video360: %dx%d is not 2:1 - equirectangular video is expected to be, the image will look wrong",
                       impl.width, impl.height));
    }

    impl.opened = true;
    impl.thread = std::thread([&impl] { impl.DecodeLoop(); });
    return true;
}

bool Video360::IsOpen() const { return m_impl->opened; }
int Video360::Width() const { return m_impl->width; }
int Video360::Height() const { return m_impl->height; }
size_t Video360::YBytes() const { return (size_t)m_impl->width * m_impl->height; }
size_t Video360::UVBytes() const { return (size_t)m_impl->width * m_impl->height / 2; }
bool Video360::FullRange() const { return m_impl->fullRange; }
bool Video360::Bt709() const { return m_impl->bt709; }
bool Video360::HwDecodeActive() const { return m_impl->hwActive; }

const Video360Frame* Video360::AcquireCurrentFrame() {
    Impl& impl = *m_impl;
    if (!impl.opened) return nullptr;

    std::unique_lock<std::mutex> lock(impl.mutex);

    // Start the clock on the first frame rather than at Open(), so however long the decoder
    // took to spin up does not count as playback time already elapsed.
    if (!impl.clockStarted) {
        if (impl.queue.empty()) return nullptr;
        impl.clockStart = std::chrono::steady_clock::now();
        impl.clockStarted = true;
    }

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - impl.clockStart).count();

    // Advance to the newest frame whose presentation time has passed. The loop (rather than a
    // single pop) means that if rendering stalls we skip stale frames instead of playing them
    // back in slow motion.
    bool advanced = false;
    while (!impl.queue.empty() && impl.queue.front().pts <= elapsed) {
        impl.current = std::move(impl.queue.front());
        impl.queue.pop_front();
        impl.haveCurrent = true;
        advanced = true;
    }
    if (advanced) impl.spaceAvailable.notify_all();

    if (!advanced) return nullptr;
    impl.currentView.y = impl.current.y.data();
    impl.currentView.uv = impl.current.uv.data();
    return &impl.currentView;
}

#endif  // HAVE_FFMPEG
