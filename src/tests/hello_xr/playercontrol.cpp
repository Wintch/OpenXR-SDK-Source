#include "pch.h"
#include "common.h"
#include "logger.h"
#include "playercontrol.h"

#include <cstdio>

#if defined(XR_OS_LINUX) || defined(XR_OS_APPLE)
#include <termios.h>
#include <unistd.h>
#endif

namespace PlayerControl {
namespace {

// The speeds the [ and ] keys step through. 1.0 is the resting point.
constexpr double kRates[] = {0.125, 0.25, 0.5, 1.0, 2.0, 4.0};
constexpr int kRateCount = (int)(sizeof(kRates) / sizeof(kRates[0]));
constexpr int kNormalRateIndex = 3;

std::atomic<int> g_rateIndex{kNormalRateIndex};
std::atomic<bool> g_paused{false};
std::atomic<bool> g_nextTrack{false};
std::atomic<bool> g_quit{false};

#if defined(XR_OS_LINUX) || defined(XR_OS_APPLE)
termios g_savedTermios{};
bool g_rawActive = false;
#endif

void ReportRate() {
    if (g_paused) {
        Log::Write(Log::Level::Info, "player: EN PAUSA (espacio para seguir)");
    } else {
        Log::Write(Log::Level::Info, Fmt("player: velocidad %gx", kRates[g_rateIndex.load()]));
    }
}

}  // namespace

double Rate() { return g_paused ? 0.0 : kRates[g_rateIndex.load()]; }

bool IsPaused() { return g_paused; }

void TogglePause() {
    g_paused = !g_paused;
    ReportRate();
}

void Slower() {
    int i = g_rateIndex.load();
    if (i > 0) g_rateIndex = i - 1;
    g_paused = false;
    ReportRate();
}

void Faster() {
    int i = g_rateIndex.load();
    if (i < kRateCount - 1) g_rateIndex = i + 1;
    g_paused = false;
    ReportRate();
}

void NormalSpeed() {
    g_rateIndex = kNormalRateIndex;
    g_paused = false;
    ReportRate();
}

bool TakeNextTrackRequest() { return g_nextTrack.exchange(false); }

bool QuitRequested() { return g_quit; }

bool HandleKey(int c) {
    switch (c) {
        case ' ':
            TogglePause();
            return true;
        case '[':
        case ',':
        case '-':
            Slower();
            return true;
        case ']':
        case '.':
        case '+':
        case '=':
            Faster();
            return true;
        case '1':
            NormalSpeed();
            return true;
        case 'n':
        case 'N':
            g_nextTrack = true;
            Log::Write(Log::Level::Info, "player: siguiente");
            return true;
        case 'q':
        case 'Q':
        case 27:   // ESC
        case 3:    // Ctrl-C, which raw mode would otherwise swallow
        case 4:    // Ctrl-D
        case EOF:  // the pipe on stdin closed - this is how a timed run ends
            g_quit = true;
            return true;
        default:
            return false;
    }
}

void BeginRawInput() {
#if defined(XR_OS_LINUX) || defined(XR_OS_APPLE)
    // Only meaningful on a real terminal. Under `sleep N | hello_xr` stdin is a pipe, the
    // keys go nowhere, and the run ends on EOF - which is exactly what we want there.
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &g_savedTermios) != 0) return;

    termios raw = g_savedTermios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);  // deliver each keypress, do not print it
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
    g_rawActive = true;
#endif
}

void EndRawInput() {
#if defined(XR_OS_LINUX) || defined(XR_OS_APPLE)
    if (!g_rawActive) return;
    tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTermios);
    g_rawActive = false;
#endif
}

const char* HelpLine() {
    return "  Teclas: [espacio] pausa   [ ] velocidad   1 normal   n siguiente   q salir";
}

}  // namespace PlayerControl
