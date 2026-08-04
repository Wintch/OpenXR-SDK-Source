// Keyboard transport controls for the 360 viewer.
//
// The input is a global (the process's stdin, read by one thread in main.cpp) and the
// consumer sits deep inside the render loop, so the state between them is a small set of
// atomics rather than a parameter threaded through the graphics plugin interface.
//
//   space   pause / resume
//   [  ]    slower / faster (1x, 0.5x, 0.25x, 2x ...)
//   1       back to normal speed
//   n       next file in the playlist
//   q ESC   quit

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

// Set by 'n', cleared by whoever acts on it. Ignored when there is only one file.
bool TakeNextTrackRequest();

// True once a quit key has been pressed.
bool QuitRequested();

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
