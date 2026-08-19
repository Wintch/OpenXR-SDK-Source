// Copyright (c) 2017-2026 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "pch.h"
#include "common.h"
#include "options.h"
#include "platformdata.h"
#include "platformplugin.h"
#include "graphicsplugin.h"
#include "openxr_program.h"
#include "playercontrol.h"

#include <cstdlib>
#include <cstring>

#if defined(XR_OS_LINUX) || defined(XR_OS_APPLE)
#include <poll.h>
#include <unistd.h>
#endif

namespace {
// One byte from stdin, deliberately via raw read(2) and NOT getchar(). Found 2026-08-09,
// via gdb on a live hang: stdio's getchar() holds stdin's FILE lock for the whole blocking
// read, and glibc's exit() -> _IO_flush_all() needs that same lock - so any exit that
// happens while the keyboard thread is still parked in getchar() (i.e. every quit that
// came from the CONTROLLER rather than a key: Menu hold, HELLO_XR_ANY_KEY_QUITS) was a
// guaranteed deadlock inside exit(), with the XR session already ended and the process
// refusing to die. Latent since the Menu-quit patch (0005) - masked until now because the
// old `sleep N |` stdin pipe delivered EOF at N seconds and unblocked the thread late,
// and interactive runs had timeout's SIGTERM as a backstop. A kernel-level read() holds
// no user-space locks, so the thread can stay parked in it at exit() harmlessly forever.
// Returns EOF on end-of-stream or error, matching what getchar() reported.
int ReadKeyByte() {
#if defined(XR_OS_LINUX) || defined(XR_OS_APPLE)
    unsigned char b;
    return (read(STDIN_FILENO, &b, 1) == 1) ? (int)b : EOF;
#else
    return getchar();
#endif
}
}  // namespace

#if defined(_WIN32)
// Favor the high performance NVIDIA or AMD GPUs
extern "C" {
// http://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
// https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
}
#endif  // defined(_WIN32)

namespace {

#ifdef XR_USE_PLATFORM_ANDROID
void ShowHelp() {
    Log::Write(Log::Level::Info, "adb shell setprop debug.xr.graphicsPlugin OpenGLES|Vulkan");
    Log::Write(Log::Level::Info, "adb shell setprop debug.xr.formFactor Hmd|Handheld");
    Log::Write(Log::Level::Info, "adb shell setprop debug.xr.viewConfiguration Stereo|Mono");
    Log::Write(Log::Level::Info, "adb shell setprop debug.xr.blendMode Opaque|Additive|AlphaBlend");
}

bool UpdateOptionsFromSystemProperties(Options& options) {
#if defined(DEFAULT_GRAPHICS_PLUGIN_OPENGLES)
    options.GraphicsPlugin = "OpenGLES";
#elif defined(DEFAULT_GRAPHICS_PLUGIN_VULKAN)
    options.GraphicsPlugin = "Vulkan";
#endif

    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.xr.graphicsPlugin", value) != 0) {
        options.GraphicsPlugin = value;
    }

    if (__system_property_get("debug.xr.formFactor", value) != 0) {
        options.FormFactor = value;
    }

    if (__system_property_get("debug.xr.viewConfiguration", value) != 0) {
        options.ViewConfiguration = value;
    }

    if (__system_property_get("debug.xr.blendMode", value) != 0) {
        options.EnvironmentBlendMode = value;
    }

    try {
        options.ParseStrings();
    } catch (std::invalid_argument& ia) {
        Log::Write(Log::Level::Error, ia.what());
        ShowHelp();
        return false;
    }
    return true;
}
#else
void ShowHelp() {
    // TODO: Improve/update when things are more settled.
    Log::Write(Log::Level::Info,
               "HelloXr --graphics|-g <Graphics API> [--formfactor|-ff <Form factor>] [--viewconfig|-vc <View config>] "
               "[--blendmode|-bm <Blend mode>] [--space|-s <Space>] [--verbose|-v]");
    Log::Write(Log::Level::Info, "Graphics APIs:            D3D11, D3D12, OpenGLES, OpenGL, Vulkan2, Vulkan, Metal");
    Log::Write(Log::Level::Info, "Form factors:             Hmd, Handheld");
    Log::Write(Log::Level::Info, "View configurations:      Mono, Stereo");
    Log::Write(Log::Level::Info, "Environment blend modes:  Opaque, Additive, AlphaBlend");
    Log::Write(Log::Level::Info, "Spaces:                   View, Local, Stage");
}

bool UpdateOptionsFromCommandLine(Options& options, int argc, char* argv[]) {
    int i = 1;  // Index 0 is the program name and is skipped.

    auto getNextArg = [&] {
        if (i >= argc) {
            throw std::invalid_argument("Argument parameter missing");
        }

        return std::string(argv[i++]);
    };

    while (i < argc) {
        const std::string arg = getNextArg();
        if (EqualsIgnoreCase(arg, "--graphics") || EqualsIgnoreCase(arg, "-g")) {
            options.GraphicsPlugin = getNextArg();
        } else if (EqualsIgnoreCase(arg, "--formfactor") || EqualsIgnoreCase(arg, "-ff")) {
            options.FormFactor = getNextArg();
        } else if (EqualsIgnoreCase(arg, "--viewconfig") || EqualsIgnoreCase(arg, "-vc")) {
            options.ViewConfiguration = getNextArg();
        } else if (EqualsIgnoreCase(arg, "--blendmode") || EqualsIgnoreCase(arg, "-bm")) {
            options.EnvironmentBlendMode = getNextArg();
        } else if (EqualsIgnoreCase(arg, "--space") || EqualsIgnoreCase(arg, "-s")) {
            options.AppSpace = getNextArg();
        } else if (EqualsIgnoreCase(arg, "--verbose") || EqualsIgnoreCase(arg, "-v")) {
            Log::SetLevel(Log::Level::Verbose);
        } else if (EqualsIgnoreCase(arg, "--help") || EqualsIgnoreCase(arg, "-h")) {
            ShowHelp();
            return false;
        } else {
            throw std::invalid_argument(Fmt("Unknown argument: %s", arg.c_str()));
        }
    }

    // Check for required parameters.
    if (options.GraphicsPlugin.empty()) {
        Log::Write(Log::Level::Error, "GraphicsPlugin parameter is required");
        ShowHelp();
        return false;
    }

    try {
        options.ParseStrings();
    } catch (std::invalid_argument& ia) {
        Log::Write(Log::Level::Error, ia.what());
        ShowHelp();
        return false;
    }
    return true;
}
#endif
}  // namespace

#ifdef XR_USE_PLATFORM_ANDROID

struct AndroidAppState {
    ANativeWindow* NativeWindow = nullptr;
    bool Resumed = false;
};

/**
 * Process the next main command.
 */
static void app_handle_cmd(struct android_app* app, int32_t cmd) {
    AndroidAppState* appState = (AndroidAppState*)app->userData;

    switch (cmd) {
        // There is no APP_CMD_CREATE. The ANativeActivity creates the
        // application thread from onCreate(). The application thread
        // then calls android_main().
        case APP_CMD_START: {
            Log::Write(Log::Level::Info, "    APP_CMD_START");
            Log::Write(Log::Level::Info, "onStart()");
            break;
        }
        case APP_CMD_RESUME: {
            Log::Write(Log::Level::Info, "onResume()");
            Log::Write(Log::Level::Info, "    APP_CMD_RESUME");
            appState->Resumed = true;
            break;
        }
        case APP_CMD_PAUSE: {
            Log::Write(Log::Level::Info, "onPause()");
            Log::Write(Log::Level::Info, "    APP_CMD_PAUSE");
            appState->Resumed = false;
            break;
        }
        case APP_CMD_STOP: {
            Log::Write(Log::Level::Info, "onStop()");
            Log::Write(Log::Level::Info, "    APP_CMD_STOP");
            break;
        }
        case APP_CMD_DESTROY: {
            Log::Write(Log::Level::Info, "onDestroy()");
            Log::Write(Log::Level::Info, "    APP_CMD_DESTROY");
            appState->NativeWindow = NULL;
            break;
        }
        case APP_CMD_INIT_WINDOW: {
            Log::Write(Log::Level::Info, "surfaceCreated()");
            Log::Write(Log::Level::Info, "    APP_CMD_INIT_WINDOW");
            appState->NativeWindow = app->window;
            break;
        }
        case APP_CMD_TERM_WINDOW: {
            Log::Write(Log::Level::Info, "surfaceDestroyed()");
            Log::Write(Log::Level::Info, "    APP_CMD_TERM_WINDOW");
            appState->NativeWindow = NULL;
            break;
        }
    }
}

/**
 * This is the main entry point of a native application that is using
 * android_native_app_glue.  It runs in its own thread, with its own
 * event loop for receiving input events and doing other things.
 */
void android_main(struct android_app* app) {
    try {
        JNIEnv* Env;
        app->activity->vm->AttachCurrentThread(&Env, nullptr);

        AndroidAppState appState = {};

        app->userData = &appState;
        app->onAppCmd = app_handle_cmd;

        std::shared_ptr<Options> options = std::make_shared<Options>();
        if (!UpdateOptionsFromSystemProperties(*options)) {
            return;
        }

        std::shared_ptr<PlatformData> data = std::make_shared<PlatformData>();
        data->applicationVM = app->activity->vm;
        data->applicationActivity = app->activity->clazz;

        bool requestRestart = false;
        bool exitRenderLoop = false;

        // Create platform-specific implementation.
        std::shared_ptr<IPlatformPlugin> platformPlugin = CreatePlatformPlugin(data);
        // Create graphics API implementation.
        std::shared_ptr<IGraphicsPlugin> graphicsPlugin = CreateGraphicsPlugin(options->GraphicsPlugin);

        // Initialize the OpenXR program.
        std::shared_ptr<IOpenXrProgram> program = CreateOpenXrProgram(platformPlugin, graphicsPlugin);

        // Initialize the loader for this platform
        PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
        if (XR_SUCCEEDED(
                xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction*)(&initializeLoader)))) {
            XrLoaderInitInfoAndroidKHR loaderInitInfoAndroid = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
            loaderInitInfoAndroid.applicationVM = app->activity->vm;
            loaderInitInfoAndroid.applicationContext = app->activity->clazz;
            initializeLoader((const XrLoaderInitInfoBaseHeaderKHR*)&loaderInitInfoAndroid);
        }

        program->CreateInstance();
        program->InitializeSystem(options->Parsed.FormFactor, options->Parsed.ViewConfigType,
                                  options->Parsed.EnvironmentBlendModeOverride, options->Parsed.EnvironmentBlendMode);

        graphicsPlugin->SetClearColor(program->GetBackgroundClearColor());

        program->InitializeDevice();
        program->InitializeSession(options->AppSpace);
        program->CreateSwapchains();

        while (app->destroyRequested == 0) {
            // Read all pending events.
            for (;;) {
                int events;
                struct android_poll_source* source;
                // If the timeout is zero, returns immediately without blocking.
                // If the timeout is negative, waits indefinitely until an event appears.
                const int timeoutMilliseconds =
                    (!appState.Resumed && !program->IsSessionRunning() && app->destroyRequested == 0) ? -1 : 0;
                if (ALooper_pollAll(timeoutMilliseconds, nullptr, &events, (void**)&source) < 0) {
                    break;
                }

                // Process this event.
                if (source != nullptr) {
                    source->process(app, source);
                }
            }

            program->PollEvents(&exitRenderLoop, &requestRestart);
            if (exitRenderLoop) {
                ANativeActivity_finish(app->activity);
                continue;
            }

            if (!program->IsSessionRunning()) {
                // Throttle loop since xrWaitFrame won't be called.
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }

            program->PollActions();
            program->RenderFrame();
        }

        app->activity->vm->DetachCurrentThread();
    } catch (const std::exception& ex) {
        Log::Write(Log::Level::Error, ex.what());
    } catch (...) {
        Log::Write(Log::Level::Error, "Unknown Error");
    }
}
#else
int main(int argc, char* argv[]) {
    try {
        // Parse command-line arguments into Options.
        std::shared_ptr<Options> options = std::make_shared<Options>();
        if (!UpdateOptionsFromCommandLine(*options, argc, argv)) {
            return 1;
        }

        std::shared_ptr<PlatformData> data = std::make_shared<PlatformData>();

        // One thread reading stdin drives the transport controls. It used to be "any key
        // quits"; now only q/ESC/EOF do, and the rest are pause, speed and track skip - unless
        // HELLO_XR_ANY_KEY_QUITS=1 opts back into the old behavior (added 2026-08-09 for
        // play-with-legend.sh's "press anything to get past the controls screen"; Menu's own
        // hold-to-confirm gesture is untouched either way). On a pipe (the timed
        // `sleep N | hello_xr` runs) there are no keys and getchar returns EOF when the pipe
        // closes, which still ends the run exactly as before.
        {
            const char* anyKeyQuits = getenv("HELLO_XR_ANY_KEY_QUITS");
            PlayerControl::SetAnyKeyQuits(anyKeyQuits != nullptr && strcmp(anyKeyQuits, "1") == 0);
        }

        // HELLO_XR_DURATION_S=<seconds>: end the run automatically after this many seconds of
        // process time, through PlayerControl::RequestQuit() - the exact same graceful path as
        // pressing q (openxr_program.cpp's PollActions sees QuitRequested() and calls
        // xrRequestExitSession() once), not a SIGTERM. 0 means run forever. Unset preserves the
        // current behavior exactly: hello_xr has never self-limited its own runtime, only ever
        // reacted to stdin EOF (see ReadKeyByte/HandleKey above) or an external `timeout`.
        //
        // Found 2026-08-19 (T221): a real measurement window kept dying at ~300s even with
        // stdin held open via `sleep 14400 | ...`, which should have blocked forever short of
        // EOF. There is no such limit anywhere in this codebase (checked the whole render loop
        // and every player patch) - the ~300s was play360.sh's SECONDS_TO_RUN=300 default,
        // which wraps hello_xr in `timeout 300`. `timeout` kills on a SIGTERM regardless of
        // stdin, so the documented EOF trap looked like the cause but wasn't. This gives
        // hello_xr its own internal, gracefully-exiting duration option so a direct invocation
        // doesn't need an external `timeout` (and its abrupt SIGTERM) to bound a run at all.
        double durationSeconds = 0.0;
        if (const char* v = getenv("HELLO_XR_DURATION_S")) {
            const int parsed = atoi(v);
            if (parsed >= 0) {
                durationSeconds = (double)parsed;
            } else {
                Log::Write(Log::Level::Warning,
                           Fmt("HELLO_XR_DURATION_S='%s' not a non-negative integer - ignoring (running forever)", v));
            }
        }
        const auto processStart = std::chrono::steady_clock::now();
        bool durationQuitRequested = false;

        static bool quitKeyPressed = false;
        PlayerControl::BeginRawInput();
        std::atexit(PlayerControl::EndRawInput);
        auto exitPollingThread = std::thread{[] {
            Log::Write(Log::Level::Info, PlayerControl::HelpLine());
            while (!quitKeyPressed) {
                const int c = ReadKeyByte();
#if defined(XR_OS_LINUX) || defined(XR_OS_APPLE)
                // Arrow keys arrive as the 3-byte escape sequence ESC '[' A/B/C/D. ESC alone is
                // also the quit key, so on ESC we peek for more bytes with a short poll()
                // timeout instead of assuming: a real Escape keypress has nothing following
                // it, while an arrow key's remaining bytes are already sitting in the
                // terminal's input buffer by the time we get here (the pty writes all three
                // as one burst). Left/right and up/down also have plain '<'/'>' and '^'/'v' as
                // a keyboard-only fallback in HandleKey, in case a terminal encodes arrows
                // differently.
                if (c == 27 && isatty(STDIN_FILENO)) {
                    pollfd pfd{STDIN_FILENO, POLLIN, 0};
                    if (poll(&pfd, 1, 30) > 0) {
                        const int c2 = ReadKeyByte();
                        if (c2 == '[') {
                            pollfd pfd2{STDIN_FILENO, POLLIN, 0};
                            if (poll(&pfd2, 1, 30) > 0) {
                                const int c3 = ReadKeyByte();
                                if (c3 == 'C') {
                                    PlayerControl::StepFrame(1);
                                } else if (c3 == 'D') {
                                    PlayerControl::StepFrame(-1);
                                } else if (c3 == 'A') {
                                    PlayerControl::ZoomIn();
                                } else if (c3 == 'B') {
                                    PlayerControl::ZoomOut();
                                }
                                // any other CSI sequence (Home/End, F-keys...): not handled,
                                // just swallowed along with its introducer bytes.
                            }
                            continue;
                        }
                        // ESC followed by something that isn't '[': treat the ESC as the
                        // real quit keypress it looks like, then let the next byte act too.
                        PlayerControl::HandleKey(27);
                        if (PlayerControl::QuitRequested()) quitKeyPressed = true;
                        PlayerControl::HandleKey(c2);
                        if (PlayerControl::QuitRequested()) quitKeyPressed = true;
                        continue;
                    }
                }
#endif
                PlayerControl::HandleKey(c);
                if (PlayerControl::QuitRequested()) quitKeyPressed = true;
                if (c == EOF) break;
            }
        }};
        exitPollingThread.detach();

        bool requestRestart = false;
        do {
            // Create platform-specific implementation.
            std::shared_ptr<IPlatformPlugin> platformPlugin = CreatePlatformPlugin(data);

            // Create graphics API implementation.
            std::shared_ptr<IGraphicsPlugin> graphicsPlugin = CreateGraphicsPlugin(options->GraphicsPlugin);

            // Initialize the OpenXR program.
            std::shared_ptr<IOpenXrProgram> program = CreateOpenXrProgram(platformPlugin, graphicsPlugin);

            program->CreateInstance();
            program->InitializeSystem(options->Parsed.FormFactor, options->Parsed.ViewConfigType,
                                      options->Parsed.EnvironmentBlendModeOverride, options->Parsed.EnvironmentBlendMode);

            graphicsPlugin->SetClearColor(program->GetBackgroundClearColor());

            program->InitializeDevice();
            program->InitializeSession(options->AppSpace);
            program->CreateSwapchains();

            while (!quitKeyPressed) {
                bool exitRenderLoop = false;
                program->PollEvents(&exitRenderLoop, &requestRestart);
                if (exitRenderLoop) {
                    break;
                }

                // HELLO_XR_DURATION_S: latched so this only fires once - RequestQuit() just
                // flips a flag (harmless to call again), but the log line would otherwise
                // repeat every frame for however long PollActions/session teardown takes.
                if (durationSeconds > 0.0 && !durationQuitRequested) {
                    const double elapsed =
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - processStart).count();
                    if (elapsed >= durationSeconds) {
                        Log::Write(Log::Level::Info,
                                   Fmt("player: HELLO_XR_DURATION_S=%.0f elapsed - quitting", durationSeconds));
                        PlayerControl::RequestQuit();
                        durationQuitRequested = true;
                    }
                }

                if (program->IsSessionRunning()) {
                    program->PollActions();
                    program->RenderFrame();
                } else {
                    // Throttle loop since xrWaitFrame won't be called.
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }

        } while (!quitKeyPressed && requestRestart);

        return 0;
    } catch (const std::exception& ex) {
        Log::Write(Log::Level::Error, ex.what());
        return 1;
    } catch (...) {
        Log::Write(Log::Level::Error, "Unknown Error");
        return 1;
    }
}
#endif
