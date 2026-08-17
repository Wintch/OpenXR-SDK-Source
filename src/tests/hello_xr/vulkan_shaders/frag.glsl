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
                           // 7-14/15-16. y/z/w: progress bar and quit-hold, see near the
                           // bottom of this file (also reused by card/toggle test patterns).
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
        return;
    }

    // COUNTER: also screen-space only, checked here ahead of all the ray/projection math since
    // it never needs any of it, and must win over the `discard` below for pixels outside the
    // loaded content - easiest to guarantee by simply returning before reaching that check.
    if (testPattern == 3 && CounterPatch(s, t, ubuf.mode.x)) {
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
}
