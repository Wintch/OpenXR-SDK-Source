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
    gl_Position = vec4(oNdc, 0.0, 1.0);
}
