#include "pch.h"
#include "common.h"
#include "logger.h"
#include "playercontrol.h"

#include <chrono>
#include <cstdio>
#include <cstring>

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
std::atomic<int> g_seekJumpSeconds{0};
std::atomic<int> g_frameStepRequest{0};
std::atomic<int> g_quitHoldPermille{0};
std::atomic<bool> g_recenterRequested{false};
// Stored as bit-pattern-in-an-int64 (via memcpy) rather than std::atomic<double>, which isn't
// guaranteed lock-free on every platform this builds for; this is - matches g_lastInteractionMs.
std::atomic<int64_t> g_recenterYawBits{0};

// Same bit-pattern trick, and the same "0 means unset" sentinel g_lastInteractionMs uses - the
// bit pattern of 1.0 is not all-zero, so a freshly-started process (bits == 0) correctly reads
// as "no zoom" without needing a non-constexpr initializer to store 1.0's bits up front.
std::atomic<int64_t> g_zoomBits{0};

constexpr double kZoomStep = 1.15;  // multiplicative, per press/threshold-cross
constexpr double kZoomMin = 0.5;
constexpr double kZoomMax = 4.0;

// Same bit-pattern/sentinel trick as g_zoomBits.
std::atomic<int64_t> g_brightnessBits{0};

constexpr double kBrightnessStep = 1.15;  // multiplicative, per button press
constexpr double kBrightnessMin = 0.2;
constexpr double kBrightnessMax = 3.0;

// Milliseconds since steady_clock's epoch. 0 means "never" (SecondsSinceLastInteraction()
// then returns a large number, which is what "never touched" should look like to the
// progress-bar auto-hide timer).
std::atomic<int64_t> g_lastInteractionMs{0};

void TouchInteraction() {
    g_lastInteractionMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
}

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
    TouchInteraction();
    ReportRate();
}

void Slower() {
    int i = g_rateIndex.load();
    if (i > 0) g_rateIndex = i - 1;
    g_paused = false;
    TouchInteraction();
    ReportRate();
}

void Faster() {
    int i = g_rateIndex.load();
    if (i < kRateCount - 1) g_rateIndex = i + 1;
    g_paused = false;
    TouchInteraction();
    ReportRate();
}

void NormalSpeed() {
    g_rateIndex = kNormalRateIndex;
    g_paused = false;
    TouchInteraction();
    ReportRate();
}

bool TakeNextTrackRequest() { return g_nextTrack.exchange(false); }

int TakeSeekRequest() { return g_seekJumpSeconds.exchange(0); }

void QueueSeek(int seconds) {
    g_seekJumpSeconds += seconds;
    TouchInteraction();
    Log::Write(Log::Level::Info, Fmt("player: seek %+ds", seconds));
}

int TakeFrameStepRequest() { return g_frameStepRequest.exchange(0); }

void StepFrame(int frames) {
    g_paused = true;
    g_frameStepRequest += frames;
    TouchInteraction();
    Log::Write(Log::Level::Info, Fmt("player: frame %+d", frames));
}

double Zoom() {
    const int64_t bits = g_zoomBits.load();
    if (bits == 0) return 1.0;
    double z;
    std::memcpy(&z, &bits, sizeof(z));
    return z;
}

namespace {
void SetZoom(double z) {
    if (z < kZoomMin) z = kZoomMin;
    if (z > kZoomMax) z = kZoomMax;
    int64_t bits;
    std::memcpy(&bits, &z, sizeof(bits));
    g_zoomBits = bits;
    TouchInteraction();
    Log::Write(Log::Level::Info, Fmt("player: zoom %.2fx", z));
}
}  // namespace

void ZoomIn() { SetZoom(Zoom() * kZoomStep); }
void ZoomOut() { SetZoom(Zoom() / kZoomStep); }
void ResetZoom() { SetZoom(1.0); }

double Brightness() {
    const int64_t bits = g_brightnessBits.load();
    if (bits == 0) return 1.0;
    double b;
    std::memcpy(&b, &bits, sizeof(b));
    return b;
}

namespace {
void SetBrightness(double b) {
    if (b < kBrightnessMin) b = kBrightnessMin;
    if (b > kBrightnessMax) b = kBrightnessMax;
    int64_t bits;
    std::memcpy(&bits, &b, sizeof(bits));
    g_brightnessBits = bits;
    TouchInteraction();
    Log::Write(Log::Level::Info, Fmt("player: brillo %.2fx", b));
}
}  // namespace

void BrightnessUp() { SetBrightness(Brightness() * kBrightnessStep); }
void BrightnessDown() { SetBrightness(Brightness() / kBrightnessStep); }
void ResetBrightness() { SetBrightness(1.0); }

double SecondsSinceLastInteraction() {
    const int64_t last = g_lastInteractionMs.load();
    if (last == 0) return 1e9;  // never touched - "a long time ago" for the auto-hide timer
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
    return (double)(now - last) / 1000.0;
}

bool QuitRequested() { return g_quit; }

void SetQuitHoldFraction(double frac) {
    if (frac < 0.0) frac = 0.0;
    if (frac > 1.0) frac = 1.0;
    g_quitHoldPermille = (int)(frac * 1000.0);
}

int QuitHoldPermille() { return g_quitHoldPermille.load(); }

void RequestRecenter() {
    g_recenterRequested = true;
    TouchInteraction();
    Log::Write(Log::Level::Info, "player: recenter");
}

bool TakeRecenterRequest() { return g_recenterRequested.exchange(false); }

void SetRecenterYaw(double radians) {
    int64_t bits;
    std::memcpy(&bits, &radians, sizeof(bits));
    g_recenterYawBits = bits;
}

double RecenterYaw() {
    const int64_t bits = g_recenterYawBits.load();
    double radians;
    std::memcpy(&radians, &bits, sizeof(radians));
    return radians;
}

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
        case 'h':
        case 'H':
            QueueSeek(-10);
            return true;
        case 'l':
        case 'L':
            QueueSeek(10);
            return true;
        case '<':
            StepFrame(-1);
            return true;
        case '>':
            StepFrame(1);
            return true;
        case '^':
            ZoomIn();
            return true;
        case 'v':
        case 'V':
            ZoomOut();
            return true;
        case '0':
            ResetZoom();
            return true;
        case 'b':
        case 'B':
            BrightnessUp();
            return true;
        case 'd':
        case 'D':
            BrightnessDown();
            return true;
        case '9':
            ResetBrightness();
            return true;
        case '\r':
        case '\n':
            RequestRecenter();
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
    return "  Teclas: [espacio] pausa   [ ] velocidad   1 normal   h/l -10s/+10s   "
           "<-/-> (o < >) frame a frame   arriba/abajo (o ^ v) zoom   0 zoom normal   "
           "b/d brillo   9 brillo normal   "
           "enter recentra   n siguiente   q salir "
           "(mando WMR: trigger pausa, stick X seek, stick Y zoom, grip recentra, "
           "A/B (der.) brillo, mantener Menu ~1.5s sale)";
}

}  // namespace PlayerControl
