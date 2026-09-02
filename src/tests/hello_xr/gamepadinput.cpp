// Copyright (c) 2017-2026 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "pch.h"
#include "common.h"
#include "logger.h"
#include "playercontrol.h"
#include "gamepadinput.h"

// See gamepadinput.h for the why. Everything below is Linux-only (joydev is a Linux kernel
// API); on every other platform Start() is a harmless no-op further down.
#if defined(XR_OS_LINUX)

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <fstream>
#include <linux/joystick.h>
#include <optional>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace GamepadInput {
namespace {

// Same VID:PID this repo's own scripts already use to identify an Xbox 360 pad (see
// scripts/power-on.py's GAMEPAD_ID / scripts/reseat_audio.py's GAMEPAD_ID, both
// "045e:028e") - split into two strings here because that's the shape sysfs hands back
// (id/vendor and id/product are two separate files), not because the ID means anything
// different in this file.
constexpr const char* kVendor = "045e";
constexpr const char* kProduct = "028e";

// Xbox 360 pad button indices as reported through joydev on this rig's xpad driver,
// confirmed live against the actual pad with `jstest --normal /dev/input/js0` (2026-09-02):
// "11 buttons (BtnA, BtnB, BtnX, BtnY, BtnTL, BtnTR, BtnSelect, BtnStart, BtnMode, BtnThumbL,
// BtnThumbR)", in that index order. Only the ones this file binds get a name; B, X, Y,
// Select, and both thumbstick clicks are read (harmlessly ignored) but not wired to
// anything, matching the requested "core controls" scope - the sticks and triggers aren't
// read at all yet either (a natural follow-up, not done here).
constexpr uint8_t kBtnA = 0;      // pause / resume            (mirrors the VR trigger)
constexpr uint8_t kBtnTL = 4;     // left shoulder / LB        - previous track
constexpr uint8_t kBtnTR = 5;     // right shoulder / RB       - next track
constexpr uint8_t kBtnStart = 7;  // Start                     - recenter (mirrors the VR grip)
constexpr uint8_t kBtnMode = 8;   // Xbox / Guide button, held - quit (mirrors the VR Menu hold)

// Mirrors openxr_program.cpp's own Menu-hold-to-quit gesture (kQuitHoldSeconds there): hold,
// don't tap, and releasing early cancels with no cooldown. Kept as a separate constant here
// rather than shared with that file - same value, but the two hold timers run on completely
// independent threads with no shared state, so there is nothing to actually share.
constexpr double kQuitHoldSeconds = 1.5;

// Looks for a /dev/input/jsN node whose kernel-reported vendor:product is the Xbox 360
// pad's (045e:028e), via the joystick input-core's own id files
// (/sys/class/input/jsN/device/id/{vendor,product}) rather than parsing `lsusb` text (this
// repo's convention elsewhere, but that only proves *a* matching device is on the USB bus,
// not which input node it became) or hardcoding a jsN number, which the kernel is free to
// reassign across replugs, reboots, or whenever some other joystick is also plugged in.
// Returns an empty string if none is found right now - normal whenever the pad isn't
// plugged in, not logged as an error.
std::string FindDevice() {
    for (int n = 0; n < 32; ++n) {
        const std::string base = "/sys/class/input/js" + std::to_string(n) + "/device/id/";
        std::ifstream vendorFile(base + "vendor");
        std::ifstream productFile(base + "product");
        std::string vendor, product;
        if (!(vendorFile >> vendor) || !(productFile >> product)) continue;
        std::transform(vendor.begin(), vendor.end(), vendor.begin(), ::tolower);
        std::transform(product.begin(), product.end(), product.begin(), ::tolower);
        if (vendor == kVendor && product == kProduct) {
            return "/dev/input/js" + std::to_string(n);
        }
    }
    return {};
}

// One button transition. Buttons only, on purpose (see the file header): each joydev
// JS_EVENT_BUTTON is already an edge (a press or a release), never a continuous level like
// the VR controller's analog trigger/thumbstick axes, so there is no hysteresis-latch to
// reimplement here - PlayerControl's own functions are called at most once per real
// press, exactly like the keyboard path already does.
void HandleButton(uint8_t number, bool down, std::optional<std::chrono::steady_clock::time_point>* guideHoldStart) {
    switch (number) {
        case kBtnA:
            if (down) PlayerControl::TogglePause();
            break;
        case kBtnStart:
            if (down) PlayerControl::RequestRecenter();
            break;
        case kBtnTR:
            if (down) PlayerControl::RequestNextTrack();
            break;
        case kBtnTL:
            if (down) PlayerControl::RequestPreviousTrack();
            break;
        case kBtnMode:
            if (down) {
                *guideHoldStart = std::chrono::steady_clock::now();
            } else {
                guideHoldStart->reset();  // released early - cancels, same as the VR Menu hold
            }
            break;
        default:
            break;  // not wired - see the file header for scope
    }
}

// The whole thread: find the pad, poll it, map its buttons onto PlayerControl, and go back
// to looking for it if it ever disappears. Never exits, never throws. Deliberately never
// told to stop - it is harmless to leave parked in poll() with a short timeout for the rest
// of the process's life, the same way the keyboard thread is deliberately left parked in
// read() past quit (see main.cpp's ReadKeyByte comment) rather than torn down.
void PollLoop() {
    Log::Write(Log::Level::Info,
               "gamepad: watching for an Xbox 360 pad (045e:028e) - optional; A pause/resume, "
               "Start recentra, RB/LB next/prev track, hold Guide ~1.5s to quit. Useful when a "
               "VR controller isn't paired/available.");
    int fd = -1;
    std::optional<std::chrono::steady_clock::time_point> guideHoldStart;

    while (true) {
        if (fd < 0) {
            const std::string path = FindDevice();
            if (path.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            // O_NONBLOCK: reads must never block this thread past the poll() below, and
            // joydev's own "give me the current state of every axis/button" burst on open
            // (JS_EVENT_INIT, drained in the loop below) would otherwise do exactly that if
            // it ever raced with an empty read buffer.
            fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                // Race with the device disappearing again between FindDevice() and open(),
                // or a permissions hiccup - either way, just try again shortly.
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            Log::Write(Log::Level::Info, Fmt("gamepad: connected (%s)", path.c_str()));
        }

        pollfd pfd{fd, POLLIN, 0};
        // A 100ms cap, not an unbounded wait: the Guide-hold-to-quit timer below needs to
        // keep advancing even while the pad sits idle mid-hold with no new event to wake us.
        const int pr = poll(&pfd, 1, 100);
        if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            Log::Write(Log::Level::Info, "gamepad: disconnected");
            close(fd);
            fd = -1;
            guideHoldStart.reset();
            continue;
        }
        if (pr > 0 && (pfd.revents & POLLIN)) {
            js_event ev;
            bool disconnected = false;
            while (true) {
                const ssize_t n = read(fd, &ev, sizeof(ev));
                if (n == (ssize_t)sizeof(ev)) {
                    // JS_EVENT_INIT is OR'd onto the type for the synthetic "here's the
                    // current state" events joydev sends right after open() - masked off so
                    // an already-held button at open time isn't treated as a fresh press
                    // (most relevant for Guide: holding it through a reconnect shouldn't
                    // silently start the quit timer).
                    if ((ev.type & ~JS_EVENT_INIT) == JS_EVENT_BUTTON) {
                        HandleButton(ev.number, ev.value != 0, &guideHoldStart);
                    }
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;  // drained
                // Anything else (unplugged mid-read, a short read, ...) is the normal,
                // expected "pad went away" case here, not a fatal error - go back to
                // searching rather than propagating it.
                disconnected = true;
                break;
            }
            if (disconnected) {
                Log::Write(Log::Level::Info, "gamepad: disconnected");
                close(fd);
                fd = -1;
                guideHoldStart.reset();
                continue;
            }
        }

        // Guide-hold-to-quit timing, checked on every poll wakeup (event or 100ms timeout
        // alike) so it fires the moment the hold crosses the threshold instead of only when
        // the next unrelated event happens to arrive.
        if (guideHoldStart) {
            const double held =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - *guideHoldStart).count();
            if (held >= kQuitHoldSeconds) {
                Log::Write(Log::Level::Info, "gamepad: Guide held ~1.5s - quitting");
                PlayerControl::RequestQuit();
                guideHoldStart.reset();
            }
        }
    }
}

}  // namespace

void Start() {
    std::thread(PollLoop).detach();
}

}  // namespace GamepadInput

#else  // !XR_OS_LINUX

namespace GamepadInput {
void Start() {}
}  // namespace GamepadInput

#endif
