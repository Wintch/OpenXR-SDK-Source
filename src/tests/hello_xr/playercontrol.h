// Keyboard transport controls for the 360 viewer.
//
// The input is a global (the process's stdin, read by one thread in main.cpp) and the
// consumer sits deep inside the render loop, so the state between them is a small set of
// atomics rather than a parameter threaded through the graphics plugin interface.
//
//   space   pause / resume (also: WMR controller trigger)
//   [  ]    slower / faster (1x, 0.5x, 0.25x, 2x ...)
//   1       back to normal speed
//   h  l    seek -10s / +10s (also: WMR controller thumbstick X)
//   <- ->   step back / forward one frame (also: < >). Pauses first - the jump is a single
//           frame, too small to see while still playing.
//   up down zoom in / out (also: WMR controller thumbstick Y; ^ v as a plain-key fallback,
//           same idea as < > for the arrow keys above)
//   0       back to no zoom (1x)
//   b  d    brighter / dimmer (also: Touch controller A/B, right hand)
//   9       back to normal brightness (1x)
//   enter   recenter forward (also: WMR controller squeeze/grip)
//   n       next file in the playlist
//   q ESC   quit (also: hold the WMR Menu button ~1.5s)

#pragma once

#include <atomic>

namespace PlayerControl {

// Playback speed. 0 means paused; the previous speed is remembered across a pause.
double Rate();
void TogglePause();
void Slower();
void Faster();
void NormalSpeed();
bool IsPaused();

// Set by 'n' (also: Touch controller Y, left hand), cleared by whoever acts on it. Ignored
// when there is only one file.
bool TakeNextTrackRequest();
void RequestNextTrack();

// Seconds to jump the playback position by (negative = back), accumulated from 'h'/'l' and
// controller thumbstick pushes since the last call. Cleared on read. 0 most of the time.
int TakeSeekRequest();

// Queues a seek jump in seconds (positive = forward). Called by HandleKey and by the
// controller thumbstick poll in openxr_program.cpp.
void QueueSeek(int seconds);

// Frames to step (positive = forward, negative = back), accumulated since the last call.
// Cleared on read. The consumer (graphicsplugin_vulkan.cpp) turns this into a Video360::Seek
// of frameCount * frame duration - there's no separate frame-accurate decode path, it's the
// same seek-and-flush machinery h/l use, just for a much smaller delta.
int TakeFrameStepRequest();

// Queues a frame step and forces a pause: stepping while playing would move less than a
// frame's worth of the eye can register, since normal playback is already advancing many
// frames a second.
void StepFrame(int frames);

// How long ago (seconds) the user last touched a transport control (pause/seek/speed) -
// drives the progress bar's auto-hide timer. A large number once nothing has happened for a
// while.
double SecondsSinceLastInteraction();

// True once a quit key has been pressed.
bool QuitRequested();

// How far into the hold-to-confirm quit gesture the WMR Menu button is (0..1), written by
// openxr_program.cpp's PollActions each frame and read by the renderer for the on-screen
// hold indicator. 0 when Menu isn't held or the hold was released early.
void SetQuitHoldFraction(double frac);
int QuitHoldPermille();

// Digital zoom on the panoramic/flat content: >1 magnifies (a narrower slice of the source
// fills the view), <1 shows more of it (mild fisheye-like widening past the native FOV).
// 1.0 is native/off. Driven by the controller thumbstick Y (the vertical axis is otherwise
// unused - QueueSeek already owns X) and by Up/Down (or ^/v as a keyboard fallback).
double Zoom();
void ZoomIn();
void ZoomOut();
void ResetZoom();

// Brightness multiplier on the final displayed color: >1 brighter, <1 dimmer, 1.0 is
// native/off. Driven by the Touch controller's A/B buttons (right hand only - the sticks are
// already spoken for by seek and zoom) and by b/d on the keyboard.
double Brightness();
void BrightnessUp();
void BrightnessDown();
void ResetBrightness();

// Recenter: squeeze/grip (either hand) resets "forward" to wherever you're currently facing,
// same idea as the recenter button most 360 video players have. Requested by the input-poll
// side; the renderer (which is the only place that has the current head pose) does the actual
// yaw capture and clears the request.
void RequestRecenter();
bool TakeRecenterRequest();
void SetRecenterYaw(double radians);
double RecenterYaw();

// Feeds one character from the terminal. Returns false if the key was not a control key,
// so the caller can decide what to do with it.
bool HandleKey(int c);

// Puts the terminal into non-canonical, no-echo mode so single keypresses arrive without
// Enter, and restores it at exit. Safe to call when stdin is not a terminal (does nothing).
void BeginRawInput();
void EndRawInput();

// One line describing the controls, for the startup banner.
const char* HelpLine();

}  // namespace PlayerControl
