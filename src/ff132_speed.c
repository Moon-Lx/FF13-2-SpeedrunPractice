/*
 * FF13-2-SpeedrunPractice — speedrun practice mod for FINAL FANTASY XIII-2 (Steam).
 * Copyright (C) 2026 Akiyoshi
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

// Game speed: the engine's own framework frame-speed float, written from the per-frame hook.
// Nothing is hooked or called here.
//
// ff132_speed_toggle runs on the hotkey thread and only moves g_gsStep; ff132_speed_pump runs on the game
// thread and is the only writer of the float; ff132_speed_draw runs on the render thread.
#include "ff132_internal.h"
#include "ff132_addrs.h"

#define FF132_COL_GAMESPEED   0xFFD2FA69        // lime green, as FF13's GAME SPEED line
#define GS_MEASURE_SEC     5.0

static volatile int g_gsStep = -1;          // -1 = off, else an index into g_gsSteps
static volatile int g_pumpSeen = 0;


// ---- the step (hotkey thread / state file) ----------------------------------------------------
static int step_live(int s) { return s >= 0 && s < g_gsStepN; }

void ff132_speed_toggle(void) {
    int s = g_gsStep + 1;
    if (s >= g_gsStepN) s = -1;                 // past the last factor is off
    g_gsStep = s;
    // ON needs no flash: the status line appears (or changes) on the next frame and says the same.
    if (s >= 0) {
        ff132_log(FF132_TAG "game speed: ON at %.2fx (step %d of %d)\n", g_gsSteps[s], s + 1, g_gsStepN);
    } else {
        ff132_log(FF132_TAG "game speed: OFF\n");
        ff132_flash(ui_text(T_GAMESPEED_OFF));
    }
    if (!g_pumpSeen)
        ff132_loga(FF132_TAG "game speed: the per-frame hook has not run yet -- nothing is applied until"
                      " it does\n");
    ff132_state_save();
}

int ff132_speed_step_get(void) {
    int s = g_gsStep;
    return step_live(s) ? s + 1 : 0;
}
// Called after config_load: a step past a list that has since been shortened is off.
void ff132_speed_step_set(int n) {
    if (n < 0 || n > g_gsStepN) {
        ff132_log(FF132_TAG "game speed: saved step %d is not in the list of %d -> off\n", n, g_gsStepN);
        n = 0;
    }
    g_gsStep = n - 1;
    if (n) ff132_log(FF132_TAG "game speed: restored ON at %.2fx (step %d of %d)\n", g_gsSteps[n - 1], n,
                 g_gsStepN);
}

// Off BEFORE the array is rewritten: the pump reads g_gsSteps[step] on the game thread, and a
// live step must never index a list that is half copied.
void ff132_speed_steps_set(const double* v, int n) {
    if (n < 1) return;
    if (n > FF132_GS_STEPS_MAX) n = FF132_GS_STEPS_MAX;
    int s = g_gsStep;
    double on = step_live(s) ? g_gsSteps[s] : 0.0;
    ff132_speed_step_set(0);
    for (int i = 0; i < n; i++) g_gsSteps[i] = v[i];
    g_gsStepN = n;
    if (on > 0.0) {
        for (int i = 0; i < n; i++) {
            double d = v[i] - on;
            if (d < 0.005 && d > -0.005) { g_gsStep = i; break; }
        }
        if (g_gsStep >= 0)
            ff132_log(FF132_TAG "game speed: list changed, still ON at %.2fx (step %d of %d)\n",
                  g_gsSteps[g_gsStep], g_gsStep + 1, n);
        else
            ff132_log(FF132_TAG "game speed: list changed, %.2fx is gone -> OFF\n", on);
    }
    ff132_state_save();
}

// ---- the pump (game thread, once per frame) ---------------------------------------------------
static float g_ours = 1.0f;         // the last value this mod wrote
static int   g_haveOurs = 0;
static int   g_foreignLogged = 0;
static int   g_vtLogged = 0;

// Wall clock vs the engine's tick counter. Mode 1 = measuring while on; 2 = one window after off.
static int      g_mMode = 0;
static int      g_mStep = -2;       // the step the current window was started under
static LONGLONG g_mQpc0 = 0;
static int64_t  g_mTick0 = 0;
static double   g_qpcFreq = 0.0;

static void measure(uintptr_t gm, int step) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t tick = *(const volatile int64_t*)(gm + FF132_OFF_GM_TICKS);
    if (step != g_mStep) {
        g_mMode = step >= 0 ? 1 : (g_mStep >= 0 ? 2 : 0);
        g_mStep = step;
        g_mQpc0 = now.QuadPart;
        g_mTick0 = tick;
        return;
    }
    if (!g_mMode) return;
    if (g_qpcFreq <= 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcFreq = (double)f.QuadPart;
        if (g_qpcFreq <= 0.0) { g_mMode = 0; return; }
    }
    double wall = (double)(now.QuadPart - g_mQpc0) / g_qpcFreq;
    if (wall < GS_MEASURE_SEC) return;
    int64_t dt = tick - g_mTick0;
    double wanted = g_mMode == 1 ? g_gsSteps[step] : 1.0;
    if (dt <= 0)
        ff132_logv(FF132_TAG "game speed: the tick counter did not advance over %.1f s (paused or"
                      " loading) -- window skipped\n", wall);
    else
        ff132_log(FF132_TAG "game speed: measured %.2fx over %.1f s (wanted %.2fx)\n",
              ((double)dt / FF132_TICKS_PER_SEC) / wall, wall, wanted);
    g_mQpc0 = now.QuadPart;
    g_mTick0 = tick;
    if (g_mMode == 2) g_mMode = 0;
}

// GameMain when its +8 is the frame timer, 0 otherwise; *vtOut gets what +8 held, for the log.
static uintptr_t frame_timer_gm(uint32_t* vtOut) {
    uintptr_t gm = (uintptr_t)rd32(g_base + FF132_RVA_GAMEMAIN_PTR);
    if (!rd_ok((void*)gm, FF132_OFF_GM_TICKS + 8)) return 0;
    uint32_t vt = rd32(gm + FF132_OFF_GM_FRAME_TIMER);
    if (vtOut) *vtOut = vt;
    return vt == (uint32_t)(g_base + FF132_RVA_FRAME_TIMER_VT) ? gm : 0;
}

void ff132_speed_pump(void) {
    g_pumpSeen = 1;
    int step = g_gsStep;
    if (!step_live(step)) step = -1;
    uint32_t vt = 0;
    uintptr_t gm = frame_timer_gm(&vt);
    if (!gm) {
        if (vt && !g_vtLogged) {
            g_vtLogged = 1;
            ff132_loga(FF132_TAG "game speed: GameMain+8 has vtable 0x%08X, not the frame timer's 0x%08X"
                          " -- the speed is not touched while it is\n", vt,
                   (unsigned)(g_base + FF132_RVA_FRAME_TIMER_VT));
        }
        return;
    }
    volatile float* sp = (volatile float*)(gm + FF132_OFF_GM_FRAME_SPEED);
    float want = step >= 0 ? (float)g_gsSteps[step] : 1.0f;
    float cur = *sp;
    if (cur == want) {
        g_foreignLogged = 0;
    } else {
        // Only a value that is the engine default or our own is replaced: anything else was set by
        // someone other than this mod and stays theirs.
        int mine = g_haveOurs && cur == g_ours;
        if (step >= 0 ? (cur == 1.0f || mine) : mine) {
            *sp = want;
            ff132_log(FF132_TAG "game speed: frame speed %.2f -> %.2f\n", cur, want);
            g_ours = want;
            g_haveOurs = step >= 0;
            g_foreignLogged = 0;
        } else if (step >= 0 || g_haveOurs) {
            if (!g_foreignLogged) {
                g_foreignLogged = 1;
                ff132_log(FF132_TAG "game speed: the frame speed is %.2f, not ours -- left alone\n", cur);
            }
            if (step < 0) g_haveOurs = 0;
        }
    }
    measure(gm, step);
}

// The engine's frame speed as it stands, whoever set it; 1 when the frame timer does not check out.
// Any thread: one aligned float read.
double ff132_speed_frame_factor(void) {
    uintptr_t gm = frame_timer_gm(NULL);
    if (!gm) return 1.0;
    float f = *(volatile float*)(gm + FF132_OFF_GM_FRAME_SPEED);
    return (f >= 0.05f && f <= 20.0f) ? (double)f : 1.0;
}

// ---- the status line (render thread) ----------------------------------------------------------
int ff132_speed_visible(void) { return step_live(g_gsStep); }

int ff132_speed_draw(int x, int y) {
    int s = g_gsStep;
    if (!step_live(s)) return y;
    char key[32], f[16], buf[64];
    vk_label(g_gameSpeedVk, key, sizeof key);
    ff132_gs_fmt(g_gsSteps[s], f, sizeof f);
    snprintf(buf, sizeof buf, ui_text(T_GAMESPEED), key, f);
    ff132_ovl_say(x, y, buf, FF132_COL_GAMESPEED);
    return y + FF132_OVL_LINE_H;
}
