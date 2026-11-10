#!/bin/sh
# Action Kart para iPhone y iPad: de tu ROM a la app, en un paso (como PadMint de SpaghettiPad, pero la ROM va aquí:
# el código del juego sale de tu ROM).
#   sh ios/construir.sh            -> build/ActionKart.ipa, sin firmar y sin la ROM (la pide al abrirse): instálala con AltStore,
#                                     SideStore o Sideloadly (la firman con tu Apple ID)
#   sh ios/construir.sh simulador  -> la app en el simulador de iOS de este Mac, para probar
# Hace falta: Xcode (con el SDK de iOS) y tu ROM en rom/mkds.nds (Mario Kart DS europeo, AMCP).
# La .ipa lleva el juego traducido de TU copia: es solo para tus aparatos, no la compartas.
set -e
cd "$(dirname "$0")/.."
[ -d /opt/homebrew/bin ] && PATH="/opt/homebrew/bin:$PATH"  # cmake de Homebrew (Mac con chip de Apple)
[ -d /usr/local/bin ] && PATH="$PATH:/usr/local/bin"
DESTINO=${1:-iphone}
[ -f rom/mkds.nds ] || { echo "Falta la ROM: copia tu Mario Kart DS europeo (AMCP) a rom/mkds.nds"; exit 1; }
command -v xcrun > /dev/null || { echo "Falta Xcode (con el SDK de iOS)"; exit 1; }

# 1. la traducción de tu ROM a C (la misma que para el Mac; si ya está hecha, no se repite)
sh linux/preparar.sh
[ -d generado ] || { echo "No está la traducción (generado/): borra build/traduccion.txt y repite"; exit 1; }

# 2. SDL2 para iOS (una vez; queda en ios/deps, fuera de git)
SDLV=2.30.9
D=ios/deps
S=$D/SDL2-$SDLV
if [ "$DESTINO" = simulador ]; then SDK=iphonesimulator; TGT=arm64-apple-ios15.0-simulator; else SDK=iphoneos; TGT=arm64-apple-ios15.0; fi
mkdir -p $D
if [ ! -d $S ]; then
    echo "Bajando SDL2 $SDLV..."
    curl -fsSL -o $D/sdl.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-$SDLV/SDL2-$SDLV.tar.gz
    tar xzf $D/sdl.tar.gz -C $D
    rm $D/sdl.tar.gz
fi
LIB=$(find $D/sdl-$SDK -name libSDL2.a 2> /dev/null | head -1)
if [ -z "$LIB" ]; then
    # con CMake (no con su proyecto de Xcode: ese compila los sombreadores de Metal y Xcode 26 ya no trae esa
    # herramienta; CMake usa los que vienen hechos en SDL)
    command -v cmake > /dev/null || { echo "Falta cmake (brew install cmake)"; exit 1; }
    echo "Compilando SDL2 para $SDK..."
    cmake -S $S -B $D/sdl-$SDK -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=$SDK -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=15.0 -DCMAKE_BUILD_TYPE=Release -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST=OFF \
        > $D/sdl-$SDK.log 2>&1 || { tail -20 $D/sdl-$SDK.log; exit 1; }
    cmake --build $D/sdl-$SDK -j "$(sysctl -n hw.ncpu)" >> $D/sdl-$SDK.log 2>&1 || { tail -20 $D/sdl-$SDK.log; exit 1; }
    LIB=$(find $D/sdl-$SDK -name libSDL2.a | head -1)
fi
mkdir -p $D/sdlinc
[ -e $D/sdlinc/SDL2 ] || ln -s "../SDL2-$SDLV/include" $D/sdlinc/SDL2  # el código pide <SDL2/SDL.h>

# la huella de tu ROM: la app pide la ROM al abrirse y solo acepta esta (el código va traducido de ella)
ROMCRC=$(python3 -c "import zlib,sys; print(hex(zlib.crc32(open('rom/mkds.nds','rb').read())))")

# 3. compilar (en paralelo; solo lo que haya cambiado)
SYSROOT=$(xcrun --sdk $SDK --show-sdk-path)
OBJ=build/ios-$SDK/obj
mkdir -p $OBJ
n=${NUCLEOS:-$(($(sysctl -n hw.ncpu) / 2))}  # la mitad de los núcleos (NUCLEOS=n para cambiarlo): el Mac sigue usable
[ "$n" -lt 1 ] && n=1
lista() {
    for d in generado/*/; do
        d=$(basename "$d"); mkdir -p $OBJ/$d
        for f in generado/$d/*.c; do
            o=$OBJ/$d/$(basename "$f" .c).o
            [ -f "$o" ] && [ "$o" -nt "$f" ] || echo "$f $o $d"
        done
    done
    mkdir -p $OBJ/runtime
    for f in runtime/*.m; do  # (Objective-C: solo en iOS, p. ej. pedir la ROM)
        o=$OBJ/runtime/$(basename "$f" .m).o
        [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt ios/construir.sh ] || echo "$f $o objc"
    done
    for f in runtime/*.c $S/src/main/uikit/SDL_uikit_main.c; do
        case "$f" in runtime/ventana.c) continue;; esac  # (la ventana de Windows)
        o=$OBJ/runtime/$(basename "$f" .c).o
        [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt runtime/compat.h ] && [ "$o" -nt runtime/menu.h ] || echo "$f $o runtime"
    done
    for f in runtime/menu.cpp runtime/imgui/imgui.cpp runtime/imgui/imgui_draw.cpp runtime/imgui/imgui_tables.cpp \
             runtime/imgui/imgui_widgets.cpp runtime/imgui/imgui_impl_sdlrenderer2.cpp; do
        o=$OBJ/runtime/$(basename "$f" .cpp).o
        [ -f "$o" ] && [ "$o" -nt "$f" ] && [ "$o" -nt runtime/menu.h ] || echo "$f $o menu"
    done
}
total=$(lista | wc -l)
if [ "$total" -gt 0 ]; then
    echo "Compilando $total archivos para $SDK con $n núcleos..."
    lista | SYSROOT="$SYSROOT" TGT="$TGT" SDK="$SDK" S="$S" D="$D" ROMCRC="$ROMCRC" xargs -P "$n" -L 1 sh -c '
        extra=""
        [ "$3" = mkds7 ] && extra="-DCPU7"
        [ "$3" != runtime ] && [ "$3" != menu ] && extra="$extra -Igenerado/$3"
        [ "$3" = menu ] && extra="-fno-exceptions -fno-rtti -fno-threadsafe-statics"
        [ "$3" = objc ] && extra="-fobjc-arc -DROM_CRC=$ROMCRC"
        xcrun --sdk "$SDK" clang -target "$TGT" -isysroot "$SYSROOT" -O2 -w -Iruntime -I"$S/include" -I"$D/sdlinc" \
            -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -D_DARWIN_C_SOURCE $extra -c "$1" -o "$2" || { echo "ERROR en $1"; exit 255; }' sh
fi

# 4. unir (la pila del hilo principal, 256 MB, como en el Mac)
APP=build/ios-$SDK/Payload/ActionKart.app
mkdir -p $APP
xcrun --sdk $SDK clang++ -target $TGT -isysroot "$SYSROOT" -o $APP/ActionKart $OBJ/*/*.o "$LIB" \
    -framework UIKit -framework Foundation -framework Metal -framework QuartzCore -framework CoreGraphics \
    -framework AVFoundation -framework AudioToolbox -framework CoreAudio -framework GameController \
    -framework CoreMotion -framework CoreHaptics -framework OpenGLES -framework CoreBluetooth -framework CoreFoundation \
    -framework UniformTypeIdentifiers -lz \
    -Wl,-stack_size,0x10000000

# 5. la app: el programa, su Info.plist y el icono. La ROM no va dentro: la app la pide al abrirse (ios_rom.m)
cp ios/Info.plist $APP/Info.plist
cp ios/icono/AppIcon*.png $APP/  # el icono (ios/icono: diseño propio de Action Kart)
rm -f $APP/mkds.nds
if [ "$DESTINO" = simulador ]; then
    codesign -s - --force $APP > /dev/null 2>&1
    xcrun simctl list devices booted | grep -q Booted || {
        dev=$(xcrun simctl list devices available | grep -E 'iPhone' | head -1 | sed -E 's/.*\(([0-9A-F-]+)\).*/\1/')
        xcrun simctl boot "$dev"
        open -a Simulator
    }
    xcrun simctl install booted $APP
    xcrun simctl launch booted com.skyendo.actionkart
    echo "Abierta en el simulador."
else
    rm -f build/ActionKart.ipa
    (cd build/ios-$SDK && zip -qr ../ActionKart.ipa Payload)
    echo "Lista: build/ActionKart.ipa (sin firmar). Instálala con AltStore, SideStore o Sideloadly. Solo para tus aparatos."
fi
