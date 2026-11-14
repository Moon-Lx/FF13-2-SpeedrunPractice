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

// Battle auto-retry: a won battle is turned into the game's own abort (BattleMain state
// 0x27) at the decided-fight branch, then the pause menu's Retry is replayed from the frame site.
// Addresses in ff132_addrs.h.
//
// ff132_autoretry_toggle runs on the hotkey worker and only moves g_autoOn. ar_decide (the cave,
// inside BattleMain::update), on_regist_script (a VM native) and ff132_autoretry_pump (frame site)
// all run on the game thread and own every other static here. The status line is drawn on the
// render thread from flags.
#include "ff132_internal.h"
#include "ff132_addrs.h"
#include "ff13_hook.h"

#define FF132_COL_AUTORETRY 0xFF6798FE         // blue
#define AR_STEP_TIMEOUT_MS 10000
#define AR_WATCH_MS 60000
#define AR_EAX_SLOT 0x1C                    // pushad's saved eax, seen from after pushad

enum { AR_IDLE, AR_FADE0, AR_FADE1, AR_FADE2, AR_W46, AR_WBM0, AR_FIRE, AR_WATCH };
static const char* const kStep[] = { "IDLE", "FADE0", "FADE1", "FADE2", "W46", "WBM0", "FIRE",
                                     "WATCH" };
enum { CLS_SCRIPT_WIN, CLS_LOSE, CLS_ESCAPE, CLS_END_LOSE, CLS_END_WIN, CLS_KILL_WIN, CLS_NO_LEADER };
static const char* const kCls[] = { "SCRIPT_WIN", "LOSE", "ESCAPE", "END_LOSE", "END_WIN", "KILL_WIN",
                                    "NO_LEADER" };

static volatile int g_autoOn = 0;           // the player's choice; kept in the state file
static volatile int g_siteOk = 0;           // every signature matched and the cave is in

// Game thread only.
static int g_step = AR_IDLE;
static DWORD g_armAt = 0, g_stepAt = 0;
static int g_prevScene = -1;                // the scene the retry was taken in
static int g_lastBmSt = -1;
static int g_refundLogged = 0;
static int g_lastZsm = -1;
static int g_sawBmIdle = 0;

// A fight whose retry reloads at a restart point from before an earlier fight of the same chain.
// After the reload has rebuilt the party, one callback name the zone script registers is swapped
// for the step that starts the retried fight, so the script itself sets that fight up and calls it.
typedef struct { int zone, scene; const char* cls; const char* from; const char* to; } ar_hop_t;
static const ar_hop_t kHops[] = {
    // Valhalla opening: phase 4 has no restart point and reloads at the autosave before phase 1.
    // Going past its cutscene straight to the battle call starts the fight with the leader down.
    { 160, 1010, "scr100", "efEventStart_setuplightning_cb", "efAfterEv030Result_cb" },
};
static volatile int g_hop = -1;             // the kHops entry a retry armed, -1 = none

typedef int  (__cdecl *ar_fn0_t)(void);
typedef void (__thiscall *ar_fade_layer_t)(void* gm, int layer, float secs);
typedef void (__cdecl *ar_fader_start_t)(float secs);
typedef void (__thiscall *ar_zm_fn_t)(void* zm);

static int rd8s(uintptr_t p) {
    return (int)*(const volatile signed char*)p;
}
static uintptr_t ptr_at(uintptr_t rva, size_t need) {
    uintptr_t p = g_base + rva;
    if (!rd_ok((void*)p, 4)) return 0;
    uintptr_t v = (uintptr_t)rd32(p);
    return rd_ok((void*)v, need) ? v : 0;
}
static uintptr_t battle_main(void) { return ptr_at(FF132_RVA_BATTLE_MAIN_PTR, FF132_OFF_BM_WAVES + 4); }
static uintptr_t game_main(void)   { return ptr_at(FF132_RVA_GAMEMAIN_PTR, FF132_OFF_GM_WORLD + 4); }
static uintptr_t zone_mgr(void)    { return ptr_at(FF132_RVA_ZONE_MGR_PTR, FF132_OFF_ZM_RESTART_SCENE + 4); }
static uintptr_t fader(void)       { return ptr_at(FF132_RVA_AR_FADER_PTR, FF132_OFF_AR_FADER_BUSY + 4); }

static int call0(uintptr_t rva) { return ((ar_fn0_t)(g_base + rva))(); }

static int zone_num(void) {
    uintptr_t sd = ptr_at(FF132_RVA_SAVEDATA_PTR, FF132_OFF_SAVE_ZONE_NUM + 4);
    return sd ? (int)rd32(sd + FF132_OFF_SAVE_ZONE_NUM) : -1;
}


// ---- the decision (game thread, inside BattleMain::update, called from the cave) ---------------
// Mirrors 0x497080 without its side effects. Flag 0x26 is deliberately unused:
// an ordinary last kill sets it too.
static int end_type(uintptr_t bm) {
    uintptr_t db = (uintptr_t)rd32(bm + FF132_OFF_BM_SCENE_DB + 4);
    if (!rd_ok((void*)(db + 0x20), 4)) return -1;
    return (int)((rd32(db + 0x20) >> FF132_END_TYPE_SHIFT) & 0xF);
}

static int classify(uintptr_t bm, uint32_t* ldrF27e, int* endType) {
    uint32_t f40 = rd32(bm + FF132_OFF_BM_FLAGS + 8), f44 = rd32(bm + FF132_OFF_BM_FLAGS + 12);
    *ldrF27e = 0xFFFFFFFFu;
    *endType = -1;
    if (f40 & FF132_BM40_SCRIPT_WIN) return CLS_SCRIPT_WIN;
    if (f44 & FF132_BM44_LOST) return CLS_LOSE;
    if (rd8s(bm + FF132_OFF_BM_PARTY_LEFT) <= 0) return CLS_LOSE;
    uintptr_t c = ff132_chara_at(bm, rd32(bm + FF132_OFF_BM_LEADER), FF132_OFF_CHARA_F27E + 1);
    if (!c) return CLS_NO_LEADER;
    uint8_t f = *(const volatile uint8_t*)(c + FF132_OFF_CHARA_F27E);
    *ldrF27e = f;
    if (f & FF132_CHARA_DEAD_BIT) return CLS_LOSE;
    if (rd8s(bm + FF132_OFF_BM_ENEMIES_LEFT) <= 0 || (f40 & FF132_BM40_NO_END)) return CLS_KILL_WIN;
    if (!(f40 & FF132_BM40_END_BY_TYPE)) return CLS_END_WIN;
    *endType = end_type(bm);
    if (*endType < 0) return CLS_END_LOSE;
    if (*endType == FF132_END_TYPE_ESCAPE) return CLS_ESCAPE;
    if (*endType == FF132_END_TYPE_LOSE) return CLS_END_LOSE;
    return CLS_END_WIN;
}

// ---- the hop (game thread: armed from the decision, used from the VM, dropped by the pump) ------
static void hop_drop(const char* why) {
    if (g_hop < 0) return;
    ff132_log(FF132_TAG "ar hop: dropped unused (%s), the retry went where the game sent it\n", why);
    g_hop = -1;
}

static void hop_arm(int scene) {
    g_hop = -1;
    int zone = zone_num();
    for (int i = 0; i < (int)(sizeof kHops / sizeof kHops[0]); i++)
        if (kHops[i].zone == zone && kHops[i].scene == scene) {
            g_hop = i;
            ff132_log(FF132_TAG "ar hop: armed for scene %d in zone %d (%s.%s -> %s)\n", scene, zone,
                  kHops[i].cls, kHops[i].from, kHops[i].to);
            return;
        }
}

static int str_is(uintptr_t p, const char* want) {
    size_t n = strlen(want) + 1;
    return rd_ok((const void*)p, n) && memcmp((const void*)p, want, n) == 0;
}

static void __cdecl on_regist_script(uint32_t esp) {
    int h = g_hop;
    if (h < 0) return;
    if (!rd_ok((const void*)(uintptr_t)esp, 8)) return;
    uintptr_t ctx = (uintptr_t)rd32((uintptr_t)esp + 4);
    if (!rd_ok((const void*)ctx, FF132_OFF_VM_STACK + 4)) return;
    uintptr_t st = (uintptr_t)rd32(ctx + FF132_OFF_VM_STACK);
    if (!rd_ok((const void*)st, FF132_OFF_AR_RS_CLASS + 4)) return;
    const ar_hop_t* k = &kHops[h];
    if (!str_is((uintptr_t)rd32(st + FF132_OFF_AR_RS_METHOD), k->from) ||
        !str_is((uintptr_t)rd32(st + FF132_OFF_AR_RS_CLASS), k->cls))
        return;
    int zone = zone_num();
    if (zone != k->zone) return;
    *(volatile uint32_t*)(st + FF132_OFF_AR_RS_METHOD) = (uint32_t)(uintptr_t)k->to;
    g_hop = -1;
    ff132_log(FF132_TAG "ar hop: %s.%s -> %s (zone %d, %lums after the retry)\n", k->cls, k->from,
          k->to, zone, (unsigned long)(GetTickCount() - g_armAt));
}

static int __cdecl ar_decide(uint32_t bmv) {
    if (!g_autoOn) return 0;
    uintptr_t bm = (uintptr_t)bmv;
    uint32_t ldrF27e;
    int endType;
    int cls = classify(bm, &ldrF27e, &endType);

    uintptr_t enc = ptr_at(FF132_RVA_AR_ENC_PTR, FF132_OFF_AR_ENC_E + 4);
    uint32_t e = enc ? rd32(enc + FF132_OFF_AR_ENC_E) : 0;
    uintptr_t gm = game_main();
    int world = gm ? (int)rd32(gm + FF132_OFF_GM_WORLD) : -1;
    int event = enc && (e & FF132_AR_E_EVENT) && !(e & FF132_AR_E_NOT_EVENT);
    int hcxRet = (e & FF132_AR_E_HCX_RET) && world != FF132_GM_WORLD_HCX_OK;
    const char* type = !enc ? "unknown" : hcxRet ? "historia_return" : event ? "event" : "normal";
    int win = cls == CLS_KILL_WIN || cls == CLS_SCRIPT_WIN || cls == CLS_END_WIN;
    int idle = g_step == AR_IDLE || g_step == AR_WATCH;
    // A fight kill mode's instant win emptied is won again the moment it is retried.
    int emptied = ff132_kill_emptied();
    int eligible = win && enc && !hcxRet && !emptied;
    int retry = eligible && idle;
    const char* act = retry ? "retry" : eligible ? "pass(busy)"
                    : (win && enc && !hcxRet) ? "pass(instant-win)" : "pass";

    uintptr_t dfl = g_base + FF132_RVA_DFLAG_WORD;
    ff132_log(FF132_TAG "ar decide: st=%d sub=%d f38=%08X f3c=%08X f40=%08X f44=%08X p494=%d e495=%d"
                 " w499=%d fe0=%d ldr_h=%08X ldr_f27e=%02X E=%08X gm2e4=%d dfl=%08X scene=%d"
                 " end=%d cls=%s act=%s type=%s\n",
          (int)rd32(bm + FF132_OFF_BM_STATE), (int)rd32(bm + FF132_OFF_BM_SUBSTATE),
          (unsigned)rd32(bm + FF132_OFF_BM_FLAGS), (unsigned)rd32(bm + FF132_OFF_BM_FLAGS + 4),
          (unsigned)rd32(bm + FF132_OFF_BM_FLAGS + 8), (unsigned)rd32(bm + FF132_OFF_BM_FLAGS + 12),
          rd8s(bm + FF132_OFF_BM_PARTY_LEFT), rd8s(bm + FF132_OFF_BM_ENEMIES_LEFT),
          rd8s(bm + FF132_OFF_BM_PENDING), (int)rd32(bm + FF132_OFF_BM_WAVES),
          (unsigned)rd32(bm + FF132_OFF_BM_LEADER), (unsigned)(ldrF27e & 0xFF), (unsigned)e, world,
          rd_ok((void*)dfl, 4) ? (unsigned)rd32(dfl) : 0u, (int)rd32(bm + FF132_OFF_BM_SCENE),
          endType, kCls[cls], act, type);
    if (!retry) return 0;

    // Flags 0x45/0x1D and the freeze have to come from this one guarded call: setting the bits
    // alone makes a later call skip the freeze.
    call0(FF132_RVA_AR_BM_ABORT);
    ff132_battle_hold_after();
    g_prevScene = (int)rd32(bm + FF132_OFF_BM_SCENE);
    hop_arm(g_prevScene);
    g_armAt = g_stepAt = GetTickCount();
    g_step = AR_FADE0;
    g_lastBmSt = -1;
    g_refundLogged = 0;
    g_lastZsm = -1;
    g_sawBmIdle = 0;
    return 1;
}

// ---- the cave ---------------------------------------------------------------------------------
// The cave checker replays these byte statements; keep one `*p++ =` / put32 / put_rel32 per action.
static uint8_t* put32(uint8_t* p, uint32_t v) {
    memcpy(p, &v, 4);
    return p + 4;
}
static uint8_t* put_rel32(uint8_t* p, uintptr_t to) {
    return put32(p, (uint32_t)(to - ((uintptr_t)p + 4)));
}

// Entry: esi = BM, ebp = 1, ecx dead, flags dead. Every exit leaves esp and all registers but
// eax/ecx/edx as found; the fall-through arm is the original five instructions.
static int ar_emit(uint8_t* cave) {
    uint8_t* p = cave;
    *p++ = 0x60;                                                  // pushad
    *p++ = 0x56;                                                  // push esi
    *p++ = 0xB8; p = put32(p, (uint32_t)(uintptr_t)ar_decide);    // mov eax,ar_decide
    *p++ = 0xFF; *p++ = 0xD0;                                     // call eax
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                        // add esp,4
    *p++ = 0x89; *p++ = 0x44; *p++ = 0x24; *p++ = AR_EAX_SLOT;    // mov [esp+0x1C],eax
    *p++ = 0x61;                                                  // popad
    *p++ = 0x85; *p++ = 0xC0;                                     // test eax,eax
    *p++ = 0x75; uint8_t* jnz = p++;                              // jnz retry
    *p++ = 0x6A; *p++ = 0x50;                                     // push 0x50
    *p++ = 0xB8; p = put32(p, (uint32_t)(g_base + FF132_RVA_AR_DFLAG_TEST));   // mov eax,dflag test
    *p++ = 0xFF; *p++ = 0xD0;                                     // call eax
    *p++ = 0x83; *p++ = 0xC4; *p++ = 0x04;                        // add esp,4
    *p++ = 0x8B; *p++ = 0xCE;                                     // mov ecx,esi
    *p++ = 0x84; *p++ = 0xC0;                                     // test al,al
    *p++ = 0x0F; *p++ = 0x84; p = put_rel32(p, g_base + FF132_RVA_AR_RESULTS);  // jz results
    *p++ = 0xE9; p = put_rel32(p, g_base + FF132_RVA_AR_DFLAG_BODY);           // jmp dflag body
    *jnz = (uint8_t)(p - (jnz + 1));                              // retry:
    *p++ = 0x8B; *p++ = 0xCE;                                     // mov ecx,esi
    *p++ = 0xE9; p = put_rel32(p, g_base + FF132_RVA_AR_ABORT);      // jmp abort
    return (int)(p - cave);
}

void ff132_autoretry_init(void) {
    uintptr_t at = g_base + FF132_RVA_AR_DECIDED;
    if (wait_for_bytes(at, FF132_SIG_AR_DECIDED, (int)sizeof FF132_SIG_AR_DECIDED,
                       FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "auto retry: site @0x%08X mismatch -> feature off\n", (unsigned)at);
        return;
    }
#define AR_SIG(n) { FF132_RVA_AR_##n, FF132_SIG_AR_##n, sizeof FF132_SIG_AR_##n }
    static const struct { uintptr_t rva; const uint8_t* sig; size_t n; } k[] = {
        AR_SIG(DECIDED_FROM), AR_SIG(DFLAG_BODY), AR_SIG(RESULTS), AR_SIG(ABORT_TEST),
        AR_SIG(ABORT), AR_SIG(DFLAG_TEST), AR_SIG(BM_ABORT), AR_SIG(BM_WAIT46),
        AR_SIG(BM_SHUTDOWN), AR_SIG(BM_WAIT0), AR_SIG(FADE_LAYER), AR_SIG(FADER_BUSY),
        AR_SIG(FADER_START), AR_SIG(FADER_DONE), AR_SIG(ZSM_TRIGGER7), AR_SIG(PIN_FADE0),
        AR_SIG(PIN_FADE1), AR_SIG(PIN_FIRE), AR_SIG(RESULT_FN), AR_SIG(RESULT_LOSE),
        AR_SIG(RESULT_END), AR_SIG(RESULT_END_TBL), AR_SIG(END_TYPE),
        AR_SIG(CHARA_DEAD), AR_SIG(ENC_PIN), AR_SIG(IS_EVENT), AR_SIG(IS_HCX_RET),
        AR_SIG(GM_WORLD),
    };
#undef AR_SIG
    for (int i = 0; i < (int)(sizeof k / sizeof k[0]); i++)
        if (!sig_at(k[i].rva, k[i].sig, k[i].n)) {
            ff132_loga(FF132_TAG "auto retry: site @0x%08X mismatch -> feature off\n",
                   (unsigned)(g_base + k[i].rva));
            return;
        }

    uint8_t* cave = (uint8_t*)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE,
                                           PAGE_EXECUTE_READWRITE);
    if (!cave) {
        ff132_loga(FF132_TAG "auto retry: no cave memory (err %lu) -> feature off\n", GetLastError());
        return;
    }
    int len = ar_emit(cave);
    FlushInstructionCache(GetCurrentProcess(), cave, (SIZE_T)len);

    // Only a decided fight runs these bytes and none has happened yet at init.
    DWORD old = 0;
    if (!VirtualProtect((void*)at, FF132_AR_PATCH_LEN, PAGE_EXECUTE_READWRITE, &old)) {
        ff132_loga(FF132_TAG "auto retry: cannot unprotect @0x%08X (err %lu) -> feature off\n",
               (unsigned)at, GetLastError());
        VirtualFree(cave, 0, MEM_RELEASE);
        return;
    }
    uint8_t* h = (uint8_t*)at;
    h[0] = 0xE9;
    put_rel32(h + 1, (uintptr_t)cave);
    for (int i = 5; i < FF132_AR_PATCH_LEN; i++) h[i] = 0x90;
    VirtualProtect((void*)at, FF132_AR_PATCH_LEN, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, FF132_AR_PATCH_LEN);
    g_siteOk = 1;
    ff132_loga(FF132_TAG "auto retry: sites ok, decided branch @0x%08X -> cave %p (%d bytes)"
                  " -- starts %s\n", (unsigned)at, (void*)cave, len, g_autoOn ? "ON" : "OFF");

    uintptr_t rs = g_base + FF132_RVA_AR_REGIST_SCRIPT;
    void* rcave = sig_at(FF132_RVA_AR_REGIST_SCRIPT, FF132_SIG_AR_REGIST_SCRIPT,
                         sizeof FF132_SIG_AR_REGIST_SCRIPT)
                ? ff13_install_esp_hook(rs, FF132_SIG_AR_REGIST_SCRIPT,
                                        (int)sizeof FF132_SIG_AR_REGIST_SCRIPT, (void*)on_regist_script)
                : NULL;
    ff132_loga(FF132_TAG "auto retry: registScriptDatabase @0x%08X %s\n", (unsigned)rs,
           rcave ? "hooked (retry hops on)" : "mismatch -> no retry hops");
}

// ---- the pump (game thread, frame site) -------------------------------------------------------
static int fader_busy(void) {
    uintptr_t f = fader();
    return f ? (int)rd32(f + FF132_OFF_AR_FADER_BUSY) : -1;
}
static int fader_done(void) {
    return fader() ? call0(FF132_RVA_AR_FADER_DONE) : -1;
}
static int deny_count(void) {
    uintptr_t gm = (uintptr_t)rd32(g_base + FF132_RVA_GAMEMAIN_PTR);
    if (!rd_ok((void*)(gm + FF132_OFF_GAMEMAIN_FSM), 4)) return -1;
    uintptr_t m = (uintptr_t)rd32(gm + FF132_OFF_GAMEMAIN_FSM);
    return rd_ok((void*)(m + FF132_OFF_GM_DENY), 4) ? (int)rd32(m + FF132_OFF_GM_DENY) : -1;
}

static void step_to(int to, uintptr_t bm) {
    DWORD now = GetTickCount();
    ff132_log(FF132_TAG "ar step %s->%s t=%lums bm_st=%d sub=%d f40=%08X res=%d zsm=%d fbusy=%d"
                 " fdone=%d deny=%d\n",
          kStep[g_step], kStep[to], (unsigned long)(now - g_armAt),
          bm ? (int)rd32(bm + FF132_OFF_BM_STATE) : -1, bm ? (int)rd32(bm + FF132_OFF_BM_SUBSTATE) : -1,
          bm ? (unsigned)rd32(bm + FF132_OFF_BM_FLAGS + 8) : 0u,
          bm ? (int)rd32(bm + FF132_OFF_BM_RESULT) : -1, zone_state_peek(), fader_busy(),
          fader_done(), deny_count());
    g_step = to;
    g_stepAt = now;
}

static void refund_line(uintptr_t bm) {
    int n = (int)rd32(bm + FF132_OFF_BM_ITEMS_N);
    int cap = (FF132_OFF_BM_ITEMS_N - FF132_OFF_BM_ITEMS) / FF132_BM_ITEM_STRIDE;
    char buf[1024];
    int at = snprintf(buf, sizeof buf, "ar refund: n=%d", n);
    for (int i = 0; i < n && i < cap && at < (int)sizeof buf - 64; i++) {
        uintptr_t r = bm + FF132_OFF_BM_ITEMS + (uintptr_t)i * FF132_BM_ITEM_STRIDE;
        char name[33];
        memcpy(name, (const void*)r, 32);
        name[32] = 0;
        at += snprintf(buf + at, sizeof buf - (size_t)at, " [%s a=%d b=%d]", name,
                       (int)rd32(r + 0x20), (int)rd32(r + 0x24));
    }
    ff132_log(FF132_TAG "%s fd0=%d\n", buf, (int)rd32(bm + FF132_OFF_BM_FD0));
}

// After FIRE: what the zone does with the retry, and whether a battle comes back.
static void watch(uintptr_t bm, DWORD now) {
    int zsm = zone_state_peek();
    if (zsm != g_lastZsm) {
        uintptr_t zm = zone_mgr();
        ff132_log(FF132_TAG "ar zsm %d->%d t=%lu z6c4c=%d z6be8=%d\n", g_lastZsm, zsm,
              (unsigned long)(now - g_armAt), zm ? (int)rd32(zm + FF132_OFF_ZM_RESTART_SCENE) : -1,
              zm ? (int)rd32(zm + FF132_OFF_ZM_BATTLE_READY) : -1);
        g_lastZsm = zsm;
    }
    int st = bm ? (int)rd32(bm + FF132_OFF_BM_STATE) : 0;
    if (st == 0) g_sawBmIdle = 1;
    if (g_sawBmIdle && st >= 2) {
        ff132_log(FF132_TAG "ar rebattle: scene=%d prev=%d t=%lums st=%d\n",
              (int)rd32(bm + FF132_OFF_BM_SCENE), g_prevScene, (unsigned long)(now - g_armAt), st);
        hop_drop("a battle began");
        g_step = AR_IDLE;
        return;
    }
    if (now - g_stepAt >= AR_WATCH_MS) {
        ff132_log(FF132_TAG "ar watch: done, no new battle in %d s (zsm=%d)\n", AR_WATCH_MS / 1000, zsm);
        hop_drop("no battle in time");
        g_step = AR_IDLE;
    }
}

// Runs to completion even if the player turns the feature OFF mid-way: the fight is already
// frozen in the abort path and only this sends it on.
void ff132_autoretry_pump(void) {
    if (g_step == AR_IDLE) return;
    uintptr_t bm = battle_main();
    DWORD now = GetTickCount();

    if (bm && g_step != AR_WATCH) {
        int st = (int)rd32(bm + FF132_OFF_BM_STATE);
        if (st != g_lastBmSt) {
            ff132_log(FF132_TAG "ar bm: st %d->%d t=%lums sub=%d f40=%08X res=%d\n", g_lastBmSt, st,
                  (unsigned long)(now - g_armAt), (int)rd32(bm + FF132_OFF_BM_SUBSTATE),
                  (unsigned)rd32(bm + FF132_OFF_BM_FLAGS + 8), (int)rd32(bm + FF132_OFF_BM_RESULT));
            g_lastBmSt = st;
        }
        if (st == FF132_BM_STATE_ABORT_IDLE && !g_refundLogged) {
            g_refundLogged = 1;
            refund_line(bm);
        }
    }
    if (g_step == AR_WATCH) {
        watch(bm, now);
        return;
    }
    if (now - g_stepAt >= AR_STEP_TIMEOUT_MS) {
        // A fade that never settles is cosmetic: go on without it. Anything else is abandoned.
        if (g_step == AR_FADE1 || g_step == AR_FADE2) {
            ff132_log(FF132_TAG "ar timeout: %s after %d s, going on without the fade\n", kStep[g_step],
                  AR_STEP_TIMEOUT_MS / 1000);
            step_to(AR_W46, bm);
            return;
        }
        ff132_log(FF132_TAG "ar timeout: %s after %d s, abandoned\n", kStep[g_step],
              AR_STEP_TIMEOUT_MS / 1000);
        hop_drop("retry abandoned");
        step_to(AR_IDLE, bm);
        return;
    }

    switch (g_step) {
    case AR_FADE0: {
        uintptr_t gm = game_main();
        if (!gm) return;
        float secs = *(const volatile float*)(g_base + FF132_RVA_AR_FADE0_SECS);
        ((ar_fade_layer_t)(g_base + FF132_RVA_AR_FADE_LAYER))((void*)gm, 0, secs);
        step_to(AR_FADE1, bm);
        return;
    }
    case AR_FADE1: {
        if (!fader() || call0(FF132_RVA_AR_FADER_BUSY)) return;
        float secs = *(const volatile float*)(g_base + FF132_RVA_AR_FADE1_SECS);
        ((ar_fader_start_t)(g_base + FF132_RVA_AR_FADER_START))(secs);
        step_to(AR_FADE2, bm);
        return;
    }
    case AR_FADE2:
        if (fader() && call0(FF132_RVA_AR_FADER_DONE)) step_to(AR_W46, bm);
        return;
    case AR_W46:
        if (!call0(FF132_RVA_AR_BM_WAIT46)) return;
        call0(FF132_RVA_AR_BM_SHUTDOWN);
        step_to(AR_WBM0, bm);
        return;
    case AR_WBM0:
        if (call0(FF132_RVA_AR_BM_WAIT0)) step_to(AR_FIRE, bm);
        return;
    case AR_FIRE: {
        int zsm = zone_state_peek();
        // The pause menu's Retry got there first (or the zone moved on by itself).
        if (zsm == 8 || zsm == 38 || zsm == 77 || zsm == 1 || zsm == 2) {
            ff132_log(FF132_TAG "ar fire: zsm=%d already, the game took over\n", zsm);
            step_to(AR_WATCH, bm);
            return;
        }
        if (zsm != FF132_ZSM_BATTLE) {
            // Paused: wait for 7 however long it takes. Anything else times out, so a trigger
            // never lands in some later fight's state 7.
            if ((zsm >= 24 && zsm <= 32) || zsm == 75) g_stepAt = now;
            return;
        }
        uintptr_t field = ptr_at(FF132_RVA_AR_FIELD_PTR, FF132_OFF_AR_FIELD_BITS + 4);
        uintptr_t zm = zone_mgr();
        if (!field || !zm) return;
        *(volatile uint32_t*)(field + FF132_OFF_AR_FIELD_BITS) |= FF132_AR_FIELD_BIT;
        ((ar_zm_fn_t)(g_base + FF132_RVA_AR_ZSM_TRIGGER7))((void*)zm);
        ff132_log(FF132_TAG "ar fire: trigger 7 sent (zsm now %d)\n", zone_state_peek());
        step_to(AR_WATCH, bm);
        return;
    }
    default:
        return;
    }
}

void ff132_autoretry_toggle(void) {
    if (!g_siteOk) {
        ff132_log(FF132_TAG "auto retry: not installed\n");
        ff132_flash(ui_text(T_AR_NA));
        return;
    }
    int on = !g_autoOn;
    g_autoOn = on;
    ff132_log(FF132_TAG "auto retry: %s\n", on ? "ON" : "OFF");
    if (!on) ff132_flash(ui_text(T_AR_OFF));   // ON is said by the status line alone
    ff132_state_save();
}

int ff132_autoretry_on(void) { return g_autoOn; }
// ff132_state_load, before ff132_autoretry_init.
void ff132_autoretry_set(int v) {
    g_autoOn = v ? 1 : 0;
    if (g_autoOn)
        ff132_loga(FF132_TAG "auto retry: restored ON (carried over from a previous session)\n");
}

// ---- the status line (render thread) ----------------------------------------------------------
int ff132_autoretry_visible(void) { return g_siteOk && g_autoOn; }

int ff132_autoretry_draw(int x, int y) {
    if (!ff132_autoretry_visible()) return y;
    char key[32], buf[64];
    vk_label(g_battleRestartAutoVk, key, sizeof key);
    snprintf(buf, sizeof buf, ui_text(T_AR_ON), key);
    ff132_ovl_say(x, y, buf, FF132_COL_AUTORETRY);
    return y + FF132_OVL_LINE_H;
}
