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

// The dinput8 proxy entry point and the hotkey worker; each feature lives in its own ff132_*.c.
#include "ff132_internal.h"

// ---- proxy: forward DirectInput8Create to the real system dinput8 ----
// The full System32 path is required: a bare name would re-enter our own app-dir copy.
typedef HRESULT (WINAPI *DI8C_t)(HINSTANCE, DWORD, const void*, void**, void*);
static DI8C_t g_real = NULL;

__declspec(dllexport) HRESULT WINAPI DirectInput8Create(HINSTANCE hinst, DWORD ver,
                                                        const void* riid, void** out, void* punk) {
    if (!g_real) {
        char p[MAX_PATH];
        UINT n = GetSystemDirectoryA(p, MAX_PATH);
        if (n && n < MAX_PATH - 16) {
            snprintf(p + n, sizeof(p) - n, "\\dinput8.dll");
            HMODULE m = LoadLibraryA(p);
            if (m) g_real = (DI8C_t)GetProcAddress(m, "DirectInput8Create");
        }
    }
    if (!g_real) return (HRESULT)0x80004005L; // E_FAIL
    HRESULT hr = g_real(hinst, ver, riid, out, punk);
    // Wrapped so a menu of ours can silence the game's reads while it is up.
    if (hr >= 0 && out && *out) ff132_di_wrap(*out);
    return hr;
}

// ---- key gestures ----
// Every timing here is a multiple of the poll tick (HK_POLL_MS): the tick is the resolution, so a
// repeat can never be faster than the poll and a key held for less than one tick can be missed.
#define HK_REPEAT_DELAY_MS 400
#define HK_REPEAT_RATE_MS   80
// 1 on the key's RELEASE: a menu opened by the press would otherwise see the same press inside it.
typedef struct { int was; } hk_tap;
static int hk_tap_up(UINT b, hk_tap* s) {
    if (!b) return 0;
    int down = hk_down(b);
    int r = !down && s->was;
    s->was = down;
    return hk_active() ? r : 0;
}

// The repeat is re-dated from now, not from the missed due time: the clock keeps running while
// the game is in the background, and a key still held on alt-tab back must not burst through
// what it "owed".
typedef struct { int was; DWORD due; } hk_rep;
static int hk_held(UINT b, hk_rep* s, DWORD now) {
    if (!b) return 0;
    int down = hk_down(b);
    int fire = 0;
    if (down && !s->was) {
        s->due = now + HK_REPEAT_DELAY_MS;
        fire = 1;
    } else if (down && (int)(now - s->due) >= 0) {
        s->due = now + HK_REPEAT_RATE_MS;
        fire = 1;
    }
    s->was = down;
    return fire && hk_active();
}

// The gate must not lift under a key the game acts on that is still physically down: the game's
// next DirectInput read would see it as freshly pressed, and over the load list a confirm LOADS the
// save under the cursor. Asked of the RAW key -- not hk_down, whose exact-modifier test calls a
// modified binding "released" the moment the modifier goes up with the key still down.
static int gate_keys_down(void) {
    const UINT k[] = { g_menuOkVk, g_saveTidyVk, g_cancelVk };
    for (int i = 0; i < (int)(sizeof k / sizeof k[0]); i++)
        if (k[i] && (GetAsyncKeyState((int)hk_key(k[i])) & 0x8000)) return 1;
    return 0;
}
static volatile int   g_liftWait = 0;
static volatile DWORD g_liftWaitAt = 0;   // deadline base: a stuck key must not seal the player in
// All three keys, not the one that ended the menu: a screen change ends one with no trigger at all.
void menu_gate_release(void) {
    if (gate_keys_down()) {
        // Dated from the FIRST wait: ff132_tidy_screen_tick calls this on every screen change, and
        // re-stamping would push the stuck-key deadline out for as long as screens keep changing.
        if (!g_liftWait) g_liftWaitAt = GetTickCount();
        g_liftWait = 1;
        ff132_di_gate_arm();              // sealed until the worker sees them up
    } else {
        g_liftWait = 0;
        ff132_di_gate_lift();
    }
}

// ---- the hotkey worker ----
static DWORD WINAPI worker(LPVOID unused) {
    (void)unused;
    g_base = (uintptr_t)GetModuleHandleA(NULL);
    ff132_loga(FF132_TAG "worker start. imagebase=%p\n", (void*)g_base);
    config_load();
    ff132_crash_install();
    // Before the hooks: the frame hook applies the restored game-speed step from its first frame.
    ff132_state_load();
    save_install_hooks();
    ff132_load_install();
    ff132_encount_init();
    ff132_kill_init();
    ff132_autoretry_init();
    ff132_tidy_install();
    ff132_overlay_install();

    int wasSlot = 0;
    int wasBattle = 0;
    int wasSpeed = 0;
    int wasEnc = 0;
    int wasKill = 0;
    int wasArAuto = 0;
    int wasHelp = 0;
    int wasOk = 0, wasCancel = 0;
    hk_tap tidyKey = { 0 };
    hk_rep mUp = { 0, 0 }, mDown = { 0, 0 }, mLeft = { 0, 0 }, mRight = { 0, 0 };
    hk_rep lPrev = { 0, 0 }, lNext = { 0, 0 };
    DWORD crashRearmAt = 0;
    for (;;) {
        Sleep(HK_POLL_MS);
        DWORD now = GetTickCount();
        // Above every menu branch's continue, so the seat is re-taken with a menu up as well.
        if ((int)(now - crashRearmAt) >= 0) { crashRearmAt = now + 2000; ff132_crash_rearm(); }
        ff132_tidy_screen_tick();
        ff132_slots_tick();
        ff132_state_tick();
        if (g_liftWait) {
            // A menu of ours reopening owns the gate now. An editor being torn down is NOT that
            // case: ff132_tidy_screen_tick closed it a statement earlier and the flag outlives the
            // tick, so the editor branch below releases again.
            if (ff132_tidy_is_open() || ff132_tidy_list_is_open() || ff132_tidy_edit_is_open()
                || ff132_setup_is_open())
                g_liftWait = 0;
            else if (ff132_di_gate_left_ms() < 0) g_liftWait = 0;
            else if ((int)(now - g_liftWaitAt) > FF132_DI_GATE_MAX_MS) {
                // A key that never reads as released (stuck, or a remapper ate the up) must not
                // seal the player in. This hands the game the press the wait exists to swallow,
                // so it is logged at every level: "it loaded a save by itself" needs a trace.
                ff132_loga(FF132_TAG "input gate: a held key never came up in %dms -- lifting anyway"
                              " (the game may read it as a fresh press)\n", FF132_DI_GATE_MAX_MS);
                g_liftWait = 0;
                ff132_di_gate_lift();
            } else if (!gate_keys_down()) {
                g_liftWait = 0;
                ff132_di_gate_lift();
            } else if (ff132_di_gate_left_ms() < FF132_DI_GATE_MAX_MS / 2) ff132_di_gate_arm();
        }
        // Every gesture below is stepped on EVERY tick, open or shut: asked only while a menu is up,
        // a key already held when it opens would read as a fresh press.
        int ok     = hk_edge(g_menuOkVk, &wasOk);
        int cancel = hk_edge(g_cancelVk, &wasCancel);
        int up     = hk_held(g_menuUpVk,    &mUp,    now);
        int down   = hk_held(g_menuDownVk,  &mDown,  now);
        int left   = hk_held(g_menuLeftVk,  &mLeft,  now);
        int right  = hk_held(g_menuRightVk, &mRight, now);
        int lPrevFire = hk_held(g_listPrevVk, &lPrev, now);
        int lNextFire = hk_held(g_listNextVk, &lNext, now);
        int tidy = hk_tap_up(g_saveTidyVk, &tidyKey);
        // The editor owns every key while it is up: the continue below is what keeps a letter being
        // typed from also firing one of the hotkeys further down, and every edge above it was
        // stepped all the same so none of them fires as fresh the tick the editor closes.
        if (ff132_tidy_edit_is_open()) {
            hk_edge(g_slotSelectVk, &wasSlot);
            hk_edge(g_battleOverlayVk, &wasBattle);
            hk_edge(g_gameSpeedVk, &wasSpeed);
            hk_edge(g_enemySuppressVk, &wasEnc);
            hk_edge(g_killModeVk, &wasKill);
            hk_edge(g_battleRestartAutoVk, &wasArAuto);
            hk_edge(g_helpVk, &wasHelp);
            if (tidy)    ff132_tidy_edit_finish(0);
            else if (ok) ff132_tidy_edit_finish(1);
            ff132_tidy_edit_tick();
            if (ff132_tidy_edit_is_open() || ff132_tidy_is_open() || ff132_tidy_list_is_open()) {
                if (ff132_di_gate_left_ms() < FF132_DI_GATE_MAX_MS / 2) ff132_di_gate_arm();
            } else {
                menu_gate_release();
            }
            continue;
        }
        // The settings screen owns the menu keys the same way; every feature edge is stepped so none
        // fires under it and none reads as fresh the tick it closes.
        if (ff132_setup_is_open()) {
            hk_edge(g_slotSelectVk, &wasSlot);
            hk_edge(g_battleOverlayVk, &wasBattle);
            hk_edge(g_gameSpeedVk, &wasSpeed);
            hk_edge(g_enemySuppressVk, &wasEnc);
            hk_edge(g_killModeVk, &wasKill);
            hk_edge(g_battleRestartAutoVk, &wasArAuto);
            int help = hk_edge(g_helpVk, &wasHelp);
            // A capture reads the raw keys itself, the help key included: there it cancels the
            // capture rather than closing the screen.
            if (ff132_setup_capturing()) {
                ff132_setup_capture_tick();
            } else if (ff132_setup_settling()) {
                // The gesture state above still tracks the bindings from before the capture.
            } else if (help) {
                ff132_setup_close();
                menu_gate_release();
            } else {
                if (up)    ff132_setup_move(-1);
                if (down)  ff132_setup_move(+1);
                if (left)  ff132_setup_cycle(-1);
                if (right) ff132_setup_cycle(+1);
                if (ok) ff132_setup_ok();
                else if (cancel) { ff132_setup_close(); menu_gate_release(); }
            }
            if (ff132_setup_is_open() && ff132_di_gate_left_ms() < FF132_DI_GATE_MAX_MS / 2) ff132_di_gate_arm();
            continue;
        }
        // The game moves its own cursor under the list menu, so the menu takes the save it lands on.
        ff132_tidy_list_follow();
        if (ff132_tidy_list_is_open()) {
            if (up)    ff132_tidy_list_move(-1);
            if (down)  ff132_tidy_list_move(+1);
            if (left)  ff132_tidy_list_cycle(-1);
            if (right) ff132_tidy_list_cycle(+1);
            // One key at a time: two at once would be two presses in the same gate window.
            if (lPrevFire)      ff132_tidy_list_step(-1);
            else if (lNextFire) ff132_tidy_list_step(+1);
            if (ok) ff132_tidy_list_go();
            // No cancel here: the game's own list reads that key as "back" and would take itself
            // away with the menu still up.
        } else if (ff132_tidy_is_open()) {
            if (up)    ff132_tidy_move(-1);
            if (down)  ff132_tidy_move(+1);
            if (left)  ff132_tidy_cycle(-1);
            if (right) ff132_tidy_cycle(+1);
            // menu_ok acts BEFORE the menu key so a same-tick tap cannot open a menu it then
            // confirms; outside the menu the key belongs to the game.
            if (ok)     ff132_tidy_confirm();
            if (cancel) { ff132_tidy_close(); menu_gate_release(); }
        }
        {
            if (tidy) {
                if (ff132_tidy_list_is_open()) { ff132_tidy_list_close(); menu_gate_release(); }
                else if (ff132_tidy_is_open()) { ff132_tidy_close(); menu_gate_release(); }
                // A live gate is someone else's, and closing ours would lift it early. The game's
                // own list gets the key first; away from it the key is the title menu's.
                else if (ff132_di_gate_left_ms() < 0) {
                    if (!ff132_tidy_list_open()) ff132_tidy_open_menu();
                }
            }
            // Re-armed at half-life, not every tick: each arm resets the gate's one-line-per-gate
            // log latch, and a per-tick re-arm turns that line into a stream.
            if ((ff132_tidy_is_open() || ff132_tidy_list_is_open())
                && ff132_di_gate_left_ms() < FF132_DI_GATE_MAX_MS / 2) ff132_di_gate_arm();
        }
        if (hk_edge(g_slotSelectVk, &wasSlot)) save_request_picker();
        if (hk_edge(g_battleOverlayVk, &wasBattle)) ff132_battle_toggle();
        if (hk_edge(g_gameSpeedVk, &wasSpeed)) ff132_speed_toggle();
        if (hk_edge(g_enemySuppressVk, &wasEnc)) ff132_encount_toggle();
        if (hk_edge(g_killModeVk, &wasKill)) ff132_kill_toggle();
        if (hk_edge(g_battleRestartAutoVk, &wasArAuto)) ff132_autoretry_toggle();
        // Not over a menu of ours, and not under a gate still held for one: the key is consumed.
        if (hk_edge(g_helpVk, &wasHelp) && !ff132_tidy_is_open() && !ff132_tidy_list_is_open()
            && !ff132_tidy_edit_is_open() && ff132_di_gate_left_ms() < 0)
            ff132_setup_open();
    }
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        // Rotated here rather than in the worker so this line survives into the new log.
        ff132_log_reset();
        ff132_loga(FF132_TAG "DLL_PROCESS_ATTACH\n");
        CreateThread(NULL, 0, worker, NULL, 0, NULL);
    } else if (reason == DLL_PROCESS_DETACH) {
        // DO NOTHING here. On process exit this runs under the loader lock with the other threads
        // killed and the CRT half torn down: logging can deadlock.
    }
    return TRUE;
}
