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

// The number of save slots the game offers: [save] slot_count, written into the game's own count.
//
// Threads: the tick and every function that reads the disk run on the hotkey worker; the game's own
// count and the pending flag are read from the game thread too (the working slot's default).
#include "ff132_internal.h"
#include "ff132_addrs.h"

static volatile int g_gameSlots = 0;   // what WinMain wrote, recorded before any write of ours
static volatile int g_dirty = 1;       // an evaluation is owed (startup counts as one)
static volatile int g_applied = 0;     // the first evaluation has run
static int g_cloudSaid = 0, g_oddSaid = 0;

// The save list shows count + 1 rows ("new save"), so FF132_SLOTS_MAX needs this many.
#define WIDE_ROWS (FF132_SLOTS_MAX + 1)

int ff132_slots_game(void) {
    int g = g_gameSlots;
    if (g > 0) return g;
    int n = save_slot_count();
    // Recorded before the first write, so a count read as ours here means the record is in too.
    g = g_gameSlots;
    return g > 0 ? g : n;
}

int ff132_slots_pending(void) { return g_dirty || !g_gameSlots; }

int ff132_slots_last(void) {
    if (!g_applied) return 0;
    int cur = save_slot_count(), set = g_slotCount;
    if (cur <= 0) return 0;
    if (g_gameSlots != FF132_SLOTS_GAME) return cur;
    return (set >= 2 && set < cur) ? set : cur;
}

void ff132_slots_mark_dirty(void) { g_dirty = 1; }

int ff132_slots_unavailable(void) {
    if (steam_cloud_on() == 1) return 1;
    int own = ff132_slots_game();
    return (own > 0 && own != FF132_SLOTS_GAME) ? 2 : 0;
}

int ff132_slots_reach(void) {
    if (!ff132_slots_unavailable() && ff132_slots_game() == FF132_SLOTS_GAME)
        return ff132_listgui_can(WIDE_ROWS) ? FF132_SLOTS_MAX : FF132_SLOTS_MAX_NOGUI;
    int cur = save_slot_count();
    return cur > 0 ? cur : FF132_SLOTS_GAME;
}

int ff132_slots_needed(void) {
    int own = ff132_slots_game();
    if (own <= 0) own = FF132_SLOTS_GAME;
    int hi = ff132_cats_scan_numbers(0, NULL, NULL, 0);
    if (hi < -1) return -1;
    return hi + 1 > own ? hi + 1 : own;
}

int ff132_slots_saves_from(int from) {
    int above = 0;
    return ff132_cats_scan_numbers(from, &above, NULL, 0) < -1 ? -1 : above;
}

// A save numbered at or past the count makes the game abort the moment a list that enumerates it
// opens, and nothing short of moving the file helps.
static void warn_past(int count, int hi, int above, const char* where) {
    ff132_loga(FF132_TAG "slots: WARNING %d save(s) numbered %d or higher (highest ff13_2-%02d.dat in %s)"
                  " -- the game can list %d: a save or load list that reaches one closes the game."
                  " Move them below %d.\n", above, count, hi, where[0] ? where : "?", count, count);
}

static void slots_apply(void) {
    int own = g_gameSlots, cur = save_slot_count(), set = g_slotCount;
    int cloud = steam_cloud_on();
    int fixed = own != FF132_SLOTS_GAME || cloud == 1;
    int limit = fixed ? cur : FF132_SLOTS_MAX;
    char where[MAX_PATH] = "";
    int above = 0;
    int hi = ff132_cats_scan_numbers(limit, &above, where, sizeof where);
    if (fixed) {
        if (own != FF132_SLOTS_GAME && !g_oddSaid) {
            g_oddSaid = 1;
            ff132_loga(FF132_TAG "slots: the game keeps %d slots, not %d -- the count is left alone"
                          " (setting %d, highest save on disk %d)\n", own, FF132_SLOTS_GAME, set, hi);
        } else if (own == FF132_SLOTS_GAME && !g_cloudSaid) {
            g_cloudSaid = 1;
            ff132_loga(FF132_TAG "slots: Steam Cloud is on -- the slot count stays at %d (the game's own"
                          " %d, setting %d, highest save on disk %d)\n", cur, own, set, hi);
        }
        if (above > 0) warn_past(limit, hi, above, where);
        return;
    }
    g_cloudSaid = 0;
    int want = set > own ? set : own;
    if (hi < -1) {
        // Nothing on disk could be counted, so whatever the count covers now has to stay covered.
        if (cur > want) want = cur;
        ff132_loga(FF132_TAG "slots: the save folder could not be resolved -- the count is not lowered\n");
    } else if (hi + 1 > want) {
        want = hi + 1;
    }
    if (want > limit) want = limit;
    // The GUI is widened before any count past its rows is written: a list built for more saves
    // than it has rows would index past its arrays.
    if (want > ff132_listgui_rows() - 1) {
        if (ff132_save_list_up() || ff132_list_gui_recent()) { g_dirty = 1; return; }
        if (!ff132_listgui_apply(WIDE_ROWS)) {
            int cap = ff132_listgui_rows() - 1;
            ff132_loga(FF132_TAG "slots: the save list could not be widened to %d rows -- the count is held"
                          " at %d (setting %d, wanted %d)\n", WIDE_ROWS, cap, set, want);
            ff132_flash(ui_text(T_SLOTS_GUI_FAIL));
            want = cap;
            limit = cap;
            above = 0;
            hi = ff132_cats_scan_numbers(limit, &above, where, sizeof where);
        }
    }
    if (above > 0) warn_past(limit, hi, above, where);
    if (want == cur) {
        ff132_log(FF132_TAG "slots: game count %d kept (setting %d, highest save on disk %d, cloud %d)\n",
              cur, set, hi, cloud);
        return;
    }
    // Never while a list is open: its entry array was sized from the old count. Asked again right
    // before the store because the scan above took time.
    if (ff132_save_list_up()) { g_dirty = 1; return; }
    volatile int* p = (volatile int*)(g_base + FF132_RVA_MAX_SLOTS);
    if (IsBadWritePtr((void*)p, sizeof *p)) {
        ff132_loga(FF132_TAG "slots: the game's count is not writable -- left at %d\n", cur);
        return;
    }
    *p = want;
    ff132_loga(FF132_TAG "slots: game count %d -> %d (setting %d, highest save on disk %d, cloud %d)%s\n",
           cur, want, set, hi, cloud,
           want > set ? " -- raised past the setting to cover the saves on disk" : "");
}

void ff132_slots_tick(void) {
    if (!g_gameSlots) {
        int n = save_slot_count();
        if (!n) return;                       // WinMain has not written it yet
        g_gameSlots = n;
        ff132_log(FF132_TAG "slots: the game's own count is %d\n", n);
    }
#ifdef FF132_LISTGUI_TEST_N50
    // Test build: the arrays moved, 50 rows kept -- meant to look exactly like the game's own list.
    static int tested = 0;
    if (!tested && !ff132_save_list_up() && !ff132_list_gui_recent()) {
        tested = 1;
        ff132_listgui_apply(FF132_LG_OLD_ROWS);
    }
#endif
    if (!g_dirty || ff132_save_list_up()) return;
    g_dirty = 0;
    slots_apply();
    g_applied = 1;
}
