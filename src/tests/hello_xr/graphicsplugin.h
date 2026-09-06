// Copyright (c) 2017-2026 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "swapchain_image_data.h"
#include <nonstd/span.hpp>
using nonstd::span;

// Sentinel values for Cube::GizmoAxis below -1 select a plain solid color, independent of
// the axis-gizmo mechanism (0-5). Added 2026-09-05 for the reverb-g2 room-box v0 (see
// PushRoomBox in openxr_program.cpp): the room's floor/walls need a flat, uncalibrated color
// rather than the default per-face cube coloring or an axis color. Purely additive - see
// cube_vert.glsl for where these are interpreted.
constexpr int32_t kGizmoAxisSolidGray = -2;
constexpr int32_t kGizmoAxisSolidWhite = -3;

struct Cube {
    XrPosef Pose;
    XrVector3f Scale;

    // -1 (default): an ordinary cube, colored per-face like every cube has always been
    // (Geometry::c_cubeVertices). 0/1/2: a controller-gizmo axis bar (X/Y/Z) - see
    // PushPoseGizmo in openxr_program.cpp - rendered as a single solid axis color instead,
    // bright on its own positive half and dim on its negative half (see cube_vert.glsl).
    // 3/4/5 (axis + 3): that same axis's positive-tip marker cube, always full brightness,
    // no light/dark split. kGizmoAxisSolidGray/kGizmoAxisSolidWhite (-2/-3): a flat solid
    // color, no per-face or per-axis variation - see the constants above and cube_vert.glsl.
    // Reference-space cubes and everything else built with the 2-argument Cube{Pose, Scale}
    // form are unaffected: this just defaults to "off".
    int32_t GizmoAxis{-1};
};

// Wraps a graphics API so the main openxr program can be graphics API-independent.
struct IGraphicsPlugin {
    virtual ~IGraphicsPlugin() = default;

    // OpenXR extensions required by this graphics API.
    virtual std::vector<std::string> GetInstanceExtensions() const = 0;

    // Create an instance of this graphics api for the provided instance and systemId.
    virtual void InitializeDevice(XrInstance instance, XrSystemId systemId) = 0;

    // Blocks until the GPU has finished all work submitted so far. Found 2026-08-09:
    // ~OpenXrProgram() used to destroy the swapchain/session/instance with no such wait -
    // usually harmless (the GPU had already caught up by the time cleanup ran), but with
    // real controller-driven quits during a real session the app could reach here with a
    // frame's work still in flight, and destroying command buffers/semaphores the GPU is
    // still using is a validation error, not a no-op - it hung the process outright at
    // least once. Default no-op so any future non-Vulkan backend isn't forced to implement
    // this if it doesn't need the same kind of explicit wait.
    virtual void WaitForGpuIdle() {}

    // Select the preferred swapchain format from the list of available formats.
    virtual int64_t SelectColorSwapchainFormat(bool throwIfNotFound, span<const int64_t> imageFormatArray) const = 0;
    virtual int64_t SelectDepthSwapchainFormat(bool throwIfNotFound, span<const int64_t> imageFormatArray) const = 0;

    // Get the graphics binding header for session creation.
    virtual const XrBaseInStructure* GetGraphicsBinding() const = 0;

    /// Allocates an object owning (among other things) an array of XrSwapchainImage* in a portable way and
    /// returns an **observing** pointer to an interface providing generic access to the associated pointers.
    /// (The object remains owned by the graphics plugin, and will be destroyed on @ref ShutdownDevice())
    /// This is all for the purpose of being able to call the xrEnumerateSwapchainImages function
    /// in a platform-independent way. The user of this must not use the images beyond @ref ShutdownDevice()
    ///
    /// Example usage:
    ///
    /// ```c++
    /// ISwapchainImageData * p = graphicsPlugin->AllocateSwapchainImageData(3, swapchainCreateInfo);
    /// xrEnumerateSwapchainImages(swapchain, 3, &count, p->GetColorImageArray());
    /// ```
    virtual ISwapchainImageData* AllocateSwapchainImageData(size_t size, const XrSwapchainCreateInfo& swapchainCreateInfo) = 0;

    /// Allocates an object owning (among other things) an array of XrSwapchainImage* in a portable way and
    /// returns an **observing** pointer to an interface providing generic access to the associated pointers.
    ///
    /// Signals that we will use a depth swapchain allocated by the runtime, instead of a fallback depth
    /// allocated by the plugin.
    virtual ISwapchainImageData* AllocateSwapchainImageDataWithDepthSwapchain(
        size_t size, const XrSwapchainCreateInfo& colorSwapchainCreateInfo, XrSwapchain depthSwapchain,
        const XrSwapchainCreateInfo& depthSwapchainCreateInfo) = 0;

    // Render to a swapchain image for a projection view.
    virtual void RenderView(const XrCompositionLayerProjectionView& layerView, const XrSwapchainImageBaseHeader* swapchainImage,
                            int64_t swapchainFormat, const std::vector<Cube>& cubes) = 0;

    // Get recommended number of sub-data element samples in view (recommendedSwapchainSampleCount)
    // if supported by the graphics plugin. A supported value otherwise.
    virtual uint32_t GetSupportedSwapchainSampleCount(const XrViewConfigurationView& view) {
        return view.recommendedSwapchainSampleCount;
    }

    // ClearColor depends on the environment (blend mode). We will set this after initialization
    // based on the blend mode selected / available.
    virtual void SetClearColor(const std::array<float, 4> clearColor) = 0;
};

// Graphics API factories are forward declared here.
#ifdef XR_USE_GRAPHICS_API_OPENGL_ES
std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_OpenGLES();
#endif
#ifdef XR_USE_GRAPHICS_API_OPENGL
std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_OpenGL();
#endif
#ifdef XR_USE_GRAPHICS_API_VULKAN
std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_VulkanLegacy();

std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_Vulkan();
#endif
#ifdef XR_USE_GRAPHICS_API_D3D11
std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_D3D11();
#endif
#ifdef XR_USE_GRAPHICS_API_D3D12
std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_D3D12();
#endif
#ifdef XR_USE_GRAPHICS_API_METAL
std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin_Metal();
#endif

// Create a graphics plugin for the graphics API specified in the options.
std::shared_ptr<IGraphicsPlugin> CreateGraphicsPlugin(const std::string graphicsPluginName);
