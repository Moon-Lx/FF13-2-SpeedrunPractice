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

// Kill mode: instant win (enemies kept out at BattleMain::activate) and one-hit kill (an enemy's
// damage made lethal at addHp). Addresses in ff132_addrs.h.
//
// ff132_kill_toggle runs on the hotkey worker and only moves g_mode. Both hooks and ff132_kill_pump run
// on the game thread and own every counter here. The status line is drawn on the render thread.
#include "ff132_internal.h"
#include "ff132_addrs.h"
#include "ff13_hook.h"

#define FF132_COL_KILL 0xFFFF3B30              // red-orange, as FF13's kill line
#define KILL_STALL_MS 15000

static volatile int g_mode = FF132_KILL_OFF;   // the player's choice; what the state file keeps
static volatile int g_gateOk = 0;           // the activate hook is in (instant win can act)
static volatile int g_hpOk = 0;             // the addHp hook is in (one-hit kill can act)

// Game thread only. Reset when a fight drops back to state 0/1.
static int g_blocked = 0, g_passed = 0, g_onehit = 0;
static int g_modeSeen = 0;                  // the mode was not OFF at some point in this fight
static int g_inFight = 0;
static DWORD g_firstBlockAt = 0;
static int g_stallLogged = 0;
static int g_lastScene = -1, g_lastResult = -1, g_lastEnemies = -1;

static int rd8s(uintptr_t p) {
    return (int)*(const volatile signed char*)p;
}

static const char* mode_name_en(int m) {
    return ui_text_en(m == FF132_KILL_INSTANT ? T_KILL_INSTANT
                      : m == FF132_KILL_ONEHIT ? T_KILL_ONEHIT : T_KILL_OFF);
}
static const char* mode_name(int m) {
    return ui_text(m == FF132_KILL_INSTANT ? T_KILL_INSTANT
                   : m == FF132_KILL_ONEHIT ? T_KILL_ONEHIT : T_KILL_OFF);
}

static int chara_kind(uintptr_t c) { return (int)(rd32(c + FF132_OFF_CHARA_KIND) << 16) >> 28; }
static int chara_nib24(uintptr_t c) { return (int)(rd32(c + FF132_OFF_CHARA_KIND) << 4) >> 28; }
static int chara_pckind(uintptr_t c) { return rd8s(c + FF132_OFF_CHARA_PCKIND); }

// ---- instant win: BattleMain::activate(handle, flag) --------------------------------------------
// Only kinds 1/5 are what [BM+0x495] counts; kind 7 is "enemy" to isEnemy but never decides a fight.
// nib24 outside {0,1} is activated by the game even when not resident, so it is left alone too.
static int __cdecl on_activate(uint32_t ecx, uint32_t esp, int* ret) {
    if (g_mode != FF132_KILL_INSTANT) return 0;
    const uint32_t* a = (const uint32_t*)esp;
    uint32_t handle = a[1], flag = a[2];
    if (!flag) return 0;
    uintptr_t bm = (uintptr_t)ecx;
    uintptr_t c = ff132_chara_at(bm, handle, FF132_OFF_CHARA_HP_FLOOR + 4);
    if (!c) {
        ff132_logv(FF132_TAG "kill: activate passed handle=%08X flag=%u (no chara)\n", (unsigned)handle,
               (unsigned)flag);
        return 0;
    }
    int kind = chara_kind(c), nib = chara_nib24(c);
    int block = (kind == 1 || kind == 5) && (nib == 0 || nib == 1);
    int st = rd_ok((void*)(bm + FF132_OFF_BM_ENEMIES_LEFT), 1) ? (int)rd32(bm + FF132_OFF_BM_STATE) : -1;
    int e495 = st >= 0 ? rd8s(bm + FF132_OFF_BM_ENEMIES_LEFT) : -1;
    (block ? ff132_log : ff132_logv)(FF132_TAG "kill: activate %s handle=%08X flag=%u chara=%08X id=%d"
                                    " kind=%d pcKind=%d nib24=%d raw2A8=%08X bm_st=%d e495=%d\n",
                             block ? "BLOCKED" : "passed", (unsigned)handle, (unsigned)flag,
                             (unsigned)c, (int)rd32(c + FF132_OFF_CHARA_ID), kind, chara_pckind(c),
                             nib, (unsigned)rd32(c + FF132_OFF_CHARA_KIND), st, e495);
    g_modeSeen = 1;
    if (!block) {
        if (kind == 1 || kind == 5) g_passed++;
        return 0;
    }
    if (!g_blocked++) g_firstBlockAt = GetTickCount();
    *ret = 0;
    return 1;
}

// ---- one-hit kill: BattleChara::addHp(delta, force) ---------------------------------------------
// Only the delta is changed, so the game's own clamps (HP floor, flag 0x58, debug invincibility)
// still decide what is written. Shadow HP (+0x286 bit 7) is a cinematic's stand-in: left alone.
static int __cdecl on_addhp(uint32_t ecx, uint32_t esp, int* ret) {
    (void)ret;
    if (g_mode == FF132_KILL_OFF) return 0;
    int32_t* a = (int32_t*)esp;
    int delta = a[1];
    if (delta >= 0) return 0;
    uintptr_t c = (uintptr_t)ecx;
    int kind = chara_kind(c);
    if (kind != 1 && kind != 5) return 0;
    if (*(const volatile uint8_t*)(c + FF132_OFF_CHARA_HPFLAG) & 0x80) return 0;
    uintptr_t bm = (uintptr_t)rd32(g_base + FF132_RVA_KILL_BM_COPY);
    if (bm && (rd32(bm + FF132_OFF_BM_FLAGS + 4) & FF132_BM3C_NO_HP)) return 0;
    a[1] = FF132_KILL_LETHAL_DELTA;
    g_onehit++;
    g_modeSeen = 1;
    ff132_logv(FF132_TAG "kill: one-hit chara=%08X id=%d kind=%d pcKind=%d hp=%d floor=%d f289=%02X"
                  " force=%d from=%08X delta %d -> %d\n",
           (unsigned)c, (int)rd32(c + FF132_OFF_CHARA_ID), kind, chara_pckind(c),
           (int)rd32(c + FF132_OFF_CHARA_HP), (int)rd32(c + FF132_OFF_CHARA_HP_FLOOR),
           (unsigned)*(const volatile uint8_t*)(c + FF132_OFF_CHARA_FLAGS289), (int)a[2],
           (unsigned)a[0], delta, (int)a[1]);
    return 0;
}

// ---- install (worker) -------------------------------------------------------------------------

static void demote(void) {
    int m = g_mode;
    if (m == FF132_KILL_INSTANT && !g_gateOk) m = g_hpOk ? FF132_KILL_ONEHIT : FF132_KILL_OFF;
    if (m == FF132_KILL_ONEHIT && !g_hpOk) m = FF132_KILL_OFF;
    if (m != g_mode) {
        ff132_loga(FF132_TAG "kill mode: restored %s is not installed -> %s\n", mode_name_en(g_mode),
               mode_name_en(m));
        g_mode = m;
        ff132_state_save();
    }
}

void ff132_kill_init(void) {
    uintptr_t at = g_base + FF132_RVA_KILL_ACTIVATE;
    if (wait_for_bytes(at, FF132_SIG_KILL_ACTIVATE, (int)sizeof FF132_SIG_KILL_ACTIVATE,
                       FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "kill mode: activate @0x%08X mismatch -> instant win off\n", (unsigned)at);
    } else {
#define KS(n) { FF132_RVA_KILL_##n, FF132_SIG_KILL_##n, sizeof FF132_SIG_KILL_##n }
        static const struct { uintptr_t rva; const uint8_t* sig; size_t n; } k[] = {
            KS(ACT_RESIDENT), KS(ACT_NIB), KS(ACT_FAILLOG), KS(NIB_FN), KS(PCKIND_FN),
        };
#undef KS
        int ok = 1;
        for (int i = 0; i < (int)(sizeof k / sizeof k[0]) && ok; i++)
            if (!sig_at(k[i].rva, k[i].sig, k[i].n)) {
                ff132_loga(FF132_TAG "kill mode: site @0x%08X mismatch -> instant win off\n",
                       (unsigned)(g_base + k[i].rva));
                ok = 0;
            }
        void* cave = ok ? ff13_install_replace_hook(at, FF132_SIG_KILL_ACTIVATE,
                                                    FF132_KILL_ACTIVATE_PROLOGUE, (void*)on_activate,
                                                    FF132_RET_KILL_ACTIVATE)
                        : NULL;
        if (ok && !cave)
            ff132_loga(FF132_TAG "kill mode: activate @0x%08X not patched -> instant win off\n",
                   (unsigned)at);
        if (cave) {
            g_gateOk = 1;
            ff132_loga(FF132_TAG "kill mode: activate @0x%08X replaced, cave=%p\n", (unsigned)at, cave);
        }
    }
    uintptr_t hp = g_base + FF132_RVA_KILL_ADDHP;
    if (wait_for_bytes(hp, FF132_SIG_KILL_ADDHP, (int)sizeof FF132_SIG_KILL_ADDHP, FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "kill mode: addHp @0x%08X mismatch -> one-hit kill off\n", (unsigned)hp);
    } else {
        void* cave = ff13_install_replace_hook(hp, FF132_SIG_KILL_ADDHP, FF132_KILL_ADDHP_PROLOGUE,
                                               (void*)on_addhp, FF132_RET_KILL_ADDHP);
        if (!cave) {
            ff132_loga(FF132_TAG "kill mode: addHp @0x%08X not patched -> one-hit kill off\n",
                   (unsigned)hp);
        } else {
            g_hpOk = 1;
            ff132_loga(FF132_TAG "kill mode: addHp @0x%08X hooked, cave=%p\n", (unsigned)hp, cave);
        }
    }
    demote();
    ff132_loga(FF132_TAG "kill mode: instant win %s, one-hit kill %s -- starts %s\n",
           g_gateOk ? "ok" : "MISSING", g_hpOk ? "ok" : "MISSING", mode_name_en(g_mode));
}

// ---- per battle (game thread, frame site) -------------------------------------------------------
void ff132_kill_pump(void) {
    uintptr_t p = g_base + FF132_RVA_BATTLE_MAIN_PTR;
    uintptr_t bm = rd_ok((void*)p, 4) ? (uintptr_t)rd32(p) : 0;
    if (bm && !rd_ok((void*)bm, FF132_OFF_BM_SCENE + 4)) bm = 0;
    int st = bm ? (int)rd32(bm + FF132_OFF_BM_STATE) : 0;
    // State 1 is where a fight starts and also where BattleMain can rest between fights.
    int running = st >= 2;
    if (running) {
        g_inFight = 1;
        if (g_mode != FF132_KILL_OFF) g_modeSeen = 1;
        g_lastScene = (int)rd32(bm + FF132_OFF_BM_SCENE);
        g_lastResult = (int)rd32(bm + FF132_OFF_BM_RESULT);
        g_lastEnemies = rd8s(bm + FF132_OFF_BM_ENEMIES_LEFT);
        if (g_blocked && !g_stallLogged && (st == 6 || st == 7) && g_lastEnemies <= 0
            && GetTickCount() - g_firstBlockAt > KILL_STALL_MS) {
            g_stallLogged = 1;
            uint32_t f40 = rd32(bm + FF132_OFF_BM_FLAGS + 8);
            ff132_log(FF132_TAG "kill mode: instant win emptied this battle but it is not decided after"
                         " %d s: bm_st=%d sub=%d e495=%d w499=%d fe0=%d f40=%08X flag5A=%d"
                         " scene=%d (pause menu Retry/Quit gets out)\n",
                  KILL_STALL_MS / 1000, st, (int)rd32(bm + FF132_OFF_BM_SUBSTATE), g_lastEnemies,
                  rd8s(bm + FF132_OFF_BM_PENDING), (int)rd32(bm + FF132_OFF_BM_WAVES), (unsigned)f40,
                  (f40 & FF132_BM40_NO_WIPE_END) ? 1 : 0, g_lastScene);
        }
        return;
    }
    if (!g_inFight) return;
    if (g_modeSeen || g_blocked || g_onehit)
        ff132_log(FF132_TAG "kill mode: battle end: mode=%s blocked=%d passed=%d onehit=%d scene=%d"
                     " result=%d e495=%d\n",
              mode_name_en(g_mode), g_blocked, g_passed, g_onehit, g_lastScene, g_lastResult,
              g_lastEnemies);
    g_inFight = 0;
    g_blocked = g_passed = g_onehit = 0;
    g_modeSeen = 0;
    g_stallLogged = 0;
    g_lastScene = g_lastResult = g_lastEnemies = -1;
}

int ff132_kill_emptied(void) { return g_blocked > 0; }

// ---- the toggle (worker) ----------------------------------------------------------------------
void ff132_kill_toggle(void) {
    if (!g_gateOk && !g_hpOk) {
        ff132_log(FF132_TAG "kill mode: not installed\n");
        ff132_flash(ui_text(T_KILL_NA));
        return;
    }
    int m = g_mode;
    // Strongest first; a mode whose hook is missing is stepped over.
    m = m == FF132_KILL_OFF ? FF132_KILL_INSTANT : m == FF132_KILL_INSTANT ? FF132_KILL_ONEHIT : FF132_KILL_OFF;
    if (m == FF132_KILL_INSTANT && !g_gateOk) m = FF132_KILL_ONEHIT;
    if (m == FF132_KILL_ONEHIT && !g_hpOk) m = FF132_KILL_OFF;
    g_mode = m;
    ff132_log(FF132_TAG "kill mode: %s%s\n", mode_name_en(m),
          m == FF132_KILL_INSTANT && !g_hpOk ? " (one-hit kill NOT INSTALLED, instant win only)" : "");
    if (m == FF132_KILL_OFF) ff132_flash(ui_text(T_KILL_OFF_FLASH));   // ON is said by the status line
    ff132_state_save();
}

int ff132_kill_mode(void) { return g_mode; }
// ff132_state_load, before ff132_kill_init (which demotes a mode whose hook did not go in).
void ff132_kill_set(int v) {
    g_mode = (v == FF132_KILL_INSTANT || v == FF132_KILL_ONEHIT) ? v : FF132_KILL_OFF;
    if (g_mode != FF132_KILL_OFF)
        ff132_loga(FF132_TAG "kill mode: restored %s (WARNING: this is carried over from a previous"
                      " session)\n", mode_name_en(g_mode));
}

// ---- the status line (render thread) ----------------------------------------------------------
int ff132_kill_visible(void) { return (g_gateOk || g_hpOk) && g_mode != FF132_KILL_OFF; }

int ff132_kill_draw(int x, int y) {
    if (!ff132_kill_visible()) return y;
    char key[32], buf[128];
    vk_label(g_killModeVk, key, sizeof key);
    snprintf(buf, sizeof buf, ui_text(T_KILL_LINE), key, mode_name(g_mode));
    ff132_ovl_say(x, y, buf, FF132_COL_KILL);
    return y + FF132_OVL_LINE_H;
}
