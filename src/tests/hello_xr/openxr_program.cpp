// Copyright (c) 2017-2026 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "openxr/openxr.h"
#include "pch.h"
#include "common.h"
#include "platformdata.h"
#include "platformplugin.h"
#include "graphicsplugin.h"
#include "swapchain_image_data.h"
#include "openxr_program.h"
#include "playercontrol.h"
#include <common/xr_linear.h>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <set>

namespace {

#if !defined(XR_USE_PLATFORM_WIN32)
#define strcpy_s(dest, source) strncpy((dest), (source), sizeof(dest))
#endif

namespace Side {
const int LEFT = 0;
const int RIGHT = 1;
const int COUNT = 2;
}  // namespace Side

inline std::string GetXrVersionString(XrVersion ver) {
    return Fmt("%d.%d.%d", XR_VERSION_MAJOR(ver), XR_VERSION_MINOR(ver), XR_VERSION_PATCH(ver));
}

namespace Math {
namespace Pose {
static XrPosef Identity() {
    XrPosef t{};
    t.orientation.w = 1;
    return t;
}

static XrPosef Translation(const XrVector3f& translation) {
    XrPosef t = Identity();
    t.position = translation;
    return t;
}

static XrPosef RotateCCWAboutYAxis(float radians, XrVector3f translation) {
    XrPosef t = Identity();
    t.orientation.x = 0.f;
    t.orientation.y = std::sin(radians * 0.5f);
    t.orientation.z = 0.f;
    t.orientation.w = std::cos(radians * 0.5f);
    t.position = translation;
    return t;
}
}  // namespace Pose
}  // namespace Math

inline XrReferenceSpaceCreateInfo GetXrReferenceSpaceCreateInfo(const std::string& referenceSpaceTypeStr) {
    XrReferenceSpaceCreateInfo referenceSpaceCreateInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::Identity();
    if (EqualsIgnoreCase(referenceSpaceTypeStr, "View")) {
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "ViewFront")) {
        // Render head-locked 2m in front of device.
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::Translation({0.f, 0.f, -2.f}),
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "Local")) {
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "Stage")) {
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageLeft")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(0.f, {-2.f, 0.f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageRight")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(0.f, {2.f, 0.f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageLeftRotated")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(3.14f / 3.f, {-2.f, 0.5f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else if (EqualsIgnoreCase(referenceSpaceTypeStr, "StageRightRotated")) {
        referenceSpaceCreateInfo.poseInReferenceSpace = Math::Pose::RotateCCWAboutYAxis(-3.14f / 3.f, {2.f, 0.5f, -2.f});
        referenceSpaceCreateInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    } else {
        throw std::invalid_argument(Fmt("Unknown reference space type '%s'", referenceSpaceTypeStr.c_str()));
    }
    return referenceSpaceCreateInfo;
}

struct OpenXrProgram : IOpenXrProgram {
    OpenXrProgram(const std::shared_ptr<IPlatformPlugin>& platformPlugin, const std::shared_ptr<IGraphicsPlugin>& graphicsPlugin)
        : m_platformPlugin(platformPlugin), m_graphicsPlugin(graphicsPlugin) {}

    ~OpenXrProgram() override {
        // Found 2026-08-09: destroying the swapchain/session/instance below with GPU work
        // from the last rendered frame(s) still in flight is a Vulkan validation error, not
        // a harmless race - it hung the process outright at least once, reproduced live with
        // a real controller-driven quit. Usually the GPU had already caught up by the time a
        // session ended, which is why this went unnoticed for a while.
        if (m_graphicsPlugin) m_graphicsPlugin->WaitForGpuIdle();

        if (m_input.actionSet != XR_NULL_HANDLE) {
            for (auto hand : {Side::LEFT, Side::RIGHT}) {
                xrDestroySpace(m_input.handSpace[hand]);
            }
            xrDestroyActionSet(m_input.actionSet);
        }

        for (Swapchain swapchain : m_swapchains) {
            xrDestroySwapchain(swapchain.handle);
        }

        for (XrSpace visualizedSpace : m_visualizedSpaces) {
            xrDestroySpace(visualizedSpace);
        }

        if (m_appSpace != XR_NULL_HANDLE) {
            xrDestroySpace(m_appSpace);
        }

        if (m_session != XR_NULL_HANDLE) {
            xrDestroySession(m_session);
        }

        if (m_instance != XR_NULL_HANDLE) {
            xrDestroyInstance(m_instance);
        }
    }

    static void LogLayersAndExtensions() {
        // Write out extension properties for a given layer.
        const auto logExtensions = [](const char* layerName, int indent = 0) {
            uint32_t instanceExtensionCount;
            CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(layerName, 0, &instanceExtensionCount, nullptr));
            std::vector<XrExtensionProperties> extensions(instanceExtensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
            CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(layerName, (uint32_t)extensions.size(), &instanceExtensionCount,
                                                               extensions.data()));

            const std::string indentStr(indent, ' ');
            Log::Write(Log::Level::Verbose, Fmt("%sAvailable Extensions: (%d)", indentStr.c_str(), instanceExtensionCount));
            for (const XrExtensionProperties& extension : extensions) {
                Log::Write(Log::Level::Verbose, Fmt("%s  Name=%s SpecVersion=%d", indentStr.c_str(), extension.extensionName,
                                                    extension.extensionVersion));
            }
        };

        // Log non-layer extensions (layerName==nullptr).
        logExtensions(nullptr);

        // Log layers and any of their extensions.
        {
            uint32_t layerCount;
            CHECK_XRCMD(xrEnumerateApiLayerProperties(0, &layerCount, nullptr));
            std::vector<XrApiLayerProperties> layers(layerCount, {XR_TYPE_API_LAYER_PROPERTIES});
            CHECK_XRCMD(xrEnumerateApiLayerProperties((uint32_t)layers.size(), &layerCount, layers.data()));

            Log::Write(Log::Level::Info, Fmt("Available Layers: (%d)", layerCount));
            for (const XrApiLayerProperties& layer : layers) {
                Log::Write(Log::Level::Verbose,
                           Fmt("  Name=%s SpecVersion=%s LayerVersion=%d Description=%s", layer.layerName,
                               GetXrVersionString(layer.specVersion).c_str(), layer.layerVersion, layer.description));
                logExtensions(layer.layerName, 4);
            }
        }
    }

    void LogInstanceInfo() {
        CHECK(m_instance != XR_NULL_HANDLE);

        XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
        CHECK_XRCMD(xrGetInstanceProperties(m_instance, &instanceProperties));

        Log::Write(Log::Level::Info, Fmt("Instance RuntimeName=%s RuntimeVersion=%s", instanceProperties.runtimeName,
                                         GetXrVersionString(instanceProperties.runtimeVersion).c_str()));
    }

    void CreateInstanceInternal() {
        CHECK(m_instance == XR_NULL_HANDLE);

        // Create union of extensions required by platform and graphics plugins.
        std::vector<const char*> extensions;

        // Transform platform and graphics extension std::strings to C strings.
        const std::vector<std::string> platformExtensions = m_platformPlugin->GetInstanceExtensions();
        std::transform(platformExtensions.begin(), platformExtensions.end(), std::back_inserter(extensions),
                       [](const std::string& ext) { return ext.c_str(); });
        const std::vector<std::string> graphicsExtensions = m_graphicsPlugin->GetInstanceExtensions();
        std::transform(graphicsExtensions.begin(), graphicsExtensions.end(), std::back_inserter(extensions),
                       [](const std::string& ext) { return ext.c_str(); });

        uint32_t instanceExtensionCount;
        CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(nullptr, 0, &instanceExtensionCount, nullptr));
        std::vector<XrExtensionProperties> extensionProperties =
            std::vector<XrExtensionProperties>(instanceExtensionCount, {XR_TYPE_EXTENSION_PROPERTIES});
        CHECK_XRCMD(xrEnumerateInstanceExtensionProperties(nullptr, (uint32_t)extensionProperties.size(), &instanceExtensionCount,
                                                           extensionProperties.data()));

        // enable depth extension if supported
        auto depthExtensionProperties =
            std::find_if(extensionProperties.begin(), extensionProperties.end(), [](const XrExtensionProperties& item) {
                return 0 == strcmp(item.extensionName, XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
            });
        if (depthExtensionProperties != extensionProperties.end()) {
            Log::Write(Log::Level::Info, Fmt("Depth submission supported (%s)", depthExtensionProperties->extensionName));
            extensions.push_back(depthExtensionProperties->extensionName);
            m_supportsDepthLayer = true;
        } else {
            Log::Write(Log::Level::Info, Fmt("Depth submission NOT supported (%s)", XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME));
        }

        XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
        createInfo.next = m_platformPlugin->GetInstanceCreateExtension();
        createInfo.enabledExtensionCount = (uint32_t)extensions.size();
        createInfo.enabledExtensionNames = extensions.data();

        strcpy(createInfo.applicationInfo.applicationName, "HelloXR");

        // Current version is 1.1.x, but hello_xr only requires 1.0.x
        createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;

        CHECK_XRCMD(xrCreateInstance(&createInfo, &m_instance));
    }

    void CreateInstance() override {
        LogLayersAndExtensions();

        CreateInstanceInternal();

        LogInstanceInfo();
    }

    void LogViewConfigurations() {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId != XR_NULL_SYSTEM_ID);

        uint32_t viewConfigTypeCount;
        CHECK_XRCMD(xrEnumerateViewConfigurations(m_instance, m_systemId, 0, &viewConfigTypeCount, nullptr));
        std::vector<XrViewConfigurationType> viewConfigTypes(viewConfigTypeCount);
        CHECK_XRCMD(xrEnumerateViewConfigurations(m_instance, m_systemId, viewConfigTypeCount, &viewConfigTypeCount,
                                                  viewConfigTypes.data()));
        CHECK((uint32_t)viewConfigTypes.size() == viewConfigTypeCount);

        Log::Write(Log::Level::Info, Fmt("Available View Configuration Types: (%d)", viewConfigTypeCount));
        for (XrViewConfigurationType viewConfigType : viewConfigTypes) {
            Log::Write(Log::Level::Verbose, Fmt("  View Configuration Type: %s %s", to_string(viewConfigType),
                                                viewConfigType == m_viewConfigType ? "(Selected)" : ""));

            XrViewConfigurationProperties viewConfigProperties{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
            CHECK_XRCMD(xrGetViewConfigurationProperties(m_instance, m_systemId, viewConfigType, &viewConfigProperties));

            Log::Write(Log::Level::Verbose,
                       Fmt("  View configuration FovMutable=%s", viewConfigProperties.fovMutable == XR_TRUE ? "True" : "False"));

            uint32_t viewCount;
            CHECK_XRCMD(xrEnumerateViewConfigurationViews(m_instance, m_systemId, viewConfigType, 0, &viewCount, nullptr));
            if (viewCount > 0) {
                std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
                CHECK_XRCMD(
                    xrEnumerateViewConfigurationViews(m_instance, m_systemId, viewConfigType, viewCount, &viewCount, views.data()));

                for (uint32_t i = 0; i < views.size(); i++) {
                    const XrViewConfigurationView& view = views[i];

                    Log::Write(Log::Level::Verbose, Fmt("    View [%d]: Recommended Width=%d Height=%d SampleCount=%d", i,
                                                        view.recommendedImageRectWidth, view.recommendedImageRectHeight,
                                                        view.recommendedSwapchainSampleCount));
                    Log::Write(Log::Level::Verbose,
                               Fmt("    View [%d]:     Maximum Width=%d Height=%d SampleCount=%d", i, view.maxImageRectWidth,
                                   view.maxImageRectHeight, view.maxSwapchainSampleCount));
                }
            } else {
                Log::Write(Log::Level::Error, Fmt("Empty view configuration type"));
            }

            LogEnvironmentBlendMode(viewConfigType);
        }
    }

    void LogEnvironmentBlendMode(XrViewConfigurationType type) {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId != 0);

        uint32_t count;
        CHECK_XRCMD(xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, type, 0, &count, nullptr));
        CHECK(count > 0);

        Log::Write(Log::Level::Info, Fmt("  Available Environment Blend Mode count : (%d)", count));

        std::vector<XrEnvironmentBlendMode> blendModes(count);
        CHECK_XRCMD(xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, type, count, &count, blendModes.data()));

        for (XrEnvironmentBlendMode mode : blendModes) {
            const bool blendModeMatch = (mode == m_blendMode);
            Log::Write(Log::Level::Info,
                       Fmt("    Environment Blend Mode (%s) : %s", to_string(mode), blendModeMatch ? "(Selected)" : ""));
        }
    }

    std::array<float, 4> GetBackgroundClearColor() const override {
        // The 360/VR180/flat skybox shader now discards pixels outside the actual content
        // (see frag.glsl) instead of painting them black, so this color is what shows in that
        // empty space - previously it was set but never visible, since the shader used to
        // cover every pixel regardless. HELLO_XR_THEME picks it: "night" (or the older "void")
        // is the original black look, "daylight" is a plain medium grey (added because pure
        // black outside the frame reads as "did tracking break?" rather than "empty" - but
        // found 2026-08-09, VR180 content: that grey covers the entire back 180 degrees, and
        // the user's call was that of the two options, all-black reads better than all-grey
        // there - so black is the default now; pass HELLO_XR_THEME=daylight to get the grey
        // back).
        static const std::array<float, 4> Night{{0.0f, 0.0f, 0.0f, 1.0f}};
        static const std::array<float, 4> Daylight{{0.5f, 0.5f, 0.5f, 1.0f}};
        static const std::array<float, 4> TransparentBlack{{0.0f, 0.0f, 0.0f, 0.0f}};
        static const std::array<float, 4> Black{{0.0f, 0.0f, 0.0f, 1.0f}};

        switch (m_blendMode) {
            case XR_ENVIRONMENT_BLEND_MODE_OPAQUE: {
                const char* theme = getenv("HELLO_XR_THEME");
                const bool daylight = theme != nullptr && strcmp(theme, "daylight") == 0;
                return daylight ? Daylight : Night;
            }
            case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:
                return Black;
            case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND:
                return TransparentBlack;
            default:
                return Daylight;
        }
    }

    void InitializeSystem(XrFormFactor formFactor, XrViewConfigurationType viewConfigType, bool ebmOverride,
                          XrEnvironmentBlendMode ebm) override {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId == XR_NULL_SYSTEM_ID);

        XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
        systemInfo.formFactor = formFactor;
        CHECK_XRCMD(xrGetSystem(m_instance, &systemInfo, &m_systemId));

        Log::Write(Log::Level::Verbose, Fmt("Using system %d for form factor %s", m_systemId, to_string(formFactor)));
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_systemId != XR_NULL_SYSTEM_ID);

        {
            // Note: If this condition is not met, the project will need to be audited
            // to see how support should be added.
            CHECK_MSG(viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MONO ||
                          viewConfigType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                      "Unsupported view configuration type");

            m_viewConfigType = viewConfigType;
        }

        {
            uint32_t count;
            CHECK_XRCMD(xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, m_viewConfigType, 0, &count, nullptr));
            CHECK(count > 0);
            std::vector<XrEnvironmentBlendMode> blendModes(count);
            CHECK_XRCMD(
                xrEnumerateEnvironmentBlendModes(m_instance, m_systemId, m_viewConfigType, count, &count, blendModes.data()));
            if (ebmOverride) {
                if (std::find(blendModes.begin(), blendModes.end(), ebm) == blendModes.end()) {
                    THROW("Selected blendmode is not available from runtime");
                }
                m_blendMode = ebm;
            } else {
                // Runtimes return blend modes in preference order
                m_blendMode = blendModes[0];
            }
        }
    }

    void InitializeDevice() override {
        LogViewConfigurations();

        // The graphics API can initialize the graphics device now that the systemId and instance
        // handle are available.
        m_graphicsPlugin->InitializeDevice(m_instance, m_systemId);
    }

    void LogReferenceSpaces() {
        CHECK(m_session != XR_NULL_HANDLE);

        uint32_t spaceCount;
        CHECK_XRCMD(xrEnumerateReferenceSpaces(m_session, 0, &spaceCount, nullptr));
        std::vector<XrReferenceSpaceType> spaces(spaceCount);
        CHECK_XRCMD(xrEnumerateReferenceSpaces(m_session, spaceCount, &spaceCount, spaces.data()));

        Log::Write(Log::Level::Info, Fmt("Available reference spaces: %d", spaceCount));
        for (XrReferenceSpaceType space : spaces) {
            Log::Write(Log::Level::Verbose, Fmt("  Name: %s", to_string(space)));
        }
    }

    struct InputState {
        XrActionSet actionSet{XR_NULL_HANDLE};
        XrAction grabAction{XR_NULL_HANDLE};
        XrAction poseAction{XR_NULL_HANDLE};
        XrAction vibrateAction{XR_NULL_HANDLE};
        XrAction quitAction{XR_NULL_HANDLE};
        // Thumbstick X axis - seeks the 360/VR180 video player. Only bound on
        // oculus/touch_controller (what the G2 actually presents, see docs/03-controllers.md)
        // and microsoft/motion_controller: those are the two profiles bound by hand against
        // real hardware, and guessing paths for profiles we cannot verify is how
        // silently-wrong bindings happen.
        XrAction seekAction{XR_NULL_HANDLE};
        // Thumbstick Y axis, same two profiles and same rationale as seekAction - the vertical
        // axis was otherwise unused, so it drives digital zoom instead of adding a new button.
        XrAction zoomAction{XR_NULL_HANDLE};
        // Trigger value, same two profiles - toggles video pause. Same rationale as
        // seekAction: only bound on profiles we have real hardware to verify against.
        XrAction pauseAction{XR_NULL_HANDLE};
        // Squeeze/grip click, WMR motion controller only - recenters forward. grabAction is
        // also bound to this same physical button (from the original hello_xr sample, scales a
        // hand-cube visual), but graphicsplugin_vulkan.cpp's RenderView never actually draws
        // that cube - so the button was doing nothing in this player until now.
        XrAction recenterAction{XR_NULL_HANDLE};
        // A/B click, oculus/touch_controller ONLY, right hand only - A/B don't exist on the
        // left Touch controller (that's X/Y there) or on microsoft/motion_controller at all,
        // and both sticks are already spoken for (seek, zoom), so these are the next free
        // physical inputs. Brightness on the displayed content.
        XrAction brightnessUpAction{XR_NULL_HANDLE};
        XrAction brightnessDownAction{XR_NULL_HANDLE};
        // Y click, oculus/touch_controller ONLY, left hand only - Y is the left-hand mirror
        // of A/B (which are right-hand-only), and was the last free real input. Next track in
        // a directory playlist - previously keyboard-only ('n'), useless with the headset on.
        XrAction nextTrackAction{XR_NULL_HANDLE};
        // X click, same profile/hand as Y above - the other left-hand face button. Previous
        // track, the direction Y doesn't cover.
        XrAction prevTrackAction{XR_NULL_HANDLE};
        std::array<XrPath, Side::COUNT> handSubactionPath;
        std::array<XrSpace, Side::COUNT> handSpace;
        std::array<float, Side::COUNT> handScale = {{1.0f, 1.0f}};
        std::array<XrBool32, Side::COUNT> handActive;
    };

    void InitializeActions() {
        // Create an action set.
        {
            XrActionSetCreateInfo actionSetInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
            strcpy_s(actionSetInfo.actionSetName, "gameplay");
            strcpy_s(actionSetInfo.localizedActionSetName, "Gameplay");
            actionSetInfo.priority = 0;
            CHECK_XRCMD(xrCreateActionSet(m_instance, &actionSetInfo, &m_input.actionSet));
        }

        // Get the XrPath for the left and right hands - we will use them as subaction paths.
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left", &m_input.handSubactionPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right", &m_input.handSubactionPath[Side::RIGHT]));

        // Create actions.
        {
            // Create an input action for grabbing objects with the left and right hands.
            XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
            actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
            strcpy_s(actionInfo.actionName, "grab_object");
            strcpy_s(actionInfo.localizedActionName, "Grab Object");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.grabAction));

            // Create an input action getting the left and right hand poses.
            actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
            strcpy_s(actionInfo.actionName, "hand_pose");
            strcpy_s(actionInfo.localizedActionName, "Hand Pose");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.poseAction));

            // Create output actions for vibrating the left and right controller.
            actionInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
            strcpy_s(actionInfo.actionName, "vibrate_hand");
            strcpy_s(actionInfo.localizedActionName, "Vibrate Hand");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.vibrateAction));

            // Create input actions for quitting the session using the left and right controller.
            // Since it doesn't matter which hand did this, we do not specify subaction paths for it.
            // We will just suggest bindings for both hands, where possible.
            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "quit_session");
            strcpy_s(actionInfo.localizedActionName, "Quit Session");
            actionInfo.countSubactionPaths = 0;
            actionInfo.subactionPaths = nullptr;
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.quitAction));

            // Thumbstick X for video seek (see InputState::seekAction).
            actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
            strcpy_s(actionInfo.actionName, "seek_video");
            strcpy_s(actionInfo.localizedActionName, "Seek Video");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.seekAction));

            // Thumbstick Y for video zoom (see InputState::zoomAction).
            actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
            strcpy_s(actionInfo.actionName, "zoom_video");
            strcpy_s(actionInfo.localizedActionName, "Zoom Video");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.zoomAction));

            // Trigger for video pause/resume (see InputState::pauseAction) - a quick
            // debug-friendly toggle so playback state can be flipped without reaching for the
            // keyboard.
            actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
            strcpy_s(actionInfo.actionName, "pause_video");
            strcpy_s(actionInfo.localizedActionName, "Pause Video");
            actionInfo.countSubactionPaths = uint32_t(m_input.handSubactionPath.size());
            actionInfo.subactionPaths = m_input.handSubactionPath.data();
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.pauseAction));

            // Squeeze/grip click for recenter (WMR only, see InputState::recenterAction). No
            // subaction paths - like quitAction, we don't care which hand did it.
            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "recenter_video");
            strcpy_s(actionInfo.localizedActionName, "Recenter Video");
            actionInfo.countSubactionPaths = 0;
            actionInfo.subactionPaths = nullptr;
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.recenterAction));

            // A/B click for brightness (see InputState::brightnessUpAction/DownAction) - right
            // hand only, no subaction paths needed since there's only one valid source.
            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "brightness_up");
            strcpy_s(actionInfo.localizedActionName, "Brightness Up");
            actionInfo.countSubactionPaths = 0;
            actionInfo.subactionPaths = nullptr;
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.brightnessUpAction));

            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "brightness_down");
            strcpy_s(actionInfo.localizedActionName, "Brightness Down");
            actionInfo.countSubactionPaths = 0;
            actionInfo.subactionPaths = nullptr;
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.brightnessDownAction));

            // Y click for next track (see InputState::nextTrackAction) - left hand only.
            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "next_track");
            strcpy_s(actionInfo.localizedActionName, "Next Track");
            actionInfo.countSubactionPaths = 0;
            actionInfo.subactionPaths = nullptr;
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.nextTrackAction));

            // X click for previous track (see InputState::prevTrackAction) - left hand only.
            actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
            strcpy_s(actionInfo.actionName, "prev_track");
            strcpy_s(actionInfo.localizedActionName, "Previous Track");
            actionInfo.countSubactionPaths = 0;
            actionInfo.subactionPaths = nullptr;
            CHECK_XRCMD(xrCreateAction(m_input.actionSet, &actionInfo, &m_input.prevTrackAction));
        }

        std::array<XrPath, Side::COUNT> selectPath;
        std::array<XrPath, Side::COUNT> squeezeValuePath;
        std::array<XrPath, Side::COUNT> squeezeForcePath;
        std::array<XrPath, Side::COUNT> squeezeClickPath;
        std::array<XrPath, Side::COUNT> posePath;
        std::array<XrPath, Side::COUNT> hapticPath;
        std::array<XrPath, Side::COUNT> menuClickPath;
        std::array<XrPath, Side::COUNT> bClickPath;
        std::array<XrPath, Side::COUNT> aClickPath;
        std::array<XrPath, Side::COUNT> yClickPath;
        std::array<XrPath, Side::COUNT> xClickPath;
        std::array<XrPath, Side::COUNT> triggerValuePath;
        std::array<XrPath, Side::COUNT> thumbstickXPath;
        std::array<XrPath, Side::COUNT> thumbstickYPath;
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/select/click", &selectPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/select/click", &selectPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/squeeze/value", &squeezeValuePath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/squeeze/value", &squeezeValuePath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/squeeze/force", &squeezeForcePath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/squeeze/force", &squeezeForcePath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/squeeze/click", &squeezeClickPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/squeeze/click", &squeezeClickPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/grip/pose", &posePath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/grip/pose", &posePath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/output/haptic", &hapticPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/output/haptic", &hapticPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/menu/click", &menuClickPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/menu/click", &menuClickPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/b/click", &bClickPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/b/click", &bClickPath[Side::RIGHT]));
        // a/click only exists on the right hand on any profile in this file (Touch's left
        // controller has x/y, not a/b) - not bothering to resolve the left path at all.
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/a/click", &aClickPath[Side::RIGHT]));
        // Mirror of the above: y/click only exists on the left hand.
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/y/click", &yClickPath[Side::LEFT]));
        // Same story for x/click - left hand only.
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/x/click", &xClickPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/trigger/value", &triggerValuePath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/trigger/value", &triggerValuePath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/thumbstick/x", &thumbstickXPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/thumbstick/x", &thumbstickXPath[Side::RIGHT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/left/input/thumbstick/y", &thumbstickYPath[Side::LEFT]));
        CHECK_XRCMD(xrStringToPath(m_instance, "/user/hand/right/input/thumbstick/y", &thumbstickYPath[Side::RIGHT]));
        // Suggest bindings for KHR Simple.
        {
            XrPath khrSimpleInteractionProfilePath;
            CHECK_XRCMD(
                xrStringToPath(m_instance, "/interaction_profiles/khr/simple_controller", &khrSimpleInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{// Fall back to a click input for the grab action.
                                                            {m_input.grabAction, selectPath[Side::LEFT]},
                                                            {m_input.grabAction, selectPath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.quitAction, menuClickPath[Side::RIGHT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = khrSimpleInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }
        // Suggest bindings for the Oculus Touch.
        {
            XrPath oculusTouchInteractionProfilePath;
            CHECK_XRCMD(
                xrStringToPath(m_instance, "/interaction_profiles/oculus/touch_controller", &oculusTouchInteractionProfilePath));
            // The G2's controllers land on THIS profile, not microsoft/motion_controller:
            // Monado's G2 driver remaps itself to oculus/touch (it has X/Y/A/B buttons the WMR
            // profile can't express), so the player bindings must live here too. Notes: menu is
            // left-hand-only on this profile (suggesting it for the right hand fails the whole
            // call), and recenterAction is a boolean bound to the float squeeze/value - legal
            // per spec, the runtime thresholds it (Monado: 0.7).
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, squeezeValuePath[Side::LEFT]},
                                                            {m_input.grabAction, squeezeValuePath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]},
                                                            {m_input.seekAction, thumbstickXPath[Side::LEFT]},
                                                            {m_input.seekAction, thumbstickXPath[Side::RIGHT]},
                                                            {m_input.zoomAction, thumbstickYPath[Side::LEFT]},
                                                            {m_input.zoomAction, thumbstickYPath[Side::RIGHT]},
                                                            {m_input.pauseAction, triggerValuePath[Side::RIGHT]},
                                                            {m_input.recenterAction, squeezeValuePath[Side::LEFT]},
                                                            {m_input.recenterAction, squeezeValuePath[Side::RIGHT]},
                                                            {m_input.brightnessUpAction, aClickPath[Side::RIGHT]},
                                                            {m_input.brightnessDownAction, bClickPath[Side::RIGHT]},
                                                            {m_input.nextTrackAction, yClickPath[Side::LEFT]},
                                                            {m_input.prevTrackAction, xClickPath[Side::LEFT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = oculusTouchInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }
        // Suggest bindings for the Vive Controller.
        {
            XrPath viveControllerInteractionProfilePath;
            CHECK_XRCMD(
                xrStringToPath(m_instance, "/interaction_profiles/htc/vive_controller", &viveControllerInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, triggerValuePath[Side::LEFT]},
                                                            {m_input.grabAction, triggerValuePath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.quitAction, menuClickPath[Side::RIGHT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = viveControllerInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }

        // Suggest bindings for the Valve Index Controller.
        {
            XrPath indexControllerInteractionProfilePath;
            CHECK_XRCMD(
                xrStringToPath(m_instance, "/interaction_profiles/valve/index_controller", &indexControllerInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, squeezeForcePath[Side::LEFT]},
                                                            {m_input.grabAction, squeezeForcePath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, bClickPath[Side::LEFT]},
                                                            {m_input.quitAction, bClickPath[Side::RIGHT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = indexControllerInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }

        // Suggest bindings for the Microsoft Mixed Reality Motion Controller.
        {
            XrPath microsoftMixedRealityInteractionProfilePath;
            CHECK_XRCMD(xrStringToPath(m_instance, "/interaction_profiles/microsoft/motion_controller",
                                       &microsoftMixedRealityInteractionProfilePath));
            std::vector<XrActionSuggestedBinding> bindings{{{m_input.grabAction, squeezeClickPath[Side::LEFT]},
                                                            {m_input.grabAction, squeezeClickPath[Side::RIGHT]},
                                                            {m_input.poseAction, posePath[Side::LEFT]},
                                                            {m_input.poseAction, posePath[Side::RIGHT]},
                                                            {m_input.quitAction, menuClickPath[Side::LEFT]},
                                                            {m_input.quitAction, menuClickPath[Side::RIGHT]},
                                                            {m_input.vibrateAction, hapticPath[Side::LEFT]},
                                                            {m_input.vibrateAction, hapticPath[Side::RIGHT]},
                                                            {m_input.seekAction, thumbstickXPath[Side::LEFT]},
                                                            {m_input.seekAction, thumbstickXPath[Side::RIGHT]},
                                                            {m_input.zoomAction, thumbstickYPath[Side::LEFT]},
                                                            {m_input.zoomAction, thumbstickYPath[Side::RIGHT]},
                                                            {m_input.pauseAction, triggerValuePath[Side::LEFT]},
                                                            {m_input.pauseAction, triggerValuePath[Side::RIGHT]},
                                                            {m_input.recenterAction, squeezeClickPath[Side::LEFT]},
                                                            {m_input.recenterAction, squeezeClickPath[Side::RIGHT]}}};
            XrInteractionProfileSuggestedBinding suggestedBindings{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
            suggestedBindings.interactionProfile = microsoftMixedRealityInteractionProfilePath;
            suggestedBindings.suggestedBindings = bindings.data();
            suggestedBindings.countSuggestedBindings = (uint32_t)bindings.size();
            CHECK_XRCMD(xrSuggestInteractionProfileBindings(m_instance, &suggestedBindings));
        }
        XrActionSpaceCreateInfo actionSpaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        actionSpaceInfo.action = m_input.poseAction;
        actionSpaceInfo.poseInActionSpace.orientation.w = 1.f;
        actionSpaceInfo.subactionPath = m_input.handSubactionPath[Side::LEFT];
        CHECK_XRCMD(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_input.handSpace[Side::LEFT]));
        actionSpaceInfo.subactionPath = m_input.handSubactionPath[Side::RIGHT];
        CHECK_XRCMD(xrCreateActionSpace(m_session, &actionSpaceInfo, &m_input.handSpace[Side::RIGHT]));

        XrSessionActionSetsAttachInfo attachInfo{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
        attachInfo.countActionSets = 1;
        attachInfo.actionSets = &m_input.actionSet;
        CHECK_XRCMD(xrAttachSessionActionSets(m_session, &attachInfo));
    }


    /*!
     * Push a three-axis gizmo for a tracked pose instead of a single cube.
     *
     * A cube is symmetric, so it shows position but says nothing about which way the device is
     * pointing -- useless for checking whether an orientation is right, and easy to misread once
     * positional tracking starts moving things around. Three thin bars, one per axis, make the
     * frame unambiguous: X to the device's right, Y up, Z backward (OpenXR's -Z is forward, so
     * the Z bar points back out of the muzzle).
     *
     * Each bar is centered ON the pose and spans it symmetrically (half in its axis's own +
     * direction, half in -), so all three still cross exactly at the pose origin -- and now the
     * +/- split is real geometry, not just an arbitrary "arm". Found 2026-08-17 (T206, the
     * wearer live-debugging orientation): the previous "tell them apart by direction" via the
     * cube mesh's own per-face colours failed in practice -- a bar's two tiny end caps carried
     * the right colour, but its four long side faces (the vast majority of what's visible from
     * any angle) showed whatever OTHER axis happened to own that face, so every bar read as a
     * confusing multi-colour smear. Now each bar is a single solid axis colour on every face
     * (+X red, +Y green, +Z blue, dim on the negative half) -- see cube_vert.glsl, which derives
     * the colour from the vertex's own local position along the bar's axis instead of from the
     * mesh's baked per-face colours. Cube::GizmoAxis (0/1/2) is what tells cube_vert.glsl this
     * is a gizmo bar and which axis it is; -1 (the default for every other cube, reference
     * spaces included) keeps the original per-face colouring untouched.
     *
     * Tip cubes, added same day per the wearer's own live request ("metele un cubo en una punta
     * de cada eje"): one small solid cube capping the POSITIVE end of each bar only, slightly
     * larger than the bar's cross-section. Nothing marks the negative end. The asymmetry is the
     * point -- it makes handedness readable at a glance (three colour-coded caps, not a
     * symmetric jack), and because the three caps move as a rigid unit with the bars, an
     * unexpected pivot offset (rotation happening around a point that visibly isn't where the
     * caps converge) becomes obvious too. Encoded as GizmoAxis 3/4/5 (bar axis + 3) rather than
     * reusing 0/1/2 directly: a plain cube's own local vertices split -0.5..+0.5 on EVERY axis
     * regardless of where the cube is placed in the world, so naively tagging a tip cube with
     * the bar's own axis value would render it half-bright/half-dim like a tiny bar segment, not
     * the solid marker asked for -- cube_vert.glsl's `gizmoAxis >= 3` case forces full brightness
     * unconditionally instead of testing the vertex's local sign.
     */
    static void PushPoseGizmo(std::vector<Cube>& cubes, const XrPosef& pose, float scale) {
        constexpr float kLength = 0.12f;
        constexpr float kThick = 0.012f;
        // ~1.75x the bar's own cross-section (matches the wearer's own "3.5cm cube on a 2cm
        // bar" example ratio) -- big enough to read as a deliberate marker, not just a thicker
        // bit of bar.
        constexpr float kTipCube = kThick * 1.75f;
        const float len = kLength * scale;
        const float thick = kThick * scale;
        const float tipCube = kTipCube * scale;

        const XrVector3f sizes[3] = {{len, thick, thick}, {thick, len, thick}, {thick, thick, len}};
        const XrVector3f tipOffsetLocal[3] = {{len * 0.5f, 0.f, 0.f}, {0.f, len * 0.5f, 0.f}, {0.f, 0.f, len * 0.5f}};
        const XrVector3f tipScale{tipCube, tipCube, tipCube};

        for (int axis = 0; axis < 3; axis++) {
            cubes.push_back(Cube{pose, sizes[axis], axis});

            // Tip cube, centered exactly at the bar's positive end (the same point local
            // Position[axis] == +0.5 on the bar maps to), so it caps the bar rather than
            // floating past or short of it.
            XrVector3f tipOffsetWorld;
            XrQuaternionf_RotateVector3f(&tipOffsetWorld, &pose.orientation, &tipOffsetLocal[axis]);
            XrPosef tipPose = pose;
            tipPose.position.x += tipOffsetWorld.x;
            tipPose.position.y += tipOffsetWorld.y;
            tipPose.position.z += tipOffsetWorld.z;
            cubes.push_back(Cube{tipPose, tipScale, axis + 3});
        }
    }

    // Synthetic floor reference (reverb-g2, 2026-09-05, docs/08 v0 passthrough): raw camera
    // passthrough has no stable visual anchor the way a rendered game does (a floor/horizon the
    // wearer can use to judge distance and orientation while walking) -- a live wearer confirmed
    // this exact gap: "anda bien [el video]. Pero sin piso como geometria." Draws a plain line
    // grid at the Stage origin (floor level by OpenXR convention), reusing the existing cube
    // pipeline as thin boxes rather than a new shader/pipeline. Half-extent matches this
    // project's SLAM_SESSION_ANCHOR_RADIUS_CM convention (3m) purely for a familiar scale, not
    // because the two are otherwise related.
    static void PushFloorGrid(std::vector<Cube>& cubes, const XrPosef& stagePose) {
        constexpr float kHalfExtent = 3.0f;
        constexpr float kSpacing = 1.0f;
        constexpr float kLineWidth = 0.015f;
        constexpr float kLineHeight = 0.005f;
        const int lines = (int)(kHalfExtent / kSpacing);

        for (int i = -lines; i <= lines; i++) {
            const float offset = i * kSpacing;

            // Line running along local X, at local Z = offset.
            XrVector3f zLineOffsetLocal{0.f, 0.f, offset};
            XrVector3f zLineOffsetWorld;
            XrQuaternionf_RotateVector3f(&zLineOffsetWorld, &stagePose.orientation, &zLineOffsetLocal);
            XrPosef zLinePose = stagePose;
            zLinePose.position.x += zLineOffsetWorld.x;
            zLinePose.position.y += zLineOffsetWorld.y;
            zLinePose.position.z += zLineOffsetWorld.z;
            cubes.push_back(Cube{zLinePose, {kHalfExtent * 2.f, kLineHeight, kLineWidth}});

            // Line running along local Z, at local X = offset.
            XrVector3f xLineOffsetLocal{offset, 0.f, 0.f};
            XrVector3f xLineOffsetWorld;
            XrQuaternionf_RotateVector3f(&xLineOffsetWorld, &stagePose.orientation, &xLineOffsetLocal);
            XrPosef xLinePose = stagePose;
            xLinePose.position.x += xLineOffsetWorld.x;
            xLinePose.position.y += xLineOffsetWorld.y;
            xLinePose.position.z += xLineOffsetWorld.z;
            cubes.push_back(Cube{xLinePose, {kLineWidth, kLineHeight, kHalfExtent * 2.f}});
        }
    }

    void CreateVisualizedSpaces() {
        CHECK(m_session != XR_NULL_HANDLE);

        std::string visualizedSpaces[] = {"ViewFront",        "Local", "Stage", "StageLeft", "StageRight", "StageLeftRotated",
                                          "StageRightRotated"};

        for (const auto& visualizedSpace : visualizedSpaces) {
            XrReferenceSpaceCreateInfo referenceSpaceCreateInfo = GetXrReferenceSpaceCreateInfo(visualizedSpace);
            XrSpace space;
            XrResult res = xrCreateReferenceSpace(m_session, &referenceSpaceCreateInfo, &space);
            if (XR_SUCCEEDED(res)) {
                m_visualizedSpaces.push_back(space);
                if (visualizedSpace == "Stage") {
                    m_stageSpaceForFloorGrid = space;
                }
            } else {
                Log::Write(Log::Level::Warning,
                           Fmt("Failed to create reference space %s with error %d", visualizedSpace.c_str(), res));
            }
        }
    }

    void InitializeSession(std::string appSpace) override {
        CHECK(m_instance != XR_NULL_HANDLE);
        CHECK(m_session == XR_NULL_HANDLE);

        {
            Log::Write(Log::Level::Verbose, Fmt("Creating session..."));

            XrSessionCreateInfo createInfo{XR_TYPE_SESSION_CREATE_INFO};
            createInfo.next = m_graphicsPlugin->GetGraphicsBinding();
            createInfo.systemId = m_systemId;
            CHECK_XRCMD(xrCreateSession(m_instance, &createInfo, &m_session));
        }

        LogReferenceSpaces();
        InitializeActions();
        CreateVisualizedSpaces();

        {
            XrReferenceSpaceCreateInfo referenceSpaceCreateInfo = GetXrReferenceSpaceCreateInfo(appSpace);
            CHECK_XRCMD(xrCreateReferenceSpace(m_session, &referenceSpaceCreateInfo, &m_appSpace));
        }
    }

    void CreateSwapchains() override {
        CHECK(m_session != XR_NULL_HANDLE);
        CHECK(m_swapchains.empty());
        CHECK(m_configViews.empty());

        // Read graphics properties for preferred swapchain length and logging.
        XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
        CHECK_XRCMD(xrGetSystemProperties(m_instance, m_systemId, &systemProperties));

        // Log system properties.
        Log::Write(Log::Level::Info,
                   Fmt("System Properties: Name=%s VendorId=%d", systemProperties.systemName, systemProperties.vendorId));
        Log::Write(Log::Level::Info, Fmt("System Graphics Properties: MaxWidth=%d MaxHeight=%d MaxLayers=%d",
                                         systemProperties.graphicsProperties.maxSwapchainImageWidth,
                                         systemProperties.graphicsProperties.maxSwapchainImageHeight,
                                         systemProperties.graphicsProperties.maxLayerCount));
        Log::Write(Log::Level::Info, Fmt("System Tracking Properties: OrientationTracking=%s PositionTracking=%s",
                                         systemProperties.trackingProperties.orientationTracking == XR_TRUE ? "True" : "False",
                                         systemProperties.trackingProperties.positionTracking == XR_TRUE ? "True" : "False"));

        // Query and cache view configuration views.
        uint32_t viewCount;
        CHECK_XRCMD(xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigType, 0, &viewCount, nullptr));
        m_configViews.resize(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
        CHECK_XRCMD(xrEnumerateViewConfigurationViews(m_instance, m_systemId, m_viewConfigType, viewCount, &viewCount,
                                                      m_configViews.data()));

        // Create and cache view buffer for xrLocateViews later.
        m_views.resize(viewCount, {XR_TYPE_VIEW});

        // Create the swapchain and get the images.
        if (viewCount > 0) {
            // Select a swapchain format.
            uint32_t swapchainFormatCount;
            CHECK_XRCMD(xrEnumerateSwapchainFormats(m_session, 0, &swapchainFormatCount, nullptr));
            std::vector<int64_t> swapchainFormats(swapchainFormatCount);
            CHECK_XRCMD(xrEnumerateSwapchainFormats(m_session, (uint32_t)swapchainFormats.size(), &swapchainFormatCount,
                                                    swapchainFormats.data()));
            CHECK(swapchainFormatCount == swapchainFormats.size());
            m_colorSwapchainFormat = m_graphicsPlugin->SelectColorSwapchainFormat(true, swapchainFormats);
            m_depthSwapchainFormat = m_graphicsPlugin->SelectDepthSwapchainFormat(false, swapchainFormats);

            if (m_depthSwapchainFormat == -1) {
                Log::Write(Log::Level::Info,
                           "Runtime does not support creating swapchains with a suitable depth format. Using our own  fallback "
                           "depth textures!");
                // can't submit our own fallback texture
                m_supportsDepthLayer = false;
            }

            // Print swapchain formats and the selected ones.
            {
                std::string swapchainFormatsString;
                for (int64_t format : swapchainFormats) {
                    const bool selected = (format == m_colorSwapchainFormat || format == m_depthSwapchainFormat);
                    swapchainFormatsString += " ";
                    if (selected) {
                        swapchainFormatsString += "[";
                    }
                    swapchainFormatsString += std::to_string(format);
                    if (selected) {
                        swapchainFormatsString += "]";
                    }
                }
                Log::Write(Log::Level::Verbose, Fmt("Swapchain Formats: %s", swapchainFormatsString.c_str()));
            }

            // Create a swapchain for each view.
            for (uint32_t i = 0; i < viewCount; i++) {
                const XrViewConfigurationView& vp = m_configViews[i];
                Log::Write(Log::Level::Info,
                           Fmt("Creating color %s swapchain for view %d with dimensions Width=%d Height=%d SampleCount=%d",
                               m_depthSwapchainFormat == -1 ? "" : "and depth", i, vp.recommendedImageRectWidth,
                               vp.recommendedImageRectHeight, vp.recommendedSwapchainSampleCount));

                // Create the swapchain.
                XrSwapchainCreateInfo swapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                swapchainCreateInfo.arraySize = 1;
                swapchainCreateInfo.format = m_colorSwapchainFormat;
                swapchainCreateInfo.width = vp.recommendedImageRectWidth;
                swapchainCreateInfo.height = vp.recommendedImageRectHeight;
                swapchainCreateInfo.mipCount = 1;
                swapchainCreateInfo.faceCount = 1;
                swapchainCreateInfo.sampleCount = m_graphicsPlugin->GetSupportedSwapchainSampleCount(vp);
                swapchainCreateInfo.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
                Swapchain swapchain;
                swapchain.width = swapchainCreateInfo.width;
                swapchain.height = swapchainCreateInfo.height;
                CHECK_XRCMD(xrCreateSwapchain(m_session, &swapchainCreateInfo, &swapchain.handle));

                m_swapchains.push_back(swapchain);

                uint32_t imageCount;
                CHECK_XRCMD(xrEnumerateSwapchainImages(swapchain.handle, 0, &imageCount, nullptr));

                if (m_depthSwapchainFormat != -1) {
                    XrSwapchainCreateInfo depthSwapchainCreateInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
                    depthSwapchainCreateInfo.arraySize = 1;
                    depthSwapchainCreateInfo.format = m_depthSwapchainFormat;
                    depthSwapchainCreateInfo.width = vp.recommendedImageRectWidth;
                    depthSwapchainCreateInfo.height = vp.recommendedImageRectHeight;
                    depthSwapchainCreateInfo.mipCount = 1;
                    depthSwapchainCreateInfo.faceCount = 1;
                    depthSwapchainCreateInfo.sampleCount = m_graphicsPlugin->GetSupportedSwapchainSampleCount(vp);
                    depthSwapchainCreateInfo.usageFlags =
                        XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
                    Swapchain depthSwapchain;
                    depthSwapchain.width = depthSwapchainCreateInfo.width;
                    depthSwapchain.height = depthSwapchainCreateInfo.height;
                    CHECK_XRCMD(xrCreateSwapchain(m_session, &depthSwapchainCreateInfo, &depthSwapchain.handle));

                    m_depthSwapchains.push_back(depthSwapchain);

                    uint32_t depthImageCount;
                    CHECK_XRCMD(xrEnumerateSwapchainImages(swapchain.handle, 0, &depthImageCount, nullptr));

                    // TODO: support this
                    if (depthImageCount != imageCount) {
                        THROW("This runtime has different color and depth swapchain lengths");
                    }

                    ISwapchainImageData* swapchainImages = m_graphicsPlugin->AllocateSwapchainImageDataWithDepthSwapchain(
                        imageCount, swapchainCreateInfo, depthSwapchain.handle, depthSwapchainCreateInfo);
                    CHECK_XRCMD(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount,
                                                           swapchainImages->GetColorImageArray()));

                    CHECK_XRCMD(xrEnumerateSwapchainImages(depthSwapchain.handle, depthImageCount, &depthImageCount,
                                                           swapchainImages->GetDepthImageArray()));

                    m_swapchainImages.insert(std::make_pair(swapchain.handle, std::move(swapchainImages)));
                } else {
                    ISwapchainImageData* swapchainImages =
                        m_graphicsPlugin->AllocateSwapchainImageData(imageCount, swapchainCreateInfo);
                    CHECK_XRCMD(xrEnumerateSwapchainImages(swapchain.handle, imageCount, &imageCount,
                                                           swapchainImages->GetColorImageArray()));

                    m_swapchainImages.insert(std::make_pair(swapchain.handle, std::move(swapchainImages)));
                }
            }
        }
    }

    // Return event if one is available, otherwise return null.
    const XrEventDataBaseHeader* TryReadNextEvent() {
        // It is sufficient to clear the just the XrEventDataBuffer header to
        // XR_TYPE_EVENT_DATA_BUFFER
        XrEventDataBaseHeader* baseHeader = reinterpret_cast<XrEventDataBaseHeader*>(&m_eventDataBuffer);
        *baseHeader = {XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult xr = xrPollEvent(m_instance, &m_eventDataBuffer);
        if (xr == XR_SUCCESS) {
            if (baseHeader->type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
                const XrEventDataEventsLost* const eventsLost = reinterpret_cast<const XrEventDataEventsLost*>(baseHeader);
                Log::Write(Log::Level::Warning, Fmt("%d events lost", eventsLost->lostEventCount));
            }

            return baseHeader;
        }
        if (xr == XR_EVENT_UNAVAILABLE) {
            return nullptr;
        }
        THROW_XR(xr, "xrPollEvent");
    }

    void PollEvents(bool* exitRenderLoop, bool* requestRestart) override {
        *exitRenderLoop = *requestRestart = false;

        // Process all pending messages.
        while (const XrEventDataBaseHeader* event = TryReadNextEvent()) {
            switch (event->type) {
                case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
                    const auto& instanceLossPending = *reinterpret_cast<const XrEventDataInstanceLossPending*>(event);
                    Log::Write(Log::Level::Warning, Fmt("XrEventDataInstanceLossPending by %lld", instanceLossPending.lossTime));
                    *exitRenderLoop = true;
                    *requestRestart = true;
                    return;
                }
                case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                    auto sessionStateChangedEvent = *reinterpret_cast<const XrEventDataSessionStateChanged*>(event);
                    HandleSessionStateChangedEvent(sessionStateChangedEvent, exitRenderLoop, requestRestart);
                    break;
                }
                case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                    // Print the ACTIVE profile per hand, not just the bound sources below - the
                    // runtime picks one profile per device and every binding suggested under a
                    // different profile is dead, which is invisible without this line.
                    for (const char* hand : {"/user/hand/left", "/user/hand/right"}) {
                        XrPath handPath;
                        CHECK_XRCMD(xrStringToPath(m_instance, hand, &handPath));
                        XrInteractionProfileState profileState{XR_TYPE_INTERACTION_PROFILE_STATE};
                        if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(m_session, handPath, &profileState)) &&
                            profileState.interactionProfile != XR_NULL_PATH) {
                            uint32_t sz = 0;
                            char buf[XR_MAX_PATH_LENGTH];
                            xrPathToString(m_instance, profileState.interactionProfile, sizeof(buf), &sz, buf);
                            Log::Write(Log::Level::Info, Fmt("Active profile %s: %s", hand, buf));
                        } else {
                            Log::Write(Log::Level::Info, Fmt("Active profile %s: (none)", hand));
                        }
                    }
                    LogActionSourceName(m_input.grabAction, "Grab");
                    LogActionSourceName(m_input.quitAction, "Quit");
                    LogActionSourceName(m_input.poseAction, "Pose");
                    LogActionSourceName(m_input.vibrateAction, "Vibrate");
                    LogActionSourceName(m_input.seekAction, "Seek");
                    LogActionSourceName(m_input.zoomAction, "Zoom");
                    LogActionSourceName(m_input.pauseAction, "Pause");
                    LogActionSourceName(m_input.recenterAction, "Recenter");
                    LogActionSourceName(m_input.brightnessUpAction, "BrightnessUp");
                    LogActionSourceName(m_input.brightnessDownAction, "BrightnessDown");
                    LogActionSourceName(m_input.nextTrackAction, "NextTrack");
                    LogActionSourceName(m_input.prevTrackAction, "PrevTrack");
                    break;
                case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
                default: {
                    Log::Write(Log::Level::Verbose, Fmt("Ignoring event type %d", event->type));
                    break;
                }
            }
        }
    }

    void HandleSessionStateChangedEvent(const XrEventDataSessionStateChanged& stateChangedEvent, bool* exitRenderLoop,
                                        bool* requestRestart) {
        const XrSessionState oldState = m_sessionState;
        m_sessionState = stateChangedEvent.state;

        Log::Write(Log::Level::Info, Fmt("XrEventDataSessionStateChanged: state %s->%s session=%lld time=%lld", to_string(oldState),
                                         to_string(m_sessionState), stateChangedEvent.session, stateChangedEvent.time));

        if ((stateChangedEvent.session != XR_NULL_HANDLE) && (stateChangedEvent.session != m_session)) {
            Log::Write(Log::Level::Error, "XrEventDataSessionStateChanged for unknown session");
            return;
        }

        switch (m_sessionState) {
            case XR_SESSION_STATE_READY: {
                CHECK(m_session != XR_NULL_HANDLE);
                XrSessionBeginInfo sessionBeginInfo{XR_TYPE_SESSION_BEGIN_INFO};
                sessionBeginInfo.primaryViewConfigurationType = m_viewConfigType;
                CHECK_XRCMD(xrBeginSession(m_session, &sessionBeginInfo));
                m_sessionRunning = true;
                break;
            }
            case XR_SESSION_STATE_STOPPING: {
                CHECK(m_session != XR_NULL_HANDLE);
                m_sessionRunning = false;
                CHECK_XRCMD(xrEndSession(m_session))
                break;
            }
            case XR_SESSION_STATE_EXITING: {
                *exitRenderLoop = true;
                // Do not attempt to restart because user closed this session.
                *requestRestart = false;
                break;
            }
            case XR_SESSION_STATE_LOSS_PENDING: {
                *exitRenderLoop = true;
                // Poll for a new instance.
                *requestRestart = true;
                break;
            }
            default:
                break;
        }
    }

    void LogActionSourceName(XrAction action, const std::string& actionName) const {
        XrBoundSourcesForActionEnumerateInfo getInfo = {XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO};
        getInfo.action = action;
        uint32_t pathCount = 0;
        CHECK_XRCMD(xrEnumerateBoundSourcesForAction(m_session, &getInfo, 0, &pathCount, nullptr));
        std::vector<XrPath> paths(pathCount);
        CHECK_XRCMD(xrEnumerateBoundSourcesForAction(m_session, &getInfo, uint32_t(paths.size()), &pathCount, paths.data()));

        std::string sourceName;
        for (uint32_t i = 0; i < pathCount; ++i) {
            constexpr XrInputSourceLocalizedNameFlags all = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT |
                                                            XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT |
                                                            XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;

            XrInputSourceLocalizedNameGetInfo nameInfo = {XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO};
            nameInfo.sourcePath = paths[i];
            nameInfo.whichComponents = all;

            uint32_t size = 0;
            CHECK_XRCMD(xrGetInputSourceLocalizedName(m_session, &nameInfo, 0, &size, nullptr));
            if (size < 1) {
                continue;
            }
            std::vector<char> grabSource(size);
            CHECK_XRCMD(xrGetInputSourceLocalizedName(m_session, &nameInfo, uint32_t(grabSource.size()), &size, grabSource.data()));
            if (!sourceName.empty()) {
                sourceName += " and ";
            }
            sourceName += "'";
            sourceName += std::string(grabSource.data(), size - 1);
            sourceName += "'";
        }

        Log::Write(Log::Level::Info,
                   Fmt("%s action is bound to %s", actionName.c_str(), ((!sourceName.empty()) ? sourceName.c_str() : "nothing")));
    }

    bool IsSessionRunning() const override { return m_sessionRunning; }

    bool IsSessionFocused() const override { return m_sessionState == XR_SESSION_STATE_FOCUSED; }

    void PollActions() override {
        m_input.handActive = {{XR_FALSE, XR_FALSE}};

        // Sync actions
        const XrActiveActionSet activeActionSet{m_input.actionSet, XR_NULL_PATH};
        XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
        syncInfo.countActiveActionSets = 1;
        syncInfo.activeActionSets = &activeActionSet;
        CHECK_XRCMD(xrSyncActions(m_session, &syncInfo));

        // Get pose and grab action state and start haptic vibrate when hand is 90% squeezed.
        for (auto hand : {Side::LEFT, Side::RIGHT}) {
            XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
            getInfo.action = m_input.grabAction;
            getInfo.subactionPath = m_input.handSubactionPath[hand];

            XrActionStateFloat grabValue{XR_TYPE_ACTION_STATE_FLOAT};
            CHECK_XRCMD(xrGetActionStateFloat(m_session, &getInfo, &grabValue));
            Log::Write(Log::Level::Info, Fmt("DEBUGGRAB hand=%d isActive=%d currentState=%f changedSinceLastSync=%d",
                                             (int)hand, (int)grabValue.isActive, grabValue.currentState,
                                             (int)grabValue.changedSinceLastSync));
            if (grabValue.isActive == XR_TRUE) {
                // Scale the rendered hand by 1.0f (open) to 0.5f (fully squeezed).
                m_input.handScale[hand] = 1.0f - 0.5f * grabValue.currentState;
                if (grabValue.currentState > 0.9f) {
                    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
                    vibration.amplitude = 0.5;
                    vibration.duration = XR_MIN_HAPTIC_DURATION;
                    vibration.frequency = XR_FREQUENCY_UNSPECIFIED;

                    XrHapticActionInfo hapticActionInfo{XR_TYPE_HAPTIC_ACTION_INFO};
                    hapticActionInfo.action = m_input.vibrateAction;
                    hapticActionInfo.subactionPath = m_input.handSubactionPath[hand];
                    CHECK_XRCMD(xrApplyHapticFeedback(m_session, &hapticActionInfo, (XrHapticBaseHeader*)&vibration));
                }
            }

            getInfo.action = m_input.poseAction;
            XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
            CHECK_XRCMD(xrGetActionStatePose(m_session, &getInfo, &poseState));
            m_input.handActive[hand] = poseState.isActive;

            // Video seek: thumbstick push left/right jumps -10s/+10s. Hysteresis latch, not a
            // raw per-frame trigger - a stick held past the threshold would otherwise queue a
            // jump on every single frame it stays pushed. Push past 0.7 to fire once, must come
            // back under 0.3 before it can fire again.
            static std::array<bool, Side::COUNT> seekLatched{{false, false}};
            getInfo.action = m_input.seekAction;
            XrActionStateFloat seekValue{XR_TYPE_ACTION_STATE_FLOAT};
            CHECK_XRCMD(xrGetActionStateFloat(m_session, &getInfo, &seekValue));
            if (seekValue.isActive == XR_TRUE) {
                const float v = seekValue.currentState;
                if (!seekLatched[hand] && std::fabs(v) > 0.7f) {
                    PlayerControl::QueueSeek(v > 0.0f ? 10 : -10);
                    seekLatched[hand] = true;
                } else if (seekLatched[hand] && std::fabs(v) < 0.3f) {
                    seekLatched[hand] = false;
                }
            }

            // Video zoom: thumbstick push up/down steps zoom in/out. Same hysteresis-latch
            // shape as seek, on the vertical axis seek doesn't use. Sign assumed from the
            // usual OpenXR/gamepad convention (+y = stick pushed up = zoom in) - not yet
            // confirmed with the headset on; if it comes out backwards on a live test, flip
            // the ZoomIn()/ZoomOut() branches below, not the sign of v itself.
            static std::array<bool, Side::COUNT> zoomLatched{{false, false}};
            getInfo.action = m_input.zoomAction;
            XrActionStateFloat zoomValue{XR_TYPE_ACTION_STATE_FLOAT};
            CHECK_XRCMD(xrGetActionStateFloat(m_session, &getInfo, &zoomValue));
            if (zoomValue.isActive == XR_TRUE) {
                const float v = zoomValue.currentState;
                if (!zoomLatched[hand] && std::fabs(v) > 0.7f) {
                    if (v > 0.0f) {
                        PlayerControl::ZoomIn();
                    } else {
                        PlayerControl::ZoomOut();
                    }
                    zoomLatched[hand] = true;
                } else if (zoomLatched[hand] && std::fabs(v) < 0.3f) {
                    zoomLatched[hand] = false;
                }
            }

            // Video pause: trigger toggles play/pause. Same hysteresis-latch shape as seek -
            // fire once past 0.7, must fall back under 0.3 before it can fire again.
            static std::array<bool, Side::COUNT> pauseLatched{{false, false}};
            getInfo.action = m_input.pauseAction;
            XrActionStateFloat pauseValue{XR_TYPE_ACTION_STATE_FLOAT};
            CHECK_XRCMD(xrGetActionStateFloat(m_session, &getInfo, &pauseValue));
            Log::Write(Log::Level::Info, Fmt("DEBUGTRIGGER hand=%d isActive=%d currentState=%f",
                                             (int)hand, (int)pauseValue.isActive, pauseValue.currentState));

            if (hand == Side::LEFT) {
                XrActionStateGetInfo anyInfo{XR_TYPE_ACTION_STATE_GET_INFO};
                anyInfo.action = m_input.pauseAction;
                anyInfo.subactionPath = XR_NULL_PATH;
                XrActionStateFloat anyPauseValue{XR_TYPE_ACTION_STATE_FLOAT};
                CHECK_XRCMD(xrGetActionStateFloat(m_session, &anyInfo, &anyPauseValue));
                Log::Write(Log::Level::Info, Fmt("DEBUGANY isActive=%d currentState=%f",
                                                 (int)anyPauseValue.isActive, anyPauseValue.currentState));
            }

            if (pauseValue.isActive == XR_TRUE) {
                if (!pauseLatched[hand] && pauseValue.currentState > 0.7f) {
                    PlayerControl::TogglePause();
                    pauseLatched[hand] = true;
                } else if (pauseLatched[hand] && pauseValue.currentState < 0.3f) {
                    pauseLatched[hand] = false;
                }
            }
        }

        // There were no subaction paths specified for the quit action, because we don't care which hand did it.
        //
        // Hold-to-confirm: a bare tap used to exit immediately, and the WMR Menu button (three
        // lines) is easy to hit by accident going for something else - so this now requires
        // holding it for kQuitHoldSeconds, with SetQuitHoldFraction() driving an on-screen fill
        // indicator (see frag.glsl) so the hold has feedback instead of being a silent timer.
        // Releasing early cancels; there is no cooldown, so the next press starts fresh.
        XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, m_input.quitAction, XR_NULL_PATH};
        XrActionStateBoolean quitValue{XR_TYPE_ACTION_STATE_BOOLEAN};
        CHECK_XRCMD(xrGetActionStateBoolean(m_session, &getInfo, &quitValue));
        static std::optional<std::chrono::steady_clock::time_point> quitHoldStart;
        constexpr double kQuitHoldSeconds = 1.5;
        if ((quitValue.isActive == XR_TRUE) && (quitValue.currentState == XR_TRUE)) {
            const auto now = std::chrono::steady_clock::now();
            if (!quitHoldStart) quitHoldStart = now;
            const double held = std::chrono::duration<double>(now - *quitHoldStart).count();
            PlayerControl::SetQuitHoldFraction(held / kQuitHoldSeconds);
            if (held >= kQuitHoldSeconds) {
                CHECK_XRCMD(xrRequestExitSession(m_session));
                quitHoldStart.reset();
                PlayerControl::SetQuitHoldFraction(0.0);
            }
        } else {
            quitHoldStart.reset();
            PlayerControl::SetQuitHoldFraction(0.0);
        }

        // Bug found 2026-08-09: PlayerControl::g_quit (set by keyboard q/ESC/EOF, and by
        // MaybeQuitOnAnyKey() under HELLO_XR_ANY_KEY_QUITS=1) used to only be watched by
        // main.cpp's keyboard-reading thread - which sits blocked in getchar() the entire
        // time in a controller-only session, so it never actually noticed. User report:
        // "zoom, menu, recenter works, brightness works, but nothing triggers playing
        // video" - zoom/recenter/brightness all correctly set g_quit via MaybeQuitOnAnyKey(),
        // but nothing was polling for it here. g_quit is never cleared once set, and this
        // runs every frame, so latch the call - a second xrRequestExitSession() once the
        // session has left RUNNING is a spec error, not a harmless no-op.
        static bool exitSessionRequested = false;
        if (!exitSessionRequested && PlayerControl::QuitRequested()) {
            CHECK_XRCMD(xrRequestExitSession(m_session));
            exitSessionRequested = true;
        }

        // Recenter: no subaction paths, same "don't care which hand" shape as quit.
        XrActionStateGetInfo recenterGetInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, m_input.recenterAction,
                                             XR_NULL_PATH};
        XrActionStateBoolean recenterValue{XR_TYPE_ACTION_STATE_BOOLEAN};
        CHECK_XRCMD(xrGetActionStateBoolean(m_session, &recenterGetInfo, &recenterValue));
        if ((recenterValue.isActive == XR_TRUE) && (recenterValue.changedSinceLastSync == XR_TRUE) &&
            (recenterValue.currentState == XR_TRUE)) {
            PlayerControl::RequestRecenter();
        }

        // Brightness: A/B on the right Touch controller, edge-triggered like recenter (fires
        // once per press, not repeatedly while held).
        XrActionStateGetInfo brightUpGetInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, m_input.brightnessUpAction,
                                             XR_NULL_PATH};
        XrActionStateBoolean brightUpValue{XR_TYPE_ACTION_STATE_BOOLEAN};
        CHECK_XRCMD(xrGetActionStateBoolean(m_session, &brightUpGetInfo, &brightUpValue));
        if ((brightUpValue.isActive == XR_TRUE) && (brightUpValue.changedSinceLastSync == XR_TRUE) &&
            (brightUpValue.currentState == XR_TRUE)) {
            PlayerControl::BrightnessUp();
        }

        XrActionStateGetInfo brightDownGetInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, m_input.brightnessDownAction,
                                               XR_NULL_PATH};
        XrActionStateBoolean brightDownValue{XR_TYPE_ACTION_STATE_BOOLEAN};
        CHECK_XRCMD(xrGetActionStateBoolean(m_session, &brightDownGetInfo, &brightDownValue));
        if ((brightDownValue.isActive == XR_TRUE) && (brightDownValue.changedSinceLastSync == XR_TRUE) &&
            (brightDownValue.currentState == XR_TRUE)) {
            PlayerControl::BrightnessDown();
        }

        // Next track: Y on the left Touch controller, same edge-triggered shape.
        XrActionStateGetInfo nextTrackGetInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, m_input.nextTrackAction,
                                              XR_NULL_PATH};
        XrActionStateBoolean nextTrackValue{XR_TYPE_ACTION_STATE_BOOLEAN};
        CHECK_XRCMD(xrGetActionStateBoolean(m_session, &nextTrackGetInfo, &nextTrackValue));
        if ((nextTrackValue.isActive == XR_TRUE) && (nextTrackValue.changedSinceLastSync == XR_TRUE) &&
            (nextTrackValue.currentState == XR_TRUE)) {
            PlayerControl::RequestNextTrack();
        }

        // Previous track: X on the left Touch controller, same edge-triggered shape.
        XrActionStateGetInfo prevTrackGetInfo{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, m_input.prevTrackAction,
                                              XR_NULL_PATH};
        XrActionStateBoolean prevTrackValue{XR_TYPE_ACTION_STATE_BOOLEAN};
        CHECK_XRCMD(xrGetActionStateBoolean(m_session, &prevTrackGetInfo, &prevTrackValue));
        if ((prevTrackValue.isActive == XR_TRUE) && (prevTrackValue.changedSinceLastSync == XR_TRUE) &&
            (prevTrackValue.currentState == XR_TRUE)) {
            PlayerControl::RequestPreviousTrack();
        }
    }

    void RenderFrame() override {
        CHECK(m_session != XR_NULL_HANDLE);

        XrFrameWaitInfo frameWaitInfo{XR_TYPE_FRAME_WAIT_INFO};
        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        CHECK_XRCMD(xrWaitFrame(m_session, &frameWaitInfo, &frameState));

        XrFrameBeginInfo frameBeginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        CHECK_XRCMD(xrBeginFrame(m_session, &frameBeginInfo));

        std::vector<XrCompositionLayerBaseHeader*> layers;
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        std::vector<XrCompositionLayerProjectionView> projectionLayerViews;
        std::vector<XrCompositionLayerDepthInfoKHR> depthInfos;
        if (frameState.shouldRender == XR_TRUE) {
            if (RenderLayer(frameState.predictedDisplayTime, projectionLayerViews, depthInfos, layer)) {
                layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
            }
        }

        XrFrameEndInfo frameEndInfo{XR_TYPE_FRAME_END_INFO};
        frameEndInfo.displayTime = frameState.predictedDisplayTime;
        frameEndInfo.environmentBlendMode = m_blendMode;
        frameEndInfo.layerCount = (uint32_t)layers.size();
        frameEndInfo.layers = layers.data();
        CHECK_XRCMD(xrEndFrame(m_session, &frameEndInfo));
    }

    bool RenderLayer(XrTime predictedDisplayTime, std::vector<XrCompositionLayerProjectionView>& projectionLayerViews,
                     std::vector<XrCompositionLayerDepthInfoKHR>& depthInfos, XrCompositionLayerProjection& layer) {
        XrResult res;

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        uint32_t viewCapacityInput = (uint32_t)m_views.size();
        uint32_t viewCountOutput;

        XrViewLocateInfo viewLocateInfo{XR_TYPE_VIEW_LOCATE_INFO};
        viewLocateInfo.viewConfigurationType = m_viewConfigType;
        viewLocateInfo.displayTime = predictedDisplayTime;
        viewLocateInfo.space = m_appSpace;

        res = xrLocateViews(m_session, &viewLocateInfo, &viewState, viewCapacityInput, &viewCountOutput, m_views.data());
        CHECK_XRRESULT(res, "xrLocateViews");
        if ((viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) == 0 ||
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) == 0) {
            return false;  // There is no valid tracking poses for the views.
        }

        CHECK(viewCountOutput == viewCapacityInput);
        CHECK(viewCountOutput == m_configViews.size());
        CHECK(viewCountOutput == m_swapchains.size());

        projectionLayerViews.resize(viewCountOutput);
        if (m_supportsDepthLayer) {
            depthInfos.resize(viewCountOutput);
        }

        // For each locatable space that we want to visualize, render a 25cm cube.
        std::vector<Cube> cubes;

        // HELLO_XR_FIXED_POSE (reverb-g2, 2026-09-05, docs/08 passthrough v0): a live camera-
        // passthrough view, not ordinary rendered content -- the controller/reference-space
        // gizmo cubes below z-fight against the passthrough image (both opaque geometry at
        // similar apparent depth) and are meaningless for a raw passthrough demo anyway, so both
        // are skipped in this mode; a floor grid is pushed instead, further down, as the visual
        // anchor a passthrough view otherwise lacks entirely.
        const bool passthroughMode = getenv("HELLO_XR_FIXED_POSE") != nullptr;

        // Reference-space cubes (ViewFront, Local, Stage, ...) are OFF by default; set
        // HELLO_XR_SPACE_CUBES=1 to get the original sample's behaviour back.
        //
        // In a 3dof session the head sits exactly at the Local origin, so the Local cube lands
        // centred on the viewer's face -- and since the pipeline draws with cullMode NONE, its
        // inner faces render too and the viewer ends up sealed inside an opaque box that hides
        // everything else, controller cubes included. Found the hard way: "estoy dentro de un
        // cubo de colores, no veo los controles".
        for (XrSpace visualizedSpace :
             ((m_spaceCubesEnabled && !passthroughMode) ? m_visualizedSpaces : std::vector<XrSpace>{})) {
            XrSpaceLocation spaceLocation{XR_TYPE_SPACE_LOCATION};
            res = xrLocateSpace(visualizedSpace, m_appSpace, predictedDisplayTime, &spaceLocation);
            CHECK_XRRESULT(res, "xrLocateSpace");
            if (XR_UNQUALIFIED_SUCCESS(res)) {
                if ((spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                    (spaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
                    cubes.push_back(Cube{spaceLocation.pose, {0.25f, 0.25f, 0.25f}});
                }
            } else {
                Log::Write(Log::Level::Verbose, Fmt("Unable to locate a visualized reference space in app space: %d", res));
            }
        }

        // Kept for the pose log below: the loop's own copies go out of scope, and the log has to
        // report a hand that is NOT tracked just as clearly as one that is -- that is precisely
        // the state worth seeing.
        XrSpaceLocation handLocation[Side::COUNT] = {{XR_TYPE_SPACE_LOCATION}, {XR_TYPE_SPACE_LOCATION}};

        // Render a 10cm cube scaled by grabAction for each hand. Note renderHand will only be
        // true when the application has focus.
        for (auto hand : {Side::LEFT, Side::RIGHT}) {
            XrSpaceLocation spaceLocation{XR_TYPE_SPACE_LOCATION};
            res = xrLocateSpace(m_input.handSpace[hand], m_appSpace, predictedDisplayTime, &spaceLocation);
            CHECK_XRRESULT(res, "xrLocateSpace");
            handLocation[hand] = spaceLocation;
            if (XR_UNQUALIFIED_SUCCESS(res)) {
                // 2026-09-05: controllers are drawn in passthrough mode too now that the
                // cube pipeline's view matrix (graphicsplugin_vulkan.cpp) tracks real live head
                // orientation + recenter, matching the video -- previously skipped here only to
                // avoid the z-fight/desync the OLD frozen-vs-real mismatch caused.
                if ((spaceLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                    (spaceLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
                    PushPoseGizmo(cubes, spaceLocation.pose, m_input.handScale[hand]);
                }
            } else {
                // Tracking loss is expected when the hand is not active so only log a message
                // if the hand is active.
                if (m_input.handActive[hand] == XR_TRUE) {
                    const char* handName[] = {"left", "right"};
                    Log::Write(Log::Level::Verbose,
                               Fmt("Unable to locate %s hand action space in app space: %d", handName[hand], res));
                }
            }
        }

        // Synthetic floor reference for passthrough mode (see PushFloorGrid's comment).
        //
        // Height comes straight from Stage, not from any live recalibration: this project
        // already has a proper per-wearer eye-height calibration (docs/58's T223 addendum +
        // docs/59), tape-measured (standing 1.70 m, seated 1.35 m, see ~/vr/vr-profile.conf)
        // and applied by jack-in-wayland.sh as XRT_TRACKING_ORIGIN_OFFSET_Y, which Monado's
        // space overseer bakes into every tracking origin -- Stage's Y=0 IS the calibrated
        // real floor already (see docs/08, 2026-09-05 entry, for the full mechanism and why
        // an earlier live-recalibration attempt here was a regression, not a fix).
        //
        // floorYForLog: same m_appSpace-relative Y as the head/controller HELLO_XR_POSE_LOG
        // line just below -- both come from xrLocateSpace/xrLocateViews against the SAME
        // m_appSpace, so (head.y - floorYForLog) is directly the wearer's real measured eye
        // height above wherever this grid actually renders. 2026-09-05: added to settle a
        // live "feels too high" report with a number instead of another guess.
        float floorYForLog = std::numeric_limits<float>::quiet_NaN();
        if (passthroughMode && m_stageSpaceForFloorGrid != XR_NULL_HANDLE) {
            XrSpaceLocation stageLocation{XR_TYPE_SPACE_LOCATION};
            res = xrLocateSpace(m_stageSpaceForFloorGrid, m_appSpace, predictedDisplayTime, &stageLocation);
            if (XR_UNQUALIFIED_SUCCESS(res) &&
                (stageLocation.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0 &&
                (stageLocation.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0) {
                floorYForLog = stageLocation.pose.position.y;
                PushFloorGrid(cubes, stageLocation.pose);
            }
        }

        // Head + both controllers, once a second, in app space. HELLO_XR_POSE_LOG=1.
        //
        // Added 2026-08-11 so controller tracking can be watched WITHOUT anyone wearing the
        // headset: on this hardware every session put on and taken off means moving a cable
        // whose connector has a long history of marginal contact (docs/22), so a check that
        // needs a human's head is a check with a real hardware cost. This one runs off the
        // poses the frame loop already locates.
        //
        // The head is the midpoint of the two eye poses, not m_views[0], so it does not sit
        // 3cm off to the left of where anyone would expect "the head" to be.
        //
        // The flags matter as much as the numbers: a controller with no positional tracking
        // still reports a perfectly plausible-looking position (Monado pins untracked devices
        // at a fixed offset from the tracking origin), so a bare XYZ triplet cannot be told
        // apart from a real one. POS/-- and TRK/-- are what distinguish them.
        if (m_poseLogEnabled) {
            const auto now = std::chrono::steady_clock::now();
            if (now - m_lastPoseLog >= std::chrono::seconds(1)) {
                m_lastPoseLog = now;

                XrVector3f head{0, 0, 0};
                for (uint32_t i = 0; i < viewCountOutput; i++) {
                    head.x += m_views[i].pose.position.x / (float)viewCountOutput;
                    head.y += m_views[i].pose.position.y / (float)viewCountOutput;
                    head.z += m_views[i].pose.position.z / (float)viewCountOutput;
                }

                auto describe = [](const XrSpaceLocation& loc) {
                    const bool posValid = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
                    const bool posTracked = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;

                    // Where the controller POINTS, not just where it is. Added 2026-08-12 so
                    // controller orientation can be judged from a flat monitor instead of by
                    // putting the headset on for every A/B -- and because "the left one points at
                    // me" is a claim about a direction, which a number settles and an impression
                    // does not. -Z is forward in OpenXR, so this is the grip's forward axis rotated
                    // into the reference space, plus a plain-language reading of it.
                    const XrQuaternionf& q = loc.pose.orientation;
                    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
                    const float fy = -2.0f * (q.y * q.z - q.w * q.x);
                    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
                    const char* dir = "?";
                    if (fz < -0.5f) dir = "AWAY";        // pointing away from the wearer
                    else if (fz > 0.5f) dir = "AT-ME";   // pointing back at the wearer
                    else if (fx < -0.5f) dir = "LEFT";
                    else if (fx > 0.5f) dir = "RIGHT";
                    else if (fy > 0.5f) dir = "UP";
                    else if (fy < -0.5f) dir = "DOWN";

                    return Fmt("(%+6.3f %+6.3f %+6.3f) pos:%s trk:%s fwd(%+5.2f %+5.2f %+5.2f)=%s",
                               loc.pose.position.x, loc.pose.position.y, loc.pose.position.z,
                               posValid ? "OK" : "--", posTracked ? "OK" : "--", fx, fy, fz, dir);
                };

                Log::Write(Log::Level::Info,
                           Fmt("POSE head (%+6.3f %+6.3f %+6.3f) floorY %+6.3f eyeAboveFloor %+6.3f | left %s | right %s",
                               head.x, head.y, head.z, floorYForLog, head.y - floorYForLog,
                               describe(handLocation[Side::LEFT]).c_str(), describe(handLocation[Side::RIGHT]).c_str()));
            }
        }

        // Render view to the appropriate part of the swapchain image.
        for (uint32_t i = 0; i < viewCountOutput; i++) {
            // Each view has a separate swapchain which is acquired, rendered to, and released.
            const Swapchain viewSwapchain = m_swapchains[i];

            XrSwapchainImageAcquireInfo acquireInfo{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};

            uint32_t swapchainImageIndex;
            CHECK_XRCMD(xrAcquireSwapchainImage(viewSwapchain.handle, &acquireInfo, &swapchainImageIndex));

            XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            waitInfo.timeout = XR_INFINITE_DURATION;
            CHECK_XRCMD(xrWaitSwapchainImage(viewSwapchain.handle, &waitInfo));

            m_swapchainImages[viewSwapchain.handle]->AcquireAndWaitDepthSwapchainImage(swapchainImageIndex);

            projectionLayerViews[i] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            projectionLayerViews[i].pose = m_views[i].pose;
            projectionLayerViews[i].fov = m_views[i].fov;
            projectionLayerViews[i].subImage.swapchain = viewSwapchain.handle;
            projectionLayerViews[i].subImage.imageRect.offset = {0, 0};
            projectionLayerViews[i].subImage.imageRect.extent = {viewSwapchain.width, viewSwapchain.height};

            if (m_supportsDepthLayer) {
                projectionLayerViews[i].next = &depthInfos[i];
                depthInfos[i].type = XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR;
                depthInfos[i].subImage.swapchain = m_depthSwapchains[i].handle;
                depthInfos[i].subImage.imageRect.offset = {0, 0};
                depthInfos[i].subImage.imageRect.extent = {m_depthSwapchains[i].width, m_depthSwapchains[i].height};
                depthInfos[i].minDepth = 0;
                depthInfos[i].maxDepth = 1;
                depthInfos[i].nearZ = 0.05f;
                depthInfos[i].farZ = 100.0f;
            }

            const XrSwapchainImageBaseHeader* const swapchainImage =
                m_swapchainImages[viewSwapchain.handle]->GetGenericColorImage(swapchainImageIndex);
            m_graphicsPlugin->RenderView(projectionLayerViews[i], swapchainImage, m_colorSwapchainFormat, cubes);

            XrSwapchainImageReleaseInfo releaseInfo{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            CHECK_XRCMD(xrReleaseSwapchainImage(viewSwapchain.handle, &releaseInfo));

            m_swapchainImages[viewSwapchain.handle]->ReleaseDepthSwapchainImage();
        }

        layer.space = m_appSpace;
        layer.layerFlags =
            m_blendMode == XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND
                ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT
                : 0;
        layer.viewCount = (uint32_t)projectionLayerViews.size();
        layer.views = projectionLayerViews.data();
        return true;
    }

   private:
    XrEnvironmentBlendMode m_blendMode{XR_ENVIRONMENT_BLEND_MODE_MAX_ENUM};
    XrViewConfigurationType m_viewConfigType{XR_VIEW_CONFIGURATION_TYPE_MAX_ENUM};

    std::shared_ptr<IPlatformPlugin> m_platformPlugin;
    std::shared_ptr<IGraphicsPlugin> m_graphicsPlugin;
    XrInstance m_instance{XR_NULL_HANDLE};
    XrSession m_session{XR_NULL_HANDLE};
    XrSpace m_appSpace{XR_NULL_HANDLE};
    XrSystemId m_systemId{XR_NULL_SYSTEM_ID};

    // We may still use a runtime allocated depth swapchain but not submit depth if false
    bool m_supportsDepthLayer{false};

    std::vector<XrViewConfigurationView> m_configViews;
    std::vector<Swapchain> m_swapchains;
    std::vector<Swapchain> m_depthSwapchains;
    std::map<XrSwapchain, ISwapchainImageData*> m_swapchainImages;
    std::vector<XrView> m_views;

    // Periodic head/controller pose log, see the block that uses these. ON by default, opt
    // OUT with HELLO_XR_POSE_LOG=0.
    //
    // Deliberately not opt-in. One line per second is nothing next to what this project has
    // repeatedly paid for a missing line: the cost here has never been too much log, it has
    // always been arriving at a question with no data for the run that already happened, and
    // then having to reproduce it on hardware whose connector does not enjoy being handled.
    const bool m_poseLogEnabled{[] {
        const char* v = std::getenv("HELLO_XR_POSE_LOG");
        return v == nullptr || (v[0] != '0' && v[0] != '\0');
    }()};
    std::chrono::steady_clock::time_point m_lastPoseLog{};
    int64_t m_colorSwapchainFormat{-1};
    int64_t m_depthSwapchainFormat{-1};

    std::vector<XrSpace> m_visualizedSpaces;

    //! Dedicated handle to the "Stage" space specifically (reverb-g2, 2026-09-05, docs/08
    //! passthrough v0 floor grid) -- independent of m_visualizedSpaces/m_spaceCubesEnabled,
    //! which are about the OPT-IN reference-space cube visualisation, off by default. The
    //! floor grid always wants Stage specifically (it IS the floor, by OpenXR convention),
    //! regardless of whether that debug toggle is on.
    XrSpace m_stageSpaceForFloorGrid{XR_NULL_HANDLE};

    //! See the loop that reads this. Opt-in: HELLO_XR_SPACE_CUBES=1.
    const bool m_spaceCubesEnabled{[] {
        const char* v = std::getenv("HELLO_XR_SPACE_CUBES");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }()};

    // Application's current lifecycle state according to the runtime
    XrSessionState m_sessionState{XR_SESSION_STATE_UNKNOWN};
    bool m_sessionRunning{false};

    XrEventDataBuffer m_eventDataBuffer;
    InputState m_input;
};
}  // namespace

std::shared_ptr<IOpenXrProgram> CreateOpenXrProgram(const std::shared_ptr<IPlatformPlugin>& platformPlugin,
                                                    const std::shared_ptr<IGraphicsPlugin>& graphicsPlugin) {
    return std::make_shared<OpenXrProgram>(platformPlugin, graphicsPlugin);
}
