#!/usr/bin/env bash
# FoxSearch setup – run this once after unzipping
set -e

echo "Setting up FoxSearch project structure..."

mkdir -p source cia_assets romfs

# Move all source files
for f in main.c browser.c browser.h js_engine.c js_engine.h \
          net.c net.h bookmarks.c bookmarks.h html.c html.h js.h; do
    [ -f "$f" ] && mv "$f" source/ && echo "  moved $f -> source/"
done

# Move CIA assets
for f in build_cia.sh gen_assets.py gen_binaries.py FoxSearch.rsf; do
    [ -f "$f" ] && mv "$f" cia_assets/ && echo "  moved $f -> cia_assets/"
done

echo ""
echo "Cloning QuickJS (required for JS engine)..."
git clone --depth 1 https://github.com/bellard/quickjs.git source/quickjs

echo ""
echo "Generating icon and banner..."
python3 cia_assets/gen_assets.py

echo ""
echo "Done! Now run: make cia"
