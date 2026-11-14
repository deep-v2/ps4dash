#!/usr/bin/env bash
# Build the runner PKG on Windows (Git Bash) with LLVM + OpenOrbis.
# Same steps as OpenOrbis' sample Makefile, which only supports Linux/macOS.
set -euo pipefail
cd "$(dirname "$0")"

TC="${OO_PS4_TOOLCHAIN:?set OO_PS4_TOOLCHAIN to your OpenOrbis PS4Toolchain folder}"
TC="$(cygpath -u "$TC")"
LLVM="/c/Program Files/LLVM/bin"
SAMPLE="$TC/samples/hello_world"

TITLE="Geometry Dash (gdps4)"
VERSION="1.00"
TITLE_ID="GDPS00004"
CONTENT_ID="IV0000-GDPS00004_00-GDPS4RUNNER00000"

OUT=build
mkdir -p "$OUT/obj" "$OUT/pkg/sce_sys/about" "$OUT/pkg/sce_module"
rm -f "$OUT"/obj/*.o   # link exactly the sources listed below

BUILD_ID="$(date +%Y%m%d-%H%M)-$(git -C ../.. rev-parse --short HEAD 2>/dev/null || echo nogit)"
CXXFLAGS=(-DGDPS4_BUILD="\"$BUILD_ID\"" --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -std=gnu++17
          -isysroot "$TC" -isystem "$TC/include/c++/v1" -isystem "$TC/include")
for src in main.cpp desktop.cpp gl_swgl.cpp ../../swgl/swgl.cpp ../../swgl/shaders.cpp ../../loader/src/pe_loader.cpp ../../win32/kernel32.cpp ../../win32/platform_posix.cpp; do
  "$LLVM/clang++" "${CXXFLAGS[@]}" -c "$src" -o "$OUT/obj/$(basename "${src%.cpp}").o"
done

"$LLVM/ld.lld" "$OUT"/obj/*.o -o "$OUT/runner.elf" -m elf_x86_64 -pie --script "$TC/link.x" \
  --eh-frame-hdr -L"$TC/lib" -lc -lkernel -lc++ -lScePad -lSceUserService -lSceVideoOut -lSceSysmodule -lSceSystemService "$TC/lib/crt1.o"

BIN="$TC/bin/windows"
export DOTNET_ROLL_FORWARD=Major   # PkgTool.Core targets .NET Core 3.0; run it on any newer runtime
"$BIN/create-fself.exe" -in="$OUT/runner.elf" -out="$OUT/runner.oelf" --eboot "$OUT/pkg/eboot.bin" \
  --paid 0x3800000000000011

cp "$SAMPLE/sce_sys/icon0.png" "$OUT/pkg/sce_sys/"
cp "$SAMPLE/sce_sys/about/right.sprx" "$OUT/pkg/sce_sys/about/"
cp "$SAMPLE/sce_module/"*.prx "$OUT/pkg/sce_module/"

cd "$OUT/pkg"
SFO=sce_sys/param.sfo
"$BIN/PkgTool.Core.exe" sfo_new $SFO
set_sfo() { "$BIN/PkgTool.Core.exe" sfo_setentry $SFO "$1" --type "$2" --maxsize "$3" --value "$4"; }
set_sfo APP_TYPE Integer 4 1
set_sfo APP_VER Utf8 8 "$VERSION"
set_sfo ATTRIBUTE Integer 4 0
set_sfo CATEGORY Utf8 4 gd
set_sfo CONTENT_ID Utf8 48 "$CONTENT_ID"
set_sfo DOWNLOAD_DATA_SIZE Integer 4 0
set_sfo SYSTEM_VER Integer 4 0
set_sfo TITLE Utf8 128 "$TITLE"
set_sfo TITLE_ID Utf8 12 "$TITLE_ID"
set_sfo VERSION Utf8 8 "$VERSION"

FILES="eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png $(ls sce_module/* | tr '\n' ' ')"
"$BIN/create-gp4.exe" -out pkg.gp4 --content-id="$CONTENT_ID" --files "$FILES"
"$BIN/PkgTool.Core.exe" pkg_build pkg.gp4 ..
ls -la ../*.pkg
echo "build id: $BUILD_ID"
