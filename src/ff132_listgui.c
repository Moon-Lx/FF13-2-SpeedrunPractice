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

// The save list GUI's row count: past 50 rows by relocating its two embedded arrays.
//
// Threads: the hotkey worker only, and only while no save/load list exists (the patched code runs
// only while one does). Once widened the GUI stays widened for the session.
#include "ff132_internal.h"
#include "ff132_addrs.h"

static int g_layout = 0;         // 0 = the game's own code, else the N the sites were written for
static int g_failed = 0;         // a write failed once: never tried again this session

int ff132_listgui_rows(void) { return g_layout ? g_layout : FF132_LG_OLD_ROWS; }

static int valid_n(int n) {
    return n >= FF132_LG_OLD_ROWS && n % 10 == 0 && n <= 127;   // 127: the imm8 at 0x8956B9
}

static uint32_t orig_field(const ff132_lg_site* s) {
    uint32_t v = 0;
    for (int i = 0; i < s->width; i++) v |= (uint32_t)s->code[s->at + i] << (8 * i);
    return v;
}

static uint32_t field_for(const ff132_lg_site* s, int n) {
    uint32_t o = orig_field(s);
    switch (s->kind) {
    case FF132_LG_ALLOC:  return FF132_LG_OLD_SIZE + (uint32_t)n * 0x14;
    case FF132_LG_STATE:  return FF132_LG_OLD_SIZE + (o - FF132_LG_OLD_STATES);
    case FF132_LG_WIDGET: return FF132_LG_OLD_SIZE + (uint32_t)n * 0x10 + (o - FF132_LG_OLD_WIDGETS);
    case FF132_LG_N:      return (uint32_t)n;
    case FF132_LG_N1:     return (uint32_t)n - 1;
    default:           return (uint32_t)n / 10;
    }
}

static uint32_t field_now(const ff132_lg_site* s) {
    const volatile uint8_t* p = (const volatile uint8_t*)(g_base + s->rva + s->at);
    uint32_t v = 0;
    for (int i = 0; i < s->width; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

// 1 = the instruction around the field is the original and the field holds what this mod last
// wrote (the original when untouched) or already the value for n. *bad gets the first mismatch.
static int sites_ok(int n, int* bad) {
    for (int i = 0; i < FF132_LISTGUI_NSITES; i++) {
        const ff132_lg_site* s = &FF132_LISTGUI_SITES[i];
        const uint8_t* p = (const uint8_t*)(g_base + s->rva);
        int ok = !IsBadReadPtr(p, s->len);
        for (int b = 0; ok && b < s->len; b++)
            if ((b < s->at || b >= s->at + s->width) && p[b] != s->code[b]) ok = 0;
        if (ok) {
            uint32_t v = field_now(s);
            ok = v == (g_layout ? field_for(s, g_layout) : orig_field(s)) || v == field_for(s, n);
        }
        if (!ok) { if (bad) *bad = i; return 0; }
    }
    return 1;
}

int ff132_listgui_can(int n) {
    if (g_layout >= n) return 1;
    return !g_failed && valid_n(n) && sites_ok(n, NULL);
}

static int poke(const ff132_lg_site* s, uint32_t v) {
    uint8_t* p = (uint8_t*)(g_base + s->rva + s->at);
    DWORD old = 0;
    if (!VirtualProtect(p, s->width, PAGE_EXECUTE_READWRITE, &old)) return 0;
    for (int i = 0; i < s->width; i++) ((volatile uint8_t*)p)[i] = (uint8_t)(v >> (8 * i));
    VirtualProtect(p, s->width, old, &old);
    FlushInstructionCache(GetCurrentProcess(), p, s->width);
    return 1;
}

// A list GUI alive or about to be built: the open flag rises before the GUI is created, and the
// update stamp covers a GUI the flag no longer vouches for.
static int list_exists(void) { return ff132_save_list_up() || ff132_list_gui_recent(); }

int ff132_listgui_apply(int n) {
    if (g_layout >= n) return 1;
    if (!valid_n(n)) {
        ff132_loga(FF132_TAG "listgui: FAILED -- %d rows is not a multiple of 10 in %d..127\n",
               n, FF132_LG_OLD_ROWS);
        return 0;
    }
    if (g_failed) return 0;
    if (list_exists()) {
        ff132_log(FF132_TAG "listgui: a save list is up -- rows stay %d for now\n", ff132_listgui_rows());
        return 0;
    }
    int bad = -1;
    if (!sites_ok(n, &bad)) {
        const ff132_lg_site* s = &FF132_LISTGUI_SITES[bad];
        const uint8_t* p = (const uint8_t*)(g_base + s->rva);
        char got[3 * 8 + 1] = "", want[3 * 8 + 1] = "";
        for (int b = 0; b < s->len; b++) {
            snprintf(got + 3 * b, 4, "%02X ", IsBadReadPtr(p + b, 1) ? 0 : p[b]);
            snprintf(want + 3 * b, 4, "%02X ", s->code[b]);
        }
        ff132_loga(FF132_TAG "listgui: FAILED -- site %d/%d @0x%08X reads %s(the game's own: %s, field"
                      " for %d rows 0x%X) -- nothing written, rows stay %d\n",
               bad + 1, FF132_LISTGUI_NSITES, (unsigned)(g_base + s->rva), got, want, n,
               (unsigned)field_for(s, n), ff132_listgui_rows());
        return 0;
    }
    // Table order is the write order. The allocation goes first: arrays moved past +0x540 of a
    // 0x540-byte object overrun the heap. The counts go last: raised over unmoved arrays they
    // write past the object's fields.
    uint32_t prev[FF132_LISTGUI_NSITES];
    int done = 0;
    for (; done < FF132_LISTGUI_NSITES; done++) {
        const ff132_lg_site* s = &FF132_LISTGUI_SITES[done];
        prev[done] = field_now(s);
        if (prev[done] != field_for(s, n) && !poke(s, field_for(s, n))) break;
    }
    if (done < FF132_LISTGUI_NSITES) {
        DWORD err = GetLastError();
        const ff132_lg_site* s = &FF132_LISTGUI_SITES[done];
        int undone = 1;
        while (done-- > 0)
            if (field_now(&FF132_LISTGUI_SITES[done]) != prev[done]
                && !poke(&FF132_LISTGUI_SITES[done], prev[done])) undone = 0;
        g_failed = 1;
        ff132_loga(FF132_TAG "listgui: FAILED -- site @0x%08X not writable (error %lu); %s, rows stay %d\n",
               (unsigned)(g_base + s->rva), err,
               undone ? "every site written before it was restored"
                      : "RESTORE FAILED TOO -- the list GUI is in a mixed state", ff132_listgui_rows());
        return 0;
    }
    ff132_loga(FF132_TAG "listgui: rows %d -> %d (%d sites)\n", ff132_listgui_rows(), n, FF132_LISTGUI_NSITES);
    g_layout = n;
    return 1;
}
