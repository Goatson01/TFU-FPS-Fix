#!/bin/sh
# Builds the DLL and creates the release zip: dist/TFU-FPS-Fix-<version>.zip
set -e
cd "$(dirname "$0")"
VERSION="${1:-1.0}"
./build.sh
NAME="TFU-FPS-Fix-$VERSION"
rm -rf "build/$NAME" "dist/$NAME.zip"
mkdir -p "build/$NAME"
cp build/dinput8.dll dist/tfufix.ini dist/README.txt LICENSE "build/$NAME/"
(cd build && python3 -c "
import zipfile, os, sys
name = sys.argv[1]
with zipfile.ZipFile(os.path.join('..', 'dist', name + '.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
    for f in sorted(os.listdir(name)):
        z.write(os.path.join(name, f))
" "$NAME")
echo "Created dist/$NAME.zip"
