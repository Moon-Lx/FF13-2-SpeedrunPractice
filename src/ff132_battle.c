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

// The battle overlay: the block the FF13 mod draws in a fight -- TIME (GAME / REAL and the
// countdown to the next paradigm-shift ATB refill) and TARGET (HP / ATB / CHAIN) -- on FF13-2's
// own fields.
// Read-only: nothing here is hooked. Every address is in ff132_addrs.h.
//
// ff132_battle_frame and ff132_battle_draw run on the render thread (the EndScene hook) and are the only
// things that touch a battle object; ff132_battle_toggle runs on the hotkey thread and touches nothing
// but the flag, the log and the flash.
#include "ff132_internal.h"
#include "ff132_addrs.h"


// ---- the battle object ------------------------------------------------------------------------
// The probe span reaches the last field read off BM anywhere in this file, so every caller below
// may read without re-checking. A battle can be torn down between two EndScene passes, which is
// why the pointer is re-read every time rather than cached.
static uintptr_t battle_main(void) {
    uintptr_t p = (uintptr_t)(g_base + FF132_RVA_BATTLE_MAIN_PTR);
    if (!rd_ok((void*)p, 4)) return 0;
    uintptr_t bm = (uintptr_t)rd32(p);
    return rd_ok((void*)bm, FF132_OFF_BM_TARGET + 4) ? bm : 0;
}

static int battle_flag(uintptr_t bm, int n) {
    uintptr_t p = bm + FF132_OFF_BM_FLAGS + (uintptr_t)(n >> 5) * 4;
    if (!rd_ok((void*)p, 4)) return 0;
    return (int)((rd32(p) >> (n & 31)) & 1);
}

// The game's own isBattleEnd, which is the only definition of "this fight is over" that is not a
// guess: the scene option at flag 3 shifts the threshold.
static int battle_over(uintptr_t bm) {
    int st = (int)rd32(bm + FF132_OFF_BM_STATE);
    return st >= (battle_flag(bm, FF132_BFLAG_END_LATE) ? FF132_BM_STATE_END_LATE : FF132_BM_STATE_END);
}

int ff132_battle_in_battle(void) {
    uintptr_t bm = battle_main();
    return bm && (int)rd32(bm + FF132_OFF_BM_STATE) != 0;
}

// ---- the target and its fields ----------------------------------------------------------------
// `need` = how far into the actor the caller reads off the returned pointer.
uintptr_t ff132_chara_at(uintptr_t bm, uint32_t handle, size_t need) {
    if (handle == 0xFFFFFFFFu) return 0;
    // getCharaByHandle (the slot lookup at 0x48ECB0, which 0x43FB10 also resolves through) indexes
    // the table through cwde: only the low signed 16 bits are the slot, the top half is the
    // handle's serial, and testing the whole 32 bits rejects every live handle.
    int h = (short)(handle & 0xFFFF);
    if (h < 0 || h >= FF132_BM_ACTOR_SLOTS) return 0;
    if (!rd_ok((void*)(bm + FF132_OFF_BM_ACTORS), 4)) return 0;
    uintptr_t tbl = (uintptr_t)rd32(bm + FF132_OFF_BM_ACTORS);
    if (!rd_ok((void*)tbl, (size_t)(h + 1) * 4)) return 0;
    uintptr_t c = (uintptr_t)rd32(tbl + (uintptr_t)h * 4);
    return rd_ok((void*)c, need) ? c : 0;
}
// What the overlay reads of an actor ends with its chain gauge.
#define CHARA_NEED ((size_t)FF132_OFF_CHARA_CHAIN + FF132_OFF_CHAIN_LEFT + 4)

// +0x474 only. Its feeder at +0x46C is NOT a fallback: it keeps a stale handle after +0x474 has
// gone back to -1, which would draw a dead enemy's HP under TARGET (measured).
static uintptr_t target_chara(uintptr_t bm) {
    return ff132_chara_at(bm, rd32(bm + FF132_OFF_BM_TARGET), CHARA_NEED);
}

static int chara_hp(uintptr_t c) {
    return (*(const volatile unsigned char*)(c + FF132_OFF_CHARA_HPFLAG) & 0x80)
               ? (int)rd32(c + FF132_OFF_CHARA_HP_ALT)
               : (int)rd32(c + FF132_OFF_CHARA_HP);
}

static int chara_is_enemy(uintptr_t c) {
    int k = (int)(rd32(c + FF132_OFF_CHARA_KIND) << 16) >> 28;
    // The range test stands in for the game's 8-bit shl al,cl, which answers 0 for a kind outside it.
    return (k >= 0 && k < 8) && ((1 << k) & FF132_CHARA_ENEMY_KINDS);
}

static int chara_atb(uintptr_t c) {
    int atb = (int)rd32(c + FF132_OFF_CHARA_ATB);
    return atb < 0 ? 0 : atb;
}

static int chara_chain_left_ms(uintptr_t c) {
    uintptr_t ch = c + FF132_OFF_CHARA_CHAIN;
    int st   = *(const volatile unsigned char*)(ch + FF132_OFF_CHAIN_STATE);
    int left = (int)rd32(ch + FF132_OFF_CHAIN_LEFT);
    return (st && left > 0) ? left : 0;   // state 0 leaves the ms field stale, not short
}

// ---- paradigm shift -> party-wide ATB refill ---------------------------------------------------
// Milliseconds until the next shift refills the party's ATB; *out <= 0 = the next one does.
// 0 = there is no interval to count against, which is NOT the same as "ready".
static int atb_shift_left_ms(uintptr_t bm, int* out, int* period) {
    uintptr_t pIv = g_base + FF132_RVA_OPT_ATB_TIME;
    if (!rd_ok((void*)pIv, 4)) return 0;
    int iv = (int)rd32(pIv);
    if (iv <= 0) return 0;                  // the battle database has not loaded yet
    if (battle_flag(bm, FF132_BFLAG_EASY)) {
        uintptr_t pCh = g_base + FF132_RVA_EASY_OPT_CH;
        if (!rd_ok((void*)pCh, 4)) return 0;
        iv = (int)((float)iv * *(const volatile float*)pCh);
    }
    *out    = iv - (int)(rd32(bm + FF132_OFF_BM_CLOCK) - rd32(bm + FF132_OFF_BM_OPTSTAMP));
    *period = iv;                           // easy mode scales it, so never hardcode the interval
    return 1;
}

// ---- the REAL timer ----------------------------------------------------------------------------
// Its own clock, off the game's: QPC ticks at the start and the last movement of the fight. The
// fight is followed through GAME rather than through isBattleEnd, which only goes true well after
// the fight is decided.
static LARGE_INTEGER g_qpcFreq;
static LARGE_INTEGER g_realStart, g_realEnd;
static int g_realRunning = 0;
// GAME stops in a cinematic action too, and one can be the finishing blow, so a stall alone does
// not end REAL: it runs on and stops at the later of GAME's last move and the fight being decided.
static LONGLONG g_lastMoveQpc = 0;       // QPC of the last frame GAME advanced
static LONGLONG g_downQpc = 0;           // QPC of the first frame the fight was decided, 0 = not
static int g_sawEnemy = 0;               // an enemy with HP has been in this fight
static int g_stallMax = 0, g_stalls = 0; // for the end-of-fight log line only
static int g_prevGt = 0, g_prevInBattle = 0;
// BattleMain is reused: its GAME still holds the previous fight's figure for a moment after the
// screen appears, then drops to 0. Until it has dropped GAME is neither shown nor followed, or
// that drop reads as movement and REAL runs for a few frames during the intro.
static int g_gtFresh = 0;
static int g_gtStale = 0;                // GAME as the screen came up
static DWORD g_gtMovedAt = 0;            // GetTickCount of the last frame GAME advanced
// The frame GAME is first seen above 0 has already added that frame's time to it: REAL counts from
// that much earlier (in wall time, so divided by the frame speed), or it stays a frame behind GAME
// for the whole fight. More than this is not one frame and is not backed off.
#define FF132_REAL_BACKDATE_MAX_MS 200
// Measured in milliseconds rather than in calls: this runs once per EndScene PASS and FF13-2 makes
// several of those per frame, so a count of calls would trip inside a single frame.
#define FF132_REAL_STALL_MS 250

// After an auto-retry the fight is torn down right after the fade, so the TIME block stays up for
// this long past that with the figures it ended on.
#define FF132_RETRY_HOLD_MS 3000
static volatile int g_holdArmed = 0;     // set on the game thread when this fight is being retried
static DWORD g_holdUntil = 0;            // GetTickCount the held block goes; 0 = none held
static int g_lastGt = 0;                 // GAME as last shown

void ff132_battle_hold_after(void) { g_holdArmed = 1; }

static int battle_held(void) { return g_holdUntil && (int)(g_holdUntil - GetTickCount()) > 0; }

// Verbose only, once a second: the whole target chain plus every actor slot's kind and HP. The
// TARGET rows are the one part of this file that cannot be checked without a fight, and this line
// is what says which link of the chain is empty.
static void battle_diag(uintptr_t bm) {
    if (g_logLevel < 2) return;
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (last && now - last < 1000) return;
    last = now;
    uintptr_t tbl = (uintptr_t)rd32(bm + FF132_OFF_BM_ACTORS);
    char slots[400];
    int n = 0;
    for (int i = 0; i < FF132_BM_ACTOR_SLOTS && n < (int)sizeof slots - 32; i++) {
        uintptr_t c = ff132_chara_at(bm, (uint32_t)i, CHARA_NEED);
        if (!c) continue;
        int k = (int)(rd32(c + FF132_OFF_CHARA_KIND) << 16) >> 28;
        n += snprintf(slots + n, sizeof slots - (size_t)n, " %d:k%d/hp%d%s",
                      i, k, chara_hp(c), chara_is_enemy(c) ? "E" : "");
    }
    slots[n] = 0;
    ff132_logv(FF132_TAG "battle: state=%d gt=%d clock=%d target=%d feed=%d alt=%d actors=%08X"
                  " flags=%08X %08X%s\n",
           (int)rd32(bm + FF132_OFF_BM_STATE), (int)rd32(bm + FF132_OFF_BM_GAMETIME),
           (int)rd32(bm + FF132_OFF_BM_CLOCK), (int)rd32(bm + FF132_OFF_BM_TARGET),
           (int)rd32(bm + FF132_OFF_BM_TARGET_FEED), (int)rd32(bm + FF132_OFF_BM_TARGET_ALT),
           (unsigned)tbl, (unsigned)rd32(bm + FF132_OFF_BM_FLAGS),
           (unsigned)rd32(bm + FF132_OFF_BM_FLAGS + 4), slots);
}

static LONGLONG qpc_now(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

static int qpc_ms(LONGLONG from, LONGLONG to) {
    return (g_qpcFreq.QuadPart > 0 && from) ? (int)((to - from) * 1000 / g_qpcFreq.QuadPart) : -1;
}

// The fight is decided once no enemy has HP left (an enemy must have been seen this fight), a
// script has declared the win (flag 0x5F), or the game has settled it (state 0x26 results / 0x27
// abort). A cinematic-action finish raises only the script win, at the end of the cinematic, with
// the enemy still holding HP and +0x495 still 1 (measured). Many fights never read every enemy at
// HP 0, so the settle states are what stop REAL running on through the victory pose (cinematics
// are 0x29-0x2C).
static const char* g_downWhy = "";
static int g_settled = 0;                // the state has been through 0x26 / 0x27 this fight

static int battle_decided(uintptr_t bm) {
    int alive = 0;
    for (int i = 0; i < FF132_BM_ACTOR_SLOTS; i++) {
        uintptr_t c = ff132_chara_at(bm, (uint32_t)i, CHARA_NEED);
        if (c && chara_is_enemy(c) && chara_hp(c) > 0) alive++;
    }
    if (alive) g_sawEnemy = 1;
    int hpDown = g_sawEnemy && !alive;
    int win = (rd32(bm + FF132_OFF_BM_FLAGS + 8) & FF132_BM40_SCRIPT_WIN) != 0;
    int st = (int)rd32(bm + FF132_OFF_BM_STATE);
    if (st == FF132_BM_STATE_RESULTS || st == FF132_BM_STATE_ABORT) g_settled = 1;
    g_downWhy = hpDown ? "enemies down" : win ? "script win" : g_settled ? "settled" : "";
    return hpDown || win || g_settled;
}

static void real_stop(const char* why) {
    if (!g_realRunning) return;
    int down = g_downQpc && g_downQpc > g_lastMoveQpc;
    g_realEnd.QuadPart = down ? g_downQpc : g_lastMoveQpc;
    g_realRunning = 0;
    ff132_log(FF132_TAG "battle REAL: %d ms by %s (GAME last moved %d ms, decided %d ms,"
                 " stalls %d, longest %d ms)\n",
          qpc_ms(g_realStart.QuadPart, g_realEnd.QuadPart), down ? g_downWhy : why,
          qpc_ms(g_realStart.QuadPart, g_lastMoveQpc),
          g_downQpc ? qpc_ms(g_realStart.QuadPart, g_downQpc) : -1, g_stalls, g_stallMax);
}

void ff132_battle_frame(void) {
    if (!g_qpcFreq.QuadPart) QueryPerformanceFrequency(&g_qpcFreq);
    uintptr_t bm = battle_main();
    int inBattle = bm && (int)rd32(bm + FF132_OFF_BM_STATE) != 0;
    int gt = bm ? (int)rd32(bm + FF132_OFF_BM_GAMETIME) : 0;
    DWORD now = GetTickCount();

    if (!inBattle) {                      // BattleMain is gone: keep the last figure, stop counting
        if (g_prevInBattle) {
            real_stop("battle gone");
            if (g_holdArmed) {
                g_holdArmed = 0;
                g_holdUntil = (now + FF132_RETRY_HOLD_MS) | 1;
                ff132_log(FF132_TAG "battle overlay: TIME held %d ms past the retried fight (GAME %d)\n",
                      FF132_RETRY_HOLD_MS, g_lastGt);
            }
        }
        g_prevInBattle = 0; g_prevGt = 0;
        return;
    }
    // The battle screen is up a moment before its clock starts, so REAL is zeroed on the frame the
    // screen appears -- otherwise the previous fight's figure stands there until GAME moves.
    if (!g_prevInBattle) {
        g_holdUntil = 0;
        g_holdArmed = 0;
        g_gtFresh = 0;
        g_lastGt = 0;
        g_gtStale = gt;
        g_realRunning = 0;
        QueryPerformanceCounter(&g_realStart);
        g_realEnd = g_realStart;
        g_gtMovedAt = now;
        g_lastMoveQpc = g_realStart.QuadPart;
        g_downQpc = 0; g_sawEnemy = 0; g_settled = 0; g_stalls = 0; g_stallMax = 0;
    }
    if (!g_gtFresh) {
        if (gt != 0 && gt >= g_gtStale) { g_prevInBattle = inBattle; return; }
        g_gtFresh = 1;
        g_prevGt = 0;
    }
    if (gt != 0 && g_prevGt == 0) {       // the fight's clock has started
        QueryPerformanceCounter(&g_realStart);
        double fs = ff132_speed_frame_factor();
        int back = (int)(gt / fs + 0.5);
        int ok = back > 0 && back <= FF132_REAL_BACKDATE_MAX_MS;
        if (ok) g_realStart.QuadPart -= (LONGLONG)back * g_qpcFreq.QuadPart / 1000;
        ff132_log(FF132_TAG "battle REAL: starts with GAME at %d ms (frame speed %.2f) -> counted from"
                     " %d ms back%s\n", gt, fs, ok ? back : 0, ok ? "" : " (not one frame -- from now)");
        g_realEnd = g_realStart;
        g_lastMoveQpc = g_realStart.QuadPart;
        g_realRunning = 1;
        g_gtMovedAt = now;
    }
    if (!gt) {                            // the fight's clock has not started yet
        g_prevInBattle = inBattle; g_prevGt = gt; battle_diag(bm);
        return;
    }
    g_lastGt = gt;
    LONGLONG q = qpc_now();
    int stalled = (now - g_gtMovedAt) > FF132_REAL_STALL_MS;
    if (gt != g_prevGt) {
        // GAME moved, so the fight is still running; g_realStart is deliberately left alone --
        // REAL is the whole fight's wall clock, pause and cinematic actions included.
        int was = (int)(now - g_gtMovedAt);
        if (was > FF132_REAL_STALL_MS) { g_stalls++; if (was > g_stallMax) g_stallMax = was; }
        g_lastMoveQpc = q;
        g_gtMovedAt = now;
        g_realRunning = 1;
        stalled = 0;
    }
    if (battle_decided(bm)) { if (!g_downQpc) g_downQpc = q; }
    else g_downQpc = 0;
    if (g_realRunning && stalled && g_downQpc) real_stop("GAME stopped");
    if (g_realRunning && battle_over(bm)) real_stop("battle end");
    g_prevInBattle = inBattle;
    g_prevGt = gt;
    battle_diag(bm);
}

// ---- the toggle ---------------------------------------------------------------------------------
volatile unsigned char g_battleOverlay = 1;

void ff132_battle_toggle(void) {
    g_battleOverlay = g_battleOverlay ? 0 : 1;
    ff132_log(FF132_TAG "battle overlay: %s\n", g_battleOverlay ? "ON" : "OFF");
    if (g_battleOverlay) ff132_flash_c(ui_text(T_BATTLE_OVL_ON), FF132_COL_ON);
    else                 ff132_flash(ui_text(T_BATTLE_OVL_OFF));
    ff132_state_save();
}

int ff132_battle_visible(void) {
    return g_battleOverlay && (ff132_battle_in_battle() || battle_held());
}

// ---- drawing -------------------------------------------------------------------------------------
#define FF132_BATTLE_COL    18   // where a figure ENDS, in characters from x
#define FF132_BATTLE_INDENT 2    // ...and where a row's label starts, under its heading
#define FF132_OVL_BATTLE      0xFFFFFFFF   // the figures and their labels
#define FF132_OVL_BATTLE_HEAD 0xFFC4CFCF   // ...and the TIME / TARGET headings

static void battle_head(int x, int y, const char* label) {
    ff132_ovl_say(x, y, label, FF132_OVL_BATTLE_HEAD);
}

// The label is drawn from the left and the figure from the right edge of the column, so the digits
// stay under each other as they change; the figure is ASCII either way, so the monospaced path
// places it exactly.
static void battle_row(int x, int cw, int y, const char* label, const char* value) {
    ff132_ovl_say(x + FF132_BATTLE_INDENT * cw, y, label, FF132_OVL_BATTLE);
    ff132_ovl_ascii(x + (FF132_BATTLE_COL - (int)strlen(value)) * cw, y, value, FF132_OVL_BATTLE);
}

static void fmt_mmsscc(int ms, char* out, size_t n) {
    if (ms < 0) ms = 0;
    int m = ms / 60000, rem = ms % 60000, s = rem / 1000, cc = (rem % 1000) / 10;
    snprintf(out, n, "%d:%02d.%02d", m, s, cc);
}

int ff132_battle_draw(int x, int y) {
    uintptr_t bm = battle_main();
    int live = bm && (int)rd32(bm + FF132_OFF_BM_STATE) != 0;
    if (!live && !battle_held()) return y;
    int cw = ff132_ovl_char_w();
    if (!cw) return y;

    char t[24], lbl[24];
    battle_head(x, y, ui_text(T_BATTLE_HEAD_TIME)); y += FF132_OVL_LINE_H;
    fmt_mmsscc(!live ? g_lastGt : g_gtFresh ? (int)rd32(bm + FF132_OFF_BM_GAMETIME) : 0, t, sizeof t);
    battle_row(x, cw, y, ui_text(T_TIMER_GAME), t); y += FF132_OVL_LINE_H;

    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // Once decided, REAL shows only up to GAME's last move: it is where the figure will settle, so
    // showing the wall clock past it would make the figure run on and then jump back.
    LONGLONG end = !g_realRunning ? g_realEnd.QuadPart
                 : g_downQpc ? (g_downQpc > g_lastMoveQpc ? g_downQpc : g_lastMoveQpc) : now.QuadPart;
    int rms = (g_qpcFreq.QuadPart > 0)
                  ? (int)((end - g_realStart.QuadPart) * 1000 / g_qpcFreq.QuadPart) : 0;
    fmt_mmsscc(rms, t, sizeof t);
    battle_row(x, cw, y, ui_text(T_TIMER_REAL), t); y += FF132_OVL_LINE_H;
    if (!live) return y;

    int left = 0, period = 0;
    if (atb_shift_left_ms(bm, &left, &period)) {
        // The trailing space is load-bearing: the row is right-aligned as a whole and GAME / REAL
        // carry two decimals where this carries one -- without it the points misalign.
        if (left <= 0) snprintf(t, sizeof t, "%s", ui_text(T_ATB_READY));
        else           snprintf(t, sizeof t, "%d.%d ", left / 1000, (left % 1000) / 100);
        snprintf(lbl, sizeof lbl, ui_text(T_ATB_SHIFT), (period + 500) / 1000);
        battle_row(x, cw, y, lbl, t); y += FF132_OVL_LINE_H;
    }

    // The player can target a party member, so the enemy test is what keeps the group to enemies.
    uintptr_t c = target_chara(bm);
    if (!c || !chara_is_enemy(c)) return y;
    int hp = chara_hp(c);
    if (hp <= 0) return y;
    battle_head(x, y, ui_text(T_BATTLE_HEAD_TARGET)); y += FF132_OVL_LINE_H;
    snprintf(t, sizeof t, "%d", hp);
    battle_row(x, cw, y, ui_text(T_ENEMY_HP), t); y += FF132_OVL_LINE_H;
    // 1000 raw = one gauge segment (the unit the player's own bar shows); 100 raw = one ability.
    int atb = chara_atb(c);
    snprintf(t, sizeof t, "%d.%d ", atb / 1000, (atb % 1000) / 100);
    battle_row(x, cw, y, ui_text(T_ENEMY_ATB), t); y += FF132_OVL_LINE_H;
    int chain = chara_chain_left_ms(c);
    snprintf(t, sizeof t, "%d.%d ", chain / 1000, (chain % 1000) / 100);
    battle_row(x, cw, y, ui_text(T_CHAIN_LEFT), t); y += FF132_OVL_LINE_H;
    return y;
}
