#!/usr/bin/env bash
# Builds addon/native/cooldown_lap.dll and dist/vmknolle.cooldown-lap.swoftyaddon with zig (x86_64-windows-gnu).
# Needs the Swofty addon SDK headers (swofty_api.h + swofty/*.hpp, MIT, from the Swofty addon template
# templates/addon/native/include): put them in sdk/include or set SWOFTY_SDK=<folder>.
# The SDK copy is patched with sdk-patch/*.patch in build/sdk (your SDK folder is not changed).
set -euo pipefail
cd "$(dirname "$(readlink -f "$0")")"
ZIG=${ZIG:-zig}
SDK=${SWOFTY_SDK:-sdk/include}
[ -f "$SDK/swofty_api.h" ] && [ -d "$SDK/swofty" ] || { echo "Swofty SDK headers not found in $SDK (see README)"; exit 1; }
rm -rf build/sdk && mkdir -p build/sdk addon/native dist
cp -r "$SDK/swofty_api.h" "$SDK/swofty" build/sdk/
for p in sdk-patch/*.patch; do patch -s -d build/sdk -p1 < "$p"; done
export ZIG_GLOBAL_CACHE_DIR=$PWD/build/zig-cache ZIG_LOCAL_CACHE_DIR=$PWD/build/zig-cache
"$ZIG" c++ -target x86_64-windows-gnu -std=c++23 -O2 -Wall -Wextra -DSWOFTY_PUBLIC -DRS_PUBLIC \
    -include src/local_sigs.hpp -Ibuild/sdk -Iinclude -Isrc -shared -s -o addon/native/cooldown_lap.dll \
    src/cooldown_lap.cpp -lshell32 -luser32 -lgdi32 -lwinmm
rm -f addon/native/cooldown_lap.lib addon/native/cooldown_lap.pdb
(cd addon && rm -f ../dist/vmknolle.cooldown-lap.swoftyaddon && \
 python3 -c "import zipfile;z=zipfile.ZipFile('../dist/vmknolle.cooldown-lap.swoftyaddon','w',zipfile.ZIP_DEFLATED);[z.write(f,f) for f in ('swofty.json','README.md','native/cooldown_lap.dll')]")
echo "built addon/native/cooldown_lap.dll and dist/vmknolle.cooldown-lap.swoftyaddon"
