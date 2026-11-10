#!/bin/sh
# Action Kart para Mac: una app normal ("Action Kart.app") que se abre con doble clic, sin instalar nada (SDL2 va
# dentro). Pide la ROM al abrirse la primera vez y guarda todo en ~/Library/Application Support/Action Kart.
#   sh mac/empaquetar.sh [carpeta]   -> <carpeta>/Action Kart.app (por defecto, build/mac)
# Hace falta lo mismo que para jugar en el Mac (Xcode, cmake) y tu ROM en rom/mkds.nds.
# La app lleva el juego traducido de TU copia: solo para tu familia, nunca en público.
set -e
cd "$(dirname "$0")/.."
[ -d /opt/homebrew/bin ] && PATH="/opt/homebrew/bin:$PATH"
[ -d /usr/local/bin ] && PATH="$PATH:/usr/local/bin"
SALIDA=${1:-build/mac}
[ -f rom/mkds.nds ] || { echo "Missing ROM: copy your own Mario Kart DS (Europe, AMCP) dump to rom/mkds.nds"; exit 1; }

# 1. la traducción y los objetos del juego (los mismos que para jugar en el Mac)
sh linux/preparar.sh

# 2. SDL2 estática para macOS (una vez), para que la app no necesite nada instalado
SDLV=2.30.9
D=ios/deps
S=$D/SDL2-$SDLV
mkdir -p $D
if [ ! -d $S ]; then
    echo "Downloading SDL2 $SDLV..."
    curl -fsSL -o $D/sdl.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-$SDLV/SDL2-$SDLV.tar.gz
    tar xzf $D/sdl.tar.gz -C $D
    rm $D/sdl.tar.gz
fi
LIB=$(find $D/sdl-macosx -name libSDL2.a 2> /dev/null | head -1)
if [ -z "$LIB" ]; then
    echo "Building SDL2 for macOS..."
    cmake -S $S -B $D/sdl-macosx -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
        -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF > $D/sdl-macosx.log 2>&1 \
        || { tail -20 $D/sdl-macosx.log; exit 1; }
    cmake --build $D/sdl-macosx -j "$(sysctl -n hw.ncpu)" >> $D/sdl-macosx.log 2>&1 || { tail -20 $D/sdl-macosx.log; exit 1; }
    LIB=$(find $D/sdl-macosx -name libSDL2.a | head -1)
fi
mkdir -p $D/sdlinc
[ -e $D/sdlinc/SDL2 ] || ln -s "../SDL2-$SDLV/include" $D/sdlinc/SDL2

# 3. el runtime otra vez, con las cabeceras de esta SDL y la huella de tu ROM (la app solo acepta esa ROM)
ROMCRC=$(python3 -c "import zlib; print(hex(zlib.crc32(open('rom/mkds.nds','rb').read())))")
OBJ=build/mac-app/obj
mkdir -p $OBJ
OPC="-O2 -w -Iruntime -I$S/include -I$D/sdlinc -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE -DGL_SILENCE_DEPRECATION -mmacosx-version-min=11.0"
n=$(($(sysctl -n hw.ncpu) / 2)); [ "$n" -lt 1 ] && n=1
echo "Compiling the runtime for the app..."
for f in runtime/*.c; do case "$f" in runtime/ventana.c) continue;; esac; echo "$f"; done | \
    OPC="$OPC" ROMCRC="$ROMCRC" OBJ="$OBJ" xargs -P "$n" -I{} sh -c 'clang $OPC -DROM_CRC=$ROMCRC -c {} -o $OBJ/$(basename {} .c).o'
for f in runtime/menu.cpp runtime/imgui/imgui.cpp runtime/imgui/imgui_draw.cpp runtime/imgui/imgui_tables.cpp \
         runtime/imgui/imgui_widgets.cpp runtime/imgui/imgui_impl_sdlrenderer2.cpp; do echo "$f"; done | \
    OPC="$OPC" OBJ="$OBJ" xargs -P "$n" -I{} sh -c 'clang $OPC -fno-exceptions -fno-rtti -fno-threadsafe-statics -c {} -o $OBJ/$(basename {} .cpp).o'

# 4. unir: el juego traducido (de build/obj) con este runtime y SDL dentro
APP="$SALIDA/Action Kart.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
clang++ -o "$APP/Contents/MacOS/ActionKart" build/obj/mkds*/*.o $OBJ/*.o "$LIB" -mmacosx-version-min=11.0 \
    -framework Cocoa -framework IOKit -framework CoreAudio -framework AudioToolbox -framework CoreVideo \
    -framework Carbon -framework ForceFeedback -framework GameController -framework Metal -framework QuartzCore \
    -framework CoreHaptics -framework OpenGL -framework AVFoundation -framework CoreFoundation -lz -liconv \
    -Wl,-stack_size,0x10000000

# 5. el icono y el Info.plist
I=build/mac-app/AppIcon.iconset
rm -rf $I; mkdir -p $I
for t in 16 32 128 256 512; do
    sips -z $t $t ios/icono/icono-1024.png --out $I/icon_${t}x${t}.png > /dev/null
    sips -z $((t * 2)) $((t * 2)) ios/icono/icono-1024.png --out $I/icon_${t}x${t}@2x.png > /dev/null
done
iconutil -c icns $I -o "$APP/Contents/Resources/AppIcon.icns"
cat > "$APP/Contents/Info.plist" << 'FIN'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleIdentifier</key><string>com.skyendo.actionkart.mac</string>
	<key>CFBundleName</key><string>Action Kart</string>
	<key>CFBundleDisplayName</key><string>Action Kart</string>
	<key>CFBundleExecutable</key><string>ActionKart</string>
	<key>CFBundleIconFile</key><string>AppIcon</string>
	<key>CFBundlePackageType</key><string>APPL</string>
	<key>CFBundleShortVersionString</key><string>0.1</string>
	<key>CFBundleVersion</key><string>1</string>
	<key>LSMinimumSystemVersion</key><string>11.0</string>
	<key>NSHighResolutionCapable</key><true/>
	<key>NSAppSleepDisabled</key><true/>
	<key>NSMicrophoneUsageDescription</key><string>Some parts of the game listen to the microphone.</string>
	<key>NSLocalNetworkUsageDescription</key><string>Used to race with players on the same network.</string>
</dict>
</plist>
FIN

# 6. firma local (sin ella no abre en Mac con chip de Apple)
codesign -s - --force --deep "$APP" > /dev/null 2>&1
echo "Done: $APP"
