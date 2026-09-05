// Copyright (c) 2017-2026 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0
#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

#pragma fragment

#define PI 3.14159265359

#define PROJ_360  0   // full equirectangular sphere
#define PROJ_180  1   // half-equirectangular: a hemisphere in front of you
#define PROJ_FLAT 2   // ordinary rectangular video on a virtual screen

layout (std140, push_constant) uniform buf
{
    mat4 viewRotation;    // eye orientation: view space -> world space
    vec4 fovTangents;     // tan(angleLeft), tan(angleRight), tan(angleUp), tan(angleDown)
    vec4 uvScaleOffset;   // xy scale, zw offset: this eye's sub-rectangle of a stereo frame
    vec4 panoFov;         // 180: half-angles in radians. flat: half-extents of the screen.
    ivec4 mode;           // x: PROJ_* in bits 0-3, eye in bit 4, HELLO_XR_TEST_PATTERN in
                           // bits 5-6 (see below), counter-mode frame count/phase in bits
                           // 7-14/15-16, HELLO_XR_GPU_LOAD percentage (0-100) in bits 17-23
                           // (see GpuLoadPerturb below - always present, independent of
                           // HELLO_XR_TEST_PATTERN), HELLO_XR_PASSTHROUGH_FISHEYE_CORRECT in
                           // bit 24 (see Cam0Distort below - only ever set for real PROJ_FLAT
                           // camera-passthrough content, never test patterns or ordinary
                           // pano/photo/video). y/z/w: progress bar and quit-hold, see
                           // near the bottom of this file (also reused by card/toggle test
                           // patterns).
} ubuf;

layout (set = 0, binding = 0) uniform sampler2D equirectTex;

layout (location = 0) in vec2 iNdc;
layout (location = 0) out vec4 FragColor;

// sRGB (display code value, 0..1) -> linear light. The player's swapchain format is *_SRGB
// (see SelectColorSwapchainFormat in graphicsplugin_vulkan.cpp), so the GPU re-encodes
// whatever a shader writes here on the way into the framebuffer - a value has to be
// pre-compensated with this to land on the panel at the CODE VALUE named, not at that value
// again after a second gamma curve. Used by CardColor below; the .cpp has its own copy for
// the toggle-mode gray pair, since host and shader can't share code across that boundary.
float SrgbToLinear(float c)
{
    return (c <= 0.04045) ? (c / 12.92) : pow((c + 0.055) / 1.055, 2.4);
}

// HELLO_XR_PASSTHROUGH_FISHEYE_CORRECT (docs/08, 2026-09-05): a live wearer asked for the raw
// camera-passthrough view's fisheye barrel distortion to be corrected so straight real-world
// lines (door frames, wall edges) look straight, the way they actually look to the eye. cam0's
// raw frame was being shown with a plain linear/pinhole UV mapping and no distortion
// correction at all (correct for ordinary rectilinear video/photos, wrong for a fisheye lens).
//
// cam0's calibration (~/vr/camera-calibration.json on the lab rig) is Basalt's "fisheye624"
// model - confirmed from Basalt's own source, not assumed from the coefficient names:
// basalt/thirdparty/basalt-headers/include/basalt/camera/fisheye624_camera.hpp. It is a
// Kannala-Brandt equidistant radial term (theta = atan(r), distorted by an order-12 odd
// polynomial in theta with coefficients k1..k6) plus a Brown-Conrady tangential term (p1, p2).
// The model also supports a "thin prism" term (s1..s4); cam0's calibration doesn't carry those
// coefficients (equivalent to 0), so they're omitted below. This mirrors that header's
// distort() function (the analytic FORWARD direction: undistorted normalized ray -> raw pixel)
// term for term - forward Kannala-Brandt distortion is a closed-form polynomial, but its
// inverse generally isn't, so rather than trying to undistort the source image, every output
// pixel's already-rectilinear ray is distorted forward to find where it lands in the raw
// fisheye source and sampled there ("distort the sample coordinate, not the image").
//
// Baked as shader constants rather than plumbed through the push-constant buffer: that struct
// is already at Vulkan's guaranteed 128-byte push-constant limit (see vulkan_utils.h /
// graphicsplugin_vulkan.cpp), and this passthrough viewer only ever reads cam0 (camera0.pgm) -
// its intrinsics don't change at runtime. A second live camera would need a real uniform
// buffer instead of more constants here.
const float kCam0Fx = 270.848579;
const float kCam0Fy = 270.904999;
const float kCam0Cx = 324.454842;
const float kCam0Cy = 242.324553;
const float kCam0ImgW = 640.0;
const float kCam0ImgH = 480.0;
const float kCam0K1 = 0.447793;
const float kCam0K2 = 0.359404;
const float kCam0K3 = 0.008073;
const float kCam0K4 = 0.710360;
const float kCam0K5 = 0.393890;
const float kCam0K6 = 0.062225;
const float kCam0P1 = -0.000176;
const float kCam0P2 = 0.000239;

// xy: an UNDISTORTED normalized ray in Basalt/OpenCV camera convention (x right, y DOWN,
// z forward) - i.e. (X/Z, Y/Z) for a point along that ray. Returns the raw fisheye pixel
// coordinate (in cam0's 640x480 pixel space) that ray actually lands on through the real lens.
vec2 Cam0Distort(vec2 xy)
{
    float rp = length(xy);
    if (rp < 1e-8) {
        // On-axis: theta is 0 and cos/sin(phi) are undefined, but the distorted point is just
        // the principal point regardless (every term above is proportional to theta or rp).
        return vec2(kCam0Cx, kCam0Cy);
    }
    float th = atan(rp);
    float th2 = th * th;
    // theta * (1 + k1*th^2 + k2*th^4 + ... + k6*th^12), Horner form - matches the header
    // exactly (see distort()'s theta_dist).
    float thetaDist = th * (1.0 + th2 * (kCam0K1 + th2 * (kCam0K2 + th2 * (kCam0K3 + th2 * (kCam0K4 + th2 * (kCam0K5 + th2 * kCam0K6))))));
    vec2 r = (thetaDist / rp) * xy;  // xr, yr: distorted radial coordinate
    float rd2 = dot(r, r);
    vec2 tangential = vec2((2.0 * r.x * r.x + rd2) * kCam0P1 + 2.0 * r.x * r.y * kCam0P2,
                           (2.0 * r.y * r.y + rd2) * kCam0P2 + 2.0 * r.x * r.y * kCam0P1);
    vec2 pp = r + tangential;
    return vec2(kCam0Fx * pp.x + kCam0Cx, kCam0Fy * pp.y + kCam0Cy);
}

// HELLO_XR_TEST_PATTERN=card (see graphicsplugin_vulkan.cpp): thin white border + checkerboard
// corner markers around an interior fill that swaps with the background grey on mode.y (0 or
// 1 - set from the same frame counter/cadence toggle mode uses). The two greys are the
// ~10%/~40% sRGB pair mode 1's "gray" toggle also uses (see kTogglePairs in the .cpp) -
// interior and background are always opposite members of that pair, so the whole quad+
// backdrop only ever shows those two values, just swapping which region has which.
vec3 CardColor(vec2 uv, float inside)
{
    float dark = SrgbToLinear(0.10);
    float mid = SrgbToLinear(0.40);
    bool swapped = ubuf.mode.y != 0;
    float interiorGrey = swapped ? mid : dark;
    float backgroundGrey = swapped ? dark : mid;

    if (inside < 0.5) {
        return vec3(backgroundGrey);
    }

    // Thin white outline just inside the quad's edge - a static, always-sharp reference so
    // edge-arrival can be judged separately from how long the larger fill areas take to settle.
    float edgeDist = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));
    if (edgeDist < 0.03) {
        return vec3(1.0);
    }

    // Checkerboard corner markers: a fine black/white grid in each corner, inside the border.
    const float kCornerSize = 0.18;
    const float kCell = 0.03;
    bool inCornerX = (uv.x < kCornerSize) || (uv.x > 1.0 - kCornerSize);
    bool inCornerY = (uv.y < kCornerSize) || (uv.y > 1.0 - kCornerSize);
    if (inCornerX && inCornerY) {
        float cx = floor(uv.x / kCell);
        float cy = floor(uv.y / kCell);
        bool white = mod(cx + cy, 2.0) < 1.0;
        return vec3(white ? 1.0 : 0.0);
    }

    return vec3(interiorGrey);
}

// HELLO_XR_TEST_PATTERN=counter (see graphicsplugin_vulkan.cpp): a small head-locked patch,
// screen-space only like the progress/quit-hold bars further down, so an external high-speed
// camera can read off exactly which rendered frame is on screen. modeX bits 7-14 carry a
// wrapping 0-255 frame counter (bit on = white, off = black, 8 blocks packed 4x2, MSB first);
// bits 15-16 carry a color phase (0=R 1=G 2=B, cycling every frame) shown in the gaps between
// blocks - a coarse signal visible even out of focus, on top of the precise per-frame count.
// Returns false (and leaves FragColor untouched) outside the patch.
bool CounterPatch(float s, float t, int modeX)
{
    const float kPatchSMin = 0.03, kPatchSMax = 0.25;
    const float kPatchTMin = 0.72, kPatchTMax = 0.92;
    if (s < kPatchSMin || s > kPatchSMax || t < kPatchTMin || t > kPatchTMax) {
        return false;
    }

    int counter = (modeX >> 7) & 0xFF;
    int phase = (modeX >> 15) & 0x3;
    vec3 phaseColor = (phase == 0) ? vec3(1.0, 0.0, 0.0) : (phase == 1) ? vec3(0.0, 1.0, 0.0) : vec3(0.0, 0.0, 1.0);

    float ps = (s - kPatchSMin) / (kPatchSMax - kPatchSMin) * 4.0;
    float pt = (t - kPatchTMin) / (kPatchTMax - kPatchTMin) * 2.0;
    float cellS = fract(ps);
    float cellT = fract(pt);
    int bitIndex = int(pt) * 4 + int(ps);
    bool bitOn = ((counter >> (7 - bitIndex)) & 1) != 0;
    bool inBlock = cellS > 0.12 && cellS < 0.88 && cellT > 0.12 && cellT < 0.88;

    FragColor = vec4(inBlock ? (bitOn ? vec3(1.0) : vec3(0.0)) : phaseColor, 1.0);
    return true;
}

// HELLO_XR_GPU_LOAD (see graphicsplugin_vulkan.cpp): synthetic per-fragment busy work for
// sweeping GPU utilization against VR frame pacing with no real game driving the GPU -
// scripts/gpu-load-sweep.sh in the lab's reverb-g2 repo is what launches this. mode.x bits
// 17-23 carry the requested load as the literal 0-100 percentage parsed from the env var (see
// the .cpp for why it's not pre-multiplied into an iteration count there) - kGpuLoadItersPerPercent
// below is the ONLY calibration knob, so retuning for a different GPU/resolution never needs a
// pipeline rebuild, just a different HELLO_XR_GPU_LOAD value at the next launch.
//
// The loop bound is a runtime value (read out of a push constant, not a compile-time literal),
// so the compiler cannot unroll or hoist it away, and the accumulated hash feeds directly into
// the caller's FragColor - it cannot be dead-code-eliminated either. Each iteration is one
// sin() plus a fused multiply-add: sin/cos route through the SFU on NVIDIA parts, at a
// fraction of the core FMA rate, so this is deliberately more expensive per iteration than
// plain arithmetic - "thousands of FMAs" worth of shader-core-equivalent cost without needing
// an equally huge iteration count. The result is scaled by 1e-9 before being handed back, so
// it perturbs the caller's color imperceptibly: HELLO_XR_GPU_LOAD=0 gives iterations=0,
// acc=0.0, and FragColor + vec3(0.0) is bit-exact - see the .cpp for the "unset is
// byte-identical" rule this project holds every HELLO_XR_* option to.
//
// Calibration (rough, per the project's own brief - measure with gpu-load-sweep.sh, don't
// trust this as exact): kGpuLoadItersPerPercent=40 puts HELLO_XR_GPU_LOAD=100 at 4000
// sin+FMA iterations per fragment. At 4320x2160@90 stereo (the G2's native mode) that's
// expected to bring an RTX 3060 Ti close to saturation; retune this constant against a real
// pacing sweep rather than assuming the number holds on other GPUs or resolutions.
const int kGpuLoadItersPerPercent = 40;
vec3 GpuLoadPerturb(int modeX, vec2 fragXy)
{
    int loadPct = (modeX >> 17) & 0x7F;
    int iterations = loadPct * kGpuLoadItersPerPercent;

    // Seed from screen position: keeps the compiler from treating the loop as fragment-
    // invariant (which could let it hoist/cache the result once for the whole draw), and
    // keeps neighboring fragments from all walking the identical hash trajectory.
    float h = fragXy.x * 12.9898 + fragXy.y * 78.233;
    float acc = 0.0;
    for (int i = 0; i < iterations; ++i) {
        h = fract(sin(h) * 43758.5453);
        acc = fma(h, h, acc - h * 0.5);
    }
    return vec3(acc) * 1e-9;
}

void main()
{
    // Vulkan NDC: x runs -1 (left) .. +1 (right), y runs -1 (top) .. +1 (bottom).
    float s = iNdc.x * 0.5 + 0.5;
    float t = iNdc.y * 0.5 + 0.5;

    // HELLO_XR_TEST_PATTERN (see graphicsplugin_vulkan.cpp for how mode.x's bits 5-6 get set,
    // and docs/pruebas.jsonl T206 in the lab repo for why this exists - a suspected LCD
    // strobe-crosstalk/gray-to-gray artifact needing patterns that isolate the display chain
    // from tracking/reprojection/content-decode). 0 (unset) takes none of these branches, so
    // behavior is byte-identical to before this feature existed.
    int testPattern = (ubuf.mode.x >> 5) & 0x3;

    // TOGGLE: full-field solid color, zero geometry, zero motion - returns before any of the
    // ray/projection math below runs. Color comes straight from fovTangents (see the .cpp -
    // that slot is unused by this mode, reused instead of growing the push-constant struct).
    if (testPattern == 1) {
        FragColor = vec4(ubuf.fovTangents.rgb, 1.0);
        FragColor.rgb += GpuLoadPerturb(ubuf.mode.x, gl_FragCoord.xy);
        return;
    }

    // COUNTER: also screen-space only, checked here ahead of all the ray/projection math since
    // it never needs any of it, and must win over the `discard` below for pixels outside the
    // loaded content - easiest to guarantee by simply returning before reaching that check.
    if (testPattern == 3 && CounterPatch(s, t, ubuf.mode.x)) {
        FragColor.rgb += GpuLoadPerturb(ubuf.mode.x, gl_FragCoord.xy);
        return;
    }

    float xv = mix(ubuf.fovTangents.x, ubuf.fovTangents.y, s);  // left -> right
    float yv = mix(ubuf.fovTangents.z, ubuf.fovTangents.w, t);  // up -> down

    // View space looks down -Z. Rotate the ray into world space; no translation, the image is at infinity.
    vec3 dir = normalize(mat3(ubuf.viewRotation) * vec3(xv, yv, -1.0));

    // mode.x carries the eye index in bit 4 alongside PROJ_* in the low bits (spare bits in an
    // int that only ever needed 0/1/2 - see the overlay-bar parallax comment below for why).
    int projType = ubuf.mode.x & 0xF;
    bool isRightEye = (ubuf.mode.x & 0x10) != 0;

    vec2 uv;
    float inside = 1.0;
    bool wraps = false;

    // Digital zoom (panoFov.z - see vulkan_utils.h, unused before this): >1 magnifies, <1
    // shows more of the source than the headset's native FOV would. Applied as a divisor on
    // whatever angular/screen coordinate each branch below maps into uv - the sphere or
    // screen itself never changes, just how much of it one physical ray picks up. Floored well
    // above 0 so a runaway zoom-out can't divide by (near-)zero.
    float zoom = max(ubuf.panoFov.z, 0.01);

    if (projType == PROJ_FLAT) {
        // A virtual screen floating straight ahead. Rays going sideways or backwards miss it.
        float depth = -dir.z;
        vec2 screen = dir.xy / max(depth, 1e-4);
        screen /= zoom;
        uv = screen / vec2(ubuf.panoFov.x, -ubuf.panoFov.y) * 0.5 + 0.5;
        inside = (depth > 0.0 && all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) ? 1.0 : 0.0;

        // HELLO_XR_PASSTHROUGH_FISHEYE_CORRECT (see Cam0Distort above): `screen` is already
        // exactly (X/Z, Y/Z) for this output pixel's ray - the undistorted normalized
        // coordinate Cam0Distort expects - just in this shader's view-space convention (+Y
        // up). Flip Y to Basalt's convention (+Y down, same flip the panoFov.y negation above
        // already applies going the other way), forward-distort it to find cam0's raw fisheye
        // pixel, and sample there instead of at the plain linear `uv`. `inside` above still
        // gates on the requested virtual screen's extent (panoFov, e.g. HELLO_XR_SCREEN_FOV);
        // AND it with the raw sample landing inside cam0's actual 640x480 frame too, so corners
        // the fisheye lens doesn't actually cover (possible once corrected to a wide rectilinear
        // FOV) go black instead of smearing the source's edge pixels.
        if (((ubuf.mode.x >> 24) & 0x1) != 0) {
            vec2 rawPixel = Cam0Distort(vec2(screen.x, -screen.y));
            vec2 uvRaw = rawPixel / vec2(kCam0ImgW, kCam0ImgH);
            inside = (inside > 0.5 && all(greaterThanEqual(uvRaw, vec2(0.0))) && all(lessThanEqual(uvRaw, vec2(1.0))))
                         ? 1.0
                         : 0.0;
            uv = uvRaw;
        }
    } else {
        // Equirectangular: horizontal angle -> u, vertical angle -> v.
        float az = atan(dir.x, -dir.z) / zoom;            // 0 straight ahead, +/-PI behind
        float el = asin(clamp(dir.y, -1.0, 1.0)) / zoom;  // +PI/2 straight up

        if (projType == PROJ_180) {
            // The frame only holds the hemisphere in front of you; everything else stays black
            // instead of smearing the edge columns across the back of your head.
            uv = vec2(az / (2.0 * ubuf.panoFov.x) + 0.5, 0.5 - el / (2.0 * ubuf.panoFov.y));
            inside = (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) ? 1.0 : 0.0;
        } else {
            uv = vec2(az / (2.0 * PI) + 0.5, 0.5 - el / PI);
            wraps = true;
        }
    }

    // CARD: reuses the PROJ_FLAT uv/inside just computed above - a world-locked flat quad is
    // exactly that path's own geometry (the .cpp forces projType to PROJ_FLAT whenever this
    // mode is active, regardless of the loaded content's real projection). Paints the WHOLE
    // screen, inside the quad or not, instead of discarding - see CardColor above.
    if (testPattern == 2) {
        FragColor = vec4(CardColor(uv, inside), 1.0);
        FragColor.rgb += GpuLoadPerturb(ubuf.mode.x, gl_FragCoord.xy);
        return;
    }

    // u wraps from 1 back to 0 at the seam behind the viewer. The implicit derivative there is
    // ~1.0 instead of ~0, which would drive the mip selection to the smallest level and paint a
    // blocky band across the seam - so unwrap the derivatives and select the mip explicitly.
    //
    // The derivatives are taken unconditionally, before anything is masked: in a quad where
    // some pixels had taken a different branch they would otherwise be undefined.
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    if (wraps) {
        dx.x -= round(dx.x);
        dy.x -= round(dy.x);
    }

    // Finally pick this eye's half of a stereo frame. Identity for mono footage, so the
    // mapping above never has to know how the two eyes are packed.
    uv = uv * ubuf.uvScaleOffset.xy + ubuf.uvScaleOffset.zw;
    dx *= ubuf.uvScaleOffset.xy;
    dy *= ubuf.uvScaleOffset.xy;

    // Progress bar and quit-hold bar (see below) draw over their screen-space strip whether or
    // not it's `inside` - a bar should never vanish just because it happens to fall outside the
    // content area (letterboxing in flat mode, the void beyond a 180 frame's edge, etc).
    // Found 2026-08-09: the strips used to sit at t in [0.94,0.98] / [0.02,0.06] - 2-4% from
    // the absolute edge of the render target. That's outside where anyone actually looks with
    // the headset on (no one rotates their eyes/head to the extreme edge of the lens just to
    // check a progress bar) - user report: "aparece muy abajo, no lo llego a ver". A first
    // move to [0.80,0.88] still wasn't enough ("se ve mejor, pero aun muy abajo") - the
    // effective comfortable/sharp area of this headset's optics is clearly smaller than the
    // nominal render FOV suggests. Moved much further inboard this time, explicitly trading
    // subtlety for guaranteed visibility per the user - fine if it eats into the content more.
    // Horizontal inset too (found same session): full-width (s 0..1) read as "goes off to the
    // sides" once the bar was actually inside the visible area - centered and narrowed to the
    // same comfortable window both bars now live in vertically.
    const float kBarSMin = 0.20;
    const float kBarSMax = 0.80;

    // Found same session: drawn identically for both eyes (zero disparity), the bars read as
    // glued flat to the lens/at infinity, floating oddly against real stereo 3D content that
    // has its own depth from disparity baked into the footage - user: "hace que este a la
    // distancia del video". Fake a comfortable fixed HUD depth (kHudMeters) instead of true
    // per-pixel depth (which isn't knowable - it's baked into whatever the footage shows):
    // shift each eye's copy of the bar by the small angle a real object at that depth would
    // subtend given a nominal IPD, same small-angle approximation (tan(x) ~= x) used
    // everywhere else in this shader. Left eye sees a centered point shifted right, right eye
    // sees it shifted left - sign flips with isRightEye (see above).
    const float kHalfIpdMeters = 0.0315;
    const float kHudMeters = 1.5;
    float parallaxShiftS = (kHalfIpdMeters / kHudMeters) / (ubuf.fovTangents.y - ubuf.fovTangents.x);
    float sBar = isRightEye ? s + parallaxShiftS : s - parallaxShiftS;

    float barAlpha = float(ubuf.mode.z) / 255.0;
    bool inProgressBar = barAlpha > 0.0 && t > 0.58 && t < 0.62 && sBar > kBarSMin && sBar < kBarSMax;
    float quitHold = float(ubuf.mode.w) / 1000.0;
    bool inQuitFill = quitHold > 0.0 && t > 0.30 && t < 0.40 && sBar > kBarSMin &&
                       sBar < mix(kBarSMin, kBarSMax, quitHold);

    // Genuinely empty space (no content, no bar) is left to whatever the app cleared the frame
    // to (see GetBackgroundClearColor() in openxr_program.cpp / HELLO_XR_THEME) instead of
    // being painted black here - so it discards rather than writing a color.
    if (inside < 0.5 && !inProgressBar && !inQuitFill) {
        discard;
    }

    FragColor = vec4(inside > 0.5 ? textureGrad(equirectTex, uv, dx, dy).rgb : vec3(0.0), 1.0);

    // Brightness: a plain multiplier on the content, applied before the overlay bars below so
    // dimming the video never also dims the progress/quit-hold feedback.
    FragColor.rgb *= ubuf.panoFov.w;

    // Progress bar: a thin strip near the bottom of the screen, drawn straight in screen
    // space (not projected onto the pano), so it stays flat and legible no matter what
    // projection mode is active or what direction you're looking. mode.y/mode.z aren't used
    // by anything above - reusing them here instead of growing the push-constant struct past
    // the 128 bytes Vulkan guarantees (it's already exactly at that limit).
    if (inProgressBar) {
        float progress = float(ubuf.mode.y) / 1000.0;
        float sNorm = (sBar - kBarSMin) / (kBarSMax - kBarSMin);  // 0..1 across the inset bar
        vec3 barColor = (sNorm < progress) ? vec3(0.95, 0.15, 0.12) : vec3(0.30, 0.08, 0.07);
        FragColor.rgb = mix(FragColor.rgb, barColor, barAlpha);
    }

    // Quit-confirm hold: a red-orange bar near the TOP that fills left-to-right while the WMR
    // Menu button is held (openxr_program.cpp's hold-to-confirm timer). Opposite edge from the
    // playback progress bar above so the two can never be confused for one another; always
    // full alpha while holding, since unlike the progress bar there is no need to auto-hide
    // something that is only ever visible while actively held.
    if (inQuitFill) {
        vec3 quitColor = vec3(1.0, 0.25, 0.15);
        FragColor.rgb = mix(FragColor.rgb, quitColor, 0.9);
    }

    // HELLO_XR_GPU_LOAD: see GpuLoadPerturb above. Applied last so it never interacts with the
    // progress/quit-hold blending above (imperceptible either way, but this keeps the ordering
    // obviously irrelevant instead of relying on the perturbation being too small to matter).
    // Note this also runs on the `discard`ed path above (nothing there returns), which is fine
    // - a discarded fragment never reaches the framebuffer regardless of what FragColor holds.
    FragColor.rgb += GpuLoadPerturb(ubuf.mode.x, gl_FragCoord.xy);
}
