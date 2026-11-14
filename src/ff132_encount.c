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

// Random-encounter suppression: one bit in the AutoEncount manager's suppress mask, described in
// ff132_addrs.h.
//
// ff132_encount_toggle runs on the hotkey worker and only moves g_want; ff132_encount_pump runs on the game
// thread and is the only writer of the mask. ff132_encount_visible/ff132_encount_draw (render thread) read flags.
#include "ff132_internal.h"
#include "ff132_addrs.h"

#define FF132_COL_ENCOUNT 0xFFC772EB           // violet, as FF13's enemy-suppress line

static volatile int g_want = 0;             // the player's choice; what the state file keeps
static volatile int g_siteOk = 0;           // the signature matched, so the mask may be written
static int g_set = 0;                       // game thread: our bit is (or was last frame) in the mask

void ff132_encount_init(void) {
    uintptr_t at = g_base + FF132_RVA_ENCOUNT_MASKSET;
    if (wait_for_bytes(at, FF132_SIG_ENCOUNT_MASKSET, (int)sizeof FF132_SIG_ENCOUNT_MASKSET,
                       FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "encounter suppression: site @0x%08X mismatch -> feature off\n",
               (unsigned)at);
        return;
    }
    g_siteOk = 1;
    ff132_loga(FF132_TAG "encounter suppression: site @0x%08X ok -- starts %s\n", (unsigned)at,
           g_want ? "ON" : "OFF");
}

// Game thread, once per frame. The game's own writes to the mask are unlocked read-modify-writes
// on this same thread, so a lost bit can only come from its zone-change reset; the next frame re-sets it.
void ff132_encount_pump(void) {
    if (!g_siteOk) return;
    uint8_t* m = *(uint8_t* volatile*)FF132_VA_AUTOENC_MGR;
    if (!m) return;
    volatile LONG* mask = (volatile LONG*)(m + FF132_OFF_AUTOENC_MASK);
    if (g_want) {
        LONG was = *mask;
        if (!(was & FF132_AUTOENC_MASK_OURS)) {
            InterlockedOr(mask, FF132_AUTOENC_MASK_OURS);
            ff132_log(FF132_TAG "encounter suppression: mask bit set (mask was 0x%X)\n", (unsigned)was);
        }
        g_set = 1;
    } else if (g_set) {
        InterlockedAnd(mask, ~(LONG)FF132_AUTOENC_MASK_OURS);
        g_set = 0;
        ff132_log(FF132_TAG "encounter suppression: mask bit cleared\n");
    }
}

void ff132_encount_toggle(void) {
    if (!g_siteOk) {
        ff132_log(FF132_TAG "encounter suppression: not installed\n");
        ff132_flash(ui_text(T_ENC_NA));
        return;
    }
    int on = !g_want;
    g_want = on;
    ff132_log(FF132_TAG "encounter suppression: %s\n", on ? "ON" : "OFF");
    if (!on) ff132_flash(ui_text(T_ENC_OFF));   // ON is said by the status line alone
    ff132_state_save();
}

int ff132_encount_on(void) { return g_want; }
void ff132_encount_set(int v) {
    g_want = v ? 1 : 0;
    if (g_want) ff132_log(FF132_TAG "encounter suppression: restored ON\n");
}

// ---- the status line (render thread) ----------------------------------------------------------
int ff132_encount_visible(void) { return g_siteOk && g_want; }

int ff132_encount_draw(int x, int y) {
    if (!ff132_encount_visible()) return y;
    char key[32], buf[64];
    vk_label(g_enemySuppressVk, key, sizeof key);
    snprintf(buf, sizeof buf, ui_text(T_ENC_ON), key);
    ff132_ovl_say(x, y, buf, FF132_COL_ENCOUNT);
    return y + FF132_OVL_LINE_H;
}
