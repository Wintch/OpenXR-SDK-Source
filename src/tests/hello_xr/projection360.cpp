#include "pch.h"
#include "common.h"
#include "logger.h"
#include "projection360.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

std::string LowerBasename(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return name;
}

bool Contains(const std::string& haystack, const char* needle) { return haystack.find(needle) != std::string::npos; }

// Env value matching, case-insensitive, empty/unset -> false.
bool EnvIs(const char* name, std::initializer_list<const char*> values) {
    const char* v = getenv(name);
    if (v == nullptr || v[0] == '\0') return false;
    std::string s(v);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    for (const char* candidate : values) {
        if (s == candidate) return true;
    }
    return false;
}

bool EnvSet(const char* name) {
    const char* v = getenv(name);
    return v != nullptr && v[0] != '\0';
}

}  // namespace

const char* PanoProjectionName(PanoProjection p) {
    switch (p) {
        case PanoProjection::Equirect360:
            return "360 equirectangular";
        case PanoProjection::HalfEquirect180:
            return "180 half-equirectangular";
        case PanoProjection::Flat:
            return "flat (not panoramic)";
        default:
            return "unknown";
    }
}

const char* PanoStereoName(PanoStereo s) {
    switch (s) {
        case PanoStereo::Mono:
            return "mono";
        case PanoStereo::SideBySide:
            return "stereo side-by-side";
        case PanoStereo::TopBottom:
            return "stereo over-under";
        default:
            return "unknown";
    }
}

PanoLayout ResolvePanoLayout(PanoLayout detected, const std::string& path, int width, int height) {
    PanoLayout out = detected;
    const char* projectionSource = (out.projection != PanoProjection::Unknown) ? "container metadata" : nullptr;
    const char* stereoSource = (out.stereo != PanoStereo::Unknown) ? "container metadata" : nullptr;

    // 1. Explicit overrides always win - metadata in the wild is wrong often enough that
    //    there has to be a way to say "no, it is really this".
    if (EnvIs("HELLO_XR_PROJECTION", {"180", "vr180", "half"})) {
        out.projection = PanoProjection::HalfEquirect180;
        projectionSource = "HELLO_XR_PROJECTION";
    } else if (EnvIs("HELLO_XR_PROJECTION", {"360", "equirect", "full"})) {
        out.projection = PanoProjection::Equirect360;
        projectionSource = "HELLO_XR_PROJECTION";
    } else if (EnvIs("HELLO_XR_PROJECTION", {"flat", "2d", "plano", "screen"})) {
        out.projection = PanoProjection::Flat;
        projectionSource = "HELLO_XR_PROJECTION";
    } else if (EnvSet("HELLO_XR_PROJECTION") && !EnvIs("HELLO_XR_PROJECTION", {"auto"})) {
        Log::Write(Log::Level::Warning, Fmt("HELLO_XR_PROJECTION='%s' not understood (use 180, 360, flat or auto)",
                                            getenv("HELLO_XR_PROJECTION")));
    }

    if (EnvIs("HELLO_XR_STEREO", {"mono", "2d", "none"})) {
        out.stereo = PanoStereo::Mono;
        stereoSource = "HELLO_XR_STEREO";
    } else if (EnvIs("HELLO_XR_STEREO", {"sbs", "lr", "side-by-side", "sidebyside"})) {
        out.stereo = PanoStereo::SideBySide;
        stereoSource = "HELLO_XR_STEREO";
    } else if (EnvIs("HELLO_XR_STEREO", {"tb", "ou", "top-bottom", "topbottom", "over-under"})) {
        out.stereo = PanoStereo::TopBottom;
        stereoSource = "HELLO_XR_STEREO";
    } else if (EnvSet("HELLO_XR_STEREO") && !EnvIs("HELLO_XR_STEREO", {"auto"})) {
        Log::Write(Log::Level::Warning,
                   Fmt("HELLO_XR_STEREO='%s' not understood (use mono, sbs, tb or auto)", getenv("HELLO_XR_STEREO")));
    }

    // 2. Filename conventions. Cameras and rigs write these consistently enough to be worth
    //    trusting over a bare aspect ratio, and re-encodes routinely lose the MP4 boxes.
    const std::string name = LowerBasename(path);
    if (out.projection == PanoProjection::Unknown) {
        if (Contains(name, "vr180") || Contains(name, "_180") || Contains(name, "-180") || Contains(name, ".180") ||
            Contains(name, "180_") || Contains(name, "180-")) {
            out.projection = PanoProjection::HalfEquirect180;
            projectionSource = "filename";
        } else if (Contains(name, "360")) {
            out.projection = PanoProjection::Equirect360;
            projectionSource = "filename";
        }
    }
    if (out.stereo == PanoStereo::Unknown) {
        if (Contains(name, "sbs") || Contains(name, "_lr") || Contains(name, "3dh") || Contains(name, "half-sbs")) {
            out.stereo = PanoStereo::SideBySide;
            stereoSource = "filename";
        } else if (Contains(name, "_tb") || Contains(name, "_ou") || Contains(name, "3dv") || Contains(name, "over-under") ||
                   Contains(name, "overunder")) {
            out.stereo = PanoStereo::TopBottom;
            stereoSource = "filename";
        }
    }

    // 3. Aspect ratio, resolving whichever field is still open. Each panoramic layout implies
    //    a per-eye aspect of 2:1 (360) or 1:1 (180), so once one field is known the other
    //    follows. Anything that matches none of them is most likely not panoramic at all.
    const double aspect = (height > 0) ? (double)width / (double)height : 2.0;
    auto near = [aspect](double target) { return aspect > target * 0.9 && aspect < target * 1.1; };

    if (out.projection != PanoProjection::Unknown && out.stereo == PanoStereo::Unknown) {
        double monoAspect;
        switch (out.projection) {
            case PanoProjection::Equirect360:
                monoAspect = 2.0;
                break;
            case PanoProjection::HalfEquirect180:
                monoAspect = 1.0;
                break;
            default:  // flat: no canonical shape, so only a wild aspect suggests packing
                monoAspect = aspect;
                break;
        }
        if (near(monoAspect * 2.0)) {
            out.stereo = PanoStereo::SideBySide;
        } else if (near(monoAspect / 2.0)) {
            out.stereo = PanoStereo::TopBottom;
        } else {
            out.stereo = PanoStereo::Mono;
        }
        stereoSource = "aspect ratio";
    } else if (out.projection == PanoProjection::Unknown && out.stereo != PanoStereo::Unknown) {
        double perEye = aspect;
        if (out.stereo == PanoStereo::SideBySide) perEye = aspect / 2.0;
        if (out.stereo == PanoStereo::TopBottom) perEye = aspect * 2.0;
        if (perEye > 1.8) {
            out.projection = PanoProjection::Equirect360;
        } else if (perEye > 1.4) {
            out.projection = PanoProjection::Flat;  // 16:9 per eye: an ordinary 3D movie
        } else {
            out.projection = PanoProjection::HalfEquirect180;
        }
        projectionSource = "aspect ratio";
    } else if (out.projection == PanoProjection::Unknown) {
        // Nothing known at all. 2:1 is the classic mono 360 pano and the case that has been
        // working here, so it keeps that reading; 4:1 can only be 360 side-by-side. A 1:1
        // frame is far more often VR180 mono than 360 over-under. Everything else - 16:9 in
        // particular - is an ordinary video.
        if (near(2.0)) {
            out.projection = PanoProjection::Equirect360;
            out.stereo = PanoStereo::Mono;
        } else if (near(4.0)) {
            out.projection = PanoProjection::Equirect360;
            out.stereo = PanoStereo::SideBySide;
        } else if (near(1.0)) {
            out.projection = PanoProjection::HalfEquirect180;
            out.stereo = PanoStereo::Mono;
        } else if (near(0.5)) {
            out.projection = PanoProjection::HalfEquirect180;
            out.stereo = PanoStereo::TopBottom;
        } else {
            out.projection = PanoProjection::Flat;
            out.stereo = PanoStereo::Mono;
        }
        projectionSource = stereoSource = "aspect ratio";
    }

    // 4. Angular size. For 180 this is the arc the frame spans (a true VR180 file is 180x180,
    //    but YouTube's "mesh" VR180 re-encode packs a slightly different shape, so it is worth
    //    being able to nudge). For flat it is how big the virtual screen is.
    const double perEyeAspect = aspect / ((out.stereo == PanoStereo::SideBySide) ? 2.0 : 1.0) *
                                ((out.stereo == PanoStereo::TopBottom) ? 2.0 : 1.0);
    const double kDeg2Rad = 3.14159265358979323846 / 180.0;
    if (out.projection == PanoProjection::HalfEquirect180) {
        double fovX = 180.0, fovY = 180.0;
        const char* v = getenv("HELLO_XR_PANO_FOV");
        if (v != nullptr && v[0] != '\0') {
            double a = 0.0, b = 0.0;
            const int n = sscanf(v, "%lfx%lf", &a, &b);
            if (n >= 1 && a > 0.0) {
                fovX = a;
                fovY = (n >= 2 && b > 0.0) ? b : a;
            } else {
                Log::Write(Log::Level::Warning, Fmt("HELLO_XR_PANO_FOV='%s' not understood (use e.g. 180x180)", v));
            }
        }
        out.halfFovX = (float)(fovX * 0.5 * kDeg2Rad);
        out.halfFovY = (float)(fovY * 0.5 * kDeg2Rad);
    } else if (out.projection == PanoProjection::Flat) {
        double screenFovX = 70.0;  // a comfortable "big TV across the room"
        const char* v = getenv("HELLO_XR_SCREEN_FOV");
        if (v != nullptr && v[0] != '\0') {
            const double a = atof(v);
            if (a > 1.0 && a < 170.0) {
                screenFovX = a;
            } else {
                Log::Write(Log::Level::Warning, Fmt("HELLO_XR_SCREEN_FOV='%s' out of range (1..170 degrees)", v));
            }
        }
        out.halfFovX = (float)tan(screenFovX * 0.5 * kDeg2Rad);
        out.halfFovY = (float)(out.halfFovX / (perEyeAspect > 0.01 ? perEyeAspect : 1.777));
    }

    Log::Write(Log::Level::Info, Fmt("projection: %s, %s (%dx%d, %.2f:1) [projection from %s, stereo from %s]",
                                     PanoProjectionName(out.projection), PanoStereoName(out.stereo), width, height, aspect,
                                     projectionSource ? projectionSource : "default", stereoSource ? stereoSource : "default"));

    // A 2:1 file that is really VR180 side-by-side looks exactly like a 360 mono file, and
    // getting it wrong is confusing rather than obviously broken - so say how to correct it.
    if (strcmp(projectionSource ? projectionSource : "", "aspect ratio") == 0) {
        Log::Write(Log::Level::Info,
                   "projection: guessed from the aspect ratio - the file said nothing. Override with "
                   "HELLO_XR_PROJECTION=180|360|flat and HELLO_XR_STEREO=mono|sbs|tb.");
    }

    return out;
}

std::string PanoBanner(const PanoLayout& layout, int width, int height, double fps, const std::string& codec) {
    const bool stereo = (layout.stereo == PanoStereo::SideBySide || layout.stereo == PanoStereo::TopBottom);
    int eyeW = width, eyeH = height;
    if (layout.stereo == PanoStereo::SideBySide) eyeW /= 2;
    if (layout.stereo == PanoStereo::TopBottom) eyeH /= 2;

    std::string mode;
    switch (layout.projection) {
        case PanoProjection::Equirect360:
            mode = stereo ? "360 3D" : "360";
            break;
        case PanoProjection::HalfEquirect180:
            mode = stereo ? "VR180 3D" : "VR180";
            break;
        default:
            mode = stereo ? "PLANO 3D" : "PLANO";
            break;
    }

    std::string line = Fmt("  MODO: %s", mode.c_str());
    if (stereo) {
        line += Fmt(" (%s%s)", layout.stereo == PanoStereo::SideBySide ? "side-by-side" : "over-under",
                    layout.swapEyes ? ", ojos invertidos" : "");
    }
    line += Fmt("\n  Archivo: %dx%d", width, height);
    if (stereo) line += Fmt("  ->  %dx%d por ojo", eyeW, eyeH);
    if (fps > 0.0) line += Fmt("  |  %.2f fps", fps);
    if (!codec.empty()) line += Fmt("  |  %s", codec.c_str());
    return line;
}

void PanoEyeUvTransform(const PanoLayout& layout, int eye, float outScale[2], float outOffset[2]) {
    outScale[0] = outScale[1] = 1.0f;
    outOffset[0] = outOffset[1] = 0.0f;

    const int packed = layout.swapEyes ? (1 - eye) : eye;
    switch (layout.stereo) {
        case PanoStereo::SideBySide:
            outScale[0] = 0.5f;
            outOffset[0] = (packed == 0) ? 0.0f : 0.5f;
            break;
        case PanoStereo::TopBottom:
            outScale[1] = 0.5f;
            outOffset[1] = (packed == 0) ? 0.0f : 0.5f;
            break;
        default:
            break;
    }
}
