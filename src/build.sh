#!/usr/bin/env bash
# Build the 32-bit Windows proxy dinput8.dll (FF13-2-SpeedrunPractice) from the mod source and the
# shared modules. zig cross-compiles, so this runs anywhere.
#
# Two layouts, one script:
#   private monorepo  ../../common/lib holds the modules, ../../common/re/zig the toolchain
#   public repo       ../lib holds the modules, zig comes from $ZIG or PATH (CI installs it)
#
# Default = release build. Set FF132_DIAG=1 to also compile the RE investigation aids:
#     FF132_DIAG=1 ./build.sh
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"            # <repo>/ff132_sr_practice/src or <repo>/src
if [ -d "$here/../../common/lib" ]; then
    lib="$(cd "$here/../../common/lib" && pwd)"
    libspec="../../common"                       # what the dirty-tree check watches
    ZIG="${ZIG:-$here/../../common/re/zig/zig}"
else
    lib="$(cd "$here/../lib" && pwd)"
    libspec="../lib"
    ZIG="${ZIG:-zig}"
fi
out="$here/out"
mkdir -p "$out"
EXTRA=""
[ -n "${FF132_DIAG:-}" ] && EXTRA="-DFF132_DIAG"
# The log-only save-path hooks in ff132_save.c come with the diagnostics; the release leaves them out.
EXTRA="$EXTRA -DFF132_DIAG_SAVEPATH=$([ -n "${FF132_DIAG:-}" ] && echo 1 || echo 0)"
# Which source this DLL was built from, printed at the top of the log. Not __DATE__/__TIME__: the
# build is kept reproducible and clang refuses them. "+" marks a working tree with uncommitted changes.
rev="$(cd "$here" && git rev-parse --short HEAD 2>/dev/null || echo unknown)"
(cd "$here" && git diff --quiet HEAD -- . "$libspec" 2>/dev/null) || rev="$rev+"
EXTRA="$EXTRA -DFF132_BUILD_ID=\"$rev\""
"$ZIG" rc "$here/version.rc" "$out/version.res"
"$ZIG" cc -target x86-windows-gnu -shared -O2 $EXTRA \
    -I"$here" -I"$lib" \
    -o "$out/dinput8.dll" \
    "$here/dllmain.c" "$here/ff132_core.c" "$here/ff132_save.c" "$here/ff132_overlay.c" "$here/ff132_battle.c" \
    "$here/ff132_load.c" "$here/ff132_input.c" "$here/ff132_tidy.c" "$here/ff132_cats.c" \
    "$here/ff132_speed.c" "$here/ff132_encount.c" "$here/ff132_kill.c" "$here/ff132_autoretry.c" "$here/ff132_setup.c" "$here/ff132_text.c" \
    "$here/ff132_progress.c" "$here/ff132_slots.c" "$here/ff132_listgui.c" "$here/ff132_crash.c" \
    "$lib/ff13_hook.c" "$lib/ff13_ini.c" "$lib/ff13_keys.c" "$lib/ff13_crypt.c" "$here/dinput8.def" \
    "$out/version.res" \
    -lkernel32 -luser32 -lgdi32 -lshell32 -lole32
echo "built: $out/dinput8.dll${EXTRA:+ (with $EXTRA)}"
