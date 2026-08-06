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
    ivec4 mode;           // x: PROJ_*
} ubuf;

layout (set = 0, binding = 0) uniform sampler2D equirectTex;

layout (location = 0) in vec2 iNdc;
layout (location = 0) out vec4 FragColor;

void main()
{
    // Vulkan NDC: x runs -1 (left) .. +1 (right), y runs -1 (top) .. +1 (bottom).
    float s = iNdc.x * 0.5 + 0.5;
    float t = iNdc.y * 0.5 + 0.5;
    float xv = mix(ubuf.fovTangents.x, ubuf.fovTangents.y, s);  // left -> right
    float yv = mix(ubuf.fovTangents.z, ubuf.fovTangents.w, t);  // up -> down

    // View space looks down -Z. Rotate the ray into world space; no translation, the image is at infinity.
    vec3 dir = normalize(mat3(ubuf.viewRotation) * vec3(xv, yv, -1.0));

    vec2 uv;
    float inside = 1.0;
    bool wraps = false;

    if (ubuf.mode.x == PROJ_FLAT) {
        // A virtual screen floating straight ahead. Rays going sideways or backwards miss it.
        float depth = -dir.z;
        vec2 screen = dir.xy / max(depth, 1e-4);
        uv = screen / vec2(ubuf.panoFov.x, -ubuf.panoFov.y) * 0.5 + 0.5;
        inside = (depth > 0.0 && all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) ? 1.0 : 0.0;
    } else {
        // Equirectangular: horizontal angle -> u, vertical angle -> v.
        float az = atan(dir.x, -dir.z);            // 0 straight ahead, +/-PI behind
        float el = asin(clamp(dir.y, -1.0, 1.0));  // +PI/2 straight up

        if (ubuf.mode.x == PROJ_180) {
            // The frame only holds the hemisphere in front of you; everything else stays black
            // instead of smearing the edge columns across the back of your head.
            uv = vec2(az / (2.0 * ubuf.panoFov.x) + 0.5, 0.5 - el / (2.0 * ubuf.panoFov.y));
            inside = (all(greaterThanEqual(uv, vec2(0.0))) && all(lessThanEqual(uv, vec2(1.0)))) ? 1.0 : 0.0;
        } else {
            uv = vec2(az / (2.0 * PI) + 0.5, 0.5 - el / PI);
            wraps = true;
        }
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

    FragColor = vec4(textureGrad(equirectTex, uv, dx, dy).rgb * inside, 1.0);

    // Progress bar: a thin strip near the bottom of the screen, drawn straight in screen
    // space (not projected onto the pano), so it stays flat and legible no matter what
    // projection mode is active or what direction you're looking. mode.y/mode.z aren't used
    // by anything above - reusing them here instead of growing the push-constant struct past
    // the 128 bytes Vulkan guarantees (it's already exactly at that limit).
    float barAlpha = float(ubuf.mode.z) / 255.0;
    if (barAlpha > 0.0 && t > 0.94 && t < 0.98) {
        float progress = float(ubuf.mode.y) / 1000.0;
        vec3 barColor = (s < progress) ? vec3(1.0, 1.0, 1.0) : vec3(0.35, 0.35, 0.35);
        FragColor.rgb = mix(FragColor.rgb, barColor, barAlpha);
    }
}
