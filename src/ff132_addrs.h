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

// Every address here is an RVA off ffxiii2img.exe's imagebase 0x400000 (ASLR off, so VA = RVA +
// 0x400000). 0 means "not reverse-engineered yet" and the installer skips that hook.
#ifndef FF132_ADDRS_H
#define FF132_ADDRS_H
#include <stdint.h>

// Build switch for the log-only save-path hooks; build.sh passes it, this is the fallback.
#ifndef FF132_DIAG_SAVEPATH
#define FF132_DIAG_SAVEPATH 0
#endif

// ---- save-slot selection ----------------------------------------------------------------------
// The field menu save is doAutoSave polled every frame with tag="" and n=-1; the interactive
// creator's update() is what puts the Historia Crux picker on screen.
//
// doAutoSave(tag, n, s2): ZoneStateMachine 2nd-vtable +0x18, thiscall, `ret 0xc`. The replacement
// hook and the save-path diagnostics share this one site.
#define FF132_RVA_DOAUTOSAVE 0x292680
// 8 bytes = sub esp,0x60 + mov eax,[0x23A90DC]; the mov is absolute-addressed, so it relocates.
static const uint8_t FF132_SIG_DOAUTOSAVE[] = { 0x83, 0xEC, 0x60, 0xA1, 0xDC, 0x90, 0x3A, 0x02 };
// Its own stack cleanup, which the replacement cave has to reproduce when it skips the function.
#define FF132_RET_DOAUTOSAVE 0xC

#define FF132_RVA_GAMEMAIN_PTR   0x1FABAA8   // GameMain* (VA 0x23ABAA8)

// ---- game speed -------------------------------------------------------------------------------
// The framework frame timer embedded at GameMain+8: its vtable (checked before any write) and its
// speed float, which the frame function 0xC04C20 multiplies the whole frame delta by (default 1.0).
#define FF132_RVA_FRAME_TIMER_VT 0x1ED86E8   // VA 0x22D86E8; slot 0x3C = get speed, 0x40 = set speed
#define FF132_OFF_GM_FRAME_TIMER 0x08
#define FF132_OFF_GM_FRAME_SPEED 0x20        // float, = frame timer +0x18
#define FF132_OFF_GM_TICKS       0x5B8       // int64, White.getTickCount's source, 1/300000 s units
#define FF132_TICKS_PER_SEC      300000.0
#define FF132_OFF_SAVE_CREATOR   0x43C       // the INTERACTIVE SaveDataCreator (+0x450 is the silent one)
// GfSaveDataCreator::update(), thiscall, no arguments. 0 = keep polling, 1 = saved, negative =
// cancelled or failed; it resets its own +0x10 on the way out. Called, not hooked, so a drifted
// address would surface only at call time -- the bytes below are checked once before it is used.
#define FF132_RVA_CREATOR_UPDATE 0x622110
static const uint8_t FF132_SIG_CREATOR_UPDATE[] = { 0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00,
                                                 0xA1, 0xDC, 0x90, 0x3A, 0x02 };
#define FF132_OFF_CREATOR_STATE  0x10        // 0 idle / 1,2 picker on screen / 3 teardown
#define FF132_OFF_CREATOR_BUSY   0x198       // 1 while the save task owns the creator (0x6B87B0)

// Per-frame site on the game thread: the HID (pad) update that is the exe's only timeGetTime
// caller (VA 0x6A5D00, thiscall). The interactive creator's update() is driven from here, never
// from inside the menu's doAutoSave poll: while that poll is pending the main menu sits in its
// saving state and eats every input, so a picker opened there cannot be operated (measured).
#define FF132_RVA_FRAME_SITE      0x2A5D00
static const uint8_t FF132_SIG_FRAME_SITE[] = { 0x83, 0xEC, 0x10, 0xA1, 0x2C, 0x9F, 0x52, 0x02 };
#define FF132_PICKER_DELAY_FRAMES 2
#define FF132_PICKER_IDLE_WAIT    300         // frames to wait for an idle creator before giving up

// Driving the interactive creator alone writes the file but leaves the position and the zone's
// progress at whatever was last serialised, because its normal caller (the Historia Crux onsave
// machine) runs three more creators after it and the field's own save runs them too. So once the
// picker has committed a slot, the mod polls the game's own doAutoSave for that slot instead of
// reproducing those steps. Its `this` is the ZoneStateMachine's 2nd-vtable sub-object, the one the
// callers form with lea ecx,[ZoneStateMachine+0x1C].
#define FF132_OFF_ZSM_SAVE_OBJ 0x1C
// doAutoSave returns 1 without saving anything unless bit 29 (the autosave option) or bit 28 is set
// in GameMain+0x5C0, so the menu's save sets bit 28 around its own poll and the mod does the same --
// otherwise a hotkey save would silently do nothing with the autosave option turned off.
// The setter below FORCES bit 28 to its argument, so this word has to be read first and put back
// afterwards: clearing a bit the game itself had set turns the game's own autosaves off for the
// rest of the session (measured: the working slot went stale and a battle restart rolled back).
#define FF132_OFF_GAMEMAIN_FLAGS 0x5C0
#define FF132_GAMEMAIN_AUTOSAVE_BIT 28
// thiscall(GameMain, int on); it clears bit 28 and then ors in (on << 28).
#define FF132_RVA_SET_AUTOSAVE_BIT 0x2B9AE0
static const uint8_t FF132_SIG_SET_AUTOSAVE_BIT[] = { 0x81, 0xA1, 0xC0, 0x05, 0x00, 0x00,
                                                   0xFF, 0xFF, 0xFF, 0xEF };
#define FF132_FULLSAVE_IDLE_WAIT 300          // frames to wait for an idle doAutoSave before giving up

// ---- autosave redirect: the working slot ------------------------------------------------------
// The current slot's FILE is the live per-world store: doAutoSave is the only path that persists a
// world's progress flags and position, including the flush fired on the way out to the Historia
// Crux, and re-entering that world or retrying a battle reads those blocks back out of the same
// file. A save therefore cannot be retargeted per call (the world-exit flushes would all miss the
// player's slot); the mod copies the loaded slot onto the working slot and plays there instead.
// Both a script's autosave and the pause menu's save land in the same doAutoSave, and the return
// address is the only thing that tells them apart; the classification feeds the log only.
// Neither site below is hooked: their bytes are compared once at install and the addresses are
// then used as plain constants.
//
// White.doAutoSave_l (VA 0x9F5F70) is the native every script-fired autosave goes through; this is
// its `call eax` into doAutoSave, so a script call always returns to +2.
#define FF132_RVA_DOAUTOSAVE_L_CALL 0x5F605B
static const uint8_t FF132_SIG_DOAUTOSAVE_L_CALL[] = { 0xFF, 0xD0, 0x83, 0x86, 0x58, 0x09, 0x00,
                                                    0x00, 0xF8 };
#define FF132_RVA_DOAUTOSAVE_L_RET (FF132_RVA_DOAUTOSAVE_L_CALL + 2)

// The pause menu's own save poller (VA 0x72C800), which calls doAutoSave("", -1, "") from +0x1D.
// Its saves are the player's; like the others it is only classified for the log.
#define FF132_RVA_MENU_SAVE_POLL 0x32C800
static const uint8_t FF132_SIG_MENU_SAVE_POLL[] = { 0xA1, 0x28, 0x9F, 0x52, 0x02, 0x8B, 0x40, 0x38,
                                                 0x8B, 0x50, 0x1C, 0x8D, 0x48, 0x1C, 0x8B, 0x42,
                                                 0x18 };
#define FF132_RVA_MENU_SAVE_POLL_RET (FF132_RVA_MENU_SAVE_POLL + 0x1F)

// The pause menu's "Return to Historia Crux" step (GuiMainMenu state machine at VA 0x796E30) fires
// the same doAutoSave("", -1, "") from a third return address. It is the world-exit flush, named in
// the log with the script autosaves -- but it is a different call site from the plain menu "save"
// poller above, which is the player's own save.
// The signature ends in the `call edx` (FF D2), so the return address is CALL + its 19 bytes.
#define FF132_RVA_HISTORIA_RET_CALL 0x397430
static const uint8_t FF132_SIG_HISTORIA_RET_CALL[] = { 0x8B, 0x01, 0x8B, 0x50, 0x18, 0x68, 0x40, 0xB4,
                                                    0x15, 0x05, 0x6A, 0xFF, 0x68, 0x40, 0xB4, 0x15,
                                                    0x05, 0xFF, 0xD2 };
#define FF132_RVA_HISTORIA_RET (FF132_RVA_HISTORIA_RET_CALL + 0x13)

// The same pause menu's "save" choice (state 13 of that machine) is the player's own save; it only
// gets a name in the log. Same shape, `call eax` this time.
#define FF132_RVA_PAUSE_SAVE_CALL 0x397563
static const uint8_t FF132_SIG_PAUSE_SAVE_CALL[] = { 0x8B, 0x11, 0x8B, 0x42, 0x18, 0x68, 0x40, 0xB4,
                                                  0x15, 0x05, 0x6A, 0xFF, 0x68, 0x40, 0xB4, 0x15,
                                                  0x05, 0xFF, 0xD0 };
#define FF132_RVA_PAUSE_SAVE_RET (FF132_RVA_PAUSE_SAVE_CALL + 0x13)

// doAutoSave's own step state on the `this` it gets in ecx: 0 = idle, 1..9 = a save in flight.
#define FF132_OFF_DOAUTOSAVE_STATE 0x68

// int, the game's slot count (40 normally, 10 in one mode), written once at startup by 0x6B44A0 and
// compared as `slot < count` everywhere; the picker's last row is count-1. A data address, so it
// carries no signature and the signature checker prints it as skipped.
#define FF132_RVA_MAX_SLOTS 0x4D31328

// The save-path builder: thiscall on a context buffer whose +0x148 is the slot number and whose
// +0x14C is an "already initialised" flag -- zeroed, it creates the save directory itself. It
// returns the full path of that slot's ff13_2-NN.dat (the string lives at ctx+0x44C) and owns
// nothing the caller has to free. Called, not hooked, so a drifted address would surface only at
// call time: its bytes are checked once before it is used.
#define FF132_RVA_SAVE_PATH 0x2B5E30
static const uint8_t FF132_SIG_SAVE_PATH[] = { 0x83, 0xEC, 0x20, 0xA1, 0xDC, 0x90, 0x3A, 0x02,
                                            0x33, 0xC4 };
#define FF132_OFF_SAVEPATH_SLOT 0x148
#define FF132_OFF_SAVEPATH_INIT 0x14C
#define FF132_SAVEPATH_CTX      0x600
// Frames the current slot has to stay off the working slot before the takeover runs, so a load or
// a save that is still finishing is not cut into.
#define FF132_WORKSLOT_SETTLE 30

// ---- save records: was this file written in a world or at the Historia Crux? -------------------
// Nothing in a save image marks a Historia Crux save, so
// the mod writes the answer down itself when a file is finished and looks it up when one is read
// back. Both moments are calls to setCurrentSaveSlot and ONLY the return address tells them apart,
// so the three call sites are byte-checked at install and their return addresses used as constants.
//
// H4 silent save body (writes to whatever getCurrentSaveSlot returns). Three calls per save and the
// entry cannot tell which is the last, so the entry only marks the slot pending.
#define FF132_RVA_SILENT_SAVE 0x2B79D0
static const uint8_t FF132_SIG_SILENT_SAVE[] = { 0x81, 0xEC, 0xB0, 0x08, 0x00, 0x00 };

// H6 setCurrentSaveSlot(int) -- the body 0x954C40 jumps to; the whole function is 17 bytes.
#define FF132_RVA_SET_CUR_SLOT 0x5871F0
// 10 bytes: mov eax,[esp+4] is only 4, and the hook needs 5. Both instructions are
// position-independent, and the `ret` at 0x987200 stays outside the patched range.
static const uint8_t FF132_SIG_SET_CUR_SLOT[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0x0D, 0x98, 0x1F,
                                               0x15, 0x05 };

// The tail of the interactive write body 0x6B71F0: the file of the slot handed to the setter has
// been written and closed. The Historia Crux save and the picker save both end here.
#define FF132_RVA_WRITE_DONE_CALL 0x2B7593
static const uint8_t FF132_SIG_WRITE_DONE_CALL[] = { 0xE8, 0xA8, 0xD6, 0x29, 0x00, 0x8B, 0x4D, 0x00,
                                                  0x51, 0xE8 };
// The two tails of the load body 0x6B75D0: a slot's file has just been READ for a title-screen
// load. Both run before the load-completion task can see the load as finished, so the direct-load
// decision made here is still in time.
#define FF132_RVA_LOAD_DONE_CALL_A 0x2B7699
static const uint8_t FF132_SIG_LOAD_DONE_CALL_A[] = { 0xE8, 0xA2, 0xD5, 0x29, 0x00, 0x8B, 0x0F,
                                                   0x51, 0xE8, 0xBA };
#define FF132_RVA_LOAD_DONE_CALL_B 0x2B7754
static const uint8_t FF132_SIG_LOAD_DONE_CALL_B[] = { 0xE8, 0xE7, 0xD4, 0x29, 0x00, 0x8B, 0x07,
                                                   0x50, 0xE8, 0xFF };
// Each signature opens with the 5-byte call itself, so the return address is the site + 5.
#define FF132_RA_WRITE_DONE  (FF132_RVA_WRITE_DONE_CALL + 5)
#define FF132_RA_LOAD_DONE_A (FF132_RVA_LOAD_DONE_CALL_A + 5)
#define FF132_RA_LOAD_DONE_B (FF132_RVA_LOAD_DONE_CALL_B + 5)

// Zone::getCurrentState() while the Historia Crux is up (STATE_DISPOSED). Every other value is a
// world, which is what makes a save a field save.
#define FF132_ZONE_STATE_DISPOSED 78

#if FF132_DIAG_SAVEPATH
// ---- save-path diagnostics --------------------------------------------------
// Log-only hooks that answer "which function does a menu save go through".
// The signature checker re-reads every pair below out of
// ffxiii2img.exe, so keep the name suffixes of FF132_RVA_x / FF132_SIG_x identical.
//
// H1 GuiMainMenu item handler: tests Zone::getCurrentState() == 0x3E.
#define FF132_RVA_MENU_HANDLER 0x442CE0
// Stops before the `je` at 0x842CE7: a relocated Jcc would carry its original rel8 into the cave.
static const uint8_t FF132_SIG_MENU_HANDLER[] = { 0x56, 0x8B, 0xF1, 0x83, 0x7E, 0x74, 0x00 };

// H2 ZoneStateMachine 2nd-vtable +0x1C ("onsave"): the interactive save step function.
#define FF132_RVA_ONSAVE 0x291F10
static const uint8_t FF132_SIG_ONSAVE[] = { 0x81, 0xEC, 0x28, 0x01, 0x00, 0x00 };
// Sub-state driving its jump table at 0x692234; esi = this throughout the function.
#define FF132_OFF_ONSAVE_STATE 0x7C

// No H3: doAutoSave's site belongs to the replacement hook, which logs that line itself.

// H7 the load body, logged with the slot it runs on (the slot a battle restart re-reads the world
// from).
#define FF132_RVA_LOAD_BODY 0x2B7E30
static const uint8_t FF132_SIG_LOAD_BODY[] = { 0x81, 0xEC, 0xAC, 0x08, 0x00, 0x00,
                                            0xA1, 0xDC, 0x90, 0x3A, 0x02 };

// H8/H9 the battle restart: RestartDataCreator's save and load of the in-memory
// "White.RestartData" entry, which is what a restart writes and reads back (a random-encounter
// fight and its restart write no save file).
#define FF132_RVA_RESTART_SAVE 0x2B3480
// 11 bytes, NOT 10: push/push/push + mov edi,ecx is 5, and the mov ecx,[edi+0x1A8] after it is six
// long. Cutting it at ten relocates half an instruction and the game dies entering a fight
// (measured).
static const uint8_t FF132_SIG_RESTART_SAVE[] = { 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x8F,
                                               0xA8, 0x01, 0x00, 0x00 };
#define FF132_RVA_RESTART_LOAD 0x2B3510
static const uint8_t FF132_SIG_RESTART_LOAD[] = { 0x56, 0x8B, 0xB1, 0xA8, 0x01, 0x00, 0x00,
                                               0x85, 0xF6 };

// H5 slot-picker screen ("#SaveLoadX360") builder.
#define FF132_RVA_PICKER_SCREEN 0x2B87B0
static const uint8_t FF132_SIG_PICKER_SCREEN[] = { 0x81, 0xEC, 0x90, 0x05, 0x00, 0x00 };

#endif // FF132_DIAG_SAVEPATH

// ---- runtime state read by the handlers -------------------------------------------------------
#define FF132_RVA_ZONE_MGR_PTR   0x2129F28   // ZoneManager*  (VA 0x2529F28)
#define FF132_RVA_ZONE_GETSTATE  0x284370    // Zone::getCurrentState(), thiscall, ecx = ZoneManager*
// This one is called, not hooked, so the bytes are only a sanity check: mov ecx,[ecx+0x38] plus the
// tail jump, whose rel32 pins the body at 0x692E50 too.
static const uint8_t FF132_SIG_ZONE_GETSTATE[] = { 0x8B, 0x49, 0x38, 0xE9, 0xD8, 0xEA, 0x00, 0x00 };
#define FF132_RVA_SAVEDATA_PTR   0x4D51F98   // save-data singleton* (VA 0x5151F98)
#define FF132_OFF_CUR_SLOT       0x4DF88     // current save slot inside that singleton
// The lock-free walk getCurrentState() makes: ZoneManager -> ZoneStateMachine -> FSM -> state id.
#define FF132_OFF_ZONE_FSM       0x38
#define FF132_OFF_ZONE_STATE_OBJ 0x50
#define FF132_OFF_ZONE_STATE_ID  0x04

// ---- the renderer object: where the game's OWN IDirect3DDevice9 is found ----------------------
// The renderer keeps the IDirect3D9 it created at a fixed offset; an object with a d3d9 vtable
// sitting there is the anchor that says this really is the renderer and that graphics init has run.
// The IDirect3DDevice9 goes in at +0x00 (0xC20728, right after CreateDevice writes it to the global
// at VA 0x516BA88), but it lands there LATER than the anchor does, so the device is not read at a
// fixed offset: the window below is scanned and each candidate validated as a d3d9 device vtable.
// The object is 0x620 bytes, so the window stays well inside it.
// Data addresses: no signature, so the signature checker prints FF132_RVA_RENDERER_PTR as skipped.
#define FF132_RVA_RENDERER_PTR   0x4D6BAA0   // renderer* (VA 0x516BAA0), stored by 0xC26A58
#define FF132_OFF_RENDERER_D3D9  0x14C       // IDirect3D9*, stored by 0xC26A97
#define FF132_RENDERER_SCAN_LO   0x0         // starts at 0 so the known device field is step one
#define FF132_RENDERER_SCAN_HI   0x400
// IDirect3DDevice9*: CreateDevice is handed &this as ppReturnedDeviceInterface at 0xC206BA and the
// result is stored back at 0xC20723, so it is the device itself rather than a field that happens to
// hold one. Tried before the scan.
#define FF132_RVA_DEVICE_PTR     0x4D6BA88   // (VA 0x516BA88)
// ...but what lands there is the game's own 0x14-byte wrapper, not the device: 0xC315D0 allocates
// it when [0x23A8644] is set, its ctor 0xC2FB30 stores the real IDirect3DDevice9 at obj+0xC and
// hands back obj+4, so the device sits 8 bytes past the pointer everything else sees. Its vtable is
// in the exe, so patching a vtable taken from a d3d9-created device never sees the game's calls.
#define FF132_OFF_WRAPPER_DEVICE 8

// ---- battle overlay ---------------------------------------------------------------------------
// All read-only: nothing below is hooked. Each FF132_RVA_x carries an anchor instruction as its
// FF132_SIG_x so the signature checker catches a drifted offset without starting the game -- the anchors are
// the very instructions the offsets were read off.
//
// BattleMain is created once at boot (0x4A2AE0, only via the wrapper ctor 0x48C470) and reused:
// between fights its state goes back to 0/1, and a cumulative field keeps the last fight's figure
// until the next fight resets it. The OTHER copy of the pointer at VA 0x251E5FC is written again
// during teardown; this one is the one White.isBattleEnd reads.
#define FF132_RVA_BATTLE_MAIN_PTR 0x211E5F8   // BattleMain* (VA 0x251E5F8)
#define FF132_RVA_OPT_ATB_TIME    0x211E894   // iOptAtbTime (int, ms); 0 = the battle db is not in yet
#define FF132_RVA_EASY_OPT_CH     0x211E8D8   // fEasyOptCh (float)
// White.isBattleEnd's body, which is also where the BattleMain pointer's VA is read off.
#define FF132_RVA_BATTLE_END_FN   0x07CE30
static const uint8_t FF132_SIG_BATTLE_END_FN[] = { 0x8B, 0x0D, 0xF8, 0xE5, 0x51, 0x02, 0x85, 0xC9,
                                                0x75, 0x04, 0x8D, 0x41, 0x01, 0xC3 };

#define FF132_OFF_BM_FLAGS      0x38    // flag n = [BM+0x38+(n>>5)*4] & (1<<(n&31)); four words
// Battle flag 3 is a per-scene option set at setup, and what it picks is which state counts as the
// end: BattleMain::isBattleEnd (0x494770) answers [BM+0x48] >= 0x33 with it and >= 0x2D without.
#define FF132_BFLAG_END_LATE    3
#define FF132_BM_STATE_END      0x2D
#define FF132_BM_STATE_END_LATE 0x33
#define FF132_BM_STATE_RESULTS  0x26    // set in the update that decides the fight (win or loss)
#define FF132_BM_STATE_ABORT    0x27    // the abort branch (auto-retry) and the step after results
#define FF132_BFLAG_EASY        0x49    // scales the paradigm-shift interval by fEasyOptCh
#define FF132_OFF_BM_STATE      0x48    // 0 = no fight is running; the two END values above are
                                     // where the game itself calls the fight over
#define FF132_OFF_BM_CLOCK      0x50    // battle clock (ms) -- what the shift interval is measured on
#define FF132_OFF_BM_GAMETIME   0x68    // the battle timer shown as GAME (ms)
#define FF132_OFF_BM_OPTSTAMP   0xE8    // the battle clock reading at the last shift refill
#define FF132_OFF_BM_ACTORS     0x40C   // -> chara*[FF132_BM_ACTOR_SLOTS], the handle being the index
#define FF132_BM_ACTOR_SLOTS    0x13
#define FF132_OFF_BM_TARGET     0x474   // the target's actor handle; -1 = nothing targeted
// The handle setTarget is fed from (0x49AAE1) and the second slot the getter at 0x4956F0
// reads -- both are logged in the verbose battle line so a missing TARGET row can be told
// apart from an empty +0x474.
#define FF132_OFF_BM_TARGET_FEED 0x46C
#define FF132_OFF_BM_TARGET_ALT  0x478
// Per-frame clock advance: pins the flag base, the state field and GAME's 0x15752618 ceiling.
#define FF132_RVA_BM_CLOCK_ADV  0x0ADE44
static const uint8_t FF132_SIG_BM_CLOCK_ADV[] = { 0xF6, 0x46, 0x38, 0x20, 0x0F, 0x85, 0x87, 0x00,
                                               0x00, 0x00, 0x8B, 0x45, 0x08, 0x01, 0x46, 0x54,
                                               0xF6, 0x46, 0x38, 0x20, 0x75, 0x7B, 0x8B, 0x46,
                                               0x48, 0x83 };
#define FF132_RVA_BM_GAME_TIME  0x0ADE7B
static const uint8_t FF132_SIG_BM_GAME_TIME[] = { 0xF6, 0x46, 0x3F, 0x01, 0x8B, 0xD9, 0x75, 0x4A,
                                               0x01, 0x5E, 0x68, 0x8B, 0x46, 0x68, 0x8B, 0x56,
                                               0x60 };
// The paradigm-shift gate inside changeOptimaDeck: the easy flag's word and bit, both constants'
// addresses and the stamp offset, in one run of instructions.
#define FF132_RVA_OPTIMA_GATE   0x0A4925
static const uint8_t FF132_SIG_OPTIMA_GATE[] = { 0xF7, 0x46, 0x40, 0x00, 0x02, 0x00, 0x00, 0x8B,
                                              0x3D, 0x94, 0xE8, 0x51, 0x02, 0x74, 0x10, 0xF3,
                                              0x0F, 0x2A, 0xC7, 0xF3, 0x0F, 0x59, 0x05, 0xD8,
                                              0xE8, 0x51, 0x02, 0xF3, 0x0F, 0x2C, 0xF8, 0x8D,
                                              0x9E, 0xE8, 0x00, 0x00, 0x00, 0x8B, 0xCB, 0xE8,
                                              0xBF, 0x20, 0x01, 0x00 };
// isBattleEnd's two tails, which is where the flag and both thresholds come from.
#define FF132_RVA_BATTLE_END_TEST 0x0947A9
static const uint8_t FF132_SIG_BATTLE_END_TEST[] = { 0xF6, 0x43, 0x38, 0x08, 0x74, 0x19, 0x33, 0xD2,
                                                  0x83, 0x7B, 0x48, 0x33, 0x8B, 0xCE, 0x0F, 0x9D,
                                                  0xC2, 0x8B, 0xFA };
// BattleMain::setTarget -- where [BM+0x474] comes from.
#define FF132_RVA_BM_SET_TARGET 0x095680
static const uint8_t FF132_SIG_BM_SET_TARGET[] = { 0x8B, 0x81, 0x74, 0x04, 0x00, 0x00, 0x56, 0x8D,
                                                0xB1, 0x74, 0x04, 0x00, 0x00 };
// ...and the tail of getCharaByHandle, where the actor table is indexed.
#define FF132_RVA_BM_ACTOR_LOOKUP 0x08ECE5
static const uint8_t FF132_SIG_BM_ACTOR_LOOKUP[] = { 0x8B, 0x8E, 0x0C, 0x04, 0x00, 0x00, 0x98,
                                                  0x8B, 0x04, 0x81 };

#define FF132_OFF_CHARA_HPFLAG  0x286   // byte; 0x80 set = the live HP is at +0x2D8 instead
#define FF132_OFF_CHARA_HP      0x2D0
#define FF132_OFF_CHARA_HP_ALT  0x2D8
#define FF132_OFF_CHARA_KIND    0x2A8   // kind = (int)(x << 16) >> 28
#define FF132_CHARA_ENEMY_KINDS 0xA2    // ...and (1 << kind) against this says "enemy" (kinds 1/5/7)
#define FF132_OFF_CHARA_ATB     0x2E8   // raw: 1000 = one gauge segment, 100 = one ability point
#define FF132_OFF_CHARA_CHAIN   0x2218
#define FF132_OFF_CHAIN_STATE   0x10    // byte: 0 = nothing running, 3 = broken
#define FF132_OFF_CHAIN_LEFT    0x28    // ms left, counted down on the battle clock
#define FF132_RVA_CHARA_GET_HP  0x043F90
static const uint8_t FF132_SIG_CHARA_GET_HP[] = { 0xF6, 0x81, 0x86, 0x02, 0x00, 0x00, 0x80, 0x74,
                                               0x07, 0x8B, 0x81, 0xD8, 0x02, 0x00, 0x00, 0xC3,
                                               0x8B, 0x81, 0xD0, 0x02, 0x00, 0x00 };
#define FF132_RVA_CHARA_GET_ATB 0x03FFF0
static const uint8_t FF132_SIG_CHARA_GET_ATB[] = { 0x8B, 0x81, 0xE8, 0x02, 0x00, 0x00, 0x50 };
#define FF132_RVA_CHARA_IS_ENEMY 0x03FE10
static const uint8_t FF132_SIG_CHARA_IS_ENEMY[] = { 0x8B, 0x89, 0xA8, 0x02, 0x00, 0x00, 0xC1, 0xE1,
                                                 0x10, 0xC1, 0xF9, 0x1C, 0xB0, 0x01, 0xD2, 0xE0,
                                                 0xA8, 0xA2 };
#define FF132_RVA_CHARA_CHAIN_FN 0x042FE0
static const uint8_t FF132_SIG_CHARA_CHAIN_FN[] = { 0xA1, 0xFC, 0xE5, 0x51, 0x02, 0x8B, 0x40, 0x50,
                                                 0x50, 0x81, 0xC1, 0x18, 0x22, 0x00, 0x00 };
#define FF132_RVA_CHAIN_COUNTDOWN 0x081FF0
static const uint8_t FF132_SIG_CHAIN_COUNTDOWN[] = { 0x8A, 0x51, 0x10, 0x33, 0xC0, 0x3A, 0xD0, 0x74,
                                                  0x62, 0x56, 0x8B, 0x74, 0x24, 0x08, 0x29, 0x71,
                                                  0x28 };

// ---- random-encounter suppression ------------------------------------------------------------
// The random-spawn system ("AutoEncount") manager is [0x2523A8C]; any nonzero bit in its dword
// +0x1B788 ("suppress mask") stops new groups and makes a spawned one disappear ("by mask"). The game
// uses bit 8 while riding a chocobo; bit 0 is the dev menu's "Enable Auto Encount" and nothing else
// writes it. The dword is zeroed on manager creation and on zone change, so it is re-set each frame.
// Signature: the game's own set-mask-bit helper, which names both the global and the offset.
#define FF132_RVA_ENCOUNT_MASKSET 0x13C2F0  // mov ecx,[esp+4]; mov eax,[0x2523A8C]; ...; or [eax+0x1B788],edx
static const uint8_t FF132_SIG_ENCOUNT_MASKSET[] = { 0x8B, 0x4C, 0x24, 0x04, 0xA1, 0x8C, 0x3A, 0x52,
                                                  0x02, 0xBA, 0x01, 0x00, 0x00, 0x00, 0xD3, 0xE2,
                                                  0x09, 0x90, 0x88, 0xB7, 0x01, 0x00, 0xC3 };
#define FF132_VA_AUTOENC_MGR  0x2523A8C     // AutoEncount manager pointer (NULL until created)
#define FF132_OFF_AUTOENC_MASK 0x1B788      // its suppress mask
#define FF132_AUTOENC_MASK_OURS 0x1         // the dev-menu bit

// ---- battle auto-retry: a won event battle ends through the game's own abort path
// The decided-fight branch of BattleMain::update (0x4AE070) becomes `jmp cave`; the cave asks the
// mod first and, on a retry, takes the abort branch (setState 0x27) instead of the results.
// Reached only from the two jne below (flag 0x26 / the decide call), once per decided fight.
#define FF132_RVA_AR_DECIDED 0x0AF82C       // push 0x50; call dflag test; add esp,4; mov ecx,esi; test al,al; je; <dflag body>
static const uint8_t FF132_SIG_AR_DECIDED[] = { 0x6A, 0x50, 0xE8, 0x6D, 0x43, 0xF5, 0xFF, 0x83,
                                             0xC4, 0x04, 0x8B, 0xCE, 0x84, 0xC0, 0x74, 0x0F,
                                             0x81, 0x4E, 0x38, 0x00, 0x00, 0x00, 0x10, 0x55,
                                             0x6A, 0x03, 0xE9, 0x3A, 0x19, 0x00, 0x00 };
#define FF132_AR_PATCH_LEN 16               // the five instructions before the dflag body; nothing jumps inside them
#define FF132_RVA_AR_DECIDED_FROM 0x0AF5F9  // push 0x26; ...; jne DECIDED; push ebp; ...; call decide; jne DECIDED
static const uint8_t FF132_SIG_AR_DECIDED_FROM[] = { 0x6A, 0x26, 0x8B, 0xCE, 0xE8, 0x1E, 0x46, 0xF9,
                                                  0xFF, 0x85, 0xC0, 0x0F, 0x85, 0x22, 0x02, 0x00,
                                                  0x00, 0x55, 0x8B, 0xCE, 0xE8, 0x2E, 0xAC, 0xFE,
                                                  0xFF, 0x85, 0xC0, 0x0F, 0x85, 0x12, 0x02, 0x00,
                                                  0x00 };
// Cave branch targets. ABORT expects ecx = esi = BM and ebp = 1, as at the site.
#define FF132_RVA_AR_DFLAG_BODY 0x0AF83C    // or [esi+0x38],0x10000000; push ebp; push 3; jmp setState
static const uint8_t FF132_SIG_AR_DFLAG_BODY[] = { 0x81, 0x4E, 0x38, 0x00, 0x00, 0x00, 0x10, 0x55,
                                                0x6A, 0x03, 0xE9, 0x3A, 0x19, 0x00, 0x00 };
#define FF132_RVA_AR_RESULTS 0x0AF84B       // push 0x26; call bflag test (the results path)
static const uint8_t FF132_SIG_AR_RESULTS[] = { 0x6A, 0x26, 0xE8, 0xCE, 0x43, 0xF9, 0xFF };
#define FF132_RVA_AR_ABORT_TEST 0x0AF5CE    // push 0x45; call bflag test; je; <ABORT>
static const uint8_t FF132_SIG_AR_ABORT_TEST[] = { 0x6A, 0x45, 0x8B, 0xCE, 0xE8, 0x49, 0x46, 0xF9,
                                                0xFF, 0x8B, 0xCE, 0x85, 0xC0, 0x74, 0x0D, 0x55,
                                                0x6A, 0x27, 0xE8, 0xEB, 0xE6, 0xFD, 0xFF, 0xE9,
                                                0x62, 0x0F, 0x00, 0x00 };
#define FF132_RVA_AR_ABORT 0x0AF5DD         // push ebp; push 0x27; call setState; jmp
static const uint8_t FF132_SIG_AR_ABORT[] = { 0x55, 0x6A, 0x27, 0xE8, 0xEB, 0xE6, 0xFD, 0xFF, 0xE9,
                                           0x62, 0x0F, 0x00, 0x00 };
#define FF132_RVA_AR_DFLAG_TEST 0x003BA0    // cdecl bool(int n): debug flag n; the cave re-runs it
static const uint8_t FF132_SIG_AR_DFLAG_TEST[] = { 0x8B, 0x44, 0x24, 0x04, 0x8B, 0xC8, 0x83, 0xE1,
                                                0x1F, 0xBA, 0x01, 0x00, 0x00, 0x00, 0xD3, 0xE2,
                                                0xC1, 0xE8, 0x05, 0x85, 0x14, 0x85, 0xAC, 0x75,
                                                0x13, 0x05, 0x0F, 0x95, 0xC0, 0xC3 };
// The pause menu's Retry (ZSM action 0x68BC30 cases 9..0x10) and what the mod borrows from it.
// All cdecl() -> int unless noted; each is a no-op returning 0/1 when there is no BattleMain.
#define FF132_RVA_AR_BM_ABORT 0x07CDC0      // flags 0x45/0x1D + freeze, once (guarded)
static const uint8_t FF132_SIG_AR_BM_ABORT[] = { 0x8B, 0x0D, 0xF8, 0xE5, 0x51, 0x02, 0x85, 0xC9,
                                              0x75, 0x03, 0x33, 0xC0, 0xC3, 0xE8, 0xFE, 0x6B,
                                              0x01, 0x00, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
#define FF132_RVA_AR_BM_WAIT46 0x07CDE0     // BM gone / flag 0x46 / state 0
static const uint8_t FF132_SIG_AR_BM_WAIT46[] = { 0x8B, 0x0D, 0xF8, 0xE5, 0x51, 0x02, 0x85, 0xC9,
                                               0x75, 0x04, 0x8D, 0x41, 0x01, 0xC3, 0x56, 0x33,
                                               0xF6, 0xE8, 0x6A, 0x6C, 0x01, 0x00, 0x85, 0xC0,
                                               0x75, 0x0F, 0x8B, 0x0D, 0xF8, 0xE5, 0x51, 0x02,
                                               0xE8, 0xBB, 0x12, 0x01, 0x00, 0x85, 0xC0, 0x74,
                                               0x05, 0xBE, 0x01, 0x00, 0x00, 0x00, 0x8B, 0xC6,
                                               0x5E, 0xC3 };
#define FF132_RVA_AR_BM_SHUTDOWN 0x07CD60   // -> BM state 0x2F (guarded by [BM+0x3C] bit 27)
static const uint8_t FF132_SIG_AR_BM_SHUTDOWN[] = { 0x8B, 0x0D, 0xF8, 0xE5, 0x51, 0x02, 0x85, 0xC9,
                                                 0x75, 0x03, 0x33, 0xC0, 0xC3, 0xE8, 0x6E, 0x1C,
                                                 0x02, 0x00, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
#define FF132_RVA_AR_BM_WAIT0 0x07CD80      // BM gone / [BM+0x48]==0 / GameMain FSM == 0xB
static const uint8_t FF132_SIG_AR_BM_WAIT0[] = { 0x8B, 0x0D, 0xF8, 0xE5, 0x51, 0x02, 0x85, 0xC9,
                                              0x75, 0x04, 0x8D, 0x41, 0x01, 0xC3, 0x56, 0x33,
                                              0xF6, 0xE8, 0x2A, 0x13, 0x01, 0x00, 0x85, 0xC0,
                                              0x75, 0x15, 0xA1, 0xA8, 0xBA, 0x3A, 0x02, 0x8B,
                                              0x88, 0x38, 0x04, 0x00, 0x00, 0xE8, 0x86, 0xCF,
                                              0x23, 0x00, 0x83, 0xF8, 0x0B, 0x75, 0x05, 0xBE,
                                              0x01, 0x00, 0x00, 0x00, 0x8B, 0xC6, 0x5E, 0xC3 };
#define FF132_RVA_AR_FADE_LAYER 0x0028F0    // thiscall(GameMain; int layer, float secs), ret 8
static const uint8_t FF132_SIG_AR_FADE_LAYER[] = { 0x8B, 0x44, 0x24, 0x04, 0x83, 0xBC, 0x81, 0x30,
                                                0x0A, 0x00, 0x00, 0x00, 0x75, 0x5A, 0xD9, 0x44 };
#define FF132_RVA_AR_FADER_BUSY 0x379A70    // [[0x514DE0C]+0x410]
static const uint8_t FF132_SIG_AR_FADER_BUSY[] = { 0xA1, 0x0C, 0xDE, 0x14, 0x05, 0x8B, 0x80, 0x10,
                                                0x04, 0x00, 0x00, 0xC3 };
#define FF132_RVA_AR_FADER_START 0x379B60   // cdecl void(float secs), caller pops
static const uint8_t FF132_SIG_AR_FADER_START[] = { 0x8B, 0x0D, 0x0C, 0xDE, 0x14, 0x05, 0xE8, 0x85,
                                                 0xB0, 0x00, 0x00, 0xD9, 0x44, 0x24, 0x04, 0x51,
                                                 0x8B, 0x0D, 0x0C, 0xDE, 0x14, 0x05, 0xD9, 0x1C,
                                                 0x24, 0x81, 0xC1, 0x40, 0x04, 0x00, 0x00, 0xE8,
                                                 0x4C, 0x37, 0x17, 0x00, 0x8B, 0x0D, 0x0C, 0xDE,
                                                 0x14, 0x05, 0xE9, 0xC1, 0x3E, 0x22, 0x00 };
#define FF132_RVA_AR_FADER_DONE 0x379B90    // nonzero = the fade has finished
static const uint8_t FF132_SIG_AR_FADER_DONE[] = { 0x8B, 0x0D, 0x0C, 0xDE, 0x14, 0x05, 0x56, 0xE8,
                                                0x54, 0xB0, 0x00, 0x00, 0x8B, 0x0D, 0x0C, 0xDE,
                                                0x14, 0x05, 0x81, 0xC1, 0x40, 0x04, 0x00, 0x00,
                                                0xE8, 0xD3, 0x3A, 0x17, 0x00, 0x8B, 0x0D, 0x0C,
                                                0xDE, 0x14, 0x05, 0x8B, 0xF0, 0xE8, 0x96, 0x3E,
                                                0x22, 0x00, 0x8B, 0xC6, 0x5E, 0xC3 };
#define FF132_RVA_AR_ZSM_TRIGGER7 0x2843E0  // thiscall(ZoneManager), no args: ZSM trigger 7
static const uint8_t FF132_SIG_AR_ZSM_TRIGGER7[] = { 0x8B, 0x49, 0x38, 0x68, 0x40, 0xB4, 0x15, 0x05,
                                                  0x6A, 0x00, 0x6A, 0x07, 0xE8, 0xAF, 0x04, 0x01,
                                                  0x00, 0xC3 };
// Pins inside the pause Retry action for the data the pump reads/writes: the fade-layer duration,
// the fader duration, and the field bit set before the trigger.
#define FF132_RVA_AR_PIN_FADE0 0x28BDB4     // fld [0xD5CAC4]; push ecx; mov ecx,[GameMain]; ...; call FADE_LAYER
static const uint8_t FF132_SIG_AR_PIN_FADE0[] = { 0xD9, 0x05, 0xC4, 0xCA, 0xD5, 0x00, 0x51, 0x8B,
                                               0x0D, 0xA8, 0xBA, 0x3A, 0x02, 0xD9, 0x1C, 0x24,
                                               0x57, 0xE8, 0x26, 0x6B, 0xD7, 0xFF };
#define FF132_RVA_AR_PIN_FADE1 0x28BDF6     // fld [0x22EC398]; push ecx; fstp [esp]; call FADER_START
static const uint8_t FF132_SIG_AR_PIN_FADE1[] = { 0xD9, 0x05, 0x98, 0xC3, 0x2E, 0x02, 0x51, 0xD9,
                                               0x1C, 0x24, 0xE8, 0x5B, 0xDD, 0x0E, 0x00, 0x83,
                                               0xC4, 0x04 };
#define FF132_RVA_AR_PIN_FIRE 0x28BE97      // mov eax,[0x2520684]; or [eax+0x169D0],0x800; push 0x36
static const uint8_t FF132_SIG_AR_PIN_FIRE[] = { 0xA1, 0x84, 0x06, 0x52, 0x02, 0x81, 0x88, 0xD0,
                                              0x69, 0x01, 0x00, 0x00, 0x08, 0x00, 0x00, 0x6A,
                                              0x36 };
#define FF132_RVA_AR_FADE0_SECS 0x95CAC4    // float (VA 0xD5CAC4)
#define FF132_RVA_AR_FADE1_SECS 0x1EEC398   // float (VA 0x22EC398)
#define FF132_RVA_AR_FADER_PTR  0x4D4DE0C   // fader* (VA 0x514DE0C)
#define FF132_OFF_AR_FADER_BUSY 0x410
#define FF132_RVA_AR_FIELD_PTR  0x2120684   // VA 0x2520684
#define FF132_OFF_AR_FIELD_BITS 0x169D0
#define FF132_AR_FIELD_BIT      0x800
// The classification mirrors the result function 0x497080 without its side effects.
#define FF132_RVA_AR_RESULT_FN 0x097080     // test [edi+0x40],0x80000000 (flag 0x5F, script win)
static const uint8_t FF132_SIG_AR_RESULT_FN[] = { 0x51, 0x57, 0x8B, 0xF9, 0xF7, 0x47, 0x40, 0x00,
                                               0x00, 0x00, 0x80 };
#define FF132_RVA_AR_RESULT_LOSE 0x0971AF   // [+0x44]&1; [+0x494]<=0; leader dead; [+0x495]; [+0x43]&1
static const uint8_t FF132_SIG_AR_RESULT_LOSE[] = { 0xF6, 0x47, 0x44, 0x01, 0x0F, 0x85, 0x97, 0x00,
                                                 0x00, 0x00, 0x80, 0xBF, 0x94, 0x04, 0x00, 0x00,
                                                 0x00, 0x0F, 0x8E, 0x8A, 0x00, 0x00, 0x00, 0x8D,
                                                 0x8F, 0x24, 0x04, 0x00, 0x00, 0xE8, 0x3F, 0x89,
                                                 0xFA, 0xFF, 0x8B, 0xC8, 0xE8, 0xF8, 0xCE, 0xFA,
                                                 0xFF, 0x85, 0xC0, 0x75, 0x74, 0x38, 0x87, 0x95,
                                                 0x04, 0x00, 0x00, 0x7E, 0x64, 0xF6, 0x47, 0x43,
                                                 0x01 };
// Enemies left without flag 0x58: flag 0x43 set -> the scene's end type picks the result
// (2 escape, 3 win, 4 lose, 5 win + event flag 5, else win); flag 0x43 clear -> win.
#define FF132_RVA_AR_RESULT_END 0x0971E8    // jne win; push 0x43; bflag test; je win; end type; jmp [tbl]
static const uint8_t FF132_SIG_AR_RESULT_END[] = { 0x75, 0x5E, 0x6A, 0x43, 0x8B, 0xCF, 0xE8, 0x2D,
                                                0xCA, 0xFA, 0xFF, 0x85, 0xC0, 0x74, 0x36, 0x8D,
                                                0x8F, 0x60, 0x07, 0x00, 0x00, 0xE8, 0x5E, 0xEA,
                                                0x4C, 0x00, 0x83, 0xC0, 0xFE, 0x83, 0xF8, 0x03,
                                                0x77, 0x3E, 0xFF, 0x24, 0x85, 0x58, 0x72, 0x49,
                                                0x00 };
#define FF132_RVA_AR_RESULT_END_TBL 0x097258  // end type 2..5 -> escape, win, lose, win
static const uint8_t FF132_SIG_AR_RESULT_END_TBL[] = { 0x11, 0x72, 0x49, 0x00, 0x48, 0x72, 0x49, 0x00,
                                                    0x50, 0x72, 0x49, 0x00, 0x19, 0x72, 0x49, 0x00 };
#define FF132_RVA_AR_END_TYPE 0x565C60      // ([[this+4]+0x20] >> 23) & 0xF, this = BM+0x760
static const uint8_t FF132_SIG_AR_END_TYPE[] = { 0x8B, 0x41, 0x04, 0x8B, 0x40, 0x20, 0xC1, 0xE8,
                                              0x17, 0x83, 0xE0, 0x0F, 0xC3 };
#define FF132_OFF_BM_SCENE_DB 0x760
#define FF132_END_TYPE_SHIFT  23
#define FF132_END_TYPE_ESCAPE 2
#define FF132_END_TYPE_LOSE   4
#define FF132_BM40_END_BY_TYPE 0x00000008  // [BM+0x40], flag 0x43
#define FF132_RVA_AR_CHARA_DEAD 0x0440D0   // ([c+0x27E] >> 4) & 1
static const uint8_t FF132_SIG_AR_CHARA_DEAD[] = { 0x0F, 0xB6, 0x81, 0x7E, 0x02, 0x00, 0x00, 0xC1,
                                                0xE8, 0x04, 0x83, 0xE0, 0x01, 0xC3 };
#define FF132_BM40_SCRIPT_WIN 0x80000000   // [BM+0x40], flag 0x5F
#define FF132_BM40_NO_END     0x01000000   // [BM+0x40], flag 0x58
#define FF132_BM44_LOST       0x00000001   // [BM+0x44], flag 0x60
#define FF132_OFF_BM_SUBSTATE  0x4C
#define FF132_OFF_BM_RESULT    0xF4
#define FF132_OFF_BM_PARTY_LEFT   0x494      // signed byte
#define FF132_OFF_BM_ENEMIES_LEFT 0x495      // signed byte: living kind-1/5 enemies
#define FF132_OFF_BM_PENDING      0x499      // signed byte (logged)
#define FF132_OFF_BM_WAVES        0xFE0      // int (logged)
#define FF132_OFF_BM_LEADER       0x424      // actor handle
#define FF132_OFF_BM_SCENE        0x758
#define FF132_OFF_BM_ITEMS_N      0xDB8      // the item-use records the abort refunds (logged)
#define FF132_OFF_BM_ITEMS        0x8B8      // 0x28 each: name[32], +0x20, +0x24
#define FF132_BM_ITEM_STRIDE      0x28
#define FF132_OFF_BM_FD0          0xFD0      // logged
#define FF132_OFF_CHARA_F27E      0x27E
#define FF132_CHARA_DEAD_BIT      0x10
#define FF132_BM_STATE_ABORT_IDLE 0x2E       // the abort's last state; it only waits
// Encounter info: E = [[0x5151E3C]+0x278]. Event battle = (E&1) && !(E&0x40) (0x970580); E&0x100
// with [GameMain+0x2E4] != 4 retries back into the Historia Crux (0x970570, 0x4051B0).
#define FF132_RVA_AR_ENC_PIN 0x5715A0       // mov eax,[0x5151E3C]; mov eax,[eax+0x278]
static const uint8_t FF132_SIG_AR_ENC_PIN[] = { 0xA1, 0x3C, 0x1E, 0x15, 0x05, 0x8B, 0x80, 0x78,
                                             0x02, 0x00, 0x00 };
#define FF132_RVA_AR_IS_EVENT 0x570580
static const uint8_t FF132_SIG_AR_IS_EVENT[] = { 0x8B, 0x01, 0xA8, 0x01, 0x74, 0x0A, 0xA8, 0x40,
                                              0x75, 0x06, 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
#define FF132_RVA_AR_IS_HCX_RET 0x570570
static const uint8_t FF132_SIG_AR_IS_HCX_RET[] = { 0x8B, 0x01, 0xC1, 0xE8, 0x08, 0x83, 0xE0, 0x01,
                                                0xC3 };
#define FF132_RVA_AR_GM_WORLD 0x0051C2      // mov eax,[esi+0x2E4] in the GameMain getter 0x4051B0
static const uint8_t FF132_SIG_AR_GM_WORLD[] = { 0x8B, 0x86, 0xE4, 0x02, 0x00, 0x00, 0x5E, 0xC3 };
#define FF132_RVA_AR_ENC_PTR  0x4D51E3C     // VA 0x5151E3C
#define FF132_OFF_AR_ENC_E    0x278
#define FF132_AR_E_EVENT      0x1
#define FF132_AR_E_NOT_EVENT  0x40
#define FF132_AR_E_HCX_RET    0x100
// White.registScriptDatabase(class, method, n), the native behind sfMakeScript. Same context as
// doJump; n is the top operand, the method's char* is at +0xC of the operand stack and the class's
// at +0x14. It hashes the three into the "sdb..." name it pushes back and files the names under it.
#define FF132_RVA_AR_REGIST_SCRIPT 0x5F6D80
static const uint8_t FF132_SIG_AR_REGIST_SCRIPT[] = { 0x83, 0xEC, 0x24, 0xA1, 0xDC, 0x90, 0x3A, 0x02 };
#define FF132_OFF_AR_RS_METHOD 0x0C
#define FF132_OFF_AR_RS_CLASS  0x14
#define FF132_OFF_GM_WORLD    0x2E4
#define FF132_GM_WORLD_HCX_OK 4
#define FF132_OFF_GM_DENY     0x234      // [[GameMain+0x438]+0x234], pause deny counter (logged)
#define FF132_RVA_DFLAG_WORD  0x4D375B4  // VA 0x51375B4, debug flags 0x40..0x5F (logged)
// ZoneManager fields (logged only).
#define FF132_OFF_ZM_RESTART_SCENE 0x6C4C
#define FF132_OFF_ZM_BATTLE_READY  0x6BE8
#define FF132_ZSM_BATTLE        7        // the state trigger 7 leaves for 8
#define FF132_ZSM_FIELD         3

// ---- direct load: into the saved world instead of the Historia Crux ---------------------------
// The load-completion task (VA 0x6BA2B0) sends the main FSM trigger 0x18 when no Historia Crux
// gate has been opened yet and 0x13 otherwise. 0x18 reaches STATE_LOAD_EMPTY, which the script
// turns into a jump straight into Zone.getCurrentNum(); 0x13 goes through STATE_LOAD_END to
// STATE_ZONE, which is the world-selection screen. Both immediates become 0x18 to give every save
// the early-game path.
// BOTH sites, never one: the state change is synchronous, so a patched first site followed by an
// untouched second one drives LOAD_EMPTY straight on to STATE_ZONE before the script ever sees
// state 20. With both patched the second 0x18 is dropped (LOAD_EMPTY has no transition on it) and
// the script sends 19 itself, exactly as it does for an early-game save.
#define FF132_RVA_LOAD_TRIGGER_A 0x2BA52D   // push 0x515b440; push ebx; push 0x13; call 0x59ADB0
static const uint8_t FF132_SIG_LOAD_TRIGGER_A[] = { 0x68, 0x40, 0xB4, 0x15, 0x05, 0x53, 0x6A, 0x13,
                                                 0xE8, 0x76, 0x08, 0xEE, 0xFF };
#define FF132_RVA_LOAD_TRIGGER_B 0x2BA57F   // the same three pushes, the branch's second send
static const uint8_t FF132_SIG_LOAD_TRIGGER_B[] = { 0x68, 0x40, 0xB4, 0x15, 0x05, 0x53, 0x6A, 0x13,
                                                 0xE8, 0x24, 0x08, 0xEE, 0xFF };
#define FF132_OFF_LOAD_TRIGGER_IMM 7        // the `6A xx` operand inside either signature
#define FF132_TRG_LOAD_END   0x13           // -> STATE_LOAD_END -> STATE_ZONE, the Historia Crux
#define FF132_TRG_LOAD_EMPTY 0x18           // -> STATE_LOAD_EMPTY, which the script turns into a jump

// GameMain's FSM enter callback, five stack args and `ret 0x14`. Read off the caller's esp at the
// entry: arg2 is the state being ENTERED -- it drives the jump table at 0x6BAAD4, whose STATE_LOAD
// case is what queues the load task -- and arg3 the one being left, the order the game's own
// "main state change [%s] -> [%s]" line prints them in.
#define FF132_RVA_MAIN_STATE_ENTER 0x2BA6D0
static const uint8_t FF132_SIG_MAIN_STATE_ENTER[] = { 0x83, 0xEC, 0x08, 0xA1, 0x78, 0x05, 0x52, 0x02 };
#define FF132_ARG_MAIN_STATE_NEW 0x08
#define FF132_ARG_MAIN_STATE_OLD 0x0C

// Inside the save-data singleton (FF132_RVA_SAVEDATA_PTR): the zone the save was made in, and the
// 16-byte NUL-padded id of the history it belongs to.
#define FF132_OFF_SAVE_ZONE_NUM   0x24BC
#define FF132_OFF_SAVE_HISTORY_ID 0xB9E0
#define FF132_SAVE_HISTORY_ID_LEN 16

// White.Zone.doJump, the VM native a script calls to move to a zone. Its VM context is the first
// stack argument, the operand stack pointer sits at ctx+0x958 and the zone argument at +4 of that.
#define FF132_RVA_ZONE_DOJUMP_NATIVE 0x5FDAA0
static const uint8_t FF132_SIG_ZONE_DOJUMP_NATIVE[] = { 0x56, 0x8B, 0x74, 0x24, 0x08 };
#define FF132_OFF_VM_STACK 0x958

// White.Party.getEventFlag, the VM native a script reads a story-progress flag with. Same context
// as doJump above: the flag's name is the char* at +4 of the operand slot at ctx+0x958. The native
// POPS that slot and pushes its answer back onto the same address (tag word 1 at +0, value at +4),
// so an answer forged in place leaves ctx+0x958 exactly where it was. cdecl, plain `ret`.
#define FF132_RVA_GET_EVENT_FLAG_NATIVE 0x5EF050
static const uint8_t FF132_SIG_GET_EVENT_FLAG_NATIVE[] = { 0x83, 0xEC, 0x10, 0x56, 0x8B, 0x74,
                                                        0x24, 0x18 };
// The post-load recap is picked by ppc.getStoryMovieId(), which walks tm_abst_080..010 and returns
// the first flag that is set; with none set the script jumps past the whole recap. Every other
// script reference to these eight names is a SET, so answering false to the prefix changes nothing
// but the recap.
#define FF132_RECAP_FLAG_PREFIX "tm_abst_"

// ---- the world label: where the save list's two lines are actually built -----------------------
// The list does NOT show a save file's header text verbatim: GuiSaveLoadX360 (VA 0x898470) scans
// that text for the ddd:dd:dd play time and the ddd/ddd fragment count and formats its own two
// lines into a scratch buffer, which is what the text widget is then handed. So the mod's third
// line has to be appended to THAT buffer, at the instruction right after the second format call
// returns. The site is rebuilt on every cursor move.
// With E = the esp AFTER this `add esp,0x14` (the function's aligned frame) and the hook seeing
// the esp BEFORE it, so E = esp + FF132_LIST_FRAME_ADJ:
//   E + FF132_LIST_TEXT_AT  the rebuilt "line0\nline1", cp932, NUL-terminated, capacity FF132_LIST_TEXT
//   E + FF132_LIST_CTX_AT   the 8-byte context of the entry being shown; +4 is the entry itself and
//                        is 0 on the "new save" row. The game's own accessor (0x6B4BE0) reads that
//                        entry's raw header block from +FF132_ENTRY_HEADER (may be 0) and only trusts
//                        it while the byte at +FF132_ENTRY_STALE is 0; the header's text is at +0x10.
#define FF132_RVA_LIST_TEXT_BUILT 0x498815
static const uint8_t FF132_SIG_LIST_TEXT_BUILT[] = { 0x83, 0xC4, 0x14, 0x8D, 0x4C, 0x24, 0x70 };
#define FF132_LIST_FRAME_ADJ 0x14
#define FF132_LIST_TEXT_AT   0x98
#define FF132_LIST_TEXT      0x100
#define FF132_LIST_CTX_AT    0x2C
#define FF132_ENTRY_HEADER   0x144
#define FF132_ENTRY_STALE    0x179

// ---- the save list's GUI: the cursor, and the heading over the detail panel --------------------
// GuiSaveLoadX360, 0x540 bytes, created and destroyed every time a list opens, so its address is
// taken from the entry of its own update function rather than looked up. The mod reads it from the
// hotkey worker, which is why every read is stamped and guarded: the object is the game thread's.
//   0x898470  55 8B EC 83 E4 F0   push ebp; mov ebp,esp; and esp,~0xF   (thiscall, ecx = the gui)
#define FF132_RVA_LIST_UPDATE 0x498470
static const uint8_t FF132_SIG_LIST_UPDATE[] = { 0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0 };
#define FF132_OFF_GUI_CURSOR   0x104   // index into the savemgr entry array; count-1 is the LEFT end
#define FF132_OFF_GUI_ROWS     0x10C   // count, +1 on a save list: the last index is the "new save" row
#define FF132_OFF_GUI_ISSAVE   0x114
#define FF132_OFF_GUI_NEWROW_A 0x124   // both 0 on the "new save" row (0x8988F1 / 0x8988FA)
#define FF132_OFF_GUI_NEWROW_B 0x130
#define FF132_OFF_GUI_DIRTY    0x128   // dword; non-0 = rebuild the heading on the next update pass
// The interactive SaveDataCreator's list: non-0 while a save/load list is on screen (set 0x6B8942,
// cleared 0x6B8C09 -- the same flag FF132_OFF_CREATOR_BUSY names), and the save manager behind it,
// non-NULL only for as long as that list lives.
#define FF132_OFF_SDC_LIST_OPEN FF132_OFF_CREATOR_BUSY
#define FF132_OFF_SDC_SAVEMGR   0x178

// ---- the save list GUI's row count (ff132_listgui.c) ----------------------------------------------
// GuiSaveLoadX360 embeds two 50-entry arrays: row states at +0x134 (0x10 each, also reached as
// +0x138/+0x13C) and row widgets at +0x454 (4 each, +0x45C = that +8). N rows = move both past the
// object's old end (states +0x540, widgets +0x540+N*0x10), enlarge the allocation to cover them and
// raise the counts. Every instruction that names the arrays or the counts is here (all 37); code[]
// is the whole original instruction, the field is width bytes at +at. The table is in write order:
// allocation, relocation, counts.
#define FF132_LG_OLD_STATES  0x134
#define FF132_LG_OLD_WIDGETS 0x454
#define FF132_LG_OLD_SIZE    0x540
#define FF132_LG_OLD_ROWS    50
enum { FF132_LG_ALLOC, FF132_LG_STATE, FF132_LG_WIDGET, FF132_LG_N, FF132_LG_N1, FF132_LG_N10 };
typedef struct { uint32_t rva; uint8_t at, width, kind, len; uint8_t code[8]; } ff132_lg_site;
static const ff132_lg_site FF132_LISTGUI_SITES[] = {
    { 0x386CFE, 1, 4, FF132_LG_ALLOC, 5, { 0x68, 0x40, 0x05, 0x00, 0x00 } },   // push 0x540
    { 0x495603, 2, 4, FF132_LG_STATE, 6, { 0x8D, 0x86, 0x34, 0x01, 0x00, 0x00 } },   // lea eax, [esi + 0x134]
    { 0x49563E, 2, 4, FF132_LG_WIDGET, 6, { 0x8D, 0xBE, 0x54, 0x04, 0x00, 0x00 } },   // lea edi, [esi + 0x454]
    { 0x495865, 2, 4, FF132_LG_STATE, 6, { 0x8D, 0xB3, 0x38, 0x01, 0x00, 0x00 } },   // lea esi, [ebx + 0x138]
    { 0x495BA0, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0xBC, 0x91, 0x54, 0x04, 0x00, 0x00 } },   // mov edi, [ecx + edx*4 + 0x454]
    { 0x495BE7, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x84, 0x86, 0x54, 0x04, 0x00, 0x00 } },   // mov eax, [esi + eax*4 + 0x454]
    { 0x495C05, 3, 4, FF132_LG_WIDGET, 8, { 0x83, 0xBC, 0x86, 0x54, 0x04, 0x00, 0x00, 0x00 } },   // cmp [esi + eax*4 + 0x454], 0
    { 0x495C0F, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x84, 0x86, 0x54, 0x04, 0x00, 0x00 } },   // mov eax, [esi + eax*4 + 0x454]
    { 0x495C2D, 2, 4, FF132_LG_WIDGET, 6, { 0x8D, 0x9E, 0x54, 0x04, 0x00, 0x00 } },   // lea ebx, [esi + 0x454]
    { 0x495CC0, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x9C, 0xB1, 0x54, 0x04, 0x00, 0x00 } },   // mov ebx, [ecx + esi*4 + 0x454]
    { 0x495CD6, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x9C, 0xB1, 0x54, 0x04, 0x00, 0x00 } },   // mov ebx, [ecx + esi*4 + 0x454]
    { 0x495CEA, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x84, 0xB1, 0x54, 0x04, 0x00, 0x00 } },   // mov eax, [ecx + esi*4 + 0x454]
    { 0x495D05, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x84, 0xB1, 0x54, 0x04, 0x00, 0x00 } },   // mov eax, [ecx + esi*4 + 0x454]
    { 0x495D5F, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x9C, 0xB1, 0x54, 0x04, 0x00, 0x00 } },   // mov ebx, [ecx + esi*4 + 0x454]
    { 0x495D75, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x9C, 0xB1, 0x54, 0x04, 0x00, 0x00 } },   // mov ebx, [ecx + esi*4 + 0x454]
    { 0x495D88, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x9C, 0xB1, 0x54, 0x04, 0x00, 0x00 } },   // mov ebx, [ecx + esi*4 + 0x454]
    { 0x495D9C, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x9C, 0xB1, 0x54, 0x04, 0x00, 0x00 } },   // mov ebx, [ecx + esi*4 + 0x454]
    { 0x495DF3, 2, 4, FF132_LG_WIDGET, 6, { 0x81, 0xC1, 0x5C, 0x04, 0x00, 0x00 } },   // add ecx, 0x45c
    { 0x496461, 2, 4, FF132_LG_WIDGET, 6, { 0x81, 0xC6, 0x54, 0x04, 0x00, 0x00 } },   // add esi, 0x454
    { 0x4979C9, 3, 4, FF132_LG_WIDGET, 7, { 0x39, 0xAC, 0x87, 0x54, 0x04, 0x00, 0x00 } },   // cmp [edi + eax*4 + 0x454], ebp
    { 0x497A74, 3, 4, FF132_LG_WIDGET, 7, { 0x39, 0xAC, 0x87, 0x54, 0x04, 0x00, 0x00 } },   // cmp [edi + eax*4 + 0x454], ebp
    { 0x498070, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0xBC, 0x81, 0x54, 0x04, 0x00, 0x00 } },   // mov edi, [ecx + eax*4 + 0x454]
    { 0x498191, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x8C, 0x86, 0x54, 0x04, 0x00, 0x00 } },   // mov ecx, [esi + eax*4 + 0x454]
    { 0x498394, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x8C, 0x9E, 0x54, 0x04, 0x00, 0x00 } },   // mov ecx, [esi + ebx*4 + 0x454]
    { 0x4983A6, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x8C, 0x9E, 0x54, 0x04, 0x00, 0x00 } },   // mov ecx, [esi + ebx*4 + 0x454]
    { 0x4983C0, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x8C, 0x9E, 0x54, 0x04, 0x00, 0x00 } },   // mov ecx, [esi + ebx*4 + 0x454]
    { 0x498400, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x84, 0x91, 0x54, 0x04, 0x00, 0x00 } },   // mov eax, [ecx + edx*4 + 0x454]
    { 0x498447, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x8C, 0x86, 0x54, 0x04, 0x00, 0x00 } },   // mov ecx, [esi + eax*4 + 0x454]
    { 0x4984E0, 3, 4, FF132_LG_WIDGET, 7, { 0x8B, 0x9C, 0x87, 0x54, 0x04, 0x00, 0x00 } },   // mov ebx, [edi + eax*4 + 0x454]
    { 0x49864D, 2, 4, FF132_LG_STATE, 6, { 0x8D, 0x9F, 0x3C, 0x01, 0x00, 0x00 } },   // lea ebx, [edi + 0x13c]
    { 0x495609, 1, 4, FF132_LG_N1, 5, { 0xB9, 0x31, 0x00, 0x00, 0x00 } },   // mov ecx, 0x31
    { 0x495644, 1, 4, FF132_LG_N, 5, { 0xB9, 0x32, 0x00, 0x00, 0x00 } },   // mov ecx, 0x32
    { 0x4956B9, 2, 1, FF132_LG_N1, 3, { 0x83, 0xE7, 0x31 } },   // and edi, 0x31
    { 0x495E0A, 1, 4, FF132_LG_N, 5, { 0xB8, 0x32, 0x00, 0x00, 0x00 } },   // mov eax, 0x32
    { 0x495E0F, 4, 4, FF132_LG_N10, 8, { 0xC7, 0x44, 0x24, 0x14, 0x05, 0x00, 0x00, 0x00 } },   // mov [esp + 0x14], 5
    { 0x49648A, 4, 4, FF132_LG_N, 8, { 0xC7, 0x44, 0x24, 0x4C, 0x32, 0x00, 0x00, 0x00 } },   // mov [esp + 0x4c], 0x32
    { 0x498546, 1, 4, FF132_LG_N, 5, { 0xBA, 0x32, 0x00, 0x00, 0x00 } },   // mov edx, 0x32
};
#define FF132_LISTGUI_NSITES ((int)(sizeof FF132_LISTGUI_SITES / sizeof FF132_LISTGUI_SITES[0]))

// The heading "セーブデータ NN" over the detail panel, built in the same update function. With B =
// the esp AT this instruction (the hook's own esp, no adjustment):
//   B + FF132_TITLE_TEXT_AT   the finished heading, cp932 ASCIIZ, already cut at its 2nd comma
//   B + FF132_TITLE_DIGITS    the two display digits the heading was built from ("NN", = slot + 1)
// The game then makes a string object of it and hands that to setText([gui+0x520]).
// The "new save" row does NOT reach here: 0x89894D jumps past it, as does the negative-slot branch
// at 0x898973 -- so the digits at this point always belong to a real save.
// 0x7AA900 was given a capacity of 0x200 for this buffer and the stack canary sits at B+0x290, so
// the room past the heading is ~0x1F8 bytes; FF132_TITLE_TEXT is the cap this mod keeps to.
#define FF132_RVA_LIST_TITLE_BUILT 0x498A41
static const uint8_t FF132_SIG_LIST_TITLE_BUILT[] = { 0x56, 0x8D, 0x84, 0x24, 0x9C, 0x00, 0x00, 0x00 };
#define FF132_TITLE_TEXT_AT 0x98
#define FF132_TITLE_TEXT    0x180
#define FF132_TITLE_DIGITS  0x1C

// ---- the save list's order (ff132_tidy.c) --------------------------------------------------------
// GfSaveDataCreator::sortEntries, thiscall with ecx = the save manager, called once per list open
// right after the directory has been enumerated. It runs the CRT qsort over the entry array and
// then renumbers entry+FF132_ENTRY_INDEX 0..n-1 in the order that came out, so BOTH the order and the
// count can be changed from a hook at its entry, before the sort runs.
//   006B4700  56 8B F1 0F BE 46 14   push esi; mov esi,ecx; movsx eax,byte [esi+0x14]
//   006B470A  68 B0 46 6B 00         push 0x6B46B0    <- the comparator, patched at +1
//   006B470F  68 80 02 00 00         push 0x280       <- the entry stride
#define FF132_RVA_LIST_SORT 0x2B4700
static const uint8_t FF132_SIG_LIST_SORT[] = { 0x56, 0x8B, 0xF1, 0x0F, 0xBE, 0x46, 0x14 };
#define FF132_RVA_LIST_SORT_CMP_PUSH 0x2B470A
static const uint8_t FF132_SIG_LIST_SORT_CMP_PUSH[] = { 0x68, 0xB0, 0x46, 0x6B, 0x00,
                                                     0x68, 0x80, 0x02, 0x00, 0x00 };
#define FF132_OFF_LIST_SORT_CMP_IMM 1       // the imm32 inside that signature = the comparator

// The save manager the sort is handed. The count is a SIGNED byte, and the entry array is one
// allocation of maxslots * FF132_ENTRY_BYTES.
#define FF132_OFF_SM_ENTRIES 0x08
#define FF132_OFF_SM_COUNT   0x14           // signed char
#define FF132_OFF_SM_CUR     0x15           // signed char, indexes the same array
#define FF132_OFF_SM_ISSAVE  0x28           // 1 = the in-game SAVE list, 0 = the title's LOAD list
#define FF132_ENTRY_BYTES    0x280
#define FF132_ENTRY_INDEX    0x149          // display index, renumbered by the sort
#define FF132_ENTRY_SLOT     0x14A          // signed char, the NN of ff13_2-NN.dat
// The file's last-write time, already converted to LOCAL time, and the sole key the game's own
// comparator orders by (ascending, so the array's LAST entry is the newest = the list's first row).
#define FF132_ENTRY_TIME     0x158
#define FF132_ENTRY_PATH     0x17A          // full path on the Win32 branch, bare file name on the VFS one

// sgw.dll's virtual file system, non-NULL once white_steam_init() has answered. The enumerator
// stores only a FILE NAME in the entry when this path is live, which is why a save's path is
// rebuilt from its slot number rather than read out of the entry.
#define FF132_RVA_SGW_VFS 0x4D5AFF8

// GameMain's own state machine, the one whose transitions FF132_RVA_MAIN_STATE_ENTER reports:
// GameMain* -> +0x438 the FSM -> +0x230 the current state object -> +4 its id.
#define FF132_OFF_GAMEMAIN_FSM 0x438
#define FF132_OFF_FSM_CURRENT  0x230
#define FF132_OFF_STATE_ID     0x04

// The plaintext save header: 0x210 bytes at the head of every ff13_2-NN.dat, no checksum.
//   +0    0x0D, the version byte every save starts with
//   +8    (x >> 4) - 2 = the story step the game itself draws on the thumbnail
//   +0x10 the list text, cp932, NUL-terminated, FF132_HDR_TEXT bytes of capacity
#define FF132_HDR_BYTES    0x210
#define FF132_HDR_TEXT_AT  0x10
#define FF132_HDR_TEXT     0x100
#define FF132_HDR_MAGIC    0x0D
#define FF132_HDR_EPISODE  0x08

// ---- the game's text language -------------------------------------------
// int, 0 jp | 1 us | 2 uk | 3 it | 4 gr | 5 fr | 6 sp | 7 ru | 8 kr | 9 ck | 10 ch; -1 until engine
// init writes it. Data, so it is trusted only after its getter's bytes (which name it) check out.
#define FF132_RVA_LANGUAGE     0x1F46D98   // VA 0x2346D98
#define FF132_RVA_LANGUAGE_GET 0x2B27A0    // mov eax,[0x2346D98]; ret
static const uint8_t FF132_SIG_LANGUAGE_GET[] = { 0xA1, 0x98, 0x6D, 0x34, 0x02, 0xC3 };

// ---- kill mode (ff132_kill.c) --------------------
// BattleMain::activate(handle, flag), thiscall ret 8, ecx = BM: the only way an enemy is counted
// into [BM+0x495]. Its failure branch (model not resident) writes nothing but a log line, so
// skipping the whole function is that branch. The first 8 bytes are the relocated prologue; the
// rest pins the function (the 8 alone occur all over .text).
#define FF132_RVA_KILL_ACTIVATE 0x09FA70
static const uint8_t FF132_SIG_KILL_ACTIVATE[] = { 0x83, 0xEC, 0x44, 0xA1, 0xDC, 0x90, 0x3A, 0x02,
                                                0x33, 0xC4, 0x89, 0x44, 0x24, 0x40, 0x53, 0x56,
                                                0x8B, 0xF1, 0x57 };
#define FF132_KILL_ACTIVATE_PROLOGUE 8
#define FF132_RET_KILL_ACTIVATE 8
// Inside it: the resident test and the [+0x2A8] bits 24-27 in {0,1} test the failure branch needs,
// and the failure log's push. Checked only, never patched.
#define FF132_RVA_KILL_ACT_RESIDENT 0x09FACA
static const uint8_t FF132_SIG_KILL_ACT_RESIDENT[] = { 0x51, 0x8B, 0xC4, 0xC7, 0x00, 0xFF, 0xFF, 0xFF,
                                                    0xFF, 0x89, 0x08, 0xE8, 0xC6, 0x87, 0x14, 0x00,
                                                    0x83, 0xC4, 0x04, 0x3B, 0xC3, 0x75, 0xC4 };
#define FF132_RVA_KILL_ACT_NIB 0x09FAE1
static const uint8_t FF132_SIG_KILL_ACT_NIB[] = { 0x8D, 0x4C, 0x24, 0x54, 0xE8, 0x26, 0x00, 0xFA,
                                               0xFF, 0x8B, 0xC8, 0xE8, 0xCF, 0x03, 0xFA, 0xFF,
                                               0x85, 0xC0, 0x74, 0xB0 };
#define FF132_RVA_KILL_ACT_FAILLOG 0x09FB7A
static const uint8_t FF132_SIG_KILL_ACT_FAILLOG[] = { 0x68, 0x10, 0x20, 0xD6, 0x00 };
// The test the failure branch calls: n = ([+0x2A8] << 4) >> 28, true for n 0 or 1.
#define FF132_RVA_KILL_NIB_FN 0x03FEC0
static const uint8_t FF132_SIG_KILL_NIB_FN[] = { 0x8B, 0x81, 0xA8, 0x02, 0x00, 0x00, 0xC1, 0xE0,
                                              0x04, 0xC1, 0xF8, 0x1C };
// pcKind getter: movsx eax, byte [ecx+0x29C]; ret (0x13 = a party monster; logged only).
#define FF132_RVA_KILL_PCKIND_FN 0x03FCC0
static const uint8_t FF132_SIG_KILL_PCKIND_FN[] = { 0x0F, 0xBE, 0x81, 0x9C, 0x02, 0x00, 0x00, 0xC3 };
// BattleChara::addHp(delta, force), thiscall ret 8: every damage, instant death and scripted kill
// goes through it, and the caller kills on HP < 1 in the same call. The first 6 bytes are the
// relocated prologue (mov eax,[abs] relocates); the 22 are unique (0x456110 ends in ret 4).
#define FF132_RVA_KILL_ADDHP 0x055F30
static const uint8_t FF132_SIG_KILL_ADDHP[] = { 0x51, 0xA1, 0xFC, 0xE5, 0x51, 0x02, 0xF6, 0x40, 0x3C,
                                             0x40, 0x56, 0x8B, 0xF1, 0x74, 0x07, 0x33, 0xC0, 0x5E,
                                             0x59, 0xC2, 0x08, 0x00 };
#define FF132_KILL_ADDHP_PROLOGUE 6
#define FF132_RET_KILL_ADDHP 8
// cur + this must not overflow for any HP, and the game clamps the sum to the HP floor [+0x2DC].
#define FF132_KILL_LETHAL_DELTA (-0x40000000)
// The second copy of the BattleMain pointer (VA 0x251E5FC): the one addHp reads at its entry.
#define FF132_RVA_KILL_BM_COPY  0x211E5FC
#define FF132_BM3C_NO_HP        0x00000040   // [BM+0x3C], flag 0x26: addHp does nothing
#define FF132_BM40_NO_WIPE_END  0x04000000   // [BM+0x40], flag 0x5A: wiping the enemies does not end it
#define FF132_OFF_CHARA_ID      0x08         // charaspec id (logged)
#define FF132_OFF_CHARA_FLAGS289 0x289       // bit0 = flag 0x58 (stops at 1 HP), bit1 = 0x59 (logged)
#define FF132_OFF_CHARA_PCKIND  0x29C        // int8
#define FF132_OFF_CHARA_HP_FLOOR 0x2DC

#endif // FF132_ADDRS_H
