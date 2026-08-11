// Copyright (c) 2017-2026 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0
#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

#pragma vertex

layout (location = 0) out vec2 oNdc;
out gl_PerVertex
{
    vec4 gl_Position;
};

void main()
{
    vec2 pos[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    oNdc = pos[gl_VertexIndex];
    // Far plane, not near. This triangle is the 360 background: it must sit behind
    // everything else so that geometry drawn afterwards (the tracked-pose cubes) passes the
    // LESS depth test. Writing 0.0 here filled the depth buffer with the nearest possible
    // value and silently discarded every cube.
    //
    // Just under 1.0 rather than exactly 1.0: the depth buffer is cleared to 1.0 and the
    // compare op is LESS, so a depth of exactly 1.0 would fail against the clear value and
    // this triangle itself would stop drawing.
    gl_Position = vec4(oNdc, 0.9999, 1.0);
}
