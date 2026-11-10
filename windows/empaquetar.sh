#!/bin/sh
# Action Kart para Windows (64 bits), compilado desde el Mac: una carpeta con "Action Kart.exe" y SDL2.dll, sin
# instalar nada. Pide la ROM la primera vez (la copia en rom\ junto al .exe) y guarda allí sus ajustes.
#   sh windows/empaquetar.sh [carpeta]   -> <carpeta> (por defecto, build/win64/Action Kart)
# Hace falta lo mismo que para el Mac (python3, clang) y mingw-w64 (brew install mingw-w64), y tu ROM en rom/mkds.nds.
# WIN_SIN_PREPARAR=1 se salta linux/preparar.sh (la traducción ya hecha en generado/).
# La carpeta lleva el juego traducido de TU copia: solo para tu familia, nunca en público.
set -e
cd "$(dirname "$0")/.."
[ -d /opt/homebrew/bin ] && PATH="/opt/homebrew/bin:$PATH"
[ -d /usr/local/bin ] && PATH="$PATH:/usr/local/bin"
SALIDA=${1:-build/win64/Action Kart}
[ -f rom/mkds.nds ] || { echo "Missing ROM: copy your own Mario Kart DS (Europe, AMCP) dump to rom/mkds.nds"; exit 1; }
command -v x86_64-w64-mingw32-gcc > /dev/null || { echo "Missing mingw-w64: brew install mingw-w64"; exit 1; }

# 1. la traducción del juego (la misma que para el Mac)
[ -n "$WIN_SIN_PREPARAR" ] && [ -d generado ] || sh linux/preparar.sh

# 2. SDL2 para mingw (la oficial, una vez)
SDLV=2.30.9
D=windows/deps
S=$D/SDL2-$SDLV/x86_64-w64-mingw32
if [ ! -f $S/bin/SDL2.dll ]; then
    echo "Downloading SDL2 $SDLV for Windows..."
    mkdir -p $D
    curl -fsSL -o $D/sdl.tar.gz https://github.com/libsdl-org/SDL/releases/download/release-$SDLV/SDL2-devel-$SDLV-mingw.tar.gz
    tar xzf $D/sdl.tar.gz -C $D
    rm $D/sdl.tar.gz
fi

# 3. compilar con clang para Windows (x86_64-w64-mingw32), con las cabeceras de mingw-w64
TC=$(x86_64-w64-mingw32-gcc -print-sysroot)/mingw
CC="clang --target=x86_64-w64-mingw32 --sysroot=$TC"
ROMCRC=$(python3 -c "import zlib; print(hex(zlib.crc32(open('rom/mkds.nds','rb').read())))")
OBJ=build/win64/obj
OPC="-O2 -w -Iruntime -D_GNU_SOURCE"
SDLI="-I$S/include -I$S/include/SDL2 -DMKDS_SDL -DROM_CRC=$ROMCRC"
n=$(($(sysctl -n hw.ncpu 2> /dev/null || nproc) - 1)); [ "$n" -lt 1 ] && n=1
lista() {
    for d in generado/*/; do
        d=$(basename "$d"); mkdir -p $OBJ/$d
        for f in generado/$d/*.c; do
            o=$OBJ/$d/$(basename "$f" .c).o
            [ -f "$o" ] && [ "$o" -nt "$f" ] || echo "$f $o $d"
        done
    done
    mkdir -p $OBJ/runtime
    for f in runtime/*.c runtime/menu.cpp runtime/imgui/imgui.cpp runtime/imgui/imgui_draw.cpp \
             runtime/imgui/imgui_tables.cpp runtime/imgui/imgui_widgets.cpp runtime/imgui/imgui_impl_sdlrenderer2.cpp; do
        case "$f" in runtime/ventana.c|runtime/fibra_ios.c) continue;; esac  # la ventana es la de SDL; fibra_ios, del iPhone
        b=$(basename "$f"); o=$OBJ/runtime/${b%.*}.o
        # el runtime siempre (es poco y lleva la huella de la ROM); el juego traducido solo si cambia
        echo "$f $o runtime"
    done
}
echo "Compiling for Windows with $n cores (the first time takes a while)..."
lista | CC="$CC" OPC="$OPC" SDLI="$SDLI" xargs -P "$n" -L 1 sh -c '
    case "$3" in
        runtime) case "$1" in *.cpp) c="$CC -x c++ -fno-exceptions -fno-rtti -fno-threadsafe-statics -include math.h";; *) c="$CC";; esac
                 extra="$SDLI";;
        mkds7) c="$CC"; extra="-DCPU7 -Igenerado/$3";;
        *) c="$CC"; extra="-Igenerado/$3";;
    esac
    $c $OPC $extra -c "$1" -o "$2" || { echo "ERROR in $1"; exit 255; }' sh

# 4. el icono (PNG dentro de un .ico, lo admite Windows Vista y posteriores) y unir: 256 MB de pila, como en el Mac;
# sin consola. SDL2 va dentro del .exe (-static coge libSDL2.a): no hace falta SDL2.dll
mkdir -p "$SALIDA"
sips -z 256 256 ios/icono/icono-1024.png --out $OBJ/icono.png > /dev/null
python3 -c "
import struct; d = open('$OBJ/icono.png', 'rb').read()
open('$OBJ/icono.ico', 'wb').write(struct.pack('<HHHBBBBHHII', 0, 1, 1, 0, 0, 0, 0, 1, 32, len(d), 22) + d)"
echo '1 ICON "icono.ico"' > $OBJ/icono.rc
(cd $OBJ && x86_64-w64-mingw32-windres icono.rc -O coff -o icono.res.o)
RES=$OBJ/icono.res.o
x86_64-w64-mingw32-gcc -o "$SALIDA/Action Kart.exe" $OBJ/mkds*/*.o $OBJ/runtime/*.o $RES -L$S/lib -lSDL2 \
    -mwindows -static -Wl,--stack,0x10000000 \
    -lopengl32 -lwinmm -lws2_32 -liphlpapi -lwinhttp -lcomdlg32 -lgdi32 -luser32 -lshell32 -lole32 -loleaut32 \
    -limm32 -lversion -lsetupapi -lm
x86_64-w64-mingw32-strip "$SALIDA/Action Kart.exe"
rm -f "$SALIDA/SDL2.dll"
cp windows/LEEME.txt "$SALIDA/README.txt"
echo "Done: $SALIDA"
