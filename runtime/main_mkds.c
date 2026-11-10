// Lanzador de Mario Kart DS recompilado.
// Deja la memoria como la dejaría la DS al encender (código de los dos
// procesadores, cabecera de la ROM) y los arranca. El hardware está en ds.c.
#include "compat.h"  // antes que cpu.h: su macro R choca con windows.h
#include "ds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>  // mkdir (iOS)

void apunta_fallo(const char *que);
extern volatile uint32_t g_actividad;

// ---- trucos --------------------------------------------------------------
uint32_t g_trucos;

// ¿Está desbloqueado el elemento r0? (personajes, karts, circuitos...): sí
int truco_desbloqueado(Cpu *c) {
    if (!(g_trucos & 1)) return 0;
    c->r[0] = 1;
    return 1;
}

// Cilindrada más alta disponible: 4 = 50, 100, 150 y espejo
int truco_cilindradas(Cpu *c) {
    if (!(g_trucos & 1)) return 0;
    c->r[0] = 4;
    return 1;
}

// Misiones: el menú (overlay 1, 0219F3A4) guarda en objeto+0x4A el último nivel abierto (0 = solo el 1; se abre
// uno al superar las 9 misiones del anterior, y el 7 pide además rango ★ en todas). El objeto está en 021CA410.
int truco_niveles_mision(Cpu *c) {
    if (!(g_trucos & 1)) return 0;
    uint32_t obj = rd32_9(0x021CA410);
    if ((obj & 0xFF000000) == 0x02000000) wr16_9(obj + 0x4A, 6);
    return 1;
}

// y el jefe de cada nivel (021A0E34): objeto (021CA414) + 0xA0 = 1 si las misiones 1-8 están superadas
int truco_jefe_mision(Cpu *c) {
    if (!(g_trucos & 1)) return 0;
    uint32_t obj = rd32_9(0x021CA414);
    if ((obj & 0xFF000000) == 0x02000000) wr32_9(obj + 0xA0, 1);
    return 1;
}

// Carreras en multijugador: el intercambio de cada fotograma lo hace wifi.c con adelanto (ver allí)
void wifi_ds_empieza(uint32_t info);
int wifi_ds_paso(uint32_t info, uint32_t envio, uint32_t recibe);

// Conexión Wi-Fi de Nintendo: con MKDS_WIFI_LOG apunta cada cambio de fase del WCM (021A57FC) y quién lo pide
int traza_wcm_fase(Cpu *c) {
    static int log = -1;
    if (log < 0) log = getenv("MKDS_WIFI_LOG") != NULL;
    if (log) fprintf(stderr, "[wcm] fase %u (desde %08X, %lu ms)\n", c->r[0], c->r[14], (unsigned long)GetTickCount());
    return 0;
}

int traza_llamada(Cpu *c) {  // DEPURA_O0 (traducir_mkds.py): una llamada, con sus argumentos
    static int log = -1;
    if (log < 0) log = getenv("MKDS_WIFI_LOG") != NULL;
    if (log)
        fprintf(stderr, "[llamada] %08X(%08X, %08X, %08X, %08X) desde %08X\n", c->func & ~1u, c->r[0], c->r[1], c->r[2],
                c->r[3], c->r[14]);
    return 0;
}

int gancho_ds_empieza(Cpu *c) {  // WM_StartDataSharing: se anota y sigue el original
    wifi_ds_empieza(c->r[0]);
    return 0;
}

int gancho_ds_paso(Cpu *c) {  // WM_StepDataSharing
    int r = wifi_ds_paso(c->r[0], c->r[1], c->r[2]);
    if (r < 0) return 0;
    c->r[0] = (uint32_t)r;
    return 1;
}

static void volcado(Cpu *c) {
    fprintf(stderr, "  %s en la función %08X (%s)\n", c->id ? "ARM7" : "ARM9", c->func & ~1u,
            (c->func & 1) ? "Thumb" : "ARM");
    for (int i = 0; i < 16; i++)
        fprintf(stderr, "    r%-2d %08X%s", i, c->r[i], i % 4 == 3 ? "\n" : "");
    extern void ds_info(int id);
    ds_info(c->id);
    if (c->id) {
        fprintf(stderr, "    CPSR %08X  pila:", cpu_get_cpsr(c));
        for (int i = 0; i < 24; i++) fprintf(stderr, " %08X", rd32_7(c->r[13] + 4 * i));
        fprintf(stderr, "\n");
    }
    if (!c->id) {  // hilo actual del SDK (OSi_CurrentThread) y su prioridad
        uint32_t t = rd32_9(rd32_9(0x0216FF54));
        fprintf(stderr, "    hilo actual del juego: %08X\n", t);
        fprintf(stderr, "    datos de usuario en 027FFC80:");  // nombre (UTF-16 en +6, longitud en +1A)
        for (int i = 0; i < 0x20; i += 2) fprintf(stderr, " %04X", rd16_9(0x027FFC80 + i));
        fprintf(stderr, "\n");
        fprintf(stderr, "    CPSR %08X  pila:", cpu_get_cpsr(c));
        for (int i = 0; i < 24; i++) fprintf(stderr, " %08X", rd32_9(c->r[13] + 4 * i));
        fprintf(stderr, "\n");
        // MKDS_BUSCA="texto": dónde está ese texto en UTF-16 en la memoria principal (p. ej. el nombre)
        const char *busca = getenv("MKDS_BUSCA");
        if (busca && *busca) {
            size_t n = strlen(busca);
            fprintf(stderr, "    \"%s\" en UTF-16:", busca);
            for (uint32_t a = 0x02000000; a + 2 * n <= 0x02400000; a += 2) {
                size_t k = 0;
                while (k < n && rd16_9(a + 2 * (uint32_t)k) == (uint8_t)busca[k]) k++;
                if (k == n) fprintf(stderr, " %08X", a);
            }
            fprintf(stderr, "\n");
        }
        // MKDS_VER="02174E00 *02174E00": palabras a mirar (con * se sigue el puntero)
        const char *ver = getenv("MKDS_VER");
        while (ver && *ver) {
            int ptr = *ver == '*';
            uint32_t a = (uint32_t)strtoul(ver + ptr, (char **)&ver, 16);
            if (ptr) a = rd32_9(a);
            fprintf(stderr, "    [%08X] = %08X\n", a, rd32_9(a));
            while (*ver == ' ') ver++;
        }
    }
}

// ---- vigilante: si el juego se atasca sin tocar el hardware, dice dónde ----
static DWORD WINAPI vigilante(LPVOID p) {
    (void)p;
    uint32_t antes = ~0u;
    const char *lim = getenv("MKDS_LIMITE");  // segundos; al pasar, vuelca el estado y sale
    int limite = lim ? atoi(lim) : 0, pasados = 0;
    for (;;) {
        Sleep(3000);
        pasados += 3;
        if (limite && pasados >= limite) {
            fprintf(stderr, "\n[mkds] %d s: así están los dos procesadores\n", pasados);
            volcado(&g_cpu9);
            volcado(&g_cpu7);
            const char *vr = getenv("MKDS_VUELCA_RAM");  // también al acabar el tiempo: memoria para estudiarla
            if (vr && *vr) {
                extern uint8_t g_itcm[0x8000], g_dtcm[0x4000];
                FILE *m = fopen(vr, "wb");
                if (m) { fwrite(g_main, 1, 0x400000, m); fwrite(g_itcm, 1, 0x8000, m); fwrite(g_dtcm, 1, 0x4000, m); fclose(m); }
            }
            ExitProcess(10);
        }
#if defined(_WIN32) && defined(MKDS_SDL)
        if (pasados == 6 || pasados == 12 || pasados == 21) {  // para mkds_log.txt: cómo va el arranque
            extern uint64_t gx_frames(void);
            fprintf(stderr, "\n[diag] %d s: escenas 3D %llu\n", pasados, (unsigned long long)gx_frames());
            volcado(&g_cpu9);
            volcado(&g_cpu7);
        }
#endif
        if (g_actividad == antes) {
            fprintf(stderr, "\n[mkds] atascado 3 s sin tocar el hardware\n");
            volcado(&g_cpu9);
            volcado(&g_cpu7);
            apunta_fallo("atascado 3 s sin tocar el hardware");
            ExitProcess(7);
        }
        antes = g_actividad;
    }
}

// Las salidas del juego que no son un fallo del PC (código sin traducir, instrucción no soportada, atascado...)
// también se apuntan en fallos.txt: desde el launcher no se ve la salida de errores y si no, no queda rastro
void apunta_fallo(const char *que) {
    FILE *f = fopen("fallos.txt", "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm *l = localtime(&t);
    char fecha[32] = "?";
    if (l) strftime(fecha, sizeof fecha, "%Y-%m-%d %H:%M:%S", l);
    fprintf(f, "%s  ARM9 en %08X (función %08X, lr %08X)  ARM7 en %08X (función %08X)\n[mkds] %s\n\n", fecha,
            g_cpu9.r[15], g_cpu9.func, g_cpu9.r[14], g_cpu7.r[15], g_cpu7.func, que);
    fclose(f);
}

#ifdef _WIN32
static LONG WINAPI al_romperse(EXCEPTION_POINTERS *e) {
    DWORD cod = e->ExceptionRecord->ExceptionCode;
    if (cod != EXCEPTION_ACCESS_VIOLATION && cod != EXCEPTION_STACK_OVERFLOW &&
        cod != EXCEPTION_ILLEGAL_INSTRUCTION && cod != EXCEPTION_INT_DIVIDE_BY_ZERO)
        return EXCEPTION_CONTINUE_SEARCH;
    // en qué módulo (mkds.exe, el controlador de la tarjeta gráfica...) y desde dónde se llegó ahí
    void *dir = e->ExceptionRecord->ExceptionAddress;
    HMODULE mod = NULL;
    char nombre[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)dir, &mod))
        GetModuleFileNameA(mod, nombre, MAX_PATH);
    char txt[512];
    int k = snprintf(txt, sizeof txt, "[mkds] fallo %08lX en el PC (%s+%llX)\n", e->ExceptionRecord->ExceptionCode,
                     strrchr(nombre, '\\') ? strrchr(nombre, '\\') + 1 : nombre, (unsigned long long)((char *)dir - (char *)mod));
#ifdef _M_X64
    {  // la pila del hilo que falló: direcciones de vuelta que caen dentro de mkds.exe
        char *base = (char *)GetModuleHandle(NULL), **pila = (char **)e->ContextRecord->Rsp;
        k += snprintf(txt + k, sizeof txt - (size_t)k, "    llamadas en mkds.exe:");
        for (int i = 0, n = 0; i < 4096 && n < 12 && k < (int)sizeof txt - 24; i++)
            if (pila[i] > base && pila[i] < base + 0x8000000) {
                k += snprintf(txt + k, sizeof txt - (size_t)k, " +%llX", (unsigned long long)(pila[i] - base));
                n++;
            }
        snprintf(txt + k, sizeof txt - (size_t)k, "\n");
    }
#endif
    fprintf(stderr, "\n%s", txt);
    {  // también en fallos.txt, junto al juego: desde el launcher no se ve la salida de errores (para los issues)
        FILE *f = fopen("fallos.txt", "a");
        if (f) {
            SYSTEMTIME t;
            GetLocalTime(&t);
            fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  ARM9 en %08X  ARM7 en %08X\n%s\n", t.wYear, t.wMonth, t.wDay,
                    t.wHour, t.wMinute, t.wSecond, g_cpu9.r[15], g_cpu7.r[15], txt);
            fclose(f);
        }
    }
    volcado(&g_cpu9);
    volcado(&g_cpu7);
    fflush(stderr);
    ExitProcess(9);
}
#else
#include <signal.h>
static void al_romperse(int sen) {
    fprintf(stderr, "\n[mkds] fallo en el PC (señal %d)\n", sen);
    FILE *f = fopen("fallos.txt", "a");  // como en Windows: para los issues
    if (f) {
        fprintf(f, "[mkds] fallo en el PC (señal %d)  ARM9 en %08X  ARM7 en %08X\n\n", sen, g_cpu9.r[15], g_cpu7.r[15]);
        fclose(f);
    }
    volcado(&g_cpu9);
    volcado(&g_cpu7);
    fflush(stderr);
    _exit(9);
}
#endif

static void *carga(const char *ruta, size_t *n) {
    FILE *f = fopen(ruta, "rb");
    if (!f) { perror(ruta); exit(1); }
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc(*n);
    fread(p, 1, *n, f);
    fclose(f);
    return p;
}

// Nombre del jugador (NOMBRE en ajustes.ini, lo pone el launcher). La partida cargada está en 0x023CE2C8
// (empieza por "NKSY") y el nombre va detrás, en UTF-16, hasta 10 letras. El juego lo copia del firmware al
// crear la partida; la del usuario se creó con el nombre vacío y el juego no deja cambiarlo. Se escribe en
// cada fotograma si no coincide; al guardar, el juego lo deja ya en la partida.
static int nombre_listo;
void nombre_cambia(void) { nombre_listo = 0; }  // el menú ha cambiado MKDS_NOMBRE

void nombre_por_frame(void) {
    static uint16_t nombre[10];
    int listo = nombre_listo;
    if (!listo) {
        listo = 1;
        const char *n = getenv("MKDS_NOMBRE");
        if (!n || !*n) n = "ActionKart";  // sin NOMBRE en ajustes.ini (en iOS, la primera vez): el de la app
        wchar_t w[11] = {0};
        if (n && *n) MultiByteToWideChar(CP_UTF8, 0, n, -1, w, 11);
        w[10] = 0;
        for (int i = 0; i < 10; i++) nombre[i] = (uint16_t)w[i];
        if (!nombre[0]) listo = 2;  // sin nombre: no se toca nada
        nombre_listo = listo;
    }
    if (listo == 2) return;
    uint8_t *p = g_main + (0x023CE2C8 & 0x3FFFFF);
    if (memcmp(p, "NKSY", 4)) return;  // la partida aún no está cargada
    if (memcmp(p + 4, nombre, sizeof nombre)) memcpy(p + 4, nombre, sizeof nombre);
}

// ajustes.ini (lo escribe el launcher): líneas CLAVE=valor que se convierten en las variables de siempre
// (ESCALA -> MKDS_ESCALA, PANTALLA_COMPLETA -> MKDS_PANTALLA_COMPLETA, NOMBRE -> MKDS_NOMBRE).
// Lo que ya venga puesto en el entorno manda.
static void lee_ajustes(void) {
    FILE *f = fopen("ajustes.ini", "r");
    if (!f) return;
    char linea[256];
    while (fgets(linea, sizeof linea, f)) {
        linea[strcspn(linea, "\r\n")] = 0;
        char *igual = strchr(linea, '=');
        if (!igual || linea[0] == '#' || linea[0] == ';' || linea[0] == '[') continue;
        *igual = 0;
        char var[64];
        snprintf(var, sizeof var, "MKDS_%s", linea);
        if (!getenv(var)) _putenv_s(var, igual + 1);
    }
    fclose(f);
}

// ---- el código de los dos procesadores, sacado directamente de la ROM del usuario ----------------------
// (así basta con elegir la ROM en el launcher; antes hacía falta extraido/ de tools/nds_rom.py)
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// "Backwards LZ" del SDK (MI_UncompressBackward), igual que blz_decompress en tools/nds_rom.py:
// los últimos 8 bytes dicen cuánto crece y qué parte está comprimida; se lee del final al principio.
static uint8_t *blz(const uint8_t *buf, size_t len, size_t *sal) {
    uint32_t crece = le32(buf + len - 4);
    uint8_t *out = malloc(len + crece);
    memcpy(out, buf, len);
    *sal = len + crece;
    if (!crece) return out;
    uint32_t cab = le32(buf + len - 8);
    size_t cab_len = cab >> 24, cod_len = cab & 0xFFFFFF;
    size_t crudo = len - cod_len;  // lo de antes va sin comprimir
    size_t src = len - cab_len, dst = len + crece;
    while (dst > crudo && src > crudo) {
        uint8_t flags = buf[--src];
        for (int k = 0; k < 8 && dst > crudo && src > crudo; k++, flags <<= 1) {
            if (flags & 0x80) {
                uint8_t b1 = buf[src - 1], b2 = buf[src - 2];
                src -= 2;
                int largo = (b1 >> 4) + 3, desp = (((b1 & 15) << 8) | b2) + 3;
                for (int i = 0; i < largo && dst > 0; i++, dst--) out[dst - 1] = out[dst - 1 + desp];
            } else {
                out[--dst] = buf[--src];
            }
        }
    }
    return out;
}

static void carga_codigo(const uint8_t *rom, size_t n) {
    uint32_t a9o = le32(rom + 0x20), a9ram = le32(rom + 0x28), a9t = le32(rom + 0x2C);
    uint32_t a7o = le32(rom + 0x30), a7ram = le32(rom + 0x38), a7t = le32(rom + 0x3C);
    if ((size_t)a9o + a9t > n || (size_t)a7o + a7t > n) { fprintf(stderr, "ROM incompleta\n"); exit(1); }
    const uint8_t *crudo = rom + a9o;
    uint8_t *arm9 = malloc(a9t);
    memcpy(arm9, crudo, a9t);
    size_t t9 = a9t;
    static const uint8_t magia[8] = {0x21, 0x06, 0xC0, 0xDE, 0xDE, 0xC0, 0x06, 0x21};
    for (uint32_t i = 0x1C; i + 8 <= a9t; i += 4) {
        if (memcmp(crudo + i, magia, 8)) continue;
        uint32_t mp = i - 0x1C, fin = le32(crudo + mp + 0x14);  // compressed_static_end
        if (fin) {
            size_t corte = fin - a9ram, td;
            uint8_t *dec = blz(crudo, corte, &td);
            free(arm9);
            t9 = td + (a9t - corte);
            arm9 = malloc(t9);
            memcpy(arm9, dec, td);
            memcpy(arm9 + td, crudo + corte, a9t - corte);
            free(dec);
            memset(arm9 + mp + 0x14, 0, 4);  // ya descomprimido: que el arranque no lo intente otra vez
        }
        break;
    }
    memcpy(g_main + (a9ram & 0x3FFFFF), arm9, t9);
    memcpy(g_main + (a7ram & 0x3FFFFF), rom + a7o, a7t);
    if (getenv("MKDS_COMPARA")) {  // comprobación: ¿sale igual que extraido/ de nds_rom.py?
        size_t m;
        uint8_t *e9 = carga("extraido/arm9.bin", &m);
        fprintf(stderr, "[mkds] ARM9 %s (%zu / %zu bytes)\n", m == t9 && !memcmp(e9, arm9, m) ? "igual" : "DISTINTO", t9, m);
        uint8_t *e7 = carga("extraido/arm7.bin", &m);
        fprintf(stderr, "[mkds] ARM7 %s\n", m == a7t && !memcmp(e7, rom + a7o, m) ? "igual" : "DISTINTO");
        free(e9); free(e7);
    }
    free(arm9);
}

// La carpeta del juego es la del launcher (rom\, ajustes.ini, partida.sav, repeticiones\), una por encima de
// build\. Si se abre build\mkds.exe con doble clic, Windows arranca en build\ y no encontraba la ROM.
#ifndef _WIN32
#if defined(__APPLE__) && !TARGET_OS_IPHONE
// La app de Mac (mac/empaquetar.sh: "Action Kart.app"): los datos (ROM, partida, ajustes) en
// ~/Library/Application Support/Action Kart, y la ROM la pide al abrirse la primera vez, como en el iPhone.
static int app_mac;
#ifdef ROM_CRC
#include <zlib.h>
static uint32_t crc_rom(const char *ruta) {
    FILE *f = fopen(ruta, "rb");
    if (!f) return 0;
    static unsigned char buf[1 << 16];
    uLong c = crc32(0L, Z_NULL, 0);
    for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0;) c = crc32(c, buf, (uInt)k);
    fclose(f);
    return (uint32_t)c;
}
#endif
static void mac_pide_rom(void) {
    if (!app_mac) return;
    const char *aviso = "To start, choose your own copy of the game: a Mario Kart DS (Europe) ROM. It is copied into "
                        "the app's folder on this Mac.";
    for (;;) {
        FILE *hay = fopen("rom/mkds.nds", "rb");
        if (hay) { fclose(hay); return; }
        char orden[1200], ruta[1024] = "";
        snprintf(orden, sizeof orden,
                 "osascript -e 'display dialog \"%s\" buttons {\"Quit\", \"Choose ROM\"} default button 2 "
                 "with title \"Action Kart\"' -e 'POSIX path of (choose file with prompt \"Choose your Mario Kart DS "
                 "(Europe) ROM\")' 2>/dev/null", aviso);
        FILE *p = popen(orden, "r");
        if (!p || !fgets(ruta, sizeof ruta, p)) { if (p) pclose(p); exit(0); }  // "Quit" (o cancelar)
        pclose(p);
        ruta[strcspn(ruta, "\r\n")] = 0;
        FILE *de = fopen(ruta, "rb"), *a = de ? fopen("rom/mkds.nds", "wb") : NULL;
        static char buf[1 << 16];
        for (size_t k; de && a && (k = fread(buf, 1, sizeof buf, de)) > 0;) fwrite(buf, 1, k, a);
        if (de) fclose(de);
        if (a) fclose(a);
#ifdef ROM_CRC
        if (crc_rom("rom/mkds.nds") != (uint32_t)ROM_CRC) {
            remove("rom/mkds.nds");
            aviso = "That file is not the ROM this app was built from. Choose the same Mario Kart DS (Europe) ROM.";
        }
#endif
    }
}
#endif

static void ve_a_la_carpeta_del_juego(void) {  // en Linux, igual: el ejecutable está en build/
    static char ruta[MAX_PATH];
#if TARGET_OS_IPHONE  // iOS: todo en Documents de la app (la ROM la pide la app al abrirse: ios_rom.m)
    snprintf(ruta, sizeof ruta, "%s/Documents", getenv("HOME"));
    if (chdir(ruta)) {}
    mkdir("rom", 0755);
    FILE *aj = fopen("ajustes.ini", "r");
    if (aj) fclose(aj);
    else if ((aj = fopen("ajustes.ini", "w"))) {  // los de iPhone: sin micrófono (su permiso bloqueaba el arranque),
        fputs("NOMBRE=ActionKart\nESCALA=1\nFPS=60\nIDIOMA=1\nMICRO=0\nDISPOSICION=3\n", aj);  // el 3D a 1x
        fclose(aj);
    }
    setenv("SDL_AUDIO_CATEGORY", "playback", 1);  // sonido aunque el interruptor de silencio esté puesto (como un juego)
    return;
#endif
#ifdef __APPLE__  // macOS no tiene /proc
    uint32_t tam = sizeof ruta;
    if (_NSGetExecutablePath(ruta, &tam)) return;
#if !TARGET_OS_IPHONE
    if (strstr(ruta, ".app/Contents/MacOS/")) {  // la app de Mac: los datos en Application Support
        app_mac = 1;
        snprintf(ruta, sizeof ruta, "%s/Library/Application Support/Action Kart", getenv("HOME"));
        mkdir(ruta, 0755);
        if (chdir(ruta)) {}
        mkdir("rom", 0755);
        FILE *aj = fopen("ajustes.ini", "r");
        if (aj) fclose(aj);
        else if ((aj = fopen("ajustes.ini", "w"))) {  // los ajustes de Action Kart: una pantalla 16:9, inglés
            fputs("NOMBRE=ActionKart\nESCALA=2\nFPS=60\nPANTALLA_COMPLETA=0\nIDIOMA=1\nMICRO=0\nDISPOSICION=3\n", aj);
            fclose(aj);
        }
        return;
    }
#endif
#else
    ssize_t n = readlink("/proc/self/exe", ruta, sizeof ruta - 1);
    if (n <= 0) return;
    ruta[n] = 0;
#endif
    char *barra = strrchr(ruta, '/');
    if (!barra) return;
    *barra = 0;
    barra = strrchr(ruta, '/');
    if (barra && !strcmp(barra + 1, "build")) *barra = 0;
    if (chdir(ruta)) {}
}
#else
static void ve_a_la_carpeta_del_juego(void) {
    wchar_t ruta[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, ruta, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t *barra = wcsrchr(ruta, L'\\');
    if (!barra) return;
    *barra = 0;  // carpeta del .exe
    barra = wcsrchr(ruta, L'\\');
    if (barra && !_wcsicmp(barra + 1, L"build")) *barra = 0;
    SetCurrentDirectoryW(ruta);
#ifdef MKDS_SDL  // la versión de Action Kart (windows/empaquetar.sh): sus ajustes la primera vez
    // sin consola (es una app de ventana): lo que el juego cuenta por stderr va a mkds_log.txt, junto al .exe
    if (freopen("mkds_log.txt", "w", stderr)) setvbuf(stderr, NULL, _IONBF, 0);
    CreateDirectoryW(L"rom", NULL);
    FILE *aj = fopen("ajustes.ini", "r");
    if (aj) fclose(aj);
    else if ((aj = fopen("ajustes.ini", "w"))) {
        fputs("NOMBRE=ActionKart\nESCALA=2\nFPS=60\nPANTALLA_COMPLETA=0\nIDIOMA=1\nMICRO=0\nDISPOSICION=3\n", aj);
        fclose(aj);
    }
#endif
}

#if defined(_WIN32) && defined(MKDS_SDL)
// Action Kart para Windows: pide la ROM la primera vez (la carpeta rom\ junto al .exe) y solo acepta la de la app
#include <commdlg.h>
static uint32_t crc_win(const char *ruta) {  // CRC-32 (el de zlib), sin zlib
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
    FILE *f = fopen(ruta, "rb");
    if (!f) return 0;
    static unsigned char buf[1 << 16];
    uint32_t c = 0xFFFFFFFFu;
    for (size_t k; (k = fread(buf, 1, sizeof buf, f)) > 0;)
        for (size_t i = 0; i < k; i++) c = t[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    fclose(f);
    return c ^ 0xFFFFFFFFu;
}
static void win_pide_rom(void) {
    const wchar_t *aviso = L"To start, choose your own copy of the game: a Mario Kart DS (Europe) ROM. "
                           L"It is copied into the rom folder next to the game.";
    for (;;) {
        FILE *hay = fopen("rom/mkds.nds", "rb");
        if (hay) { fclose(hay); return; }
        if (MessageBoxW(NULL, aviso, L"Action Kart", MB_OKCANCEL | MB_ICONINFORMATION) != IDOK) exit(0);
        wchar_t ruta[MAX_PATH] = L"";
        OPENFILENAMEW o = {sizeof o};
        o.lpstrFilter = L"DS ROM (*.nds)\0*.nds\0All files\0*.*\0";
        o.lpstrFile = ruta;
        o.nMaxFile = MAX_PATH;
        o.lpstrTitle = L"Choose your Mario Kart DS (Europe) ROM";
        o.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&o)) continue;
        CopyFileW(ruta, L"rom\\mkds.nds", FALSE);
#ifdef ROM_CRC
        if (crc_win("rom/mkds.nds") != (uint32_t)ROM_CRC) {
            DeleteFileW(L"rom\\mkds.nds");
            aviso = L"That file is not the ROM this game was built from. Choose the same Mario Kart DS (Europe) ROM.";
        }
#endif
    }
}
#endif
#endif

#if TARGET_OS_IPHONE
void ios_pide_rom(void);  // ios_rom.m: el selector de archivos, hasta tener la ROM con la que se hizo la app
#endif
#if defined(__APPLE__) && !TARGET_OS_IPHONE
// (sin los .h de objc: su BOOL choca con el de compat.h)
void *objc_getClass(const char *nombre);
void *sel_registerName(const char *nombre);
void objc_msgSend(void);
typedef void *id;
typedef void *SEL;
// macOS: sin App Nap. Con la ventana tapada (jugando en el móvil, por ejemplo) macOS frenaba los relojes del juego y
// en el multijugador, que va al paso del más lento, se arrastraba a todos. Una actividad "iniciada por el usuario y
// sensible a la latencia" (NSProcessInfo) lo evita mientras dura el juego.
static void sin_app_nap(void) {
    id info = ((id(*)(id, SEL))objc_msgSend)((id)objc_getClass("NSProcessInfo"), sel_registerName("processInfo"));
    id razon = ((id(*)(id, SEL, const char *))objc_msgSend)((id)objc_getClass("NSString"),
                                                           sel_registerName("stringWithUTF8String:"), "Action Kart");
    unsigned long long opciones = 0x00FFFFFFULL | 0xFF00000000ULL;  // NSActivityUserInitiated | NSActivityLatencyCritical
    id act = ((id(*)(id, SEL, unsigned long long, id))objc_msgSend)(info, sel_registerName("beginActivityWithOptions:reason:"),
                                                                   opciones, razon);
    if (act) ((id(*)(id, SEL))objc_msgSend)(act, sel_registerName("retain"));  // (para siempre)
}
#endif
static int arranca(void) {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    sin_app_nap();
#endif
    ve_a_la_carpeta_del_juego();
    lee_ajustes();
#if TARGET_OS_IPHONE
    ios_pide_rom();
#elif defined(__APPLE__)
    mac_pide_rom();
#elif defined(_WIN32) && defined(MKDS_SDL)
    win_pide_rom();
#endif
    size_t n;
    FILE *hay = fopen("rom/mkds.nds", "rb");
    if (!hay) {
#ifdef _WIN32
        MessageBoxW(NULL, L"No encuentro el juego (rom\\mkds.nds).\nElige tu ROM desde el launcher.",
                    L"DS Kart Recompiled", MB_ICONWARNING);
#else
        fprintf(stderr, "No encuentro el juego (rom/mkds.nds). Pon tu ROM ahí (ver LEEME).\n");
#endif
        return 1;
    }
    fclose(hay);
    uint8_t *rom = carga("rom/mkds.nds", &n);
    carga_codigo(rom, n);                     // ARM9 en 0x02000000, ARM7 en 0x02380000
    memcpy(g_main + 0x3FFE00, rom, 0x170);   // la cabecera queda en 0x027FFE00
    ds_pon_rom(rom, n);

    // Sistema operativo del juego (NitroSDK): direcciones sacadas del desensamblado
    g_so[0] = (SoConfig){
        .cp_save = 0x02146C24, .cp_restore = 0x02146BB8, .reschedule = 0x0200DE50,
        .cola_irq = 0x027E0000, .flag_resched = 0x0216FF5C,
        .off_estado = 0x64, .off_enlace = 0x78, .off_sig = 0x80,
    };
    g_so[1] = (SoConfig){
        .reschedule = 0x037F8F48, .cola_irq = 0x0380766C, .flag_resched = 0x038076FC,
        .off_estado = 0x48, .off_enlace = 0x5C, .off_sig = 0x64,
    };

    // Todo desbloqueado viene siempre de serie (lo pidió el usuario: no es una opción).
    // Solo para depurar: la variable de entorno TODO_DESBLOQUEADO=0 lo quita.
    if (!getenv("TODO_DESBLOQUEADO") || atoi(getenv("TODO_DESBLOQUEADO"))) {
        g_trucos |= 1;
        fprintf(stderr, "[mkds] todo desbloqueado\n");
    }
    CreateThread(NULL, 0, vigilante, NULL, 0, NULL);
#ifdef _WIN32
    AddVectoredExceptionHandler(1, al_romperse);
#else
    signal(SIGSEGV, al_romperse);
    signal(SIGILL, al_romperse);
    signal(SIGFPE, al_romperse);
    signal(SIGBUS, al_romperse);
#endif
    ds_arranca(0x02000800, 0x02380000);
    return 0;
}

#if defined(_WIN32) || defined(__APPLE__)
// la pila de 256 MB la pone el enlazador (/STACK en Windows, -stack_size en macOS).
// En macOS la ventana y los eventos de SDL tienen que ir en el hilo principal, así que nada de hilo aparte.
#if TARGET_OS_IPHONE
// iOS: SDL pone su propio main (el de UIKit) y llama a este como SDL_main, en el hilo principal
#include <SDL2/SDL_main.h>
int main(int argc, char *argv[]) { (void)argc; (void)argv; return arranca(); }
#else
int main(void) { return arranca(); }
#endif
#ifdef __APPLE__
__thread CompatFibra *compat_fibra_actual;
#endif
#else
__thread CompatFibra *compat_fibra_actual;  // la fibra en la que se está (compat.h)
// En Linux la pila del hilo principal es de 8 MB: el juego va en un hilo con 256 MB, como en Windows
static void *hilo_juego(void *p) { (void)p; exit(arranca()); }
int main(void) {
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 256u << 20);
    pthread_t h;
    if (pthread_create(&h, &a, hilo_juego, NULL)) return arranca();
    pthread_join(h, NULL);
    return 0;
}
#endif
