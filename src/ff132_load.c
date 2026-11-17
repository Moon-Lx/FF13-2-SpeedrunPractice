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

// Direct load: a save loaded from the title screen goes straight into its world instead of stopping
// at the Historia Crux. Two one-byte patches on the load-completion task's triggers, described in
// ff132_addrs.h; nothing else about a load is touched.
//
// The patch is decided per load, not held on: only a save this mod watched being written IN A WORLD
// skips the Historia Crux. A save written at the Historia Crux, and any file the mod has no record
// of, loads the vanilla way. The record is the sidecar below, written whenever the game finishes a
// save file and looked up when one is read back.
//
// Beside them are two log-only hooks that name what a load actually did -- the main FSM's state
// changes and the zone jump a script asks for afterwards.
//
// The story recap rides on the same two moments: between a load finishing and the first zone jump
// the script asks for, the VM native Party.getEventFlag answers false to every tm_abst_ name, which
// is how the "previously on" recap is chosen. Nothing in the save is written.
//
// The world label hangs off the same "this file is finished" moment as the sidecar: the world the
// save was made in is appended to the save list's two lines, as plain text inside the file's own
// header. The game rebuilds that header on every write, so the line is put back each time -- and
// since the list's GUI does not display that text but rebuilds its own two lines from it, a third
// hook appends the line to what the GUI built.
#include "ff132_internal.h"
#include "ff132_addrs.h"
#include "ff132_worlds.h"
#include "ff13_hook.h"


static void save_ids(int* zone, char* hist, size_t n);

volatile unsigned char g_directLoad = 1;
static int g_loadOk = 0;                  // both trigger sites matched, so the patch may be applied

volatile unsigned char g_skipRecap = 1;
static int g_recapOk = 0;                 // the getEventFlag hook is in, so the window may open
static int g_recapWindow = 0;             // a load has finished and no zone jump has come yet
static int g_recapLogged = 0;             // one line per window, not one per flag

// ---- the patch ---------------------------------------------------------------------------------
static int poke(uintptr_t at, uint8_t v) {
    DWORD old = 0;
    if (!VirtualProtect((void*)at, 1, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *(volatile uint8_t*)at = v;
    VirtualProtect((void*)at, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void*)at, 1);
    return 1;
}

// Called from the load hook that decides a load and from the state hook that undoes it, both on the
// game thread; the store is one byte, and the only frame in which either site executes is the one a
// load completes in.
static int apply(int on) {
    uint8_t v = on ? FF132_TRG_LOAD_EMPTY : FF132_TRG_LOAD_END;
    int a = poke(g_base + FF132_RVA_LOAD_TRIGGER_A + FF132_OFF_LOAD_TRIGGER_IMM, v);
    int b = poke(g_base + FF132_RVA_LOAD_TRIGGER_B + FF132_OFF_LOAD_TRIGGER_IMM, v);
    return a && b;
}

// ---- the sidecar: which file was written where --------------------------------------------------
// One record per slot, keyed by the file's identity so a file rewritten behind the mod's back (an
// older save, a copy from another machine, a cloud sync) falls back to the vanilla load.
enum { K_NONE = 0, K_FIELD = 1, K_HISTORIA = 2 };
#define FF132_SLOT_MAX 1000

struct slot_rec {
    uint64_t mtime;      // ftLastWriteTime
    uint32_t size;       // nFileSizeLow
    uint8_t  kind;
    char     hist[FF132_SAVE_HISTORY_ID_LEN + 1];   // the world's history id; "" = none was readable
};
static struct slot_rec g_rec[FF132_SLOT_MAX];

static const char* kind_name(int k) {
    return k == K_HISTORIA ? "historia" : k == K_FIELD ? "field" : "no record";
}

// save_ids answers "-" until the save image is up, and the sidecar writes "-" for a record that
// has no id; neither is a history to name a world with.
static const char* real_id(const char* hist) {
    return (hist[0] && strcmp(hist, "-") != 0) ? hist : NULL;
}

static int stat_slot(int slot, uint64_t* mtime, uint32_t* size) {
    char path[MAX_PATH];
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!save_slot_path(slot, path, sizeof path)) return 0;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad)) return 0;
    *mtime = ft_u64(fad.ftLastWriteTime);
    *size  = fad.nFileSizeLow;
    return 1;
}

int ff132_load_slot_stat(int slot, uint64_t* mtime, uint32_t* size) { return stat_slot(slot, mtime, size); }

static int rec_matches(int slot) {
    uint64_t mt;
    uint32_t sz;
    if (!g_rec[slot].kind) return 0;
    if (!stat_slot(slot, &mt, &sz)) return 0;
    return mt == g_rec[slot].mtime && sz == g_rec[slot].size;
}

// Rewritten whole on every change, which is rare (one save = one record) and keeps the file
// readable by hand. %08X twice rather than a 64-bit conversion, so the format stays portable.
// Both the worker and the game thread write it: the lock covers the snapshot and its write, so the
// newest one lands last.
static SRWLOCK g_slotsWriteLock = SRWLOCK_INIT;
static void slots_write(void) {
    char path[MAX_PATH];
    const size_t cap = 96 + FF132_SLOT_MAX * 96;
    char* buf = (char*)malloc(cap);
    if (!buf || !data_file(path, sizeof path, FF132_SLOTS_NAME)) { free(buf); return; }
    AcquireSRWLockExclusive(&g_slotsWriteLock);
    int n = snprintf(buf, cap, "; slot  last-write-time (16 hex digits)  size  field|historia  history id\n");
    for (int i = 0; i < FF132_SLOT_MAX && n < (int)cap; i++) {
        if (!g_rec[i].kind) continue;
        n += snprintf(buf + n, cap - (size_t)n, "%d %08X%08X %u %s %s\n", i,
                      (unsigned)(g_rec[i].mtime >> 32), (unsigned)(g_rec[i].mtime & 0xFFFFFFFFu),
                      (unsigned)g_rec[i].size, kind_name(g_rec[i].kind), g_rec[i].hist[0] ? g_rec[i].hist : "-");
    }
    if (n >= (int)cap || !ff132_write_whole(path, buf, (size_t)n))
        ff132_loga(FF132_TAG "direct load: %s could not be written -- the previous records stay\n",
               FF132_SLOTS_NAME);
    ReleaseSRWLockExclusive(&g_slotsWriteLock);
    free(buf);
}

static int hex64(const char* s, uint64_t* out) {
    uint64_t v = 0;
    for (int i = 0; i < 16; i++) {
        char c = s[i];
        int d = (c >= '0' && c <= '9') ? c - '0'
              : (c >= 'a' && c <= 'f') ? c - 'a' + 10
              : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) return 0;
        v = (v << 4) | (uint64_t)(unsigned)d;
    }
    if (s[16]) return 0;
    *out = v;
    return 1;
}

static void slots_read(void) {
    char path[MAX_PATH];
    if (!data_file(path, sizeof path, FF132_SLOTS_NAME)) return;
    FILE* f = fopen(path, "r");
    if (!f) {
        ff132_loga(FF132_TAG "save records: no %s yet -- every save loads the vanilla way until one is"
                      " written\n", FF132_SLOTS_NAME);
        return;
    }
    char line[160];
    int n = 0, bad = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == ';' || line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char hex[32], kindw[16], histw[32] = {0};
        int slot = -1;
        unsigned size = 0;
        uint64_t mt = 0;
        // A four-field line (no history id column) is still valid; such a line keeps no id.
        int got = sscanf(line, "%d %31s %u %15s %31s", &slot, hex, &size, kindw, histw);
        if (got < 4 || slot < 0 || slot >= FF132_SLOT_MAX || strlen(hex) != 16 || !hex64(hex, &mt)) {
            bad++;
            continue;
        }
        int kind = cfg_ieq(kindw, "historia") ? K_HISTORIA : cfg_ieq(kindw, "field") ? K_FIELD
                                                                                     : K_NONE;
        if (!kind) { bad++; continue; }
        g_rec[slot].mtime = mt;
        g_rec[slot].size  = size;
        g_rec[slot].kind  = (uint8_t)kind;
        snprintf(g_rec[slot].hist, sizeof g_rec[slot].hist, "%s",
                 (got >= 5 && real_id(histw)) ? histw : "");
        n++;
    }
    fclose(f);
    ff132_loga(FF132_TAG "save records: %d slot(s) read from %s%s\n", n, FF132_SLOTS_NAME,
           bad ? " (some lines were unreadable and skipped)" : "");
}

// Worker thread. The file the records come from is a different one after a category switch, so
// the table is dropped whole and read again rather than merged: a record for a slot the new
// sidecar says nothing about must not survive.
void ff132_load_sidecar_reload(void) {
    memset(g_rec, 0, sizeof g_rec);
    slots_read();
}

// Worker thread. src's record as the sidecar line for dstSlot -- the identity fields are copied
// across unchanged because CopyFileA carries the source's write time and size onto the copy.
int ff132_load_rec_row(int srcSlot, int dstSlot, char* out, size_t n) {
    if (!out || !n) return 0;
    out[0] = 0;
    if (srcSlot < 0 || srcSlot >= FF132_SLOT_MAX || dstSlot < 0 || dstSlot >= FF132_SLOT_MAX) return 0;
    if (!rec_matches(srcSlot)) return 0;
    snprintf(out, n, "%d %08X%08X %u %s %s\n", dstSlot, (unsigned)(g_rec[srcSlot].mtime >> 32),
             (unsigned)(g_rec[srcSlot].mtime & 0xFFFFFFFFu), (unsigned)g_rec[srcSlot].size,
             kind_name(g_rec[srcSlot].kind), g_rec[srcSlot].hist[0] ? g_rec[srcSlot].hist : "-");
    return 1;
}

// Worker thread. slot's file has left this category -- moved away, or deleted -- so its record
// goes with it: a later save written into that number would otherwise be met by a stale identity.
// The file is rewritten per call rather than once per range, because it is a few hundred bytes and
// a range cut short by a crash then leaves the records agreeing with the files that survived.
void ff132_load_rec_forget(int slot) {
    if (slot < 0 || slot >= FF132_SLOT_MAX || !g_rec[slot].kind) return;
    memset(&g_rec[slot], 0, sizeof g_rec[slot]);
    slots_write();
}

// Worker thread. The renumber renamed the files, so the records go with them. Two sweeps into a
// fresh table, never an in-place cycle walk: the walk needs a true bijection and a run that moves
// only some of the saves is not one (FF13 hung there). The keeps go down first so a record landing
// on a number another slot was keeping wins -- a file is there now.
void ff132_load_rec_permute(const int* newSlotOf, int n) {
    if (!newSlotOf || n <= 0) return;
    if (n > FF132_SLOT_MAX) n = FF132_SLOT_MAX;
    struct slot_rec* fresh = (struct slot_rec*)calloc(FF132_SLOT_MAX, sizeof *fresh);
    if (!fresh) {
        ff132_loga(FF132_TAG "save records: the renumbered saves could not take their records with them"
                      " -- they load the vanilla way until each is saved again\n");
        return;
    }
    for (int s = 0; s < FF132_SLOT_MAX; s++)
        if (s >= n || newSlotOf[s] < 0) fresh[s] = g_rec[s];
    int moved = 0;
    for (int s = 0; s < n; s++) {
        int d = newSlotOf[s];
        if (d < 0 || d >= FF132_SLOT_MAX) continue;
        fresh[d] = g_rec[s];
        if (g_rec[s].kind) moved++;
    }
    memcpy(g_rec, fresh, sizeof g_rec);
    free(fresh);
    slots_write();
    ff132_loga(FF132_TAG "save records: %d record(s) moved with the renumbered saves\n", moved);
}

// ---- the world label: a third line in the save list ---------------------------------------------
// The save header's layout is in ff132_addrs.h. Nothing checksums it, so a third line can be appended
// -- but only after each write, since the game rebuilds the whole block every time it saves.
volatile unsigned char g_keepNote = 0;

// The list is cp932 and every string in this mod is UTF-8.
static int to_cp932(const char* utf8, char* out, size_t n) {
    WCHAR w[FF132_HDR_TEXT];
    if (!MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, (int)(sizeof w / sizeof w[0]))) return 0;
    return WideCharToMultiByte(932, 0, w, -1, out, (int)n, NULL, NULL) > 0;
}

// The area the empty-id saves are filed under (Valhalla). It goes by its name alone, never with an
// age: the user's call, whichever of its ids a save carries.
static int no_id_row(void) {
    static int row = -2;
    if (row == -2) {
        int r = -1;
        for (size_t i = 0; i < sizeof kWorlds / sizeof kWorlds[0]; i++)
            if (strcmp(kWorlds[i].id, FF132_NO_ID_WORLD) == 0) { r = (int)i; break; }
        row = r;
    }
    return row;
}
static const char* row_area(int row, int english) {
    return english ? kWorlds[row].area_en : kWorlds[row].area;
}
// Which rows go by the name alone is decided on the Japanese column, so the two languages group
// the rows alike.
void ff132_world_display(int row, int english, char* out, size_t n) {
    int b = no_id_row();
    const char* age = kWorlds[row].age;
    const char* area = row_area(row, english);
    if (!age[0] || (b >= 0 && strcmp(kWorlds[row].area, kWorlds[b].area) == 0))
        snprintf(out, n, "%s", area);
    else
        snprintf(out, n, "%s %s", area, age);
}

// UTF-8 out, in the game's language when the save is made: the line is part of the file and does
// not follow a later change of language. An id kWorlds does not carry stands in for the age and the
// area, which is the most the mod knows about it.
static void world_label(int atHistoria, const char* hist, char* out, size_t n) {
    int english = ui_english();
    char where[96];
    snprintf(where, sizeof where, "%s", hist);
    for (size_t i = 0; i < sizeof kWorlds / sizeof kWorlds[0]; i++)
        if (strcmp(kWorlds[i].id, hist) == 0) { ff132_world_display((int)i, english, where, sizeof where); break; }
    if (!atHistoria)  snprintf(out, n, "%s", where);
    else if (english) snprintf(out, n, "Historia Crux (%s)", where);
    else              snprintf(out, n, "ヒストリアクロス(%s)", where);
}

// The two lines this mod keeps past the game's own two. The note's line is marked with a prefix
// because a save may carry the note without the label (one made before the label existed), and
// position alone could not then tell them apart.
void ff132_hdr_mod_lines(const char* text, size_t cap, const char** label, size_t* labelLen,
                      const char** note, size_t* noteLen) {
    *label = NULL; *note = NULL;
    *labelLen = 0; *noteLen = 0;
    if (!text) return;
    const size_t pfx = sizeof FF132_HDR_NOTE_PREFIX - 1;
    size_t i = 0, nl = 0;
    // cp932 trail bytes never reach 0x0A, so this byte scan finds the game's own two lines.
    while (i < cap && text[i]) if (text[i++] == '\n' && ++nl == 2) break;
    if (nl < 2) return;
    while (i < cap && text[i]) {
        size_t at = i;
        while (i < cap && text[i] && text[i] != '\n') i++;
        size_t len = i - at;
        if (len >= pfx && memcmp(text + at, FF132_HDR_NOTE_PREFIX, pfx) == 0) {
            if (!*note) { *note = text + at + pfx; *noteLen = len - pfx; }
        } else if (!*label && len) {
            *label = text + at; *labelLen = len;
        }
        if (i < cap && text[i] == '\n') i++;
    }
}

// The one place that rewrites a save's header text: the world label is put back after every write
// the game makes, and the note is written from the editor, and both land in the same 0x100 bytes.
// The game has closed the file by now, so this opens it again and puts the last-write time back
// afterwards: that timestamp is both the file identity the sidecar records and the date the save
// list shows, and neither may move because a line was added.
int ff132_hdr_write_lines(int slot, const char* label, const char* note, char* why, size_t whyN) {
    char path[MAX_PATH];
    if (why && whyN) why[0] = 0;
    const char* msg = NULL;
    if (!save_slot_path(slot, path, sizeof path)) {
        if (why && whyN) snprintf(why, whyN, "it has no path");
        return 0;
    }
    HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (why && whyN) snprintf(why, whyN, "it could not be opened (error %lu)",
                                  (unsigned long)GetLastError());
        return 0;
    }
    FILETIME ct, at, wt;
    unsigned char hdr[FF132_HDR_BYTES];
    char* text = (char*)hdr + FF132_HDR_TEXT_AT;
    DWORD n = 0;
    size_t len = 0;
    if (!GetFileTime(h, &ct, &at, &wt)) msg = "its timestamps could not be read";
    else if (!ReadFile(h, hdr, sizeof hdr, &n, NULL) || n != sizeof hdr)
        msg = "its header could not be read";
    else if (hdr[0] != FF132_HDR_MAGIC) msg = "its header does not start with 0x0D";
    if (!msg) {
        while (len < FF132_HDR_TEXT && text[len]) len++;
        if (len == FF132_HDR_TEXT) msg = "its list text is not terminated";
    }
    if (!msg) {
        const char* oldL; const char* oldN;
        size_t oldLn, oldNn;
        ff132_hdr_mod_lines(text, len, &oldL, &oldLn, &oldN, &oldNn);
        const char* la = label ? label : (oldL ? oldL : "");
        size_t laN = label ? strlen(label) : oldLn;
        const char* no = note ? note : (oldN ? oldN : "");
        size_t noN = note ? strlen(note) : oldNn;
        const size_t pfx = sizeof FF132_HDR_NOTE_PREFIX - 1;
        size_t keep = len, nl = 0;
        for (size_t i = 0; i < len; i++) if (text[i] == '\n' && ++nl == 2) { keep = i; break; }
        size_t need = keep + (laN ? 1 + laN : 0) + (noN ? 1 + pfx + noN : 0);
        if (need + 1 > FF132_HDR_TEXT) msg = "the lines would not fit";
        else {
            char out[FF132_HDR_TEXT];
            size_t w = keep;
            memcpy(out, text, keep);
            if (laN) { out[w++] = '\n'; memcpy(out + w, la, laN); w += laN; }
            if (noN) {
                out[w++] = '\n';
                memcpy(out + w, FF132_HDR_NOTE_PREFIX, pfx); w += pfx;
                memcpy(out + w, no, noN); w += noN;
            }
            memset(out + w, 0, FF132_HDR_TEXT - w);
            if (memcmp(out, text, FF132_HDR_TEXT) == 0) { CloseHandle(h); return 1; }
            LONG hi = 0;
            if (SetFilePointer(h, FF132_HDR_TEXT_AT, &hi, FILE_BEGIN) == INVALID_SET_FILE_POINTER &&
                GetLastError() != NO_ERROR) msg = "its header could not be seeked to";
            else if (!WriteFile(h, out, FF132_HDR_TEXT, &n, NULL) || n != FF132_HDR_TEXT)
                msg = "its header could not be written";
            else if (!SetFileTime(h, &ct, &at, &wt)) msg = "its last-write time could not be restored";
        }
    }
    CloseHandle(h);
    // The write time is put back above; if that failed, the working slot's new identity is still known.
    {
        uint64_t mt;
        uint32_t sz;
        if (stat_slot(slot, &mt, &sz)) save_work_note_written(slot, mt, "header rewritten");
    }
    if (msg && why && whyN) snprintf(why, whyN, "%s", msg);
    return msg == NULL;
}

// After a save: the game rebuilt the whole header, so the note is gone from the file. `note` (cp932,
// or NULL) is the one the slot carried before the write, put back when keep_note asks for it.
static void note_header(int slot, const char* note) {
    char why[128];
    if (ff132_hdr_write_lines(slot, NULL, note, why, sizeof why))
        ff132_log(FF132_TAG "keep note: slot %d keeps its note\n", slot);
    else
        ff132_log(FF132_TAG "keep note: slot %d -> %s, note lost\n", slot, why);
}
static void label_header(int slot, const char* labelUtf8, const char* note) {
    char label[FF132_HDR_TEXT], why[128];
    if (!to_cp932(labelUtf8, label, sizeof label)) {
        ff132_log(FF132_TAG "world label: slot %d -> \"%s\" does not convert to cp932, not labelled\n",
              slot, labelUtf8);
        if (note) note_header(slot, note);
        return;
    }
    if (ff132_hdr_write_lines(slot, label, note, why, sizeof why))
        ff132_log(FF132_TAG "world label: slot %d -> %s%s\n", slot, labelUtf8,
              note ? " (note kept)" : "");
    else
        ff132_log(FF132_TAG "world label: slot %d -> %s, not labelled\n", slot, why);
}

// cp932. The area name alone: the save carries no age to put before it.
static int no_id_line(int english, char* out, size_t n) {
    int b = no_id_row();
    return b >= 0 && to_cp932(row_area(b, english), out, n);
}

// Labels already in a save file were written in whichever form the mod used then: "<age> <area>",
// "<area><age>" or "<area> <age>", bare or inside the Historia Crux's parentheses. Each is shown in
// today's form (ff132_world_display) and the file is left as it is; a label in either language whose
// area goes by its name alone (`bare`, `bare2`) loses its age. 1 = out differs from in. cp932:
// walked a character at a time, since a trail byte can equal 'A' or 'F'.
static int age_tok(const char* s, size_t n) {
    if (n < 3 || s[0] != 'A' || s[1] != 'F') return 0;
    for (size_t i = 2; i < n; i++)
        if (!isalnum((unsigned char)s[i]) && s[i] != '?') return 0;
    return 1;
}
static int is_bare(const char* bare, const char* s, size_t n) {
    return bare && bare[0] && strlen(bare) == n && memcmp(bare, s, n) == 0;
}
static int label_reorder(const char* in, size_t len, const char* bare, const char* bare2, char* out,
                         size_t n) {
    size_t b = 0, e = len;                       // the world part: after '(' and before ')', if any
    for (size_t i = 0; i < len; i += (cp932_lead((unsigned char)in[i]) && i + 1 < len) ? 2 : 1) {
        if (in[i] == '(' && !b) b = i + 1;
        else if (in[i] == ')' && b) { e = i; break; }
    }
    size_t ab = 0, ae = 0, rb = 0, re = 0;        // age and area spans
    size_t sp = b;                               // "<age> <area>": the first space
    while (sp < e && in[sp] != ' ') sp++;
    if (sp < e && age_tok(in + b, sp - b)) { ab = b; ae = sp; rb = sp + 1; re = e; }
    else {
        size_t k = e;                            // "<area>[ ]<age>": the last char boundary at "AF"
        for (size_t i = b; i < e; i += (cp932_lead((unsigned char)in[i]) && i + 1 < e) ? 2 : 1)
            if (i > b && age_tok(in + i, e - i)) { k = i; break; }
        if (k == e) return 0;
        ab = k; ae = e; rb = b; re = (in[k - 1] == ' ') ? k - 1 : k;
    }
    if (re <= rb) return 0;
    int noAge = is_bare(bare, in + rb, re - rb) || is_bare(bare2, in + rb, re - rb);
    size_t need = b + (re - rb) + 1 + (ae - ab) + (len - e) + 1;
    if (need > n) return 0;
    size_t o = 0;
    memcpy(out + o, in, b);              o += b;
    memcpy(out + o, in + rb, re - rb);   o += re - rb;
    if (!noAge) { out[o++] = ' '; memcpy(out + o, in + ab, ae - ab); o += ae - ab; }
    memcpy(out + o, in + e, len - e);    o += len - e;
    out[o] = 0;
    return o != len || memcmp(out, in, len) != 0;
}

// The header line alone is not shown: the list's GUI rebuilds the two lines into a buffer of its own
// (frame layout in ff132_addrs.h) instead of displaying the header text, so the third line is appended
// there too. Game thread, once per rebuild -- which is once per cursor move.
static void __cdecl on_list_text_built(uint32_t esp) {
    uintptr_t frame = (uintptr_t)esp + FF132_LIST_FRAME_ADJ;
    if (!rd_ok((const void*)(frame + FF132_LIST_CTX_AT), 8)) return;
    uintptr_t entry = (uintptr_t)rd32(frame + FF132_LIST_CTX_AT + 4);
    if (!rd_ok((const void*)entry, FF132_ENTRY_STALE + 1)) return;
    if (*(const volatile uint8_t*)(entry + FF132_ENTRY_STALE)) return;
    uintptr_t hdr = (uintptr_t)rd32(entry + FF132_ENTRY_HEADER);
    if (!rd_ok((const void*)hdr, FF132_HDR_TEXT_AT + FF132_HDR_TEXT)) return;
    const char* src = (const char*)(hdr + FF132_HDR_TEXT_AT);
    const char* world; const char* note;
    size_t add, noteN;
    ff132_hdr_mod_lines(src, FF132_HDR_TEXT, &world, &add, &note, &noteN);
    // Shown in the language in force; an older label is matched in both.
    static char noIdJp[32], noIdEn[32];
    if (!noIdJp[0]) no_id_line(0, noIdJp, sizeof noIdJp);
    if (!noIdEn[0]) no_id_line(1, noIdEn, sizeof noIdEn);
    const char* noIdLine = ui_english() ? noIdEn : noIdJp;
    if (!world || !add) {                     // the note has its own place, on the heading line
        // A save with an empty id was never labelled (there was no world to name when it was
        // written); its line comes from the list's own read of the file instead.
        if (!noIdLine[0]) return;
        if (!rd_ok((const void*)(entry + FF132_ENTRY_SLOT), 1)) return;
        if (!ff132_tidy_slot_no_id(*(const volatile signed char*)(entry + FF132_ENTRY_SLOT))) return;
        world = noIdLine;
        add = strlen(noIdLine);
    }
    static char reordered[FF132_HDR_TEXT];
    if (label_reorder(world, add, noIdJp, noIdEn, reordered, sizeof reordered)) {
        world = reordered;
        add = strlen(reordered);
    }
    char* buf = (char*)(frame + FF132_LIST_TEXT_AT);
    if (!rd_ok(buf, FF132_LIST_TEXT)) return;
    size_t len = 0;
    while (len < FF132_LIST_TEXT && buf[len]) len++;
    if (len == FF132_LIST_TEXT || len + 1 + add >= FF132_LIST_TEXT) return;
    buf[len] = '\n';
    memcpy(buf + len + 1, world, add);
    buf[len + 1 + add] = 0;
    static char shown[FF132_HDR_TEXT];
    if (strncmp(shown, world, add) != 0 || shown[add]) {
        snprintf(shown, sizeof shown, "%.*s", (int)add, world);
        ff132_logv(FF132_TAG "save list: entry shows \"%s\"\n", shown);
    }
}

// ---- what the save hooks report ------------------------------------------------------------------
static int g_pendSlot = -1;               // a silent save is running on this slot
static int g_pendHist = 0;

static void note_written(int slot, int atHistoria, int silent) {
    if (slot < 0 || slot >= FF132_SLOT_MAX) return;
    // The game rebuilt the header, so the note that was in it is gone -- unless it is put back from
    // the cache, which the list filled when it opened and the editor and the copies keep current.
    // An automatic save into the working slot never keeps one, whatever keep_note says.
    char keep[64];
    const char* keepNote = (g_keepNote && ff132_tidy_note_peek(slot, keep, sizeof keep)) ? keep : NULL;
    if (keepNote && silent && slot == save_work_slot()) {
        ff132_log(FF132_TAG "keep note: slot %d is the working slot, its note is dropped on an automatic save\n",
              slot);
        keepNote = NULL;
    }
    if (!keepNote) ff132_tidy_note_dropped(slot);
    int zone;
    char hist[FF132_SAVE_HISTORY_ID_LEN + 1];
    char label[128];
    save_ids(&zone, hist, sizeof hist);
    label[0] = 0;
    if (real_id(hist)) {
        world_label(atHistoria, hist, label, sizeof label);
        // Before the stat below: the identity recorded there has to be the file as it stands once
        // the header has been written (restoring the write time keeps the two equal anyway).
        label_header(slot, label, keepNote);
        keepNote = NULL;
    }
    if (keepNote) note_header(slot, keepNote);
    uint64_t mt;
    uint32_t sz;
    if (!stat_slot(slot, &mt, &sz)) {
        int had = g_rec[slot].kind != K_NONE;
        g_rec[slot].kind = K_NONE;
        g_rec[slot].hist[0] = 0;
        if (had) slots_write();
        ff132_log(FF132_TAG "save record: slot %d could not be read back -> no record\n", slot);
        return;
    }
    g_rec[slot].mtime = mt;
    g_rec[slot].size  = sz;
    g_rec[slot].kind  = (uint8_t)(atHistoria ? K_HISTORIA : K_FIELD);
    snprintf(g_rec[slot].hist, sizeof g_rec[slot].hist, "%s", real_id(hist) ? hist : "");
    slots_write();
    save_work_note_written(slot, mt, "the game wrote it");
    ff132_log(FF132_TAG "save record: slot %d written -> %s (%u bytes) %s\n", slot,
          kind_name(g_rec[slot].kind), (unsigned)sz, label[0] ? label : "(world unknown)");
}

void ff132_load_note_written(int slot, int atHistoria) { note_written(slot, atHistoria, 0); }

void ff132_load_note_pending(int slot, int atHistoria) {
    if (slot < 0 || slot >= FF132_SLOT_MAX) return;
    g_pendSlot = slot;
    g_pendHist = atHistoria;
}

void ff132_load_frame(int saveInFlight) {
    if (g_pendSlot < 0 || saveInFlight) return;
    int slot = g_pendSlot, hist = g_pendHist;
    g_pendSlot = -1;
    note_written(slot, hist, 1);
}

void ff132_load_note_copied(int src, int dst) {
    if (dst < 0 || dst >= FF132_SLOT_MAX) return;
    // The file was copied whole, header and all, whatever the sidecar below decides about it.
    ff132_tidy_note_copied(src, dst);
    uint64_t mt;
    uint32_t sz;
    if (src >= 0 && src < FF132_SLOT_MAX && rec_matches(src) && stat_slot(dst, &mt, &sz)) {
        g_rec[dst].mtime = mt;
        g_rec[dst].size  = sz;
        g_rec[dst].kind  = g_rec[src].kind;
        // The copy carries the header, label and all, so it carries the id that named it too.
        memcpy(g_rec[dst].hist, g_rec[src].hist, sizeof g_rec[dst].hist);
        slots_write();
        ff132_log(FF132_TAG "save record: slot %d copied to %d -> %s\n", src, dst,
              kind_name(g_rec[dst].kind));
        return;
    }
    int had = g_rec[dst].kind != K_NONE;
    g_rec[dst].kind = K_NONE;
    g_rec[dst].hist[0] = 0;
    if (had) slots_write();
    ff132_log(FF132_TAG "save record: slot %d copied to %d, slot %d has no usable record -> slot %d has"
                 " none either\n", src, dst, src, dst);
}

void ff132_load_on_loaded(int slot) {
    const char* why = "no record";
    int direct = 0;
    if (slot >= 0 && slot < FF132_SLOT_MAX && g_rec[slot].kind) {
        if (!rec_matches(slot)) why = "file changed";
        else {
            why = kind_name(g_rec[slot].kind);
            direct = g_directLoad && g_rec[slot].kind == K_FIELD;
        }
    }
    if (!g_loadOk) direct = 0;
    else if (!apply(direct))
        ff132_loga(FF132_TAG "direct load: the trigger bytes could not be written\n");
    ff132_log(FF132_TAG "direct load: slot %d -> %s (%s)\n", slot,
          direct ? "direct" : "historia crux", why);
    if (g_recapOk) {
        g_recapLogged = 0;
        g_recapWindow = 1;
    }
}

// ---- log-only hooks ----------------------------------------------------------------------------
static const char* const kMainState[] = {
    "NA", "INIT", "DEBUG_MENU", "SHOW_LOGO", "TITLE", "ZONE", "RESERVED_001", "RESERVED_000",
    "FIELD_DEBUG", "BATTLE_DEBUG", "COMMON_DEBUG", "SYSTEM_END", "ZONE_PAUSE", "CINEMA_DEBUG",
    "COMMUNICATE", "LOAD", "LOAD_END", "MOTION_TEST", "DISPOSE_RESOURCE", "MENU_TEST",
    "LOAD_EMPTY", "YARISUGI_DEBUG", "TUTORIAL", "DLC",
};
static const char* state_name(int id) {
    return (id >= 0 && id < (int)(sizeof kMainState / sizeof kMainState[0])) ? kMainState[id] : "?";
}
enum { MS_TITLE = 4, MS_LOAD = 15, MS_LOAD_END = 16, MS_LOAD_EMPTY = 20 };

// The zone and the history the save image currently holds, which is what says whether a load has
// read the file in yet. Both are absent until the singleton is up, hence the -1 / "-".
static void save_ids(int* zone, char* hist, size_t n) {
    *zone = -1;
    snprintf(hist, n, "-");
    uintptr_t sd = (uintptr_t)rd32(g_base + FF132_RVA_SAVEDATA_PTR);
    if (!rd_ok((const void*)sd, FF132_OFF_SAVE_HISTORY_ID + FF132_SAVE_HISTORY_ID_LEN)) return;
    *zone = (int)rd32(sd + FF132_OFF_SAVE_ZONE_NUM);
    const volatile unsigned char* s = (const volatile unsigned char*)(sd + FF132_OFF_SAVE_HISTORY_ID);
    size_t i = 0;
    for (; i < FF132_SAVE_HISTORY_ID_LEN && i + 1 < n && s[i]; i++)
        hist[i] = (s[i] >= 0x20 && s[i] < 0x7F) ? (char)s[i] : '.';
    if (i) hist[i] = 0;
}

static void __cdecl on_main_state(uint32_t esp) {
    if (!rd_ok((const void*)(uintptr_t)esp, FF132_ARG_MAIN_STATE_OLD + 4)) return;
    int ns = (int)rd32((uintptr_t)esp + FF132_ARG_MAIN_STATE_NEW);
    int os = (int)rd32((uintptr_t)esp + FF132_ARG_MAIN_STATE_OLD);
    int zone;
    char hist[FF132_SAVE_HISTORY_ID_LEN + 1];
    save_ids(&zone, hist, sizeof hist);
    ff132_log(FF132_TAG "main state %d(%s) -> %d(%s) zone=%d history=%s\n",
          os, state_name(os), ns, state_name(ns), zone, hist);
    // Never on the way INTO LOAD_EMPTY: the task that sent that trigger still has its second push
    // to execute, and restoring here would turn that send back into 0x13 and drive LOAD_EMPTY
    // straight on to STATE_ZONE.
    if (g_loadOk && ns != MS_LOAD && ns != MS_LOAD_END && ns != MS_LOAD_EMPTY) apply(0);
    if (ns == MS_TITLE) {
        g_recapWindow = 0;
        if (ff132_nomod_stuck()) ff132_flash_c(ui_text(T_NOMOD_STUCK), FF132_COL_ERR);
    }
}

static void __cdecl on_zone_jump(uint32_t esp) {
    if (!rd_ok((const void*)(uintptr_t)esp, 8)) return;
    uintptr_t ctx = (uintptr_t)rd32((uintptr_t)esp + 4);
    if (!rd_ok((const void*)ctx, FF132_OFF_VM_STACK + 4)) return;
    uintptr_t st = (uintptr_t)rd32(ctx + FF132_OFF_VM_STACK);
    if (!rd_ok((const void*)st, 8)) return;
    ff132_log(FF132_TAG "zone jump requested: %d\n", (int)rd32(st + 4));
    g_recapWindow = 0;
}

// Entry replacement on Party.getEventFlag. Inside the window it answers false to the recap's own
// flags by writing the native's own result format into the operand slot -- see ff132_addrs.h -- and
// skipping the body; every other name, and every moment outside the window, falls through.
static int __cdecl on_get_event_flag(uint32_t ecx, uint32_t esp, int* ret) {
    (void)ecx;
    if (!g_skipRecap || !g_recapWindow) return 0;
    if (!rd_ok((const void*)(uintptr_t)esp, 8)) return 0;
    uintptr_t ctx = (uintptr_t)rd32((uintptr_t)esp + 4);
    if (!rd_ok((const void*)ctx, FF132_OFF_VM_STACK + 4)) return 0;
    uintptr_t st = (uintptr_t)rd32(ctx + FF132_OFF_VM_STACK);
    if (!rd_ok((const void*)st, 8)) return 0;
    const char* name = (const char*)rd32(st + 4);
    const size_t pfx = sizeof FF132_RECAP_FLAG_PREFIX - 1;
    if (!rd_ok(name, pfx) || memcmp(name, FF132_RECAP_FLAG_PREFIX, pfx) != 0) return 0;
    *(volatile uint16_t*)st = 1;
    *(volatile uint32_t*)(st + 4) = 0;
    *ret = 0;
    if (!g_recapLogged) {
        g_recapLogged = 1;
        char nm[24];
        size_t i = 0;
        for (; i + 1 < sizeof nm && rd_ok(name + i, 1) && name[i]; i++) nm[i] = name[i];
        nm[i] = 0;
        ff132_log(FF132_TAG "recap skipped: %s and the rest answered false\n", nm);
    }
    return 1;
}

static void log_hook(const char* tag, uintptr_t rva, const uint8_t* sig, int len, void* handler) {
    uintptr_t at = g_base + rva;
    if (wait_for_bytes(at, sig, len, FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "direct load: %s @0x%08X never matched -> not logged\n", tag, (unsigned)at);
        return;
    }
    void* cave = ff13_install_esp_hook(at, sig, len, handler);
    ff132_loga(FF132_TAG "direct load: %s @0x%08X %s\n", tag, (unsigned)at,
           cave ? "logged" : "not patched -> not logged");
}

// ---- install -------------------------------------------------------------------------------------
void ff132_load_install(void) {
    // Before the site checks below, which can spin for their whole budget: a record has to be in
    // hand before the first load the hooks report, and those are already installed by now.
    slots_read();
    uintptr_t a = g_base + FF132_RVA_LOAD_TRIGGER_A;
    uintptr_t b = g_base + FF132_RVA_LOAD_TRIGGER_B;
    int okA = wait_for_bytes(a, FF132_SIG_LOAD_TRIGGER_A, (int)sizeof FF132_SIG_LOAD_TRIGGER_A,
                             FF132_SITE_WAIT_MS) >= 0;
    int okB = wait_for_bytes(b, FF132_SIG_LOAD_TRIGGER_B, (int)sizeof FF132_SIG_LOAD_TRIGGER_B,
                             FF132_SITE_WAIT_MS) >= 0;
    if (!okA || !okB) {
        ff132_loga(FF132_TAG "direct load: trigger site @0x%08X %s, @0x%08X %s -> feature off\n",
               (unsigned)a, okA ? "ok" : "mismatch", (unsigned)b, okB ? "ok" : "mismatch");
    } else {
        g_loadOk = 1;
        ff132_loga(FF132_TAG "direct load: trigger sites @0x%08X/@0x%08X ok -- starts %s\n",
               (unsigned)a, (unsigned)b, g_directLoad ? "ON" : "OFF");
        // The sites carry their vanilla trigger until a load is decided for, so the toggle alone
        // changes nothing on disk or in memory.
        if (!apply(0))
            ff132_loga(FF132_TAG "direct load: the trigger bytes could not be written\n");
    }
    log_hook("main-state hook", FF132_RVA_MAIN_STATE_ENTER, FF132_SIG_MAIN_STATE_ENTER,
             (int)sizeof FF132_SIG_MAIN_STATE_ENTER, (void*)on_main_state);
    log_hook("zone-jump hook", FF132_RVA_ZONE_DOJUMP_NATIVE, FF132_SIG_ZONE_DOJUMP_NATIVE,
             (int)sizeof FF132_SIG_ZONE_DOJUMP_NATIVE, (void*)on_zone_jump);
    // A mismatch here costs the display only: the line is still written into every save's header.
    uintptr_t lt = g_base + FF132_RVA_LIST_TEXT_BUILT;
    void* lcave = NULL;
    if (wait_for_bytes(lt, FF132_SIG_LIST_TEXT_BUILT, (int)sizeof FF132_SIG_LIST_TEXT_BUILT,
                       FF132_SITE_WAIT_MS) >= 0)
        lcave = ff13_install_esp_hook(lt, FF132_SIG_LIST_TEXT_BUILT,
                                      (int)sizeof FF132_SIG_LIST_TEXT_BUILT,
                                      (void*)on_list_text_built);
    ff132_loga(FF132_TAG "save list: label hook @0x%08X %s\n", (unsigned)lt,
           lcave ? "ok" : "mismatch -> the list stays as it was");
    uintptr_t gf = g_base + FF132_RVA_GET_EVENT_FLAG_NATIVE;
    if (wait_for_bytes(gf, FF132_SIG_GET_EVENT_FLAG_NATIVE, (int)sizeof FF132_SIG_GET_EVENT_FLAG_NATIVE,
                       FF132_SITE_WAIT_MS) < 0) {
        ff132_loga(FF132_TAG "story recap: getEventFlag @0x%08X never matched -> the recap plays\n",
               (unsigned)gf);
        return;
    }
    void* cave = ff13_install_replace_hook(gf, FF132_SIG_GET_EVENT_FLAG_NATIVE,
                                           (int)sizeof FF132_SIG_GET_EVENT_FLAG_NATIVE,
                                           (void*)on_get_event_flag, 0);
    if (!cave) {
        ff132_loga(FF132_TAG "story recap: getEventFlag @0x%08X not patched -> the recap plays\n",
               (unsigned)gf);
        return;
    }
    g_recapOk = 1;
    ff132_loga(FF132_TAG "story recap: getEventFlag @0x%08X replaced, cave=%p -- the recap is %s\n",
           (unsigned)gf, cave, g_skipRecap ? "skipped" : "played");
}

// ---- the toggle ----------------------------------------------------------------------------------
void ff132_load_toggle(void) {
    if (!g_loadOk) {
        ff132_log(FF132_TAG "direct load: not installed\n");
        ff132_flash(ui_text(T_HIST_NA));
        return;
    }
    g_directLoad = g_directLoad ? 0 : 1;
    ff132_log(FF132_TAG "direct load: %s\n", g_directLoad ? "ON" : "OFF");
    if (g_directLoad) ff132_flash_c(ui_text(T_HIST_ON), FF132_COL_ON);
    else              ff132_flash(ui_text(T_HIST_OFF));
}
