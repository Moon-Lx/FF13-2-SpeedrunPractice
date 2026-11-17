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

// Save categories: more than one set of saves, swapped without restarting the game.
// The category IN USE is the game's own save folder, untouched; every other one is parked under
// {save folder}\categories\{name}\ together with the one sidecar that is keyed by slot number, and
// the marker that names it as the category ff13-2-run.bat lends the unmodded game.
//
// Threads: all of it runs on the hotkey worker. The name getters are read from the render thread
// while it draws the menu's rows, so each copies out of a fixed array with a bounded read.
// ff132_tidy.c owns the menu's words and does the flashing, all but a category's own label, which
// ff132_cats_name / ff132_cats_name_q format whole (name, English quotes, save count); this file logs.
#include "ff132_internal.h"

#include <objbase.h>
#include <shellapi.h>

// Folder names: equal and ordered as the file system compares them (case folded beyond ASCII too),
// read in the ANSI code page the *A calls that made the folders use.
static int name_cmp(const char* a, const char* b) {
    wchar_t wa[MAX_PATH], wb[MAX_PATH];
    if (MultiByteToWideChar(CP_ACP, 0, a, -1, wa, MAX_PATH) <= 0 ||
        MultiByteToWideChar(CP_ACP, 0, b, -1, wb, MAX_PATH) <= 0)
        return strcmp(a, b);
    return CompareStringOrdinal(wa, -1, wb, -1, TRUE) - CSTR_EQUAL;
}
static int name_eq(const char* a, const char* b) { return name_cmp(a, b) == 0; }

#define CATS_DIR "categories"
// Every number a save file can carry (two digits). Whatever moves a whole category walks all of
// them whatever the count is now: a file left behind would mix into the next category.
#define CAT_SLOTS_MAX 100
#define CAT_FILES(n)  ((n) + 2)          // the saves, the direct-load sidecar, the no-mod marker
#define CAT_MARKER(n) ((n) + 1)          // the no-mod marker's index among them

static char g_catCur[FF132_CATNAME_BYTES] = "main";
static char g_catPrev[FF132_CATNAME_BYTES] = "";
static char g_catCurU8[FF132_CATNAME_U8];
static char g_catList[FF132_CAT_MAX][FF132_CATNAME_BYTES];
static char g_catListU8[FF132_CAT_MAX][FF132_CATNAME_U8];
static volatile int g_catListSaves[FF132_CAT_MAX];
static volatile int g_catListN = 0;
static volatile int g_catSel   = -1;     // the delete row's pick; -1 only when nothing is parked
static volatile int g_swSel    = -1;     // the switch row's pick; -1 = none picked

static volatile int   g_delArm   = -1;   // the g_catSel this confirm belongs to; -1 = idle
static volatile int   g_nomodAt  = -2;   // ff132_nomod_at's answer

static int file_exists(const char* p) { return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES; }

// On a slow disk a full copy can outlast the input gate: without this the gate lapses mid-copy and
// the render thread's guard closes the menu underneath. Re-armed at half-life, not per file -- every arm resets the gate's own log latch.
static void gate_keep_alive(void) {
    int left = ff132_di_gate_left_ms();
    if (left >= 0 && left < FF132_DI_GATE_MAX_MS / 2) ff132_di_gate_arm();
}

// The precision bounds the read at the source array's own size: the worker may be halfway through
// rewriting the entry this render-thread read is copying, and a missing terminator must not send
// the copy off the end of the array.
static void copy_u8(char* out, size_t n, const char* src, size_t srcCap) {
    if (!out || !n) return;
    snprintf(out, n, "%.*s", (int)(srcCap - 1), src);
}

// ---- names ------------------------------------------------------------------------------------
// Paths here are cp932 and the overlay draws UTF-8, so each name is kept in both forms rather than
// converted on the drawing thread.
static void name_to_u8(char* out, size_t n, const char* ansi) {
    wchar_t w[FF132_CATNAME_BYTES];
    if (MultiByteToWideChar(932, 0, ansi, -1, w, (int)(sizeof w / sizeof w[0])) > 0
        && WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL) > 0) return;
    snprintf(out, n, "%s", ansi);
}
static void name_trim(char* b) {
    size_t n = strlen(b);
    while (n && (unsigned char)b[n - 1] <= ' ') b[--n] = 0;
}
static void name_set_cur(const char* ansi) {
    snprintf(g_catCur, sizeof g_catCur, "%s", ansi);
    name_to_u8(g_catCurU8, sizeof g_catCurU8, g_catCur);
}

static int name_file(char* p, size_t n) { return data_file(p, n, FF132_CATNAME_FILE); }

static void name_load(void) {
    name_to_u8(g_catCurU8, sizeof g_catCurU8, g_catCur);
    char p[MAX_PATH];
    if (!name_file(p, sizeof p)) return;
    FILE* f = fopen(p, "r");
    if (!f) return;
    char b[FF132_CATNAME_BYTES];
    if (fgets(b, sizeof b, f)) { name_trim(b); if (b[0]) name_set_cur(b); }
    // Line 2 (the category switched away from last) may be absent.
    if (fgets(b, sizeof b, f)) {
        name_trim(b);
        if (b[0] && !name_eq(b, g_catCur)) snprintf(g_catPrev, sizeof g_catPrev, "%s", b);
    }
    fclose(f);
}
static void name_save(void) {
    char p[MAX_PATH];
    if (!name_file(p, sizeof p)) return;
    char buf[2 * FF132_CATNAME_BYTES + 4];
    int n = snprintf(buf, sizeof buf, "%s\n%s%s", g_catCur, g_catPrev, g_catPrev[0] ? "\n" : "");
    if (n >= (int)sizeof buf || !ff132_write_whole(p, buf, (size_t)n))
        ff132_loga(FF132_TAG "category: cannot write %s\n", p);
}

// ---- paths ------------------------------------------------------------------------------------
// The save folder is the game's own path builder's answer with the file name taken off: there is
// no other authority on where the saves are, and the builder creates the folder on its first call.
// The builder joins with '/' ("%s/%s" at 0x6B5E96) onto a folder that may carry '\', so the cut is
// at the LAST separator of either kind (measured: a '\'-only search cuts the wrong place).
static int save_root_dir(char* out, size_t n) {
    char p[MAX_PATH];
    if (!save_slot_path(0, p, sizeof p)) return 0;
    char* s1 = strrchr(p, '\\');
    char* s2 = strrchr(p, '/');
    char* s = (s1 > s2) ? s1 : s2;
    if (!s || s == p) return 0;
    *s = 0;
    if (strlen(p) >= n) return 0;
    snprintf(out, n, "%s", p);
    return 1;
}
static int cats_root(char* out, size_t n) {
    char root[MAX_PATH];
    if (!save_root_dir(root, sizeof root)) return 0;
    snprintf(out, n, "%s\\" CATS_DIR, root);
    return 1;
}
// Every parked category's folder under root (the save folder), in folder order: fn(dir, name)
// returning nonzero stops the walk. A folder whose path will not fit is skipped and sets *err, as
// does a listing that fails part-way. -1 = the listing could not start; no categories folder is 0.
static int parked_walk(const char* root, int (*fn)(const char* dir, const char* name, void* ctx),
                       void* ctx, int* err) {
    char pat[MAX_PATH];
    if (snprintf(pat, sizeof pat, "%s\\" CATS_DIR "\\*", root) >= (int)sizeof pat) return -1;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        return (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) ? 0 : -1;
    }
    int n = 0, stop = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        char dir[MAX_PATH];
        if (snprintf(dir, sizeof dir, "%s\\" CATS_DIR "\\%s", root, fd.cFileName) >= (int)sizeof dir) {
            *err = 1;
            continue;
        }
        n++;
        stop = fn(dir, fd.cFileName, ctx);
    } while (!stop && FindNextFileA(h, &fd));
    if (!stop && GetLastError() != ERROR_NO_MORE_FILES) *err = 1;
    FindClose(h);
    return n;
}

// The write time of slot's file in each parked category's folder: the working copies the working
// slot's markers still have to know. Any thread. A file that is not there, or a folder name the
// ANSI calls cannot spell (no category the mod can switch to), counts as absent; any other failure
// answers -1, and the caller then drops no marker this time.
typedef struct { int slot, n, max, bad; uint64_t* out; } parked_ids_ctx;
static int parked_ids_at(const char* dir, const char* name, void* vctx) {
    parked_ids_ctx* c = (parked_ids_ctx*)vctx;
    (void)name;
    char p[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (snprintf(p, sizeof p, "%s\\ff13_2-%02d.dat", dir, c->slot) >= (int)sizeof p) { c->bad = 1; return 1; }
    if (GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) {
        if (c->n >= c->max) { c->bad = 1; return 1; }
        c->out[c->n++] = ft_u64(fa.ftLastWriteTime);
    } else {
        DWORD e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND && e != ERROR_INVALID_NAME) {
            c->bad = 1;
            return 1;
        }
    }
    return 0;
}
int ff132_cats_parked_idents(int slot, uint64_t* out, int max) {
    char root[MAX_PATH];
    int err = 0;
    if (!save_root_dir(root, sizeof root)) return -1;
    parked_ids_ctx c = { slot, 0, max, 0, out };
    int r = parked_walk(root, parked_ids_at, &c, &err);
    return (r < 0 || err || c.bad) ? -1 : c.n;
}

static int cat_dir_of(char* out, size_t n, const char* name) {
    char cats[MAX_PATH];
    out[0] = 0;
    if (!name || !name[0] || !cats_root(cats, sizeof cats)) return 0;
    snprintf(out, n, "%s\\%s", cats, name);
    return 1;
}

// The numbers the game offers right now. A save is never CREATED at or past it: the game aborts
// when a list it opens finds one.
static int slot_count(void) {
    int n = save_slot_count();
    if (n < 1) return FF132_SLOTS_GAME;
    return n < CAT_SLOTS_MAX ? n : CAT_SLOTS_MAX;
}

// Two paths per index: the sidecar and the marker live beside the exe while their category is in
// use, yet a parked category keeps its copies next to the saves they belong to.
static int cat_file_pair(int i, int slots, const char* dir, char* act, size_t an,
                         char* in, size_t bn) {
    if (i < slots) {
        if (!save_slot_path(i, act, an)) return 0;
        snprintf(in, bn, "%s\\ff13_2-%02d.dat", dir, i);
        return 1;
    }
    const char* name = (i == CAT_MARKER(slots)) ? FF132_NOMOD_NAME : FF132_SLOTS_NAME;
    if (!data_file(act, an, name)) return 0;
    snprintf(in, bn, "%s\\%s", dir, name);
    return 1;
}

// MOVEFILE_REPLACE_EXISTING is deliberately absent: a move onto a file that is already there fails
// and is logged, rather than writing over someone's save.
static int cat_move_all(const char* dir, int intoSet) {
    int slots = CAT_SLOTS_MAX, moved = 0;
    for (int i = 0; i < CAT_FILES(slots); i++) {
        char a[MAX_PATH], b[MAX_PATH];
        if (!cat_file_pair(i, slots, dir, a, sizeof a, b, sizeof b)) continue;
        const char* src = intoSet ? a : b;
        const char* dst = intoSet ? b : a;
        if (!file_exists(src)) continue;
        if (MoveFileExA(src, dst, MOVEFILE_COPY_ALLOWED)) moved++;
        else ff132_loga(FF132_TAG "category: %s -> %s failed (err %lu)\n", src, dst,
                    (unsigned long)GetLastError());
    }
    return moved;
}

// How many of the files a park moves are still on the given side (0 = the save folder, 1 = parked
// under `dir`); savesOnly narrows it to the save files themselves.
static int cat_files_at(const char* dir, int parkedSide, int savesOnly) {
    int slots = CAT_SLOTS_MAX, n = 0;
    int top = savesOnly ? slots : CAT_FILES(slots);
    for (int i = 0; i < top; i++) {
        char a[MAX_PATH], b[MAX_PATH];
        if (cat_file_pair(i, slots, dir, a, sizeof a, b, sizeof b)
            && file_exists(parkedSide ? b : a)) n++;
    }
    return n;
}

// The glob also matches the parked sidecar, so only the exact ff13_2-NN.dat shape counts.
static int save_file_number(const char* f) {
    if (strlen(f) == 13 && f[7] >= '0' && f[7] <= '9' && f[8] >= '0' && f[8] <= '9'
        && !_stricmp(f + 9, ".dat")) return (f[7] - '0') * 10 + (f[8] - '0');
    return -1;
}

// The highest number among the saves in dir (-1 = none); *n counts every save, *above those
// numbered `from` or higher.
static int dir_numbers(const char* dir, int from, int* n, int* above) {
    char glob[MAX_PATH];
    int hi = -1;
    snprintf(glob, sizeof glob, "%s\\ff13_2-*.dat", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        int s = save_file_number(fd.cFileName);
        if (s < 0) continue;
        if (n) (*n)++;
        if (above && s >= from) (*above)++;
        if (s > hi) hi = s;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return hi;
}

static int cat_saves_in(const char* dir) {
    int n = 0;
    dir_numbers(dir, 0, &n, NULL);
    return n;
}

// Every folder under categories\, not only the ones the menu lists: the list stops at FF132_CAT_MAX
// and leaves out a folder named like the category in use, and a save in either still counts.
typedef struct { int from, hi, past; char* where; size_t whereN; } numbers_ctx;
static int numbers_at(const char* dir, const char* name, void* vctx) {
    numbers_ctx* c = (numbers_ctx*)vctx;
    (void)name;
    int d = dir_numbers(dir, c->from, NULL, &c->past);
    if (d > c->hi) {
        c->hi = d;
        if (c->where && c->whereN) snprintf(c->where, c->whereN, "%s", dir);
    }
    return 0;
}
int ff132_cats_scan_numbers(int from, int* above, char* where, size_t whereN) {
    char root[MAX_PATH];
    int past = 0;
    if (above) *above = 0;
    if (where && whereN) where[0] = 0;
    if (!save_root_dir(root, sizeof root)) return -2;
    numbers_ctx c = { from, dir_numbers(root, from, NULL, &past), 0, where, whereN };
    if (c.hi >= 0 && where && whereN) snprintf(where, whereN, "%s", root);
    c.past = past;
    // A folder left unread could hold the save that keeps the slot limit up: no answer beats a low one.
    int err = 0;
    if (parked_walk(root, numbers_at, &c, &err) < 0 || err) return -2;
    if (above) *above = c.past;
    return c.hi;
}

// While the working slot is on, its number is never free: a save sent there would be refused as the
// player's own once that category is switched in, and the working slot would go OFF. While it is
// off the number is an ordinary slot, as the README says; turning it on later refuses what is there.
int ff132_cat_free_slot(const char* dir) {
    int slots = slot_count(), w = save_work_slot();
    for (int s = 0; s < slots; s++) {
        if (s == w) continue;
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\ff13_2-%02d.dat", dir, s);
        if (!file_exists(p)) return s;
    }
    return -1;
}

int ff132_cat_free_count(const char* dir) {
    int slots = slot_count(), w = save_work_slot(), n = 0;
    for (int s = 0; s < slots; s++) {
        if (s == w) continue;
        char p[MAX_PATH];
        snprintf(p, sizeof p, "%s\\ff13_2-%02d.dat", dir, s);
        if (!file_exists(p)) n++;
    }
    return n;
}

// ---- the journal --------------------------------------------------------------------------------
// phase 1 = the category in use is not parked yet; phase 2 = it is, and the target's files are on
// their way in. Replaying phase 1 over a half-finished phase 2 would park the TARGET's files under
// the old category's name.
static int cat_journal_path(char* p, size_t n) { return data_file(p, n, FF132_CATJNL_NAME); }

static int cat_journal_write(int phase, const char* cur, const char* target) {
    char p[MAX_PATH], buf[32 + 2 * FF132_CATNAME_BYTES];
    if (!cat_journal_path(p, sizeof p)) return 0;
    int n = snprintf(buf, sizeof buf, "phase %d\n%s\n%s\n", phase, cur, target);
    // Swapped in whole: the phase-2 rewrite happens with every save parked, and a torn journal
    // there is a record the recovery would drop -- with the saves still parked.
    return n < (int)sizeof buf && ff132_write_whole(p, buf, (size_t)n);
}
static void cat_journal_drop(void) {
    char p[MAX_PATH];
    if (cat_journal_path(p, sizeof p)) DeleteFileA(p);
}

// A switch cut short still names files it has to put back, and its journal names the folders they
// are mid-move between; a renumber cut short leaves saves under a temp name that only its own
// journal can land. Anything that deletes a save or takes one out of the save folder has to stand
// down until recovery has run, so all of them ask here -- one question, both journals.
int ff132_recovery_owns_saves(void) {
    char p[MAX_PATH];
    if (cat_journal_path(p, sizeof p) && file_exists(p)) return 1;
    // A lend not fully returned: the root still holds the lent category's saves.
    if (data_file(p, sizeof p, FF132_LENT_NAME) && file_exists(p)) return 1;
    return ff132_tidy_renum_pending();
}

// ---- the list -----------------------------------------------------------------------------------
static int cat_name_cmp(const void* A, const void* B) { return name_cmp((const char*)A, (const char*)B); }

static int list_add_at(const char* dir, const char* name, void* vn) {
    int* n = (int*)vn;
    (void)dir;
    if (strlen(name) >= FF132_CATNAME_BYTES || name_eq(name, g_catCur)) return 0;
    if (*n >= FF132_CAT_MAX) return 1;
    snprintf(g_catList[*n], FF132_CATNAME_BYTES, "%s", name);
    (*n)++;
    return 0;
}

// ---- the category the unmodded game is shown ---------------------------------------------------
// One marker file, kept with that category's files (cat_file_pair moves it in a switch), so the
// launcher can find it by a fixed name whatever the category is called.
static int nomod_path(int at, char* p, size_t n) {
    if (at == -1) return data_file(p, n, FF132_NOMOD_NAME);
    char dir[MAX_PATH];
    if (at < 0 || at >= g_catListN || !cat_dir_of(dir, sizeof dir, g_catList[at])) return 0;
    return snprintf(p, n, "%s\\" FF132_NOMOD_NAME, dir) < (int)n;
}

static void nomod_scan(void) {
    int at = -2;
    char p[MAX_PATH];
    for (int i = -1; i < g_catListN; i++) {
        if (!nomod_path(i, p, sizeof p) || !file_exists(p)) continue;
        if (at == -2) { at = i; continue; }
        // The launcher lends the first one it finds; a single marker is the only reading the two
        // are sure to agree on.
        DeleteFileA(p);
        ff132_loga(FF132_TAG "no-mod category: a second marker removed (%s)\n", p);
    }
    g_nomodAt = at;
}

int ff132_nomod_at(void) { return g_nomodAt; }

static void nomod_quoted(char* out, size_t n) {
    int at = g_nomodAt;
    if (at == -1) snprintf(out, n, "'%s'", g_catCur);
    else if (at >= 0 && at < g_catListN) snprintf(out, n, "'%s'", g_catList[at]);
    else snprintf(out, n, "(none)");
}

void ff132_nomod_label(int at, char* utf8, size_t n) {
    if (!utf8 || !n) return;
    if (at == -1) copy_u8(utf8, n, g_catCurU8, sizeof g_catCurU8);
    else if (at >= 0 && at < g_catListN) copy_u8(utf8, n, g_catListU8[at], sizeof g_catListU8[at]);
    else snprintf(utf8, n, "%s", ui_text(T_SET_NOMOD_NONE));
}

int ff132_nomod_set(int at) {
    if (at < -2 || at >= g_catListN) return 0;
    if (steam_cloud_on() == 1) {
        ff132_log(FF132_TAG "no-mod category: refused -- Steam Cloud is on\n");
        return 0;
    }
    if (ff132_recovery_owns_saves()) {
        ff132_loga(FF132_TAG "no-mod category: refused -- an unfinished switch owns these files\n");
        return 0;
    }
    static const char kText[] =
        "ff13-2-run.bat shows the unmodded game the saves of the category this file is with.\r\n"
        "Picked on FF13-2-SpeedrunPractice's settings screen.\r\n";
    char p[MAX_PATH] = "", q[MAX_PATH], nm[FF132_CATNAME_BYTES + 2];
    int was = g_nomodAt, ok = 1;
    // New marker first: a failure on either side leaves the old pick standing, never none.
    if (at != -2 && (!nomod_path(at, p, sizeof p) || !ff132_write_whole(p, kText, sizeof kText - 1))) {
        ff132_loga(FF132_TAG "no-mod category: cannot write %s (err %lu)\n", p,
               (unsigned long)GetLastError());
        ok = 0;
    } else if (was != -2 && was != at && nomod_path(was, q, sizeof q) && !DeleteFileA(q)
               && GetLastError() != ERROR_FILE_NOT_FOUND) {
        ff132_loga(FF132_TAG "no-mod category: cannot remove %s (err %lu)\n", q,
               (unsigned long)GetLastError());
        if (at != -2) DeleteFileA(p);
        ok = 0;
    }
    nomod_scan();
    nomod_quoted(nm, sizeof nm);
    ff132_log(FF132_TAG "no-mod category: %s\n", nm);
    return ok;
}

void ff132_cats_refresh(void) {
    char root[MAX_PATH];
    int n = 0, err = 0;
    g_catListN = 0;
    if (save_root_dir(root, sizeof root)) parked_walk(root, list_add_at, &n, &err);
    qsort(g_catList, (size_t)n, FF132_CATNAME_BYTES, cat_name_cmp);
    for (int i = 0; i < n; i++) {
        char dir[MAX_PATH];
        name_to_u8(g_catListU8[i], sizeof g_catListU8[i], g_catList[i]);
        g_catListSaves[i] = cat_dir_of(dir, sizeof dir, g_catList[i]) ? cat_saves_in(dir) : 0;
    }
    g_catListN = n;
    int sel = n ? 0 : -1;
    if (g_catPrev[0])
        for (int i = 0; i < n; i++)
            if (name_eq(g_catList[i], g_catPrev)) { sel = i; break; }
    g_catSel = sel;
    g_swSel = -1;
    g_delArm = -1;
    nomod_scan();
}

int ff132_cats_count(void) { return g_catListN; }

static int cat_label(int i, char* utf8, size_t n, ff132_msg fmt) {
    if (utf8 && n) utf8[0] = 0;
    if (i < 0 || i >= g_catListN) return 0;
    if (utf8 && n) {
        char name[FF132_CATNAME_U8];
        copy_u8(name, sizeof name, g_catListU8[i], sizeof g_catListU8[i]);
        snprintf(utf8, n, ui_text(fmt), name, g_catListSaves[i]);
    }
    return 1;
}
int ff132_cats_name(int i, char* utf8, size_t n)   { return cat_label(i, utf8, n, T_CAT_COUNT); }
int ff132_cats_name_q(int i, char* utf8, size_t n) { return cat_label(i, utf8, n, T_CAT_COUNT_Q); }
void ff132_cats_current(char* utf8, size_t n) { copy_u8(utf8, n, g_catCurU8, sizeof g_catCurU8); }

int ff132_cats_pick(char* utf8, size_t n) { return ff132_cats_name_q(g_catSel, utf8, n); }

// Worker thread only: the folder a send would write into and the name the log calls it by. Not
// copy_u8's bounded read -- nothing rewrites this list while the worker is inside a send.
int ff132_cats_dest(int i, char* dir, size_t dirN, char* name932, size_t nameN) {
    if (dir && dirN) dir[0] = 0;
    if (name932 && nameN) name932[0] = 0;
    if (i < 0 || i >= g_catListN) return 0;
    if (name932 && nameN) snprintf(name932, nameN, "%s", g_catList[i]);
    return !dir || !dirN || cat_dir_of(dir, dirN, g_catList[i]);
}

int ff132_cats_cycle(int dir) {
    int n = g_catListN;
    if (n <= 0) { g_catSel = -1; return -1; }
    int at = g_catSel;
    if (at < 0 || at >= n) at = 0;
    else at = (at + (dir > 0 ? 1 : n - 1)) % n;
    g_catSel = at;
    return at;
}

// The switch row walks "none" and then every parked category, starting from the previous one.
int ff132_cats_switch_pick(char* utf8, size_t n) { return ff132_cats_name(g_swSel, utf8, n); }

void ff132_cats_switch_cycle(int dir) {
    int n = g_catListN;
    if (n <= 0) { g_swSel = -1; return; }
    int base = 0;
    if (g_catPrev[0])
        for (int i = 0; i < n; i++)
            if (name_eq(g_catList[i], g_catPrev)) { base = i; break; }
    int at = g_swSel;
    int p = (at < 0 || at >= n) ? 0 : (at - base + n) % n + 1;
    p = (p + (dir > 0 ? 1 : n)) % (n + 1);
    g_swSel = p ? (base + p - 1) % n : -1;
}


// The name the filesystem knows the picked category by, for the work below.
static const char* cat_pick_ansi(void) {
    int i = g_catSel;
    return (i >= 0 && i < g_catListN) ? g_catList[i] : NULL;
}

// ---- switching ----------------------------------------------------------------------------------
static int g_switchHigh = -1;

int ff132_cat_switch_high(void) { return g_switchHigh; }

static int cat_switch_to(const char* target, int* outN, int* inN) {
    if (outN) *outN = 0;
    if (inN)  *inN  = 0;
    g_switchHigh = -1;
    if (!target || !target[0] || name_eq(target, g_catCur)) return 0;
    int st = ff132_main_state();
    if (st != FF132_MS_TITLE) {
        // The save list keeps rows for the files it read when it opened, and moving those out from
        // under it leaves the game offering saves that are gone.
        ff132_log(FF132_TAG "category: refused -- main state %d, not the title screen\n", st);
        return 0;
    }
    // A leftover switch journal refuses it too, not only the renumber's: a switch started on top
    // of one overwrites it and loses the record of where the first one was.
    if (ff132_recovery_owns_saves()) {
        ff132_loga(FF132_TAG "category: refused -- an unfinished switch owns these files\n");
        return 0;
    }
    char cats[MAX_PATH], curDir[MAX_PATH], tgtDir[MAX_PATH];
    if (!cats_root(cats, sizeof cats) || !cat_dir_of(curDir, sizeof curDir, g_catCur)
        || !cat_dir_of(tgtDir, sizeof tgtDir, target)) return 0;
    {
        // Brought into the save folder, a number the count cannot reach makes the game abort as
        // soon as a list opens.
        int reach = ff132_slots_reach();
        int hi = dir_numbers(tgtDir, 0, NULL, NULL);
        if (hi >= reach) {
            g_switchHigh = hi;
            ff132_loga(FF132_TAG "category: refused -- '%s' holds ff13_2-%02d.dat (save %d), past the %d"
                          " the game can list\n", target, hi, hi + 1, reach);
            return 0;
        }
    }
    CreateDirectoryA(cats, NULL);
    if (!CreateDirectoryA(curDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        ff132_loga(FF132_TAG "category: cannot make %s (err %lu)\n", curDir,
               (unsigned long)GetLastError());
        return 0;
    }
    char prev[FF132_CATNAME_BYTES];
    snprintf(prev, sizeof prev, "%s", g_catCur);
    if (!cat_journal_write(1, prev, target)) {
        ff132_loga(FF132_TAG "category: cannot write the journal -- not starting\n");
        return 0;
    }
    int out = cat_move_all(curDir, 1);
    // A file the park could not lift would leave two categories sharing the save folder once the
    // target moves in: bring the parked ones back and stand down instead.
    int stuck = cat_files_at(curDir, 0, 0);
    if (stuck) {
        ff132_loga(FF132_TAG "category: %d file(s) would not park -- switching back\n", stuck);
        cat_move_all(curDir, 0);
        // Only a COMPLETE way back may drop the journal: with files still parked, it is the one
        // record recovery can act on.
        if (cat_files_at(curDir, 1, 0))
            ff132_loga(FF132_TAG "category: the way back is short too -- left for the next start\n");
        else
            cat_journal_drop();
        return 0;
    }
    if (!cat_journal_write(2, prev, target)) {
        ff132_loga(FF132_TAG "category: the journal would not advance -- putting '%s' back\n", prev);
        cat_move_all(curDir, 0);
        if (cat_files_at(curDir, 1, 0))
            ff132_loga(FF132_TAG "category: the way back is short too -- left for the next start\n");
        else
            cat_journal_drop();
        return 0;
    }
    int in = cat_move_all(tgtDir, 0);
    RemoveDirectoryA(tgtDir);
    snprintf(g_catPrev, sizeof g_catPrev, "%s", prev);
    name_set_cur(target);
    name_save();
    cat_journal_drop();
    // The records the direct load answers from are the ones in the sidecar that just arrived.
    ff132_load_sidecar_reload();
    save_work_recheck();
    ff132_tidy_rescan();
    ff132_slots_mark_dirty();
    ff132_log(FF132_TAG "category: '%s' -> '%s' (%d out, %d in)\n", prev, g_catCur, out, in);
    ff132_cats_refresh();
    if (outN) *outN = out;
    if (inN)  *inN  = in;
    return 1;
}

int ff132_cat_switch(int* outN, int* inN) {
    int i = g_swSel;
    int r = cat_switch_to((i >= 0 && i < g_catListN) ? g_catList[i] : NULL, outN, inN);
    g_swSel = -1;
    return r;
}

// A leftover journal means the moves were cut short: finish them before anything reads a save
// folder that is half one category and half another. Re-running a phase is safe. 1 = files moved.
static int cat_switch_recover(void) {
    char jp[MAX_PATH];
    if (!cat_journal_path(jp, sizeof jp)) return 0;
    FILE* f = fopen(jp, "r");
    if (!f) return 0;
    char head[64] = {0}, cur[FF132_CATNAME_BYTES + 2] = {0}, tgt[FF132_CATNAME_BYTES + 2] = {0};
    int phase = 0;
    int ok = fgets(head, sizeof head, f) && sscanf(head, "phase %d", &phase) == 1
             && fgets(cur, sizeof cur, f) && fgets(tgt, sizeof tgt, f);
    fclose(f);
    name_trim(cur);
    name_trim(tgt);
    if (!ok || (phase != 1 && phase != 2) || !cur[0] || !tgt[0]) {
        // An unreadable journal over an empty save folder means the park happened and the record
        // died with it: the active category's own files come back before the journal goes.
        int moved = 0;
        char backDir[MAX_PATH];
        if (cat_dir_of(backDir, sizeof backDir, g_catCur)
            && cat_files_at(backDir, 0, 1) == 0 && cat_files_at(backDir, 1, 1) > 0) {
            int back = cat_move_all(backDir, 0);
            RemoveDirectoryA(backDir);
            moved = back > 0;
            ff132_loga(FF132_TAG "category: '%s' was parked with no readable journal -- %d file(s)"
                          " brought back\n", g_catCur, back);
        }
        DeleteFileA(jp);
        ff132_loga(FF132_TAG "category: unreadable journal dropped\n");
        return moved;
    }
    char cats[MAX_PATH], curDir[MAX_PATH], tgtDir[MAX_PATH];
    if (!cats_root(cats, sizeof cats) || !cat_dir_of(curDir, sizeof curDir, cur)
        || !cat_dir_of(tgtDir, sizeof tgtDir, tgt)) return 0;
    ff132_loga(FF132_TAG "category: finishing an interrupted switch '%s' -> '%s' (phase %d)\n",
           cur, tgt, phase);
    if (phase == 1) {
        CreateDirectoryA(cats, NULL);
        if (!CreateDirectoryA(curDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
            ff132_loga(FF132_TAG "category: cannot make %s (err %lu) -- left unfinished\n", curDir,
                   (unsigned long)GetLastError());
            return 0;
        }
        cat_move_all(curDir, 1);
        if (cat_files_at(curDir, 0, 0)) {
            ff132_loga(FF132_TAG "category: the park is still short -- left unfinished\n");
            return 0;
        }
        if (!cat_journal_write(2, cur, tgt)) {
            ff132_loga(FF132_TAG "category: the journal would not advance -- left unfinished\n");
            return 0;
        }
    }
    cat_move_all(tgtDir, 0);
    RemoveDirectoryA(tgtDir);
    snprintf(g_catPrev, sizeof g_catPrev, "%s", cur);
    name_set_cur(tgt);
    name_save();
    cat_journal_drop();
    return 1;
}

// ---- seeding a new category ---------------------------------------------------------------------
// The sidecar is one line per save with the slot number first, so the same edit fits it: drop the
// destination slot's line and append the one being carried over.
static int send_sidecar_graft(const char* dir, int dstSlot, const char* row) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\%s", dir, FF132_SLOTS_NAME);
    FILE* in = fopen(path, "r");
    // A file that exists but will not open must abort: rewriting from nothing would shrink the
    // category's sidecar to this one row.
    if (!in && file_exists(path)) {
        ff132_loga(FF132_TAG "send: cannot read %s -- left as it was\n", path);
        return 0;
    }
    const size_t cap = 256 * 1024;
    char* buf = (char*)malloc(cap);
    size_t n = 0;
    int fits = buf != NULL;
    if (in) {
        char line[192];
        while (fits && fgets(line, sizeof line, in)) {
            int s;
            if (sscanf(line, "%d", &s) == 1 && s == dstSlot) continue;
            size_t l = strlen(line);
            if (n + l >= cap) fits = 0; else { memcpy(buf + n, line, l); n += l; }
        }
        if (ferror(in)) fits = 0;
        fclose(in);
    }
    if (fits && row && row[0]) {
        size_t l = strlen(row);
        if (n + l >= cap) fits = 0; else { memcpy(buf + n, row, l); n += l; }
    }
    // Written whole over the original, never after deleting it: a failed rewrite leaves the
    // category's sidecar standing.
    int ok = fits && ff132_write_whole(path, buf, n);
    if (!ok)
        ff132_loga(FF132_TAG "send: cannot put %s in place (err %lu) -- left as it was\n", path,
               (unsigned long)GetLastError());
    free(buf);
    return ok;
}

int ff132_send_one(int srcSlot, const char* dstDir, int dstSlot, int* recOk) {
    char sp[MAX_PATH], dp[MAX_PATH], row[192];
    if (recOk) *recOk = 1;
    if (srcSlot < 0 || !dstDir || !dstDir[0] || dstSlot < 0
        || !save_slot_path(srcSlot, sp, sizeof sp) || !file_exists(sp)) {
        ff132_loga(FF132_TAG "send: save %d cannot be sent to '%s'\n", srcSlot + 1,
               dstDir ? dstDir : "");
        return 0;
    }
    snprintf(dp, sizeof dp, "%s\\ff13_2-%02d.dat", dstDir, dstSlot);
    gate_keep_alive();
    if (!CopyFileA(sp, dp, TRUE)) {
        ff132_loga(FF132_TAG "send: %s -> %s failed (err %lu)\n", sp, dp,
               (unsigned long)GetLastError());
        return 0;
    }
    // CopyFileA carries the source's write time over, which is half of the identity the direct-load
    // record is matched by -- so the row below still describes the file it was grafted onto.
    row[0] = 0;
    if (ff132_load_rec_row(srcSlot, dstSlot, row, sizeof row)
        && !send_sidecar_graft(dstDir, dstSlot, row)) {
        ff132_loga(FF132_TAG "send: save %d's record did not travel -- the copy loads the vanilla way\n",
               srcSlot + 1);
        // Only a REFUSED graft clears it: a source with no record has nothing to carry, and a move
        // of such a save loses nothing.
        if (recOk) *recOk = 0;
    }
    ff132_log(FF132_TAG "send: save %d -> save %d in %s (copy)\n", srcSlot + 1, dstSlot + 1, dstDir);
    return 1;
}

// The most recently written save. This mod never re-stamps a save file, so the raw write time is
// the real one.
static int newest_active_slot(void) {
    int slots = slot_count(), best = -1;
    ULONGLONG bestT = 0;
    for (int s = 0; s < slots; s++) {
        char p[MAX_PATH];
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!save_slot_path(s, p, sizeof p)) return -1;
        if (!GetFileAttributesExA(p, GetFileExInfoStandard, &fa)) continue;
        ULONGLONG t = ft_u64(fa.ftLastWriteTime);
        if (t > bestT) { bestT = t; best = s; }
    }
    return best;
}

// Exactly the file set a switch moves, duplicated rather than moved, so the new category comes out
// a twin -- same save numbers and the same sidecar. -1 = a file would not copy.
static int cat_copy_all(const char* dir) {
    int slots = CAT_SLOTS_MAX, n = 0;
    DWORD t0 = GetTickCount();
    for (int i = 0; i < CAT_FILES(slots); i++) {
        char a[MAX_PATH], b[MAX_PATH];
        if (i == CAT_MARKER(slots)) continue;    // the marker names one category, not its twin
        if (!cat_file_pair(i, slots, dir, a, sizeof a, b, sizeof b) || !file_exists(a)) continue;
        gate_keep_alive();
        if (CopyFileA(a, b, TRUE)) n++;
        else {
            ff132_loga(FF132_TAG "category: copy %s -> %s failed (err %lu)\n", a, b,
                   (unsigned long)GetLastError());
            return -1;
        }
    }
    ff132_log(FF132_TAG "category: %d file(s) copied in %lu ms\n", n,
          (unsigned long)(GetTickCount() - t0));
    return n;
}

static void seed_remove(const char* dir) {
    char p[MAX_PATH];
    for (int s = 0; s < CAT_SLOTS_MAX; s++) {
        snprintf(p, sizeof p, "%s\\ff13_2-%02d.dat", dir, s);
        DeleteFileA(p);
    }
    snprintf(p, sizeof p, "%s\\%s", dir, FF132_SLOTS_NAME);
    DeleteFileA(p);
}

int ff132_cat_new(const char* name932, int all) {
    char cats[MAX_PATH], dir[MAX_PATH];
    if (!name932 || !name932[0]) return 0;
    if (ff132_recovery_owns_saves()) {
        ff132_loga(FF132_TAG "category: refused -- an unfinished switch owns these files\n");
        return 0;
    }
    // Asked before the seed rather than left to the switch: a switch refused after the copy would
    // have to undo it.
    int st = ff132_main_state();
    if (st != FF132_MS_TITLE) {
        ff132_log(FF132_TAG "category: refused -- main state %d, not the title screen\n", st);
        return 0;
    }
    if (!cats_root(cats, sizeof cats) || !cat_dir_of(dir, sizeof dir, name932)) return 0;
    CreateDirectoryA(cats, NULL);        // the parent: this may be the very first category
    if (!CreateDirectoryA(dir, NULL)) {
        ff132_loga(FF132_TAG "category: cannot make %s (err %lu)\n", dir,
               (unsigned long)GetLastError());
        return 0;
    }
    // Seeded BEFORE the switch, so the switch carries it in like any parked file: a category with
    // no save in it takes the game's "continue" entry off the title menu until the title is
    // re-entered.
    if (all) {
        if (cat_copy_all(dir) < 0) {
            seed_remove(dir);
            RemoveDirectoryA(dir);
            return 0;
        }
    } else {
        int seed = newest_active_slot();
        int dst = ff132_cat_free_slot(dir);
        if (seed >= 0 && dst >= 0) ff132_send_one(seed, dir, dst, NULL);
        else ff132_loga(FF132_TAG "category: '%s' starts empty -- no save to seed it with\n", name932);
    }
    if (!cat_switch_to(name932, NULL, NULL)) {
        seed_remove(dir);
        RemoveDirectoryA(dir);
        return 0;
    }
    ff132_log(FF132_TAG "category: '%s' made and switched to (%s)\n", name932,
          all ? "every save copied" : "the newest save copied");
    return 1;
}

// An empty category folder and nothing else -- no seed, no switch. The seeding ff132_cat_new does
// exists so a category SWITCHED INTO is never empty; a send makes this one non-empty itself.
int ff132_cat_make(const char* name932, char* dir, size_t n) {
    char cats[MAX_PATH];
    if (dir && n) dir[0] = 0;
    if (!name932 || !name932[0] || !dir || !n) return 0;
    if (!cats_root(cats, sizeof cats) || !cat_dir_of(dir, n, name932)) return 0;
    CreateDirectoryA(cats, NULL);        // the parent: this may be the very first category
    if (!CreateDirectoryA(dir, NULL)) {
        ff132_loga(FF132_TAG "category: cannot make %s (err %lu)\n", dir,
               (unsigned long)GetLastError());
        dir[0] = 0;
        return 0;
    }
    ff132_log(FF132_TAG "category: '%s' made (empty, not switched to)\n", name932);
    return 1;
}

int ff132_cat_rename(const char* name932) {
    if (!name932 || !name932[0]) return 0;
    // The category in use has no folder of its own, so its name lives entirely in that file.
    ff132_log(FF132_TAG "category: '%s' renamed to '%s'\n", g_catCur, name932);
    name_set_cur(name932);
    name_save();
    ff132_cats_refresh();
    return 1;
}

// The comma is refused on top of the filesystem's own set: the sidecars this mod writes are
// comma-separated in places, and the editor asks here so a refused character never reaches a name.
int ff132_cat_name_char_ok(wchar_t c) {
    static const wchar_t kBad[] = L"\\/:*?\"<>|,";
    if (c < L' ' || c == 0x7F) return 0;
    for (int i = 0; kBad[i]; i++) if (c == kBad[i]) return 0;
    return 1;
}

// 0 = usable, 1 = the name itself is refused, 2 = a category of that name already exists.

int ff132_cat_check_name(const wchar_t* w, int rename, char* out932, size_t n) {
    if (!w || !out932 || !n) return 1;
    out932[0] = 0;
    int m = 0;
    while (w[m]) m++;
    if (!m || m > FF132_CATNAME_MAX) return 1;
    for (int i = 0; i < m; i++) if (!ff132_cat_name_char_ok(w[i])) return 1;
    // Every path below is char and the game's own folder names are cp932, so a name cp932 cannot
    // spell is a name this cannot use.
    BOOL usedDefault = FALSE;
    if (WideCharToMultiByte(932, WC_NO_BEST_FIT_CHARS, w, -1, out932, (int)n, NULL,
                            &usedDefault) <= 0 || usedDefault) {
        out932[0] = 0;
        return 1;
    }
    size_t L = strlen(out932);
    if (!L || out932[L - 1] == '.' || out932[L - 1] == ' ') return 1;
    if (!strcmp(out932, ".") || !strcmp(out932, "..")) return 1;
    // The DOS device names: a directory called any of these cannot be reached by the paths built
    // here, with or without an extension.
    {
        static const char* const kDev[] = { "CON", "PRN", "AUX", "NUL",
            "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
        char stem[FF132_CATNAME_BYTES];
        snprintf(stem, sizeof stem, "%s", out932);
        char* dot = strchr(stem, '.');
        if (dot) *dot = 0;
        for (int i = 0; i < (int)(sizeof kDev / sizeof kDev[0]); i++)
            if (cfg_ieq(stem, kDev[i])) return 1;
    }
    if (!rename && name_eq(out932, g_catCur)) return 2;
    char dir[MAX_PATH];
    if (!cat_dir_of(dir, sizeof dir, out932)) return 1;
    if (GetFileAttributesA(dir) != INVALID_FILE_ATTRIBUTES) return 2;
    return 0;
}

// ---- deleting a parked category -------------------------------------------------------------------
// To the Recycle Bin rather than unlinked: a category can hold forty real saves. A drive with no
// bin (a network or removable one) makes the shell delete outright -- the README says so.
int ff132_cat_del_armed(void) { return g_delArm >= 0; }

void ff132_cat_del_poll(int rowActive) {
    if (g_delArm < 0) return;
    // The ways an arm goes away, in one place: a row moved off, a different category picked, or the
    // menu closed. Spread over the callers, the one that is forgotten is the one that deletes
    // something nobody asked for.
    if (!rowActive || g_delArm != g_catSel) g_delArm = -1;
}

// `list` is SHFileOperation's own form: absolute paths, each NUL-terminated, the last one twice.
// The shell wants a COM-initialised thread for the bin's namespace work (the worker has none).
int ff132_shell_recycle(const wchar_t* list, const char* what) {
    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof op);
    op.wFunc  = FO_DELETE;
    op.pFrom  = list;
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int r = SHFileOperationW(&op);
    if (SUCCEEDED(hr)) CoUninitialize();     // S_FALSE counts as an init and has to be balanced
    if (r || op.fAnyOperationsAborted) {
        ff132_loga(FF132_TAG "'%s' would not go to the bin (rc %d, aborted %d)\n", what, r,
               (int)op.fAnyOperationsAborted);
        return 0;
    }
    return 1;
}

// 0 = the first ask (the row now reads as the confirm), 1 = deleted, -1 = refused or failed.
int ff132_cat_delete_hold(void) {
    int sel = g_catSel;
    const char* pick = cat_pick_ansi();
    char dir[MAX_PATH];
    if (!pick) { g_delArm = -1; return -1; }
    if (ff132_recovery_owns_saves()) {
        ff132_loga(FF132_TAG "category: delete refused -- an unfinished switch owns these files\n");
        g_delArm = -1;
        return -1;
    }
    if (!cat_dir_of(dir, sizeof dir, pick)) { g_delArm = -1; return -1; }
    int saves = cat_saves_in(dir);
    if (saves > 0 && g_delArm != sel) {
        g_delArm = sel;
        ff132_log(FF132_TAG "category: '%s' holds %d save(s) -- confirm again to delete\n", pick, saves);
        return 0;
    }
    char name[FF132_CATNAME_BYTES];
    snprintf(name, sizeof name, "%s", pick);
    g_delArm = -1;
    {
        wchar_t w[MAX_PATH + 2];
        // The folder was made through the ANSI calls, which read the bytes in the ANSI code page.
        int n = MultiByteToWideChar(CP_ACP, 0, dir, -1, w, MAX_PATH);
        if (n <= 0) return -1;
        w[n] = 0;                                   // the double NUL SHFileOperation wants
        if (!ff132_shell_recycle(w, dir)) return -1;
    }
    ff132_log(FF132_TAG "category: deleted '%s' (%d save(s)) -- to the recycle bin\n", name, saves);
    ff132_cats_refresh();
    return 1;
}

// ---- saves parked for the unmodded game -------------------------------------------------------
// ff13-2-run.bat renames saves in the save folder root to *.dat.parked: 40..98 always (the unmodded
// game closes when its list finds one), all of 00..99 when it lends a category. 99 stays parked:
// no slot count reaches it, and any list that met it would close the game. This undoes it on the
// next start with the mod, through the practice .bat or not. A same-folder rename keeps the write
// time the sidecars key a save by. Never onto an existing file.
#define UNPARK_SUFFIX ".dat.parked"
static int parked_number(const char* f) {
    if (strlen(f) != 9 + sizeof UNPARK_SUFFIX - 1 || _strnicmp(f, "ff13_2-", 7)
        || f[7] < '0' || f[7] > '9' || f[8] < '0' || f[8] > '9' || _stricmp(f + 9, UNPARK_SUFFIX))
        return -1;
    int n = (f[7] - '0') * 10 + (f[8] - '0');
    return n < FF132_SLOTS_MAX ? n : -1;
}

static int nomod_dir_at(const char* dir, const char* name, void* vout) {
    char p[MAX_PATH];
    (void)name;
    if (snprintf(p, sizeof p, "%s\\" FF132_NOMOD_NAME, dir) >= (int)sizeof p || !file_exists(p)) return 0;
    snprintf((char*)vout, MAX_PATH, "%s", dir);
    return 1;
}

// The launcher writes the lent mark only once every save of the category in use is parked, so
// whatever .dat the root holds while it stands came from the lent category or from the unmodded
// game. The mark goes before the unpark: left standing, the next start would take the saves this
// one brings back for the lent category's.
// Anything short of a full return keeps the mark and answers 0, and the caller then leaves the
// parked saves too: with the mark gone, or with them back, the root would mix the two categories.
int ff132_nomod_return(void) {
    char lent[MAX_PATH], root[MAX_PATH], dir[MAX_PATH] = "";
    if (!data_file(lent, sizeof lent, FF132_LENT_NAME) || !file_exists(lent)) return 1;
    if (!save_root_dir(root, sizeof root)) {
        ff132_loga(FF132_TAG "no-mod saves: the save folder could not be resolved -- left as they are\n");
        return 0;
    }
    int err = 0;
    if (parked_walk(root, nomod_dir_at, dir, &err) < 0 || (!dir[0] && err)) {
        ff132_loga(FF132_TAG "no-mod saves: the categories could not all be listed -- left as they are\n");
        return 0;
    }
    if (!dir[0]) {
        // A fresh folder: one the player made under this name would refuse every number it holds.
        // Nor the one in use, which has no folder there until it is switched away.
        char cats[MAX_PATH], name[16];
        if (!cats_root(cats, sizeof cats)) return 0;
        name_load();
        CreateDirectoryA(cats, NULL);
        for (int k = 1; k < 100 && !dir[0]; k++) {
            snprintf(name, sizeof name, k == 1 ? "nomod" : "nomod%d", k);
            if (name_eq(name, g_catCur)) continue;
            if (cat_dir_of(dir, sizeof dir, name) && !CreateDirectoryA(dir, NULL)) dir[0] = 0;
        }
        if (!dir[0]) {
            ff132_loga(FF132_TAG "no-mod saves: no folder to return them to -- left as they are\n");
            return 0;
        }
        ff132_loga(FF132_TAG "no-mod saves: no category carries the marker -- they go to %s\n", dir);
    }
    int moved = 0, failed = 0;
    for (int n = 0; n < CAT_SLOTS_MAX; n++) {
        char src[MAX_PATH], dst[MAX_PATH];
        snprintf(src, sizeof src, "%s\\ff13_2-%02d.dat", root, n);
        if (!file_exists(src)) continue;
        snprintf(dst, sizeof dst, "%s\\ff13_2-%02d.dat", dir, n);
        if (MoveFileExA(src, dst, MOVEFILE_COPY_ALLOWED)) { moved++; continue; }
        ff132_loga(FF132_TAG "no-mod saves: %s -> %s failed (err %lu) -- left in the save folder\n",
               src, dst, (unsigned long)GetLastError());
        failed++;
    }
    if (failed) {
        ff132_loga(FF132_TAG "no-mod saves: %d returned to %s, %d not -- the saves in use stay "
                   "hidden until every one goes back\n", moved, dir, failed);
        return 0;
    }
    DeleteFileA(lent);
    ff132_loga(FF132_TAG "no-mod saves: %d returned to %s\n", moved, dir);
    return 1;
}

int ff132_nomod_stuck(void) {
    char p[MAX_PATH];
    return data_file(p, sizeof p, FF132_LENT_NAME) && file_exists(p);
}

void ff132_saves_unpark(void) {
    char root[MAX_PATH], glob[MAX_PATH];
    if (!save_root_dir(root, sizeof root)) {
        ff132_loga(FF132_TAG "parked saves: the save folder could not be resolved -- left as they are\n");
        return;
    }
    snprintf(glob, sizeof glob, "%s\\ff13_2-*" UNPARK_SUFFIX, root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    // Collected first, renamed after: renaming inside the enumeration can make it skip or repeat.
    char done[FF132_SLOTS_MAX] = {0};
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        int n = parked_number(fd.cFileName);
        if (n >= 0) done[n] = 1;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    int restored = 0, failed = 0, lo = -1, hi = -1;
    for (int n = 0; n < FF132_SLOTS_MAX; n++) {
        if (!done[n]) continue;
        char src[MAX_PATH], dst[MAX_PATH];
        snprintf(dst, sizeof dst, "%s\\ff13_2-%02d.dat", root, n);
        snprintf(src, sizeof src, "%s\\ff13_2-%02d" UNPARK_SUFFIX, root, n);
        if (file_exists(dst)) {
            ff132_loga(FF132_TAG "parked saves: WARNING %s and %s both exist -- both left alone\n",
                   dst, src);
            failed++;
            continue;
        }
        if (!MoveFileA(src, dst)) {
            ff132_loga(FF132_TAG "parked saves: %s -> %s failed (err %lu)\n", src, dst,
                   (unsigned long)GetLastError());
            failed++;
            continue;
        }
        if (lo < 0) lo = n;
        hi = n;
        restored++;
    }
    if (restored)
        ff132_loga(FF132_TAG "parked saves: %d restored (ff13_2-%02d..ff13_2-%02d)%s\n", restored, lo, hi,
               failed ? ", some left parked (see above)" : "");
}

// ---- install ---------------------------------------------------------------------------------------
void ff132_cats_install(void) {
    name_load();
    // Before anything reads a save: a switch cut short by a crash leaves the folder holding half
    // of one category and half of another, and the header scan behind this would cache that.
    if (cat_switch_recover()) ff132_load_sidecar_reload();
    ff132_cats_refresh();
    // The launcher cannot ask Steam, and a lend renames saves between sessions, behind Cloud's back.
    if (g_nomodAt != -2 && steam_cloud_on() == 1) {
        char p[MAX_PATH];
        if (nomod_path(g_nomodAt, p, sizeof p)) DeleteFileA(p);
        ff132_loga(FF132_TAG "no-mod category: Steam Cloud is on -- the pick is dropped\n");
        nomod_scan();
    }
    char pv[FF132_CATNAME_BYTES + 2] = "(none)", st[FF132_CATNAME_BYTES + 2] = "(none)";
    int sel = g_catSel;
    if (g_catPrev[0]) snprintf(pv, sizeof pv, "'%s'", g_catPrev);
    if (sel >= 0 && sel < g_catListN) snprintf(st, sizeof st, "'%s'", g_catList[sel]);
    char nm[FF132_CATNAME_BYTES + 2];
    nomod_quoted(nm, sizeof nm);
    ff132_loga(FF132_TAG "category: in use '%s', %d other(s) parked, previous %s,"
                  " delete row starts on %s, no-mod %s\n", g_catCur, g_catListN, pv, st, nm);
}
