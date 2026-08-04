// Copyright (c) 2017-2026 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0
#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

#pragma fragment

#define PI 3.14159265359

layout (std140, push_constant) uniform buf
{
    mat4 viewRotation;   // eye orientation: view space -> world space
    vec4 fovTangents;    // tan(angleLeft), tan(angleRight), tan(angleUp), tan(angleDown)
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

    // View space looks down -Z. Rotate the ray into world space; no translation, the photo is at infinity.
    vec3 dir = normalize(mat3(ubuf.viewRotation) * vec3(xv, yv, -1.0));

    vec2 uv = vec2(atan(dir.x, -dir.z) / (2.0 * PI) + 0.5,
                   acos(clamp(dir.y, -1.0, 1.0)) / PI);

    // u wraps from 1 back to 0 at the seam behind the viewer. The implicit derivative there is
    // ~1.0 instead of ~0, which would drive the mip selection to the smallest level and paint a
    // blocky band across the seam - so unwrap the derivatives and select the mip explicitly.
    vec2 dx = dFdx(uv);
    vec2 dy = dFdy(uv);
    dx.x -= round(dx.x);
    dy.x -= round(dy.x);

    FragColor = textureGrad(equirectTex, uv, dx, dy);
}
