// Android side of the on-screen diagnostics: paints messages into the window and shows the last
// loading stage if the app crashes.
#include <android/native_window.h>

#include <csignal>
#include <signal.h>
#include <cstdio>
#include <cstring>

#include "diag.h"

namespace {

char gStage[96] = "STARTING";
char gDetail[160] = "";
ANativeWindow* gWindow = nullptr;
volatile sig_atomic_t gInCrash = 0;

constexpr uint32_t kCrashBackground = packRgba(90, 0, 0);

const char* signalName(int sig) {
    switch (sig) {
        case SIGSEGV: return "SEGV";
        case SIGABRT: return "ABORT";
        case SIGBUS: return "BUS";
        case SIGFPE: return "FPE";
        case SIGILL: return "ILL";
        default: return "?";
    }
}

void crashHandler(int sig, siginfo_t*, void*) {
    if (!gInCrash && gWindow) {
        gInCrash = 1;
        char l1[] = "APP CRASHED";
        char l2[64];
        char l3[128];
        char l4[192];
        std::snprintf(l2, sizeof l2, "SIGNAL %d %s", sig, signalName(sig));
        std::snprintf(l3, sizeof l3, "STAGE: %s", gStage);
        std::snprintf(l4, sizeof l4, "DETAIL: %s", gDetail);
        const char* lines[] = {l1, l2, l3, l4};
        diagPaint(gWindow, kCrashBackground, lines, 4);
    }
    // Let the system handle the signal after the message is on screen (crash report, process exit).
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

}  // namespace

void diagSetStage(const char* stage) {
    std::snprintf(gStage, sizeof gStage, "%s", stage ? stage : "");
}

void diagSetDetail(const char* detail) {
    std::snprintf(gDetail, sizeof gDetail, "%s", detail ? detail : "");
}

const char* diagStage() {
    return gStage;
}

const char* diagDetail() {
    return gDetail;
}

void diagSetWindow(ANativeWindow* window) {
    gWindow = window;
}

void diagInstallCrashHandler() {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = crashHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    const int signals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};
    for (int sig : signals) sigaction(sig, &sa, nullptr);
}

bool diagPaint(ANativeWindow* window, uint32_t background, const char* const* lines, int count) {
    if (!window) return false;
    ANativeWindow_Buffer buf;
    if (ANativeWindow_lock(window, &buf, nullptr) != 0) return false;
    if (buf.format == WINDOW_FORMAT_RGBA_8888 || buf.format == WINDOW_FORMAT_RGBX_8888) {
        const PixelBuffer pb{static_cast<uint32_t*>(buf.bits), buf.width, buf.height, buf.stride};
        fillPixels(pb, background);
        drawTextLines(pb, lines, count, packRgba(255, 255, 255));
    }
    ANativeWindow_unlockAndPost(window);
    return true;
}
