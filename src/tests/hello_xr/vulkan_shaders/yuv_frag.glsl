// Copyright (c) 2017-2026 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0
#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

#pragma fragment

// NV12 -> RGB conversion pass for the 360 video path. Runs as a fullscreen triangle
// (shares vert.glsl with the skybox) rendering into level 0 of the skybox texture.
//
// Output is gamma-encoded R'G'B' (Y'CbCr is a gamma-domain encoding), written raw through a
// UNORM view of the sRGB-format target - the skybox pass then samples through the sRGB view,
// which linearizes correctly. Do NOT render this into an sRGB attachment view: the hardware
// would gamma-encode a second time.

layout (std140, push_constant) uniform buf
{
    ivec4 flags;  // x: 1 = full range, 0 = limited (MPEG) range; y: 1 = BT.709, 0 = BT.601
} ubuf;

layout (set = 0, binding = 0) uniform sampler2D texY;
layout (set = 0, binding = 1) uniform sampler2D texUV;

layout (location = 0) in vec2 iNdc;
layout (location = 0) out vec4 FragColor;

void main()
{
    vec2 uv = iNdc * 0.5 + 0.5;
    float y = texture(texY, uv).r;
    vec2 cbcr = texture(texUV, uv).rg - 0.5;

    if (ubuf.flags.x == 0) {  // limited range: Y 16..235, C 16..240
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        cbcr *= 255.0 / 224.0;
    }

    vec3 rgb;
    if (ubuf.flags.y != 0) {  // BT.709
        rgb = vec3(y + 1.5748 * cbcr.y,
                   y - 0.1873 * cbcr.x - 0.4681 * cbcr.y,
                   y + 1.8556 * cbcr.x);
    } else {  // BT.601
        rgb = vec3(y + 1.4020 * cbcr.y,
                   y - 0.3441 * cbcr.x - 0.7141 * cbcr.y,
                   y + 1.7720 * cbcr.x);
    }

    FragColor = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}
