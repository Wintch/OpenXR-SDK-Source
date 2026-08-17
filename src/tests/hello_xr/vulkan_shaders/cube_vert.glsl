// Copyright (c) 2017-2025 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0

#version 400
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

#pragma vertex

// Same push-constant range the 360 pipeline uses (offset 0, size within its 128-byte push
// constant block), so both pipelines can share one VkPipelineLayout - but this stage
// declares its OWN, independent interpretation of those bytes: mvp, then one int right after
// it (see graphicsplugin_vulkan.cpp's cube draw call, which pushes exactly this - mvp plus
// Cube::GizmoAxis - not the 360 viewer's full struct).
layout (std140, push_constant) uniform buf
{
    mat4 mvp;
    int gizmoAxis;  // -1 = off (ordinary per-face Color below). 0/1/2 = controller-gizmo
                     // axis bar (X/Y/Z) - see PushPoseGizmo in openxr_program.cpp.
} ubuf;

layout (location = 0) in vec3 Position;
layout (location = 1) in vec3 Color;

layout (location = 0) out vec4 oColor;
out gl_PerVertex
{
    vec4 gl_Position;
};

void main()
{
    if (ubuf.gizmoAxis >= 0) {
        // Controller-gizmo axis bar: one solid colour per axis (+X red, +Y green, +Z blue),
        // dimmed by the same 0.25 factor Geometry's Dark* colours already use elsewhere, on
        // whichever half of the bar sits on the negative side of ITS OWN local axis - not by
        // face identity, so every face of the bar (end caps AND the four long sides) reads the
        // right colour, not just the two tiny tips. Position is the mesh's local -0.5..+0.5
        // unit-cube coordinate, so Position[gizmoAxis]'s sign is exactly that.
        vec3 axisColor = (ubuf.gizmoAxis == 0) ? vec3(1.0, 0.0, 0.0)
                        : (ubuf.gizmoAxis == 1) ? vec3(0.0, 1.0, 0.0)
                                                 : vec3(0.0, 0.0, 1.0);
        float t = (ubuf.gizmoAxis == 0) ? Position.x : (ubuf.gizmoAxis == 1) ? Position.y : Position.z;
        float bright = (t >= 0.0) ? 1.0 : 0.25;
        oColor.rgb = axisColor * bright;
    } else {
        oColor.rgb = Color.rgb;
    }
    oColor.a = 1.0;
    gl_Position = ubuf.mvp * vec4(Position, 1);
}
