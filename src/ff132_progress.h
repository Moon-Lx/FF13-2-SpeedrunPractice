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

// Story progress inside one history, read from a save without decrypting it whole: the history's
// "area counter" (history work[1]) out of the save group of the save's current history.
// Plain C and stdio only, so the tests can build it for the host.
#ifndef FF132_PROGRESS_H
#define FF132_PROGRESS_H
#include <stdint.h>

#define FF132_PROG_NONE   (-1)
#define FF132_PROG_HID    16          // the history id field, NUL-padded ASCII

// 1 = counter found; 0 = a save, but no counter (unknown history, or its group never written);
// -1 = not readable as a save. histId/area are FF132_PROG_NONE when not known. hid (17 bytes) and
// group may be NULL. Thread-safe.
int ff132_save_progress_ex(const char* path, char* hid, int* group, int* histId, int32_t* area);
int ff132_save_progress(const char* path, int* histId, int32_t* area);

// The play time each save is ORDERED by. Saves sharing (ep, frag, hist) that all have an area
// counter take the group's play times, sorted, in (area, play, slot) order; every other save keeps
// its own. Keeps the comparator a plain key chain (strict weak ordering).
typedef struct { int slot; unsigned ep, frag; int hist; int32_t area; uint32_t play; } ff132_prog_in;
void ff132_progress_adjust(const ff132_prog_in* in, int n, uint32_t* playOut);

#endif
