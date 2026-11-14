#!/usr/bin/env bash
# Collect the files the PS4 runner loads into stage/game, ready to upload to /data/gdps4/game.
# Usage: bash tools/stage_ps4.sh "<GD 2.208 folder>" [--with-resources]
# Copies from your own install and your own Windows System32; nothing is downloaded.
set -euo pipefail
GD="${1:?path to your Geometry Dash 2.208 folder}"
OUT="$(dirname "$0")/../stage/game"
mkdir -p "$OUT"
for f in GeometryDash.exe libcocos2d.dll libExtensions.dll glew32.dll zlib1.dll libpng16.dll jpeg62.dll tiff.dll \
         pthreadVCE3.dll sqlite3.dll liblzma.dll turbojpeg.dll; do
  cp -v "$GD/$f" "$OUT/"
done
# Microsoft C/C++ runtimes, loaded as PE images by the runner (M4a).
for f in ucrtbase.dll msvcp140.dll vcruntime140.dll vcruntime140_1.dll; do
  cp -v "/c/Windows/System32/$f" "$OUT/"
done
if [[ "${2:-}" == "--with-resources" ]]; then
  cp -r "$GD/Resources" "$OUT/"
  echo "Resources copied."
fi
du -sh "$OUT"
echo "Upload the CONTENTS of $OUT to /data/gdps4/game on the PS4."
