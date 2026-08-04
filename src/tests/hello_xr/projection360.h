// How a panoramic frame maps onto the sphere, and how the two eyes are packed into it.
//
// Five layouts cover essentially everything in the wild, and three of them are ambiguous from
// the pixel dimensions alone:
//
//   360 mono          2:1     <- ambiguous with 180 stereo side-by-side
//   360 stereo TB     1:1     <- ambiguous with 180 mono
//   360 stereo SBS    4:1
//   180 mono          1:1
//   180 stereo SBS    2:1     <- by far the most common VR180 camera output
//   180 stereo TB     1:2
//
// So the aspect ratio is the last resort. In order: an explicit HELLO_XR_PROJECTION /
// HELLO_XR_STEREO override, then the container's own metadata (the MP4 `sv3d` and `st3d`
// boxes, which every VR180 camera and YouTube VR export writes), then filename conventions,
// then the aspect ratio.

#pragma once

#include <string>

enum class PanoProjection {
    Unknown,
    Equirect360,      // full sphere
    HalfEquirect180,  // front hemisphere: azimuth -90..+90, full 180 vertically
    Flat,             // not panoramic at all - an ordinary frame on a virtual screen
};

enum class PanoStereo {
    Unknown,
    Mono,
    SideBySide,  // left eye in the left half
    TopBottom,   // left eye on top
};

struct PanoLayout {
    PanoProjection projection{PanoProjection::Unknown};
    PanoStereo stereo{PanoStereo::Unknown};
    bool swapEyes{false};  // the packed frame holds the right eye first

    // Half-angles in radians for HalfEquirect180 (default 90 deg each, i.e. a true 180x180
    // hemisphere), or half-extents in tangent units for Flat. Set by ResolvePanoLayout.
    float halfFovX{0.0f};
    float halfFovY{0.0f};
};

// Fills in whatever `detected` left Unknown, using (in order) the environment overrides,
// filename conventions and the frame aspect ratio. Logs what it settled on and why. `path` may
// be empty. Never returns Unknown in either field.
PanoLayout ResolvePanoLayout(PanoLayout detected, const std::string& path, int width, int height);

// The sub-rectangle of the frame belonging to one eye, as a texture-coordinate scale and
// offset. eye 0 is left, 1 is right; identity for mono. Applied after the spherical mapping,
// so the mapping itself never has to know about stereo packing.
void PanoEyeUvTransform(const PanoLayout& layout, int eye, float outScale[2], float outOffset[2]);

const char* PanoProjectionName(PanoProjection p);
const char* PanoStereoName(PanoStereo s);

// One short human-readable line describing what is about to be shown - "what mode am I in"
// is otherwise invisible once the headset is on, and a VR180 file mistaken for a 360 one
// looks plausible rather than obviously wrong.
std::string PanoBanner(const PanoLayout& layout, int width, int height, double fps, const std::string& codec);
