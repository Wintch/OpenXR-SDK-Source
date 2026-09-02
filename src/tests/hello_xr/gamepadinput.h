// Copyright (c) 2017-2026 The Khronos Group Inc.
//
// SPDX-License-Identifier: Apache-2.0

// Xbox 360 gamepad fallback for the 360/VR180 player, Linux only.
//
// Monado has no runtime controller hotplug on this rig (the WMR device is built once at
// session startup and never re-probed - see the project's own G2-controller-hotplug notes),
// so a session started with the VR controllers off/unpaired, or that loses them mid-run, has
// no controller input at all until the whole session is restarted. This gives the player a
// second, fully independent way to reach the same core controls without touching Monado or
// OpenXR at all: an Xbox 360 pad read directly as a Linux joystick device (/dev/input/jsN,
// the joydev API), located by the same 045e:028e USB vendor:product this repo already uses
// to detect an Xbox 360 pad elsewhere (see scripts/power-on.py, scripts/reseat_audio.py)
// rather than a hardcoded device path. Unlike the VR controller path, this one is genuinely
// hot-pluggable in both directions - the kernel creates/destroys the jsN node as the pad is
// plugged or unplugged, and this polls for that continuously - so a pad plugged in mid-
// session, or unplugged and replugged, just works with no restart needed.
//
// Architecturally this is a third producer into the same shared control surface the raw-
// stdin keyboard thread (main.cpp) and the OpenXR controller-action poll
// (openxr_program.cpp's PollActions) already both feed: it calls straight into
// PlayerControl::* for pause/resume, recenter, and next/previous track, and nothing else -
// no control logic is duplicated here, only read and mapped.
#pragma once

namespace GamepadInput {

// Starts the background polling thread. Call once, from main(), alongside
// PlayerControl::BeginRawInput(). Returns immediately - the thread does its own device
// discovery and never blocks the caller. On a non-Linux build this is a no-op: joydev is a
// Linux kernel interface, and every other supported platform here (Windows/Android/Metal)
// still gets the player's existing VR-controller and keyboard paths unchanged.
//
// Never throws and never exits. A pad that is absent, unplugged mid-session, or replugged
// is the normal, expected case, not an error - it is retried silently, forever, without any
// effect on the render loop or the other two input paths.
void Start();

}  // namespace GamepadInput
