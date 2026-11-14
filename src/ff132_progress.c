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

// Pure C + stdio; no game memory is touched.
#include "ff132_progress.h"
#include "ff132_worlds.h"
#include "ff13_crypt.h"
#include <stdio.h>
#include <string.h>

// ff13_2-NN.dat: 0x210 plaintext header, APP.DAT (cipher container 0), then 32 groups of
// Q0, Q1 (349136 each), R0, R1 (1888 each); group g's blocks are container g+1.
#define SV_FILE_LEN   23014096L
#define SV_APP_AT     0x210L
#define SV_GROUP0_Q0  (SV_APP_AT + 548032L)
#define SV_GROUP_LEN  702048L
#define SV_GROUPS     32
#define SV_SEEDS      33
#define SV_HID_REL    0xB234u   // APP.DAT-relative: db_partyman+0xAAF4, current history id
#define SV_WORK_REL   0x7C0u    // Q0-relative: zone body 0xC0 + 0x700 = history work[0], [1] (BE)

// Per container, in WhiteCryptTool order; the table is built from each REVERSED. Entries 27 and 28
// really are identical.
static const uint8_t kSeeds[SV_SEEDS][8] = {
    {0xFD,0x9B,0x0C,0x81,0xB1,0x71,0x06,0xFE}, {0x4E,0xA0,0x50,0x3A,0xF3,0x31,0x02,0x67},
    {0x50,0x06,0xF2,0x01,0x48,0x28,0x88,0x38}, {0xD2,0xBF,0x02,0x05,0xFD,0x74,0x33,0xBC},
    {0x61,0xCD,0x53,0xE2,0x62,0xDB,0xA5,0x51}, {0x35,0x14,0x5D,0x48,0xC3,0x67,0x8D,0xE0},
    {0x73,0x5E,0x0D,0xED,0x9D,0x52,0xA8,0x13}, {0x66,0x66,0x03,0x35,0xB6,0x2D,0x8B,0xD7},
    {0xCE,0x8F,0x7C,0x97,0x47,0x9A,0xFD,0xCD}, {0xA6,0xAE,0xBA,0x85,0x72,0x24,0xB4,0x8B},
    {0x32,0x04,0x75,0x60,0xB6,0x9A,0x68,0xA9}, {0x56,0x1F,0x2B,0x31,0x8D,0x4B,0x26,0xC6},
    {0x46,0x65,0x09,0x02,0xA5,0x25,0xCF,0xC8}, {0x5B,0x71,0x30,0x8F,0x9E,0xA2,0x91,0x04},
    {0xD7,0x5C,0xD3,0xF6,0xE3,0xE5,0x61,0xA7}, {0xC2,0x6F,0xC6,0xBC,0x2F,0x44,0x87,0x9E},
    {0x02,0x21,0x63,0x86,0x46,0x75,0x2C,0xE0}, {0x37,0xC9,0xC2,0x41,0x1A,0x9F,0x2E,0x32},
    {0x1A,0x31,0x0E,0xE9,0xCC,0xC6,0x85,0xF2}, {0xF3,0xCA,0xE4,0x60,0xCB,0x73,0x1D,0x21},
    {0xF8,0x0E,0xD3,0xD7,0x65,0xA5,0x16,0x4C}, {0x86,0xB3,0xF8,0xA6,0xDD,0xD5,0xF7,0x6E},
    {0x63,0xB0,0xD1,0xF7,0x71,0xD7,0x5A,0x82}, {0x10,0x8F,0x8C,0x7E,0xA8,0x0E,0x18,0x8D},
    {0x70,0xF3,0x00,0x49,0xE1,0x1D,0x72,0xCE}, {0x92,0x8C,0x7E,0x53,0xC3,0x7A,0xF5,0x20},
    {0x8E,0x92,0x7B,0x34,0xEA,0x14,0x0F,0x23}, {0x3D,0xC5,0x76,0xA3,0x23,0x46,0x03,0x9E},
    {0x3D,0xC5,0x76,0xA3,0x23,0x46,0x03,0x9E}, {0x99,0x5C,0x47,0xA5,0xD1,0x18,0x78,0x74},
    {0x21,0xAE,0x8B,0x7A,0x78,0xEA,0xAF,0x2B}, {0x5F,0x5D,0xCB,0x1C,0x07,0xF0,0x12,0xCA},
    {0xD6,0x78,0xC6,0xDA,0x0F,0xD6,0x23,0xB4},
};

// Built on first use from whichever thread gets there; two threads building the same table write
// the same bytes, and the flag is set only after the table is whole.
static uint8_t g_tab[SV_SEEDS][264];
static volatile unsigned char g_tabHave[SV_SEEDS];

static const uint8_t* table_for(int i) {
    if (!g_tabHave[i]) {
        uint8_t seed[8];
        for (int k = 0; k < 8; k++) seed[k] = kSeeds[i][7 - k];
        ff13_crypt_table(seed, g_tab[i]);
        __asm__ __volatile__("" ::: "memory");
        g_tabHave[i] = 1;
    }
    return g_tab[i];
}

// `n` bytes at container-relative `rel` of the container starting at file offset `at`.
static int read_plain(FILE* f, long at, int seed, uint32_t rel, uint8_t* out, uint32_t n) {
    uint8_t buf[32];
    uint32_t a = rel & ~7u, e = (rel + n + 7u) & ~7u;
    if (e - a > sizeof buf) return 0;
    if (fseek(f, at + (long)a, SEEK_SET) || fread(buf, 1, e - a, f) != e - a) return 0;
    ff13_decrypt_window_with(table_for(seed), buf, a, e - a);
    memcpy(out, buf + (rel - a), n);
    return 1;
}

static int32_t be32(const uint8_t* p) {
    return (int32_t)(((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
                     | ((uint32_t)p[2] << 8) | p[3]);
}

static int history_int(const char* hid) {
    for (size_t i = 0; i < sizeof kHistoryInts / sizeof kHistoryInts[0]; i++)
        if (!strcmp(kHistoryInts[i].id, hid)) return kHistoryInts[i].n;
    return FF132_PROG_NONE;
}

int ff132_save_progress_ex(const char* path, char* hid, int* group, int* histId, int32_t* area) {
    char id[FF132_PROG_HID + 1] = "";
    int g = FF132_PROG_NONE, want = FF132_PROG_NONE, rc = -1;
    int32_t ar = FF132_PROG_NONE;
    FILE* f = fopen(path, "rb");
    if (f) {
        setvbuf(f, NULL, _IONBF, 0);
        uint8_t raw[FF132_PROG_HID];
        if (!fseek(f, 0, SEEK_END) && ftell(f) == SV_FILE_LEN
            && read_plain(f, SV_APP_AT, 0, SV_HID_REL, raw, FF132_PROG_HID)) {
            memcpy(id, raw, FF132_PROG_HID);
            id[FF132_PROG_HID] = 0;
            for (char* c = id; *c; c++)
                if ((unsigned char)*c < 0x20 || (unsigned char)*c > 0x7E) *c = '?';
            want = history_int(id);
            rc = 0;
            for (int i = 0; want != FF132_PROG_NONE && i < SV_GROUPS; i++) {
                long q0 = SV_GROUP0_Q0 + (long)i * SV_GROUP_LEN;
                uint8_t head[8], w[8];
                static const uint8_t zero[8];
                if (fseek(f, q0, SEEK_SET) || fread(head, 1, 8, f) != 8) { rc = -1; break; }
                if (!memcmp(head, zero, 8)) continue;          // the group was never written
                if (!read_plain(f, q0, i + 1, SV_WORK_REL, w, 8)) { rc = -1; break; }
                if (be32(w) != want) continue;
                g = i; ar = be32(w + 4); rc = 1;
                break;
            }
        }
        fclose(f);
    }
    if (hid) memcpy(hid, id, sizeof id);
    if (group) *group = g;
    if (histId) *histId = want;
    if (area) *area = ar;
    return rc;
}

int ff132_save_progress(const char* path, int* histId, int32_t* area) {
    return ff132_save_progress_ex(path, NULL, NULL, histId, area);
}

// ---- the adjusted play time ---------------------------------------------------------------------
static int same_group(const ff132_prog_in* a, const ff132_prog_in* b) {
    return a->area >= 0 && b->area >= 0 && a->ep == b->ep && a->frag == b->frag
        && a->hist == b->hist;
}
static int before(const ff132_prog_in* a, const ff132_prog_in* b) {
    if (a->area != b->area) return a->area < b->area;
    if (a->play != b->play) return a->play < b->play;
    return a->slot < b->slot;
}

void ff132_progress_adjust(const ff132_prog_in* in, int n, uint32_t* playOut) {
    enum { CAP = 256 };
    unsigned char done[CAP];
    int mem[CAP];
    uint32_t pl[CAP];
    if (n > CAP) n = CAP;
    for (int i = 0; i < n; i++) { playOut[i] = in[i].play; done[i] = 0; }
    for (int i = 0; i < n; i++) {
        if (done[i] || in[i].area < 0) continue;
        int m = 0;
        for (int j = i; j < n; j++) {
            if (done[j] || !same_group(&in[i], &in[j])) continue;
            done[j] = 1;
            int k = m++;                                       // insertion: members in story order
            while (k > 0 && before(&in[j], &in[mem[k - 1]])) { mem[k] = mem[k - 1]; k--; }
            mem[k] = j;
            uint32_t p = in[j].play;                           // and the play times ascending
            k = m - 1;
            while (k > 0 && p < pl[k - 1]) { pl[k] = pl[k - 1]; k--; }
            pl[k] = p;
        }
        for (int k = 0; k < m; k++) playOut[mem[k]] = pl[k];
    }
}
