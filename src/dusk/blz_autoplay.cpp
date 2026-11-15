// blz_autoplay: scripted pad input + per-frame timing log for automated play tests (claude-gc-native, 15 Nov 2026).
// Off unless the environment asks for it, so normal play is unchanged.
//   DUSK_PAD_SCRIPT=<file>  lines "<start_s> <dur_s> <buttons|-> <stickX> <stickY> [substickX substickY]"
//                           buttons = A+B+X+Y+START+Z+L+R+UP+DOWN+LEFT+RIGHT, times from the first sim tick.
//                           Pressed through PADSetVirtualStatus (the touch-control path), merged with real pads.
//   DUSK_FRAME_LOG=<file>   CSV "t_s,frame_ms" for every presented frame.
//   DUSK_QUIT_AFTER=<s>     exit cleanly after that many seconds of game time (for unattended runs).
#include <SDL3/SDL_timer.h>
#include <dolphin/pad.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace dusk { extern bool IsRunning; }

namespace dusk::blz {
namespace {
struct Step {
    double start, dur;
    u16 buttons;
    int sx, sy, cx, cy;
};
std::vector<Step> g_steps;
bool g_loaded = false, g_active = false;
Uint64 g_t0 = 0, g_lastFrame = 0;
FILE* g_frameLog = nullptr;
double g_quitAfter = 0;
PADStatus g_cur{};

u16 parse_buttons(const char* s) {
    u16 b = 0;
    std::string tok;
    auto flush = [&] {
        static const struct { const char* n; u16 v; } names[] = {
            {"A", PAD_BUTTON_A}, {"B", PAD_BUTTON_B}, {"X", PAD_BUTTON_X}, {"Y", PAD_BUTTON_Y},
            {"START", PAD_BUTTON_START}, {"Z", PAD_TRIGGER_Z}, {"L", PAD_TRIGGER_L}, {"R", PAD_TRIGGER_R},
            {"UP", PAD_BUTTON_UP}, {"DOWN", PAD_BUTTON_DOWN}, {"LEFT", PAD_BUTTON_LEFT}, {"RIGHT", PAD_BUTTON_RIGHT}};
        for (auto& n : names) if (tok == n.n) b |= n.v;
        tok.clear();
    };
    for (const char* p = s; *p; ++p) { if (*p == '+') flush(); else tok += *p; }
    flush();
    return b;
}

double now_s() {
    return static_cast<double>(SDL_GetPerformanceCounter() - g_t0) / static_cast<double>(SDL_GetPerformanceFrequency());
}

void load() {
    g_loaded = true;
    g_t0 = SDL_GetPerformanceCounter();
    if (const char* q = std::getenv("DUSK_QUIT_AFTER")) g_quitAfter = std::atof(q);
    if (const char* f = std::getenv("DUSK_FRAME_LOG")) {
        g_frameLog = std::fopen(f, "w");
        if (g_frameLog) std::fputs("t_s,frame_ms\n", g_frameLog);
    }
    const char* path = std::getenv("DUSK_PAD_SCRIPT");
    if (!path) return;
    FILE* fp = std::fopen(path, "r");
    if (!fp) return;
    char line[256];
    while (std::fgets(line, sizeof line, fp)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        Step s{};
        char btn[96] = "-";
        int n = std::sscanf(line, "%lf %lf %95s %d %d %d %d", &s.start, &s.dur, btn, &s.sx, &s.sy, &s.cx, &s.cy);
        if (n < 3) continue;
        s.buttons = std::strcmp(btn, "-") == 0 ? 0 : parse_buttons(btn);
        g_steps.push_back(s);
    }
    std::fclose(fp);
    g_active = !g_steps.empty();
    std::fprintf(stderr, "[blz_autoplay] %zu script steps from %s\n", g_steps.size(), path);
}
}  // namespace

// Called before every mDoCPd_c::read().
void autoplay_tick() {
    if (!g_loaded) load();
    if (g_quitAfter > 0 && now_s() > g_quitAfter) dusk::IsRunning = false;
    if (!g_active) return;
    const double t = now_s();
    PADStatus st{};
    bool any = false;
    for (const auto& s : g_steps) {
        if (t >= s.start && t < s.start + s.dur) {
            st.button |= s.buttons;
            if (s.sx) st.stickX = static_cast<s8>(s.sx);
            if (s.sy) st.stickY = static_cast<s8>(s.sy);
            if (s.cx) st.substickX = static_cast<s8>(s.cx);
            if (s.cy) st.substickY = static_cast<s8>(s.cy);
            any = true;
        }
    }
    if (st.button & PAD_TRIGGER_L) st.triggerLeft = 180;
    if (st.button & PAD_TRIGGER_R) st.triggerRight = 180;
    if (st.button & PAD_BUTTON_A) st.analogA = 255;
    if (st.button & PAD_BUTTON_B) st.analogB = 255;
    (void)any;
    g_cur = st;  // applied inside PADRead by blz_pad_override (touch controls clear the virtual pad each frame)
}

// Called after every aurora_end_frame() of the game loop.
void frame_presented() {
    if (!g_loaded) load();
    const Uint64 c = SDL_GetPerformanceCounter();
    if (g_frameLog && g_lastFrame) {
        const double ms = 1000.0 * static_cast<double>(c - g_lastFrame) / static_cast<double>(SDL_GetPerformanceFrequency());
        std::fprintf(g_frameLog, "%.3f,%.3f\n", now_s(), ms);
    }
    g_lastFrame = c;
}
}  // namespace dusk::blz

// Called by aurora's PADRead for every port after it filled the status: a scripted run owns port 0 completely
// (connected, no real pad needed; Dusklight's UI pad block is bypassed so the 'No Device Assigned' toast can't eat it).
extern "C" int blz_pad_override(unsigned port, PADStatus* status) {
    if (port != 0 || !dusk::blz::g_active) return 0;
    *status = dusk::blz::g_cur;
    status->err = PAD_ERR_NONE;
    return 1;
}
