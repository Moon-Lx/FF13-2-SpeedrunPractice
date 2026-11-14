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

// Save-slot selection: a hotkey in the field opens the Historia Crux slot picker, then runs the
// game's own field save into the slot it committed. The menu's own save is never intercepted.
//
// Next to it is the autosave redirect. The current slot's file is the live per-world store, so the
// target of a single save cannot be moved without stranding that world's progress: the mod copies
// the slot the player loaded onto a dedicated one and makes that the current slot for the rest of
// the session. Every automatic save, gate flush and world re-read then lands in the same file, and
// the slot the player loaded from stays frozen as a restore point.
//
// It also carries the two hooks the direct-load sidecar is fed from -- the silent save's entry and
// the current-slot setter, whose return address says whether a file was just written or just read --
// and, under FF132_DIAG_SAVEPATH, log-only sites that record which save function a menu save goes
// through. None of them changes what the game does.
#include "ff132_internal.h"
#include "ff132_addrs.h"
#include "ff13_hook.h"


// The pointer walk Zone::getCurrentState() makes, without its mutex. For the hotkey thread, which
// must not call into the game; a hook handler on the game thread calls the real getter instead.
int zone_state_peek(void) {
    uintptr_t mgr = (uintptr_t)rd32(g_base + FF132_RVA_ZONE_MGR_PTR);
    if (!rd_ok((void*)mgr, FF132_OFF_ZONE_FSM + 4)) return -1;
    uintptr_t fsm = (uintptr_t)rd32(mgr + FF132_OFF_ZONE_FSM);
    if (!rd_ok((void*)fsm, FF132_OFF_ZONE_STATE_OBJ + 4)) return -1;
    uintptr_t st = (uintptr_t)rd32(fsm + FF132_OFF_ZONE_STATE_OBJ);
    if (!rd_ok((void*)st, FF132_OFF_ZONE_STATE_ID + 4)) return -1;
    return (int)rd32(st + FF132_OFF_ZONE_STATE_ID);
}

// The same shape for GameMain's own state machine, whose id says which SCREEN is up: 4 = the title,
// 15 = the title's load list, 5/12 = a zone and its pause menu.
int ff132_main_state(void) {
    uintptr_t gm = (uintptr_t)rd32(g_base + FF132_RVA_GAMEMAIN_PTR);
    if (!rd_ok((void*)gm, FF132_OFF_GAMEMAIN_FSM + 4)) return -1;
    uintptr_t fsm = (uintptr_t)rd32(gm + FF132_OFF_GAMEMAIN_FSM);
    if (!rd_ok((void*)fsm, FF132_OFF_FSM_CURRENT + 4)) return -1;
    uintptr_t st = (uintptr_t)rd32(fsm + FF132_OFF_FSM_CURRENT);
    if (!rd_ok((void*)st, FF132_OFF_STATE_ID + 4)) return -1;
    return (int)rd32(st + FF132_OFF_STATE_ID);
}

// ---- state readers, for handlers running on the game thread ------------------------------------
// The real getter. It takes the zone mutex (WaitForSingleObject/ReleaseMutex, so re-entering it
// from inside a hooked zone function on the same thread is safe), which the peek above skips.
typedef int (__thiscall *zoneState_t)(void* self);
// The getter is called, not hooked, so nothing would catch a drifted address at call time -- its
// bytes are checked once at install and it is left alone when they do not match.
static int g_zoneGetterOk = 0;
static int zone_state(void) {
    if (!g_zoneGetterOk) return -1;
    uintptr_t mgr = (uintptr_t)rd32(g_base + FF132_RVA_ZONE_MGR_PTR);
    if (!rd_ok((void*)mgr, 4)) return -1;
    return ((zoneState_t)(uintptr_t)(g_base + FF132_RVA_ZONE_GETSTATE))((void*)mgr);
}

static int cur_slot(void) {
    uintptr_t sd = (uintptr_t)rd32(g_base + FF132_RVA_SAVEDATA_PTR);
    if (!rd_ok((void*)sd, FF132_OFF_CUR_SLOT + 4)) return -1;
    return (int)rd32(sd + FF132_OFF_CUR_SLOT);
}

#if FF132_DIAG_SAVEPATH
// ---- log ---------------------------------------------------------------------------------------
// The step functions here run per frame: one fopen/fclose per frame would stutter the game and bury
// the transitions, so a line identical to the site's previous one inside the window is counted
// instead of written, and the count is flushed when the line changes.
enum { D_MENU, D_ONSAVE, D_AUTOSAVE, D_SILENT, D_PICKER, D_SETSLOT, D_SLOTSEL, D_LOAD,
       D_RSAVE, D_RLOAD, D_SITES };
#define D_DUP_MS 1000
#define D_LINE   224
#define D_VA(rva) ((unsigned)(g_base + (rva)))

static void diag_line(int site, const char* fmt, ...) {
    static char     prev[D_SITES][D_LINE];
    static DWORD    prevTick[D_SITES];
    static unsigned dup[D_SITES];
    char line[D_LINE];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    DWORD now = GetTickCount();
    if (strcmp(line, prev[site]) == 0 && now - prevTick[site] < D_DUP_MS) {
        dup[site]++;
        return;
    }
    if (dup[site]) {
        // Repeats the folded text rather than pointing back: other sites interleave, so "the line
        // above" is not necessarily this site's.
        ff132_loga(FF132_TAG "[%u] %s  (+%u folded repeats)\n", (unsigned)prevTick[site], prev[site],
               dup[site]);
        dup[site] = 0;
    }
    memcpy(prev[site], line, sizeof line);
    prevTick[site] = now;
    ff132_loga(FF132_TAG "[%u] %s\n", (unsigned)now, line);
}

// A game string is only worth logging if it is actually there: these arrive as raw pointers into
// the caller's frame and a wrong argument mapping would otherwise fault inside the save path.
static const char* safe_cstr(uintptr_t p, char* buf, size_t n) {
    if (!p) return "(null)";
    size_t i = 0;
    for (; i + 1 < n; i++) {
        if (!rd_ok((const void*)(p + i), 1)) {
            if (i == 0) return "(unreadable)";
            break;
        }
        char c = *(const char*)(p + i);
        if (!c) break;
        buf[i] = (c >= 0x20 && c < 0x7F) ? c : '.';
    }
    buf[i] = 0;
    return buf;
}
#endif

// ---- hook installer ------------------------------------------------------------------------------
enum { HK_ECX, HK_ESP, HK_STACKARG };

static void site_hook(const char* tag, uintptr_t rva, const uint8_t* sig, int sigLen,
                      void* handler, int kind, int stackOff) {
    uintptr_t at = g_base + rva;
    if (wait_for_bytes(at, sig, sigLen, FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "hook %s @0x%08X: mismatch (bytes never matched)\n", tag, (unsigned)at);
        return;
    }
    void* cave = kind == HK_ESP      ? ff13_install_esp_hook(at, sig, sigLen, handler)
               : kind == HK_STACKARG ? ff13_install_stackarg_hook(at, sig, sigLen, handler, stackOff)
                                     : ff13_install_ecx_hook(at, sig, sigLen, handler);
    if (cave) ff132_loga(FF132_TAG "hook %s @0x%08X: ok cave=%p\n", tag, (unsigned)at, cave);
    else      ff132_loga(FF132_TAG "hook %s @0x%08X: mismatch\n", tag, (unsigned)at);
}

// ---- the save-record hooks (always installed) ----------------------------------------------------
// H4 and H6 are what the sidecar in ff132_load.c is fed from, so they are in every build. Both run on
// the thread that called the site, which for these two is the game thread.
static int g_raWriteOk = 0;            // the interactive write body's setter call matched
static int g_raLoadAOk = 0;            // the load body's first setter call matched
static int g_raLoadBOk = 0;            // ...and its second

// A silent save writes the CURRENT slot and is called three times per save, so the slot is only
// marked pending here; ff132_load_frame commits it once the save machinery goes idle.
static void __cdecl on_silent_save(uint32_t ecx) {
    (void)ecx;
#if FF132_DIAG_SAVEPATH
    diag_line(D_SILENT, "silent-save %06X: cur_slot=%d zone_state=%d",
              D_VA(FF132_RVA_SILENT_SAVE), cur_slot(), zone_state());
#endif
    ff132_load_note_pending(cur_slot(), zone_state() == FF132_ZONE_STATE_DISPOSED);
}

// The handler runs before the relocated prologue stores the argument, so cur_slot() is still the
// old value and the new one is read off the caller's frame. The return address is what says whether
// this call ends a write (the file is final) or a read (a load is deciding its route); every other
// caller -- zone init, the title screen's -1 -- is only logged.
static void __cdecl on_set_cur_slot(uint32_t esp) {
    uint32_t ra = 0;
    int slot = -1;
    if (rd_ok((const void*)(uintptr_t)esp, 8)) {
        ra   = rd32((uintptr_t)esp);
        slot = (int)rd32((uintptr_t)esp + 4);
    }
#if FF132_DIAG_SAVEPATH
    diag_line(D_SETSLOT, "set-cur-slot %06X: new=%d (was %d)",
              D_VA(FF132_RVA_SET_CUR_SLOT), slot, cur_slot());
#endif
    if (g_raWriteOk && ra == (uint32_t)(g_base + FF132_RA_WRITE_DONE))
        ff132_load_note_written(slot, zone_state() == FF132_ZONE_STATE_DISPOSED);
    else if (slot >= 0 && ((g_raLoadAOk && ra == (uint32_t)(g_base + FF132_RA_LOAD_DONE_A)) ||
                           (g_raLoadBOk && ra == (uint32_t)(g_base + FF132_RA_LOAD_DONE_B))))
        ff132_load_on_loaded(slot);
}

// The call sites are never patched: their bytes are compared once and the return addresses are then
// used as plain constants, so a site that has drifted only costs that call its classification.
static int site_ok(const char* what, uintptr_t rva, const uint8_t* sig, size_t n, const char* bad) {
    int ok = sig_at(rva, sig, n);
    ff132_loga(FF132_TAG "%s @0x%08X: %s\n", what, (unsigned)(g_base + rva), ok ? "ok" : bad);
    return ok;
}

static int call_site_ok(const char* what, uintptr_t rva, const uint8_t* sig, size_t n) {
    char w[64];
    snprintf(w, sizeof w, "save records: %s call site", what);
    return site_ok(w, rva, sig, n, "mismatch -> these saves are not classified");
}

static void record_install_hooks(void) {
    site_hook("H4", FF132_RVA_SILENT_SAVE, FF132_SIG_SILENT_SAVE, (int)sizeof FF132_SIG_SILENT_SAVE,
              (void*)on_silent_save, HK_ECX, 0);
    site_hook("H6", FF132_RVA_SET_CUR_SLOT, FF132_SIG_SET_CUR_SLOT, (int)sizeof FF132_SIG_SET_CUR_SLOT,
              (void*)on_set_cur_slot, HK_ESP, 0);
    g_raWriteOk = call_site_ok("write-done", FF132_RVA_WRITE_DONE_CALL, FF132_SIG_WRITE_DONE_CALL,
                               sizeof FF132_SIG_WRITE_DONE_CALL);
    g_raLoadAOk = call_site_ok("load-done A", FF132_RVA_LOAD_DONE_CALL_A, FF132_SIG_LOAD_DONE_CALL_A,
                               sizeof FF132_SIG_LOAD_DONE_CALL_A);
    g_raLoadBOk = call_site_ok("load-done B", FF132_RVA_LOAD_DONE_CALL_B, FF132_SIG_LOAD_DONE_CALL_B,
                               sizeof FF132_SIG_LOAD_DONE_CALL_B);
}

#if FF132_DIAG_SAVEPATH
// ---- the log-only handlers ----------------------------------------------------------------------
// Each runs at its site's function entry, before the relocated prologue, on whatever thread called
// it. They read; they never write.
static void __cdecl on_menu_handler(uint32_t ecx) {
    (void)ecx;
    diag_line(D_MENU, "menu-handler %06X: zone_state=%d cur_slot=%d",
              D_VA(FF132_RVA_MENU_HANDLER), zone_state(), cur_slot());
}

static void __cdecl on_onsave(uint32_t ecx) {
    int sub = rd_ok((void*)(uintptr_t)ecx, FF132_OFF_ONSAVE_STATE + 4)
                  ? (int)rd32((uintptr_t)ecx + FF132_OFF_ONSAVE_STATE) : -1;
    diag_line(D_ONSAVE, "onsave %06X: this=%p state7C=%d zone_state=%d cur_slot=%d",
              D_VA(FF132_RVA_ONSAVE), (void*)(uintptr_t)ecx, sub, zone_state(), cur_slot());
}

static void __cdecl on_load_body(uint32_t ecx) {
    (void)ecx;
    diag_line(D_LOAD, "load %06X: cur_slot=%d zone_state=%d",
              D_VA(FF132_RVA_LOAD_BODY), cur_slot(), zone_state());
}

static void __cdecl on_restart_save(uint32_t ecx) {
    (void)ecx;
    diag_line(D_RSAVE, "restart-point stored %06X: cur_slot=%d zone_state=%d",
              D_VA(FF132_RVA_RESTART_SAVE), cur_slot(), zone_state());
}

static void __cdecl on_restart_load(uint32_t ecx) {
    (void)ecx;
    diag_line(D_RLOAD, "restart-point restored %06X: cur_slot=%d zone_state=%d",
              D_VA(FF132_RVA_RESTART_LOAD), cur_slot(), zone_state());
}

static void __cdecl on_picker_screen(uint32_t ecx) {
    (void)ecx;
    diag_line(D_PICKER, "picker-screen %06X: cur_slot=%d zone_state=%d",
              D_VA(FF132_RVA_PICKER_SCREEN), cur_slot(), zone_state());
}

static void diag_install_hooks(void) {
    ff132_loga(FF132_TAG "save-path diagnostics: log-only hooks, nothing is patched away\n");
    site_hook("H1", FF132_RVA_MENU_HANDLER, FF132_SIG_MENU_HANDLER, (int)sizeof FF132_SIG_MENU_HANDLER,
              (void*)on_menu_handler, HK_ECX, 0);
    site_hook("H2", FF132_RVA_ONSAVE, FF132_SIG_ONSAVE, (int)sizeof FF132_SIG_ONSAVE,
              (void*)on_onsave, HK_ECX, 0);
    site_hook("H7", FF132_RVA_LOAD_BODY, FF132_SIG_LOAD_BODY, (int)sizeof FF132_SIG_LOAD_BODY,
              (void*)on_load_body, HK_ECX, 0);
    site_hook("H8", FF132_RVA_RESTART_SAVE, FF132_SIG_RESTART_SAVE, (int)sizeof FF132_SIG_RESTART_SAVE,
              (void*)on_restart_save, HK_ECX, 0);
    site_hook("H9", FF132_RVA_RESTART_LOAD, FF132_SIG_RESTART_LOAD, (int)sizeof FF132_SIG_RESTART_LOAD,
              (void*)on_restart_load, HK_ECX, 0);
    site_hook("H5", FF132_RVA_PICKER_SCREEN, FF132_SIG_PICKER_SCREEN, (int)sizeof FF132_SIG_PICKER_SCREEN,
              (void*)on_picker_screen, HK_ECX, 0);
}
#endif // FF132_DIAG_SAVEPATH

// ---- slot selection ----------------------------------------------------------------------------

static int g_ssOk = 0;                 // the site and update()'s bytes both checked out
static int g_blockOk = 0;              // the doAutoSave_l call site's bytes checked out
static int g_histOk = 0;               // the "return to Historia Crux" call site's bytes checked out
static int g_menuPollOk = 0;           // the menu poller's bytes checked out (classification only)
static int g_pauseSaveOk = 0;          // the pause menu "save" site's bytes checked out (classification only)

static void* game_main(void) {
    uintptr_t gm = (uintptr_t)rd32(g_base + FF132_RVA_GAMEMAIN_PTR);
    return rd_ok((void*)gm, FF132_OFF_SAVE_CREATOR + 4) ? (void*)gm : NULL;
}

static void* save_creator(void) {
    uintptr_t gm = (uintptr_t)game_main();
    if (!gm) return NULL;
    uintptr_t c = (uintptr_t)rd32(gm + FF132_OFF_SAVE_CREATOR);
    if (!rd_ok((void*)c, FF132_OFF_CREATOR_BUSY + 4)) return NULL;
    return (void*)c;
}

// doAutoSave's `this`. Not the ZoneStateMachine itself -- the sub-object 0x1C into it, which is what
// every caller passes and what owns the +0x68 step state.
static void* zsm_save_obj(void) {
    uintptr_t mgr = (uintptr_t)rd32(g_base + FF132_RVA_ZONE_MGR_PTR);
    if (!rd_ok((void*)mgr, FF132_OFF_ZONE_FSM + 4)) return NULL;
    uintptr_t zsm = (uintptr_t)rd32(mgr + FF132_OFF_ZONE_FSM);
    uintptr_t obj = zsm + FF132_OFF_ZSM_SAVE_OBJ;
    if (!rd_ok((void*)obj, FF132_OFF_DOAUTOSAVE_STATE + 4)) return NULL;
    return (void*)obj;
}

#if FF132_DIAG_SAVEPATH
// doAutoSave's frame at its entry: [esp] return address, then tag, n, s2. The two strings only
// live as long as the caller's frame, so they are copied out while the call is still standing.
struct ss_args { uint32_t ra; const char* tag; int n; const char* s2; };
static struct ss_args ss_read_args(uint32_t esp, char* tagb, size_t tn, char* s2b, size_t sn) {
    struct ss_args a = { 0, "(no stack)", -1, "(no stack)" };
    if (rd_ok((const void*)(uintptr_t)esp, 16)) {
        a.ra  = rd32((uintptr_t)esp);
        a.tag = safe_cstr((uintptr_t)rd32((uintptr_t)esp + 4), tagb, tn);
        a.n   = (int)rd32((uintptr_t)esp + 8);
        a.s2  = safe_cstr((uintptr_t)rd32((uintptr_t)esp + 12), s2b, sn);
    }
    return a;
}
#endif

// Logs the original call's arguments and why it is let through (this site's only log). Folded,
// because a menu save polls doAutoSave every frame.
static void ss_pass_through(uint32_t esp, const char* why) {
#if FF132_DIAG_SAVEPATH
    char tagb[64], s2b[64];
    struct ss_args a = ss_read_args(esp, tagb, sizeof tagb, s2b, sizeof s2b);
    // doAutoSave returns 1 without writing when both gate bits in GameMain+0x5C0 are clear, so a
    // save that is logged here and never reaches the writer is explained by this number alone.
    unsigned gate = 0;
    {
        uintptr_t gm = (uintptr_t)rd32(g_base + FF132_RVA_GAMEMAIN_PTR);
        if (rd_ok((void*)gm, 0x5C4)) gate = (unsigned)rd32(gm + 0x5C0);
    }
    diag_line(D_AUTOSAVE,
              "doAutoSave %06X: tag=\"%s\" n=%d s2=\"%s\" ret=%06X zone_state=%d cur_slot=%d"
              " gate=%08X(b29=%d b28=%d) -> original (%s)",
              D_VA(FF132_RVA_DOAUTOSAVE), a.tag, a.n, a.s2, (unsigned)a.ra, zone_state(), cur_slot(),
              gate, (gate >> 29) & 1, (gate >> 28) & 1, why);
#else
    (void)esp; (void)why;
#endif
}

// Replaces doAutoSave. ON THE GAME THREAD ONLY -- update() is the creator's own step function and
// a second caller would fight it for [creator+0x10]; that is also why the poll is left running to
// completion once started, even if the toggle goes off underneath it.
// The handler always returns 0, which runs the real doAutoSave; the hook can answer the caller
// itself through *ret instead, but nothing here does -- the save always has to run.
typedef int (__thiscall *creatorUpdate_t)(void* self);
// doAutoSave(tag, n, s2). tag is compared 16 bytes at a time against "status", so it is handed a
// 16-byte buffer rather than a 1-byte "" literal.
typedef int (__thiscall *doAutoSave_t)(void* self, const char* tag, int n, const char* s2);
typedef void (__thiscall *setAutosaveBit_t)(void* gameMain, int on);
static const char g_saveTagEmpty[16] = { 0 };

static int g_frameOk = 0;              // the per-frame hook is in place (otherwise never intercept)
static int g_fullOk = 0;               // the autosave-enable setter's bytes checked out
static int g_pkPending = 0;            // a picker was promised to the player
static int g_pkDelay = 0;              // frames still to wait before opening it
static int g_pkDriving = 0;            // update() has been started and must run to completion
static int g_pkIdleWait = 0;
static int g_pkFullSave = 0;           // the picker committed; the game's own save is now running
static int g_fullDriving = 0;          // doAutoSave has been started and must run to completion
static int g_fullWait = 0;
// Read by on_doautosave on the same thread, inside the call below. Only the game thread touches it.
static volatile int g_ownSave = 0;

// The slot count is written once at startup, so it is read at use time: at install it can still
// be 0, and a count of 0 would name slot -1. The default is the last slot of the save slot limit,
// so it moves with the limit: a slot it moves to is checked again before anything is copied there.
static int redirect_slot(void) {
    static int logged = -1, pastSaid = 0;
    if (g_autosaveSlot >= 0) {
        // The copy into it would create a file numbered at or past the count, and the game aborts
        // the moment a list it opens finds one.
        int count = save_slot_count();
        if (count > 0 && g_autosaveSlot >= count) {
            if (pastSaid != count && !ff132_slots_pending()) {
                pastSaid = count;
                ff132_loga(FF132_TAG "working-slot: slot %d (from the ini) is past the game's slot"
                              " count %d -> no working slot\n", g_autosaveSlot, count);
            }
            return -1;
        }
        if (logged != g_autosaveSlot) {
            logged = g_autosaveSlot;
            ff132_loga(FF132_TAG "working-slot: dedicated slot = %d (%s)\n", g_autosaveSlot,
                   "from the ini");
        }
        return g_autosaveSlot;
    }
    int count = save_slot_count(), last = ff132_slots_last();
    if (last < 2 || last > count) return -1;
    if (logged != last - 1) {
        logged = last - 1;
        ff132_loga(FF132_TAG "working-slot: dedicated slot = %d (the last of the slot limit, %d;"
                      " the game's count %d)\n", last - 1, g_slotCount, count);
    }
    return last - 1;
}

static int g_wsOk = 0;                 // the save-path builder's bytes checked out
// The slot that passed the foreign-save check (work_foreign), -1 = none. Nothing treats a slot as
// the working one until it has: the ini's ON is checked on the first frame the slot resolves, and
// again whenever the slot limit moves the default slot.
static volatile int g_wsChecked = -1;
static int g_wsRefusedFlash = -1;      // startup refusal waiting for the overlay: the slot, or -1

int save_work_slot(void) {
    if (!g_autosaveRedirect) return -1;
    int w = redirect_slot();
    return (w >= 0 && w == g_wsChecked) ? w : -1;
}

// ---- the working slot's markers ------------------------------------------------------------------
// Identity = ftLastWriteTime, as the sidecar in ff132_load.c keys it. A SET, oldest first: a category
// switch swaps the working slot's file with that category's own copy, and each has to stay known.
// CopyFileA / MoveFileEx keep the write time, so a moved or copied file keeps its entry -- and a
// category made by copying every save holds a twin with the same identity.
// Game thread adds (work_mark), the worker and the render thread read: every touch takes g_wiLock.
static SRWLOCK  g_wiLock = SRWLOCK_INIT;
static int      g_wiSlot[FF132_WI_MAX];
static uint64_t g_wiIdent[FF132_WI_MAX];
static int      g_wiN = 0;

int save_work_ident_snapshot(int* slots, uint64_t* idents, int max) {
    AcquireSRWLockShared(&g_wiLock);
    int n = g_wiN < max ? g_wiN : max;
    memcpy(slots, g_wiSlot, (size_t)n * sizeof slots[0]);
    memcpy(idents, g_wiIdent, (size_t)n * sizeof idents[0]);
    ReleaseSRWLockShared(&g_wiLock);
    return n;
}

static int wi_find(int slot, uint64_t ident) {
    for (int i = 0; i < g_wiN; i++)
        if (g_wiSlot[i] == slot && g_wiIdent[i] == ident) return i;
    return -1;
}
static void wi_remove_at(int i) {
    memmove(&g_wiSlot[i], &g_wiSlot[i + 1], (size_t)(g_wiN - i - 1) * sizeof g_wiSlot[0]);
    memmove(&g_wiIdent[i], &g_wiIdent[i + 1], (size_t)(g_wiN - i - 1) * sizeof g_wiIdent[0]);
    g_wiN--;
}
// 1 = the set changed. An entry seen again moves to the newest end, so the cap drops the stalest.
static int wi_add(int slot, uint64_t ident) {
    int i = wi_find(slot, ident);
    if (i == g_wiN - 1 && i >= 0) return 0;
    if (i < 0 && g_wiN == FF132_WI_MAX) i = 0;
    if (i >= 0) wi_remove_at(i);
    g_wiSlot[g_wiN] = slot;
    g_wiIdent[g_wiN] = ident;
    g_wiN++;
    return 1;
}
void save_work_ident_add(int slot, uint64_t ident) {
    if (slot < 0) return;
    AcquireSRWLockExclusive(&g_wiLock);
    wi_add(slot, ident);
    ReleaseSRWLockExclusive(&g_wiLock);
}

// A marker is only worth keeping while its file exists: the one just written, or a parked
// category's copy of this slot. Any other marker for the slot names a file a write has replaced;
// left in, every autosave adds one and the cap evicts the parked copies' markers.
static void work_mark(int slot, uint64_t ident, const char* why) {
    AcquireSRWLockExclusive(&g_wiLock);
    int changed = wi_add(slot, ident);
    ReleaseSRWLockExclusive(&g_wiLock);
    if (!changed) return;              // one save marks twice: the first call already pruned
    uint64_t keep[FF132_WI_MAX];
    int nk = ff132_cats_parked_idents(slot, keep, FF132_WI_MAX - 1), pruned = 0;
    AcquireSRWLockExclusive(&g_wiLock);
    if (nk >= 0) {                     // the parked folders could not be read: prune nothing
        keep[nk++] = ident;
        for (int i = g_wiN - 1; i >= 0; i--) {
            if (g_wiSlot[i] != slot) continue;
            int live = 0;
            for (int k = 0; k < nk && !live; k++) live = g_wiIdent[i] == keep[k];
            if (!live) { wi_remove_at(i); pruned++; }
        }
    }
    int n = g_wiN;
    ReleaseSRWLockExclusive(&g_wiLock);
    ff132_state_save();
    ff132_log(FF132_TAG "working-slot: marker + slot %d, file %08X%08X (%s), %d known, %d gone file(s)"
                 " dropped\n", slot, (unsigned)(ident >> 32), (unsigned)ident, why, n, pruned);
}

// A category switch puts another category's file in the working slot: check it before the next copy.
void save_work_recheck(void) { g_wsChecked = -1; }

void save_work_note_written(int slot, uint64_t ident, const char* why) {
    if (slot >= 0 && slot == save_work_slot()) work_mark(slot, ident, why);
}

// 1 = the slot's file is none of the files the markers name: a save of the player's own, which the
// copy into the working slot would destroy. A missing file is not foreign.
static int work_foreign(int slot, const char* when) {
    uint64_t mt = 0;
    uint32_t sz = 0;
    int exists = ff132_load_slot_stat(slot, &mt, &sz);
    AcquireSRWLockShared(&g_wiLock);
    int known = exists && wi_find(slot, mt) >= 0;
    int forSlot = 0, wiN = g_wiN;
    for (int i = 0; i < g_wiN; i++) forSlot += g_wiSlot[i] == slot;
    ReleaseSRWLockShared(&g_wiLock);
    ff132_loga(FF132_TAG "[%u] working-slot: %s check of slot %d: exists=%d file %08X%08X, markers %d"
                  " (%d for this slot), match=%d -> %s\n", (unsigned)GetTickCount(), when, slot,
           exists, (unsigned)(mt >> 32), (unsigned)mt, wiN, forSlot, known,
           exists && !known ? "FOREIGN, refused" : "ok");
    return exists && !known;
}

int save_work_slot_foreign(int slot) {
    uint64_t mt = 0;
    uint32_t sz = 0;
    if (!ff132_load_slot_stat(slot, &mt, &sz)) return 0;
    AcquireSRWLockShared(&g_wiLock);
    int known = wi_find(slot, mt) >= 0;
    ReleaseSRWLockShared(&g_wiLock);
    return !known;
}

static void work_refused_flash(int slot) {
    char buf[288];
    snprintf(buf, sizeof buf, ui_text(T_WS_FOREIGN), slot + 1);
    ff132_flash_c(buf, FF132_COL_ERR);
}

int save_cur_slot(void) { return cur_slot(); }

// Read at use time for the same reason redirect_slot does: at install the count can still be 0.
int save_slot_count(void) {
    uintptr_t p = g_base + FF132_RVA_MAX_SLOTS;
    int n = rd_ok((const void*)p, 4) ? (int)rd32(p) : 0;
    return (n > 0 && n <= 999) ? n : 0;
}

// The game's own setter (0x9871F0) does exactly this store; it is bypassed so the hook on that
// setter does not see the mod's own takeover as a save or a load. The caller logs it instead.
static int set_cur_slot(int slot) {
    uintptr_t sd = (uintptr_t)rd32(g_base + FF132_RVA_SAVEDATA_PTR);
    if (!rd_ok((const void*)sd, FF132_OFF_CUR_SLOT + 4)) return 0;
    *(volatile uint32_t*)(sd + FF132_OFF_CUR_SLOT) = (uint32_t)slot;
    return 1;
}

typedef const char* (__thiscall *savePath_t)(void* ctx);
static int g_wsSettle = 0;             // frames the current slot has stayed off the working slot

// ctx has to arrive zeroed: FF132_OFF_SAVEPATH_INIT reading 0 is what makes the builder create the
// save directory itself before it answers.
static const char* slot_path(int slot, char* ctx) {
    if (!g_wsOk) return NULL;
    memset(ctx, 0, FF132_SAVEPATH_CTX);
    *(int*)(ctx + FF132_OFF_SAVEPATH_SLOT) = slot;
    return ((savePath_t)(uintptr_t)(g_base + FF132_RVA_SAVE_PATH))(ctx);
}

// The same path, copied out, for the sidecar in ff132_load.c -- the builder's answer lives inside the
// caller's ctx, which is why it cannot simply be handed back.
int save_slot_path(int slot, char* out, size_t n) {
    if (!out || !n) return 0;
    char ctx[FF132_SAVEPATH_CTX];
    const char* p = slot_path(slot, ctx);
    if (!p || strlen(p) >= n) return 0;
    snprintf(out, n, "%s", p);
    return 1;
}

// A save in flight owns the current slot until its state machine runs out: doAutoSave for an
// automatic save, the interactive creator for a menu or Historia Crux one.
static int save_in_flight(void) {
    void* zsm = zsm_save_obj();
    if (!zsm || (int)rd32((uintptr_t)zsm + FF132_OFF_DOAUTOSAVE_STATE) != 0) return 1;
    void* creator = save_creator();
    return creator && ((int)rd32((uintptr_t)creator + FF132_OFF_CREATOR_STATE) != 0 ||
                       (int)rd32((uintptr_t)creator + FF132_OFF_CREATOR_BUSY) != 0);
}

// Game thread, once per frame. Whenever the game has moved onto a slot that is not the working one
// -- the player loaded, or a manual save landed somewhere else -- that slot's file is copied over
// the working slot and the game is switched to it, so every later autosave, gate flush and world
// re-read stays in one file. CopyFileA is what the game's own slot-to-slot copy calls.
static void work_slot_step(void) {
    // One report per source slot: without it a slot whose file is missing would be retried, logged
    // and flashed once per settle window forever. A different current slot clears it.
    static int failedFrom = -1;
    if (g_wsRefusedFlash >= 0 && ff132_ovl_ready()) {
        work_refused_flash(g_wsRefusedFlash);
        g_wsRefusedFlash = -1;
    }
    if (!g_autosaveRedirect || !g_wsOk) { g_wsSettle = 0; return; }
    if (g_pkPending) { g_wsSettle = 0; return; }        // a hotkey save is mid-flight
    int w = redirect_slot();
    if (w >= 0 && w != g_wsChecked) {
        if (work_foreign(w, g_wsChecked < 0 ? "first-frame" : "slot-moved")) {
            g_autosaveRedirect = 0;     // this session only: the ini keeps its on
            g_wsSettle = 0;
            g_wsRefusedFlash = w;
            ff132_loga(FF132_TAG "working-slot: WARNING: slot %d holds a save this mod did not put there"
                          " -> the working slot is OFF this session (the ini still says on)\n", w);
            return;
        }
        g_wsChecked = w;
    }
    int c = cur_slot();
    if (c != failedFrom) failedFrom = -1;
    if (w < 0 || c < 0 || c == w || c == failedFrom) { g_wsSettle = 0; return; }
    if (++g_wsSettle < FF132_WORKSLOT_SETTLE) return;      // let a load or a save finish first
    g_wsSettle = 0;
    // Switching under a save in flight would hand its second half a different file.
    if (save_in_flight()) return;
    // 0x600 bytes each: only the frame that actually copies pays for them.
    char ctxA[FF132_SAVEPATH_CTX], ctxB[FF132_SAVEPATH_CTX];
    const char* src = slot_path(c, ctxA);
    const char* dst = slot_path(w, ctxB);
    if (!src || !dst) {
        failedFrom = c;
        ff132_loga(FF132_TAG "[%u] working-slot: no path for slot %d or %d -> staying on slot %d\n",
               (unsigned)GetTickCount(), c, w, c);
        return;
    }
    // Nothing has been written to that slot yet (a new game before its first save): there is no
    // state to carry over, so this stays silent and is retried once the game writes the file.
    if (GetFileAttributesA(src) == INVALID_FILE_ATTRIBUTES) return;
    DWORD t0 = GetTickCount();
    if (!CopyFileA(src, dst, FALSE)) {
        failedFrom = c;
        ff132_loga(FF132_TAG "[%u] working-slot: copy slot %d -> %d failed (err %lu) -> staying on"
                      " slot %d\n", (unsigned)t0, c, w, (unsigned long)GetLastError(), c);
        ff132_flash_c(ui_text(T_WS_COPY_FAIL), FF132_COL_ERR);
        return;
    }
    ff132_load_note_copied(c, w);
    {
        uint64_t mt;
        uint32_t sz;
        if (ff132_load_slot_stat(w, &mt, &sz)) work_mark(w, mt, "copied in");
    }
    if (!set_cur_slot(w)) {
        failedFrom = c;
        ff132_loga(FF132_TAG "[%u] working-slot: slot %d copied to %d, but the current slot could not be"
                      " written -> still on slot %d\n", (unsigned)t0, c, w, c);
        return;
    }
    failedFrom = -1;
    ff132_loga(FF132_TAG "[%u] working-slot: slot %d copied to %d in %lu ms, now playing on slot %d\n",
           (unsigned)GetTickCount(), c, w, (unsigned long)(GetTickCount() - t0), w);
    char buf[96];
    // The player counts save data from 1; the files (and everything above) are indexed from 0.
    snprintf(buf, sizeof buf, ui_text(T_WS_PLAYING), w + 1, c + 1);
    ff132_flash_c(buf, FF132_COL_ON);
}

// Classifies the caller by return address, nothing more. No save may be skipped or retargeted here:
// doAutoSave also serialises the zone into the save buffer, and all of its callers have to reach the
// same file. Never intercept the menu's "save" poll: the menu waits for a "done" answer and swallows
// every input the picker needs (measured twice).
static int __cdecl on_doautosave(uint32_t ecx, uint32_t esp, int* ret) {
    (void)ecx; (void)ret;
    // The mod's own post-picker save calls this site: it is not classified with the game's.
    if (g_ownSave) {
        ss_pass_through(esp, "the mod's own save after the picker");
        return 0;
    }
    uint32_t ra = rd_ok((const void*)(uintptr_t)esp, 4) ? rd32((uintptr_t)esp) : 0;
    int fromScript = g_blockOk    && ra == (uint32_t)(g_base + FF132_RVA_DOAUTOSAVE_L_RET);
    int fromHist   = g_histOk     && ra == (uint32_t)(g_base + FF132_RVA_HISTORIA_RET);
    int fromMenu   = g_menuPollOk && ra == (uint32_t)(g_base + FF132_RVA_MENU_SAVE_POLL_RET);
    int fromPause  = g_pauseSaveOk && ra == (uint32_t)(g_base + FF132_RVA_PAUSE_SAVE_RET);
    int isAuto     = fromScript || fromHist;
    ss_pass_through(esp, isAuto    ? (g_autosaveRedirect ? "autosave -> working slot" : "autosave")
                       : fromPause ? "pause-menu save"
                       : fromMenu  ? "menu save poller"
                                   : "unknown caller");
    return 0;
}

// Hotkey thread. Only flags are touched here; the game thread picks them up in on_frame.
void save_request_picker(void) {
    if (!g_ssOk || !g_frameOk) {
        ff132_loga(FF132_TAG "[%u] slot-select: hotkey ignored, hooks not armed\n", (unsigned)GetTickCount());
        return;
    }
    if (g_pkPending || g_pkDriving) {
        ff132_loga(FF132_TAG "[%u] slot-select: hotkey ignored, a picker is already in flight\n",
               (unsigned)GetTickCount());
        return;
    }
    // Field only: anywhere else the picker has no zone to save the position from.
    int ms = ff132_main_state();
    if (ms != FF132_MS_ZONE) {
        ff132_log(FF132_TAG "[%u] slot-select: refused -- main state %d, not the field\n",
              (unsigned)GetTickCount(), ms);
        ff132_flash(ui_text(T_SAVEHERE_FIELD));
        return;
    }
    // The Historia Crux has no zone for the field save that follows the picker (the game crashes
    // there, measured), and its own save is this same picker.
    if (zone_state_peek() == FF132_ZONE_STATE_DISPOSED) {
        ff132_log(FF132_TAG "[%u] slot-select: refused at the Historia Crux\n", (unsigned)GetTickCount());
        ff132_flash(ui_text(T_SAVEHERE_HISTORIA));
        return;
    }
    g_pkDelay = FF132_PICKER_DELAY_FRAMES;
    g_pkIdleWait = FF132_PICKER_IDLE_WAIT;
    g_pkPending = 1;
    ff132_loga(FF132_TAG "[%u] slot-select: hotkey -> picker requested (zone_state=%d)\n",
           (unsigned)GetTickCount(), zone_state_peek());
}

// Hotkey thread. Only a flag is touched here; work_slot_step reads it on the game thread.
void save_toggle_autosave_redirect(void) {
    if (!g_frameOk || !g_wsOk) {
        ff132_loga(FF132_TAG "[%u] working-slot: switch refused, not armed\n", (unsigned)GetTickCount());
        ff132_flash(ui_text(T_WS_NA));
        return;
    }
    if (!g_autosaveRedirect) {
        int w = redirect_slot();
        // Unresolved: work_slot_step makes the same check on the first frame it resolves.
        if (w >= 0 && work_foreign(w, "switch-on")) {
            work_refused_flash(w);
            return;
        }
        g_wsChecked = w;
    }
    g_autosaveRedirect = g_autosaveRedirect ? 0 : 1;
    if (!g_autosaveRedirect) {
        // Off only stops the working slot being managed; the current slot is left where it is,
        // because the file the session has been writing to is the one that holds its progress.
        ff132_loga(FF132_TAG "[%u] working-slot: OFF (still playing on slot %d)\n",
               (unsigned)GetTickCount(), cur_slot());
        ff132_flash(ui_text(T_WS_OFF));
        return;
    }
    g_wsSettle = 0;
    int slot = redirect_slot();
    if (slot >= 0) {
        ff132_loga(FF132_TAG "[%u] working-slot: ON (slot %d is taken over at the next opportunity)\n",
               (unsigned)GetTickCount(), slot);
        char buf[128];       // Japanese is 3 bytes a glyph, so this line needs the room
        snprintf(buf, sizeof buf, ui_text(T_WS_ON_AS), slot + 1);
        ff132_flash_c(buf, FF132_COL_ON);
    } else {
        ff132_loga(FF132_TAG "[%u] working-slot: ON (slot not resolved yet)\n", (unsigned)GetTickCount());
        ff132_flash_c(ui_text(T_WS_ON), FF132_COL_ON);
    }
}

// Game thread. Second half of a hotkey save: the picker has committed a slot, so the game's own
// field save now runs for it and writes the position and the zone's progress that the picker does
// not. The enable bit is held for the duration the way the menu's save holds it.
static int g_gateWas = 0;       // bit 28 as the game had it before the mod's own save raised it

static int gate_bit(void) {
    uintptr_t gm = (uintptr_t)game_main();
    if (!gm || !rd_ok((void*)gm, FF132_OFF_GAMEMAIN_FLAGS + 4)) return 0;
    return (int)((rd32(gm + FF132_OFF_GAMEMAIN_FLAGS) >> FF132_GAMEMAIN_AUTOSAVE_BIT) & 1);
}

static void full_save_end(int on) {
    void* gm = game_main();
    if (gm) ((setAutosaveBit_t)(uintptr_t)(g_base + FF132_RVA_SET_AUTOSAVE_BIT))(gm, on);
}

static void full_save_step(void) {
    void* zsm = zsm_save_obj();
    if (!zsm || !game_main()) {
        if (g_fullDriving) full_save_end(g_gateWas);
        g_pkPending = g_pkFullSave = g_fullDriving = 0;
        ff132_loga(FF132_TAG "[%u] slot-select: no zone state machine -> the picker's save stands alone\n",
               (unsigned)GetTickCount());
        return;
    }
    if (!g_fullDriving) {
        int st = (int)rd32((uintptr_t)zsm + FF132_OFF_DOAUTOSAVE_STATE);
        if (st != 0) {
            if (--g_fullWait <= 0) {
                g_pkPending = g_pkFullSave = 0;
                ff132_loga(FF132_TAG "[%u] slot-select: doAutoSave never idle (state=%d) -> the picker's"
                              " save stands alone\n", (unsigned)GetTickCount(), st);
            }
            return;
        }
        g_gateWas = gate_bit();        // put it back exactly as it was, never plain off
        full_save_end(1);
        g_fullDriving = 1;
    }
    g_ownSave = 1;
    int r = ((doAutoSave_t)(uintptr_t)(g_base + FF132_RVA_DOAUTOSAVE))(zsm, g_saveTagEmpty, -1,
                                                                   g_saveTagEmpty);
    g_ownSave = 0;
    if (r == 0) return;
    full_save_end(g_gateWas);
    g_pkPending = g_pkFullSave = g_fullDriving = 0;
    ff132_loga(FF132_TAG "[%u] slot-select: field save finished r=%d cur_slot=%d\n",
           (unsigned)GetTickCount(), r, cur_slot());
}

// Game thread, once per frame. Drives the interactive creator exactly the way the Historia Crux
// onsave action does: update() until it answers non-zero, then hands off to the field save.
static void __cdecl on_frame(uint32_t ecx) {
    (void)ecx;
    ff132_overlay_poll();
    ff132_speed_pump();
    ff132_encount_pump();
    ff132_kill_pump();
    ff132_autoretry_pump();
    ff132_load_frame(save_in_flight());
    work_slot_step();
    if (!g_pkPending) return;
    if (g_pkFullSave) { full_save_step(); return; }
    if (g_pkDelay > 0) { g_pkDelay--; return; }
    void* creator = save_creator();
    if (!creator) {
        g_pkPending = g_pkDriving = 0;
        ff132_loga(FF132_TAG "[%u] slot-select: SaveDataCreator vanished -> picker dropped\n",
               (unsigned)GetTickCount());
        return;
    }
    if (!g_pkDriving) {
        int st   = (int)rd32((uintptr_t)creator + FF132_OFF_CREATOR_STATE);
        int busy = (int)rd32((uintptr_t)creator + FF132_OFF_CREATOR_BUSY);
        if (st != 0 || busy != 0) {
            if (--g_pkIdleWait <= 0) {
                g_pkPending = 0;
                ff132_loga(FF132_TAG "[%u] slot-select: creator never idle (state=%d busy=%d) -> picker dropped\n",
                       (unsigned)GetTickCount(), st, busy);
            }
            return;
        }
        // No zone-state gate: the field's state id differs per zone (23 in one, 3 in another, on
        // hardware), so the id is only logged and the creator's idle check is the sole guard.
        int zs = zone_state();
        g_pkDriving = 1;
        ff132_loga(FF132_TAG "[%u] slot-select: driving the interactive creator (zone_state=%d, cur_slot=%d)\n",
               (unsigned)GetTickCount(), zs, cur_slot());
    }
    int r = ((creatorUpdate_t)(uintptr_t)(g_base + FF132_RVA_CREATOR_UPDATE))(creator);
    if (r == 0) return;
    g_pkDriving = 0;
    if (r > 0 && g_fullOk && zone_state() != FF132_ZONE_STATE_DISPOSED) {
        g_pkFullSave = 1;
        g_fullWait = FF132_FULLSAVE_IDLE_WAIT;
        ff132_loga(FF132_TAG "[%u] slot-select: picker committed slot %d -> running the game's own field"
                      " save for the position and the zone's progress\n",
               (unsigned)GetTickCount(), cur_slot());
        return;
    }
    g_pkPending = 0;
    ff132_loga(FF132_TAG "[%u] slot-select: picker finished r=%d cur_slot=%d (%s)\n",
           (unsigned)GetTickCount(), r, cur_slot(), r > 0 ? "saved" : "cancelled or failed");
}

static void ss_install(void) {
    uintptr_t upd = g_base + FF132_RVA_CREATOR_UPDATE;
    if (wait_for_bytes(upd, FF132_SIG_CREATOR_UPDATE, (int)sizeof FF132_SIG_CREATOR_UPDATE,
                       FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "slot-select: update() @0x%08X never matched -> feature off\n", (unsigned)upd);
        return;
    }
    uintptr_t fr = g_base + FF132_RVA_FRAME_SITE;
    if (wait_for_bytes(fr, FF132_SIG_FRAME_SITE, (int)sizeof FF132_SIG_FRAME_SITE, FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "slot-select: frame site @0x%08X never matched -> feature off\n", (unsigned)fr);
        return;
    }
    void* fcave = ff13_install_ecx_hook(fr, FF132_SIG_FRAME_SITE, (int)sizeof FF132_SIG_FRAME_SITE,
                                        (void*)on_frame);
    if (!fcave) {
        ff132_loga(FF132_TAG "slot-select: frame site @0x%08X not patched -> feature off\n", (unsigned)fr);
        return;
    }
    g_frameOk = 1;
    ff132_loga(FF132_TAG "slot-select: frame hook @0x%08X ok cave=%p\n", (unsigned)fr, fcave);
    uintptr_t at = g_base + FF132_RVA_DOAUTOSAVE;
    if (wait_for_bytes(at, FF132_SIG_DOAUTOSAVE, (int)sizeof FF132_SIG_DOAUTOSAVE, FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "slot-select: doAutoSave @0x%08X never matched -> feature off\n",
               (unsigned)at);
        return;
    }
    void* cave = ff13_install_replace_hook(at, FF132_SIG_DOAUTOSAVE, (int)sizeof FF132_SIG_DOAUTOSAVE,
                                           (void*)on_doautosave, FF132_RET_DOAUTOSAVE);
    if (!cave) {
        ff132_loga(FF132_TAG "slot-select: doAutoSave @0x%08X not patched -> feature off\n", (unsigned)at);
        return;
    }
    g_ssOk = 1;
    ff132_loga(FF132_TAG "slot-select: doAutoSave @0x%08X replaced, cave=%p, update()@0x%08X ok\n",
           (unsigned)at, cave, (unsigned)upd);
    static const char kClassOnly[] = "mismatch (caller classification only)";
    g_fullOk = site_ok("slot-select: field save after the picker", FF132_RVA_SET_AUTOSAVE_BIT,
                       FF132_SIG_SET_AUTOSAVE_BIT, sizeof FF132_SIG_SET_AUTOSAVE_BIT,
                       "mismatch -> the picker's save will miss the position and the progress");
    g_blockOk = site_ok("autosave: doAutoSave_l call site", FF132_RVA_DOAUTOSAVE_L_CALL,
                        FF132_SIG_DOAUTOSAVE_L_CALL, sizeof FF132_SIG_DOAUTOSAVE_L_CALL, kClassOnly);
    g_histOk = site_ok("autosave: historia-return call site", FF132_RVA_HISTORIA_RET_CALL,
                       FF132_SIG_HISTORIA_RET_CALL, sizeof FF132_SIG_HISTORIA_RET_CALL, kClassOnly);
    g_menuPollOk = site_ok("autosave: menu save poller", FF132_RVA_MENU_SAVE_POLL,
                           FF132_SIG_MENU_SAVE_POLL, sizeof FF132_SIG_MENU_SAVE_POLL, kClassOnly);
    g_pauseSaveOk = site_ok("autosave: pause-menu save site", FF132_RVA_PAUSE_SAVE_CALL,
                            FF132_SIG_PAUSE_SAVE_CALL, sizeof FF132_SIG_PAUSE_SAVE_CALL, kClassOnly);
    uintptr_t spath = g_base + FF132_RVA_SAVE_PATH;
    g_wsOk = sig_at(FF132_RVA_SAVE_PATH, FF132_SIG_SAVE_PATH, sizeof FF132_SIG_SAVE_PATH);
    if (g_wsOk)
        ff132_loga(FF132_TAG "working-slot: save path builder @0x%08X: ok -- starts %s\n", (unsigned)spath,
               g_autosaveRedirect ? "ON" : "OFF");
    else
        ff132_loga(FF132_TAG "working-slot: save path builder @0x%08X: mismatch -> the working slot is"
                      " off\n", (unsigned)spath);
}

void save_install_hooks(void) {
    g_zoneGetterOk = site_ok("zone getter", FF132_RVA_ZONE_GETSTATE, FF132_SIG_ZONE_GETSTATE,
                             sizeof FF132_SIG_ZONE_GETSTATE, "mismatch -> zone_state stays -1");
    record_install_hooks();
#if FF132_DIAG_SAVEPATH
    diag_install_hooks();
#endif
    ss_install();
}
