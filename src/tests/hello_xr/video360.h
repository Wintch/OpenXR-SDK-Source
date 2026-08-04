// 360 equirectangular video source for the hello_xr photo/video skybox.
//
// Decodes a video file on a background thread and hands the render loop whichever frame is
// due according to a wall clock. Playback loops forever.
//
// Decode is NVDEC (ffmpeg CUDA hwaccel) when available, with automatic fallback to software
// decode (HELLO_XR_VIDEO_HW=0 forces the fallback). Either way the output is normalized to
// 8-bit NV12 (tightly packed Y plane + interleaved half-res CbCr plane) and the YUV->RGB
// conversion happens on the GPU in a fragment shader - there is no libswscale in the loop,
// which was measured to cost more CPU than the decode itself at 4K.
//
// The decoder does NOT own its output frames: the caller hands it a set of buffers via
// SetFrameBuffers() and the decode thread writes NV12 straight into them. The point is that
// the graphics backend can pass mapped Vulkan staging memory, so a decoded frame is already
// where the GPU wants it and the render thread never copies anything. At 8K that memcpy was
// measured at 8.9 ms per frame on the render thread - more than half a 60Hz frame budget.
//
// Hence the three-step startup: Open() (learn the geometry) -> SetFrameBuffers() (the caller
// can now size its allocations) -> Start() (begin decoding).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "projection360.h"

// Where one decoded frame goes. Memory is owned by the caller and must stay valid and mapped
// for the lifetime of the Video360. y is Width()*Height() bytes; uv is interleaved CbCr,
// Width()*Height()/2 bytes. Both are written sequentially, never read - safe for the
// write-combined host memory Vulkan hands out.
struct Video360Buffer {
    uint8_t* y{nullptr};
    uint8_t* uv{nullptr};
};

class Video360 {
   public:
    // How many frame buffers the caller should provide: one held by the renderer, one being
    // written, and the rest absorbing decode-time jitter.
    //
    // This was five, which left only three frames of slack - and at 8K60 that was measured to
    // be too thin: the decoder averaged the full 59 fps but its per-frame time varies enough
    // (keyframes cost far more than P-frames) that the queue kept running dry, and the
    // renderer found nothing new to upload on ~25% of frames. Eight costs 3 more frames of
    // host memory - 126 MB at 8K, nothing at 4K - and covers the jitter.
    static constexpr size_t kFrameBuffers = 8;

    Video360();
    ~Video360();

    Video360(const Video360&) = delete;
    Video360& operator=(const Video360&) = delete;

    // Opens the file and reads the stream geometry. Does not decode anything yet, so the
    // caller can size its buffers from Width()/Height(). Returns false (and logs) on failure.
    bool Open(const std::string& path);

    // Whether to rewind and keep going at the end of the file (the default) or stop and let
    // Finished() go true, which is what a playlist wants. Call before Start().
    void SetLoop(bool loop);

    // Playback speed. 1.0 is normal, 0.5 is half speed, 0 pauses. Safe to change at any time:
    // it only affects how fast the clock runs from that point on. Decoding is unaffected -
    // the decoder simply waits longer for the renderer to hand buffers back.
    void SetRate(double rate);

    // True once the last frame has had its turn on screen. Always false when looping.
    bool Finished() const;

    // Hands over the destination buffers. Must be called after Open() and before Start(),
    // with at least two buffers (kFrameBuffers is the sane number).
    void SetFrameBuffers(std::vector<Video360Buffer> buffers);

    // Starts the decode thread. Returns false if no buffers were set.
    bool Start();

    bool IsOpen() const;
    int Width() const;
    int Height() const;

    size_t YBytes() const;   // Width() * Height()
    size_t UVBytes() const;  // Width() * Height() / 2

    // Frames per second the file declares. Useful to sanity-check what the pipeline sustains.
    double FrameRate() const;

    // Name of the video codec, for the "now playing" banner.
    std::string CodecName() const;

    // What the container says about projection and stereo packing (the MP4 `sv3d`/`st3d`
    // boxes). Either field may come back Unknown; feed the result to ResolvePanoLayout().
    PanoLayout DetectedLayout() const;

    // Colorimetry of the stream, for the GPU conversion shader. Stable after the first frame
    // using stream metadata; unspecified streams fall back to the HD/SD heuristic (>=720p ->
    // BT.709).
    bool FullRange() const;
    bool Bt709() const;

    // True when NVDEC is actually decoding (not merely requested).
    bool HwDecodeActive() const;

    // Index of the buffer that should be on screen now, or -1 when the frame already uploaded
    // is still the correct one (i.e. nothing new is due yet). The returned buffer stays valid
    // and untouched by the decoder until the next call that returns a different index.
    int AcquireCurrentSlot();

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
