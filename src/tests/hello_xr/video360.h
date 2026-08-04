// 360 equirectangular video source for the hello_xr photo/video skybox.
//
// Decodes a video file on a background thread into a small ring of NV12 frames and hands
// the render loop whichever frame is due according to a wall clock. Playback loops forever.
//
// Decode is NVDEC (ffmpeg CUDA hwaccel) when available, with automatic fallback to software
// decode (HELLO_XR_VIDEO_HW=0 forces the fallback). Either way the output is normalized to
// 8-bit NV12 (tightly packed Y plane + interleaved half-res CbCr plane) and the YUV->RGB
// conversion happens on the GPU in a fragment shader - there is no libswscale in the loop,
// which was measured to cost more CPU than the decode itself at 4K.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// One decoded frame, normalized to tightly-packed 8-bit NV12.
// y is Width()*Height() bytes; uv is interleaved CbCr, (Width()/2)*(Height()/2)*2 bytes.
struct Video360Frame {
    const uint8_t* y{nullptr};
    const uint8_t* uv{nullptr};
};

class Video360 {
   public:
    Video360();
    ~Video360();

    Video360(const Video360&) = delete;
    Video360& operator=(const Video360&) = delete;

    // Opens the file and starts the decode thread. Returns false (and logs) on failure.
    bool Open(const std::string& path);

    bool IsOpen() const;
    int Width() const;
    int Height() const;

    size_t YBytes() const;   // Width() * Height()
    size_t UVBytes() const;  // Width() * Height() / 2

    // Colorimetry of the stream, for the GPU conversion shader. Stable after Open() using
    // stream metadata; unspecified streams fall back to the HD/SD heuristic (>=720p -> BT.709).
    bool FullRange() const;
    bool Bt709() const;

    // True when NVDEC is actually decoding (not merely requested).
    bool HwDecodeActive() const;

    // Returns the frame that should be on screen now, or nullptr when the frame already
    // uploaded is still the correct one (i.e. nothing new is due yet). The returned pointer
    // stays valid until the next call.
    const Video360Frame* AcquireCurrentFrame();

   private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
