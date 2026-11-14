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

// Which language the mod speaks, and the lines it speaks it in. Any thread.
#define FF132_TEXT_TABLES
#include "ff132_internal.h"
#include "ff132_addrs.h"

static volatile int g_langSeen = -1;             // what the game last answered, -1 = not yet
// 1 = the game's global is usable, 0 = its getter did not check out (the OS language decides),
// -1 = not checked yet.
static volatile int g_langSrc  = -1;
static volatile int g_osEnglish = -1;

static const char* const kLangCode[11] =
    { "jp", "us", "uk", "it", "gr", "fr", "sp", "ru", "kr", "ck", "ch" };

static int lang_src(void) {
    if (g_langSrc >= 0) return g_langSrc;
    int ok = sig_at(FF132_RVA_LANGUAGE_GET, FF132_SIG_LANGUAGE_GET, sizeof FF132_SIG_LANGUAGE_GET);
    if (!ok)
        ff132_loga(FF132_TAG "language: the game's language getter did not match -- following Windows'"
                      " display language instead\n");
    g_langSrc = ok;
    return ok;
}

static int os_english(void) {
    if (g_osEnglish < 0) {
        LANGID id = GetUserDefaultUILanguage();
        g_osEnglish = PRIMARYLANGID(id) != LANG_JAPANESE;
        ff132_loga(FF132_TAG "language: Windows' display language is 0x%04X -> %s\n", (unsigned)id,
               g_osEnglish ? "English" : "Japanese");
    }
    return g_osEnglish;
}

int ui_english(void) {
    if (!g_base) return 0;
    if (!lang_src()) return os_english();
    int idx = *(volatile int*)(g_base + FF132_RVA_LANGUAGE);
    if (idx < 0 || idx > 10) return g_langSeen > 0;     // not resolved yet -- keep the last answer
    if (idx != g_langSeen) {
        g_langSeen = idx;
        ff132_loga(FF132_TAG "language: the game is running in \"%s\" -- the mod speaks %s\n",
               kLangCode[idx], idx ? "English" : "Japanese");
    }
    return idx != 0;
}

int ui_ascii_only(void) { return ui_english() || ff132_ovl_jp_missing(); }

const char* ui_text(ff132_msg m) {
    if ((unsigned)m >= (unsigned)T_COUNT) return "";
    if (ui_ascii_only()) return kTextEn[m];
    const char* jp = kTextJp[m];
    return (jp && jp[0]) ? jp : kTextEn[m];
}
const char* ui_text_en(ff132_msg m) {
    return (unsigned)m < (unsigned)T_COUNT ? kTextEn[m] : "";
}

void ui_arrows(char* out, size_t n, const char* value) {
    if (ui_ascii_only()) snprintf(out, n, "\xE2\x97\x84 %s \xE2\x96\xBA", value ? value : "");
    else snprintf(out, n, "\xE2\x97\x80 %s \xE2\x96\xB6", value ? value : "");
}
const char* ui_ellipsis(void) { return ui_ascii_only() ? "..." : "\xE2\x80\xA6"; }
