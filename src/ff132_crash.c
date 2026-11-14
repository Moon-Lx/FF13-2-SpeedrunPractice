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

#include "ff132_internal.h"

#ifndef FF132_BUILD_ID
#define FF132_BUILD_ID "unknown"
#endif

// Where a crash was, written from the crashing thread itself. The filter always hands the exception
// on, so the game dies exactly as it would have.

// Everything the report needs is prepared BEFORE the crash: at fault time the CRT's streams and
// the heap may be gone, so the path is already resolved and the writer is CreateFile/WriteFile.
static char g_crashPath[MAX_PATH];
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = NULL;
// One report per process, guarded on ENTRY: the filter chains, and a game filter that saved ours
// would otherwise bounce back into here forever.
static LONG volatile g_inFilter = 0;
static uintptr_t g_selfBase = 0;

static void crash_write(const char* s) {
    if (!g_crashPath[0]) return;
    HANDLE h = CreateFileA(g_crashPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(h, s, (DWORD)strlen(s), &n, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
}

// A module name + RVA if the address is in one, otherwise what the memory itself is. A hook cave
// is VirtualAlloc'd, so it belongs to no module and this is the only thing that names it.
static void addr_where(uintptr_t a, char* out, size_t n) {
    HMODULE m = NULL;
    if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)a, &m) && m) {
        char p[MAX_PATH];
        const char* b = "?";
        if (GetModuleFileNameA(m, p, MAX_PATH)) {
            const char* s = strrchr(p, '\\');
            b = s ? s + 1 : p;
        }
        snprintf(out, n, "%s+0x%X", b, (unsigned)(a - (uintptr_t)m));
        return;
    }
    MEMORY_BASIC_INFORMATION mbi;
    if (a && VirtualQuery((void*)a, &mbi, sizeof mbi) == sizeof mbi && mbi.State == MEM_COMMIT) {
        int exec = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                                   | PAGE_EXECUTE_WRITECOPY)) != 0;
        snprintf(out, n, "%s 0x%08X+0x%X%s", mbi.Type == MEM_PRIVATE ? "private memory" : "mapped",
                 (unsigned)(uintptr_t)mbi.AllocationBase,
                 (unsigned)(a - (uintptr_t)mbi.AllocationBase),
                 exec ? " (executable and in no module -- one of this mod's hook caves)" : "");
        return;
    }
    snprintf(out, n, "unmapped");
}

static int addr_is_code(uintptr_t a) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!a || VirtualQuery((void*)a, &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                           | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// No frame pointer to walk (the game is -O2 x86), so the stack is SCANNED for return addresses:
// executable memory whose preceding bytes are a call. Without the call test the report fills up
// with ASCII that happens to land in some module's range.
#define CRASH_STACK_WORDS 512
#define CRASH_STACK_HITS  14

static int after_call(uintptr_t v) {
    if (IsBadReadPtr((void*)(v - 6), 6)) return 0;
    const uint8_t* b = (const uint8_t*)v;
    if (b[-5] == 0xE8) return 1;                     // call rel32
    if (b[-3] == 0x9A) return 1;                     // call far
    for (int k = 2; k <= 6; k++) if (b[-k] == 0xFF) return 1;   // call r/m32, any addressing form
    return 0;
}

static void crash_stack(uintptr_t esp) {
    char line[400], where[200];
    int hits = 0;
    for (int pass = 0; pass < 2 && hits == 0; pass++) {
        // Pass 2 only happens when the call test found nothing at all: then anything executable is
        // better than an empty report.
        if (pass == 1) crash_write(FF132_TAG "CRASH:   (no return addresses found -- code "
                                   "pointers, which may be data)\n");
        for (int i = 0; i < CRASH_STACK_WORDS && hits < CRASH_STACK_HITS; i++) {
            uintptr_t slot = esp + (uintptr_t)i * 4;
            if (IsBadReadPtr((void*)slot, 4)) break;
            uintptr_t v = *(uintptr_t*)slot;
            if (!addr_is_code(v)) continue;
            if (pass == 0 && !after_call(v)) continue;
            addr_where(v, where, sizeof where);
            snprintf(line, sizeof line, FF132_TAG "CRASH:   [esp+0x%03X] 0x%08X  %s\n",
                     (unsigned)(i * 4), (unsigned)v, where);
            crash_write(line);
            hits++;
        }
    }
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    if (InterlockedExchange(&g_inFilter, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;
    if (ep && ep->ExceptionRecord && ep->ContextRecord) {
        const EXCEPTION_RECORD* er = ep->ExceptionRecord;
        const CONTEXT* c = ep->ContextRecord;
        char line[600], where[200];

        addr_where((uintptr_t)er->ExceptionAddress, where, sizeof where);
        snprintf(line, sizeof line,
                 FF132_TAG "CRASH: exception 0x%08X at 0x%08X (%s) on thread %u, build " FF132_BUILD_ID "\n",
                 (unsigned)er->ExceptionCode, (unsigned)(uintptr_t)er->ExceptionAddress, where,
                 (unsigned)GetCurrentThreadId());
        crash_write(line);

        if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
            uintptr_t bad = (uintptr_t)er->ExceptionInformation[1];
            addr_where(bad, where, sizeof where);
            snprintf(line, sizeof line, FF132_TAG "CRASH: while %s 0x%08X (%s)\n",
                     er->ExceptionInformation[0] == 8 ? "executing"
                     : er->ExceptionInformation[0] ? "writing" : "reading",
                     (unsigned)bad, where);
            crash_write(line);
        }

        snprintf(line, sizeof line,
                 FF132_TAG "CRASH: eip=%08X esp=%08X ebp=%08X eax=%08X ebx=%08X ecx=%08X "
                 "edx=%08X esi=%08X edi=%08X\n",
                 (unsigned)c->Eip, (unsigned)c->Esp, (unsigned)c->Ebp, (unsigned)c->Eax,
                 (unsigned)c->Ebx, (unsigned)c->Ecx, (unsigned)c->Edx, (unsigned)c->Esi,
                 (unsigned)c->Edi);
        crash_write(line);

        // Plain reads and IsBadReadPtr-guarded pointer walks only: nothing here may take a lock
        // the crashing thread could be holding.
        snprintf(line, sizeof line,
                 FF132_TAG "CRASH: exe=0x%08X dll=0x%08X | main=%d zone=%d battle=%d | kill=%d "
                 "speed_step=%d suppress=%d overlay=%d\n",
                 (unsigned)g_base, (unsigned)g_selfBase, ff132_main_state(), zone_state_peek(),
                 ff132_battle_in_battle(), ff132_kill_mode(), ff132_speed_step_get(), ff132_encount_on(),
                 (int)g_battleOverlay);
        crash_write(line);

        crash_write(FF132_TAG "CRASH: code addresses on the stack, nearest first:\n");
        crash_stack((uintptr_t)c->Esp);
    }
    return g_prevFilter ? g_prevFilter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

// The seat is taken by whoever calls last, and the game can take it after this mod's DllMain -- so
// it is re-taken on a timer, and whatever was found sitting there is kept as the one to hand on to.
void ff132_crash_rearm(void) {
    if (!g_crashPath[0]) return;
    LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(crash_filter);
    if (prev != crash_filter) g_prevFilter = prev;
}

void ff132_crash_install(void) {
    if (!data_file(g_crashPath, sizeof g_crashPath, FF132_LOG_NAME)) {
        ff132_loga(FF132_TAG "crash report: no writable log path -- not installed\n");
        return;
    }
    g_selfBase = (uintptr_t)GetModuleHandleA("dinput8.dll");
    ff132_crash_rearm();
    ff132_loga(FF132_TAG "crash report: armed -- a crash writes where it happened to this log "
                  "(and is then handed on to the game unchanged)\n");
}
