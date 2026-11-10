// Ventana para Linux (y cualquier sistema con SDL2): lo mismo que ventana.c (la de Windows) con SDL2 y OpenGL.
// Las dos pantallas de la DS una encima de otra (x2), el teclado como botones, el ratón como lápiz en la de
// abajo y los mandos con la API GameController de SDL (Xbox, PlayStation y los que SDL conozca).
//
// Controles:  flechas = cruceta   X = A   Z = B   S = X   A = Y
//             Q = L   W = R   Enter = Start   Retroceso = Select   F11 = pantalla completa
//             F10 = guardar los últimos 30 segundos en vídeo (repeticiones/)   F1 = menú de ajustes
// Mando: botón de abajo = A, derecha = B, arriba = X, izquierda = Y (por posición, como la DS), LB/LT = L,
//        RB/RT = R, Start, Back/Create = Select, cruceta o stick izquierdo; L3 + R3 = menú de ajustes
//
// Como en Windows, las imágenes se pintan en un hilo aparte (hilo_pinta) al ritmo del monitor, desde una cola de
// NM imágenes con la hora a la que toca enseñar cada una (así caben las intermedias de MKDS_FPS).
// El cartel de la repetición instantánea va en el título de la ventana (aquí no hay GDI para dibujar texto).
#include "compat.h"
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "video.h"
#include "gpu3d.h"
#include "menu.h"

#define ESCALA 2

const char *repe_aviso(void);
void repe_frame(const uint32_t *arriba, const uint32_t *abajo);
void repe_guarda(void);
int video_escala(void);
const uint32_t *video_hd(int abajo);

volatile uint16_t g_keyinput = 0x3FF;
volatile uint16_t g_keyxy = 0x3;
volatile int g_lapiz, g_lapiz_x, g_lapiz_y;
// Botones que pulsa el guion de pruebas (MKDS_GUION, en ds.c): se suman al teclado y al mando
volatile uint16_t g_guion_in = 0x3FF, g_guion_xy = 3;

// ancho: con la cámara ancha (gpu3d.c), la pantalla de arriba a 16:9: el centro, la imagen de siempre; los lados, el 3D
typedef struct { uint32_t baja[256 * 384]; uint32_t hd[1024 * 1536]; uint32_t ancho[342 * 4 * 768]; int ancho_w; } Marco;
#define NM 8
static Marco marcos[NM];
static int64_t marco_ts[NM];   // cuándo enseñarla; 0 = aún no está lista
static int i_escribe, i_reservada;
static volatile int i_viendo = -1;
static volatile LONG enseñadas;
static const Marco *ultimo = &marcos[0];
static CRITICAL_SECTION cs_marcos, cs_destino;

static SDL_Window *ventana;
int skyendo_pinta(SDL_Renderer *r, int ancho, int alto, uint32_t ms);
const uint8_t *video_sprites_arriba(void);
static volatile uint32_t g_imagenes;  // imágenes del juego desde el arranque
static uint64_t mascara_actual[48];  // la rejilla de la pantalla de abajo (ventana_muestra_en)
extern volatile int g_menu_propio, g_ver_arriba;
volatile uint32_t g_huella;  // huella de la pantalla de abajo (DISPOSICION=3)
static SDL_Renderer *pintor;  // SDL_Renderer: Metal en macOS/iOS, sin OpenGL de escritorio
static int oculto, pantalla_completa;
static int dst_x, dst_y, dst_w = 256 * ESCALA, dst_h = 384 * ESCALA;  // dónde va la imagen (en puntos de la ventana)
static uint16_t teclado_in = 0x3FF, teclado_xy = 3;
static char titulo[160];
static volatile int titulo_nuevo;

static int64_t reloj(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
int64_t ventana_reloj(void) { return reloj(); }

// Disposición de las pantallas (DISPOSICION en ajustes.ini, menú F1): 0 las dos iguales (una sobre otra en ventana,
// lado a lado en pantalla completa); 1 la de arriba con todo el alto y la táctil al lado, a un tercio de su tamaño y
// abajo a la derecha: juntas son 16:9; 2 la de arriba entera y la táctil pequeña superpuesta en esa esquina (issue
// #11). La táctil va en ov_*
static int disposicion;
static int en_carrera;  // DISPOSICION=3: se está corriendo (mucho 3D arriba); el mapa se aparta a la derecha
extern volatile int g_carrera, g_presentacion;  // video.c

// ---- pantallas con botones propios (DISPOSICION=3) ----
// Cada menú reconocido por la huella de su pantalla de abajo (ventana_muestra_en): sus botones en coordenadas de la
// pantalla de abajo de la DS (donde están los del juego: tocarlos manda el toque ahí) y nuestro texto. El de volver
// (tipo 1) se pinta abajo a la derecha y toca donde está el del juego (tx, ty). Un menú que no está aquí sale tal cual.
// tipo: como MenuBoton (0 barra, 1 volver, 2/3 flechas, 4 acción). acción al tocarlo: 0 toque en el centro de su
// rectángulo, 1 toque en (tx, ty), 2 pulsar A, 3/4 personaje anterior/siguiente (toque en su retrato). icono: MenuBoton.
// Los de tipo 1-4 no usan x0..y1: van en sitios fijos de la imagen 16:9 (rect_p). panel: 1 tarjeta del personaje,
// 2 tarjeta del kart (nombre y estadísticas, leídos de la pantalla de abajo del juego).
typedef struct { int x0, y0, x1, y1; const char *texto; int tipo, accion, tx, ty, icono; } BotonP;
// Selector (panel 5): una tarjeta con su nombre y el valor que enseña el juego (recortado de su pantalla de abajo:
// vx, vy, vw, vh), en la fila fy..fy+48 de la pantalla (sus flechas son botones de tipo 2/3 con sitio propio)
typedef struct { const char *etiqueta; int vx, vy, vw, vh, fy; } Selector;
// mensaje: el texto del juego de esa pantalla (lo que pone arriba), dicho con el nuestro en una tarjeta
// arriba: se deja lo que el juego pone en la de arriba (los récords, la descripción de la misión), sobre nuestro fondo
// Recorte: un texto del juego (nombres, valores) que se enseña en nuestro botón "boton": se quedan solo las letras
// (lo que se aparta mucho del color de fondo del recuadro), en blanco; con etiqueta, ella a la izquierda y el texto
// a la derecha. Un botón de barra sin texto del juego dice "EMPTY"
typedef struct { int boton, x, y, w, h; const char *etiqueta; } Recorte;
typedef struct { uint32_t huella; const char *titulo; int n, panel; BotonP b[16]; const char *mensaje; const Selector *sel; int nsel; int arriba; const Recorte *rec; int nrec; } PantallaP;
// el multijugador del juego (sus grupos): los nombres de los grupos, la lista de corredores y los valores de las reglas
static const Recorte rec_grupos[] = {{0, 68, 15, 132, 18, 0}, {1, 68, 47, 132, 18, 0}, {2, 68, 79, 132, 18, 0}, {3, 68, 111, 132, 18, 0}};
static const Recorte rec_lista[] = {{0, 44, 2, 170, 146, 0}};
static const Recorte rec_reglas[] = {{0, 130, 8, 106, 18, "CLASS"}, {3, 130, 36, 106, 18, "CPU KART"}, {6, 130, 64, 106, 18, "COURSE"},
                                     {9, 24, 106, 208, 18, "RULES"}, {12, 130, 135, 106, 18, "TEAM"}};
static const Recorte rec_reglas_inv[] = {{0, 130, 8, 106, 18, "CLASS"}, {1, 130, 36, 106, 18, "CPU KART"}, {2, 130, 64, 106, 18, "COURSE"},
                                         {3, 24, 106, 208, 18, "RULES"}, {4, 130, 135, 106, 18, "TEAM"}};
#define FILA_REGLA(r, ly, lx) {30, 2 + (r) * 30, 226, 28 + (r) * 30, "", 5, 15, 0, 0, 0}, \
    {4, 2 + (r) * 30, 28, 28 + (r) * 30, "", 2, 1, lx, ly, 0}, {228, 2 + (r) * 30, 252, 28 + (r) * 30, "", 3, 1, 242, ly, 0}
#define FILA_VER(r) {30, 2 + (r) * 30, 226, 28 + (r) * 30, "", 5, 15, 0, 0, 0}
static const Selector sel_ajustes[] = {{"SOUND", 33, 44, 192, 22, 10}, {"LOCATE FRIEND", 33, 108, 192, 22, 82}};
static const Selector sel_partida[] = {{"VS. OPPONENT", 40, 78, 176, 20, 58}};
#define VOLVER(x, y) {0, 0, 0, 0, "BACK", 1, 1, x, y, 0}
#define COPA(x0, y0, nombre, k, cx, cy) {x0, y0, x0 + 58, y0 + 66, nombre, 9, 5, cx, cy, 11 + k}
static const PantallaP pantallas_p[] = {
    {0x2e87fa7au, "ACTION KART", 4, 0,  // (sin RECORDS; el multijugador va por las salas privadas, hasta 8)
     {{16, 4, 250, 38, "SINGLE PLAYER", 0, 1, 128, 24, 0}, {16, 48, 250, 82, "ONLINE  (UP TO 4)", 0, 1, 128, 116, 0},
      {16, 92, 250, 126, "PRIVATE ROOM  (UP TO 8)", 0, 12, 0, 0, 0}, {16, 136, 250, 170, "OPTIONS", 0, 1, 193, 165, 0}}},
    {0x549a4e4au, "SELECT MODE", 6, 0,
     {{40, 6, 240, 35, "GRAND PRIX", 0, 1, 150, 20, 0}, {40, 40, 240, 69, "TIME TRIALS", 0, 1, 150, 54, 0},
      {40, 74, 240, 103, "VS RACE", 0, 1, 150, 88, 0}, {40, 108, 240, 137, "BATTLE", 0, 1, 150, 122, 0},
      {40, 142, 240, 171, "MISSIONS", 0, 1, 150, 156, 0}, VOLVER(18, 178)}},
    {0x2dbce42du, "SELECT CLASS", 5, 0,
     {{10, 8, 122, 78, "50cc", 8, 1, 60, 40, 1}, {136, 8, 248, 78, "100cc", 8, 1, 196, 40, 2},
      {10, 90, 122, 160, "150cc", 8, 1, 60, 112, 3}, {136, 90, 248, 160, "MIRROR", 8, 1, 196, 112, 4},
      VOLVER(18, 178)}},
    {0x9222da88u, "CHARACTER", 4, 1,
     {{0, 0, 0, 0, "", 2, 3, 0, 0, 0}, {0, 0, 0, 0, "", 3, 4, 0, 0, 0},
      {0, 0, 0, 0, "SELECT", 4, 2, 0, 0, 0}, VOLVER(18, 178)}},
    {0xace38f13u, "KART", 4, 2,
     {{0, 0, 0, 0, "", 2, 1, 17, 21, 0}, {0, 0, 0, 0, "", 3, 1, 226, 21, 0},
      {0, 0, 0, 0, "OK", 4, 1, 128, 172, 0}, VOLVER(18, 178)}},
    {0xfcc949ccu, "SELECT CUP", 9, 0,  // paso 1: las 8 copas (tocar una la marca en el juego y abre el paso 2)
     {COPA(4, 4, "SPARK CUP", 0, 30, 52), COPA(66, 4, "BLAZE CUP", 1, 82, 52), COPA(128, 4, "COMET CUP", 2, 134, 52),
      COPA(190, 4, "CROWN CUP", 3, 186, 52), COPA(4, 84, "GEAR CUP", 4, 72, 132), COPA(66, 84, "WAVE CUP", 5, 124, 132),
      COPA(128, 84, "LEAF CUP", 6, 176, 132), COPA(190, 84, "BOLT CUP", 7, 228, 132), VOLVER(18, 178)}},
    {0, "", 3, 3,  // paso 2 (sin huella: se llega desde el paso 1): las pistas de la copa y empezar
     {{0, 0, 0, 0, "START", 4, 2, 0, 0, 0}, {0, 0, 0, 0, "", 2, 6, 0, 0, 0}, VOLVER(18, 178)}},
    {0, "OPTIONS", 4, 0,
     {{32, 14, 225, 47, "SETTINGS", 0, 1, 128, 32, 0}, {32, 62, 225, 95, "EDIT PROFILE", 0, 1, 128, 80, 0},
      {32, 110, 225, 143, "ERASE DATA", 0, 1, 128, 128, 0}, VOLVER(18, 178)}},
    {0, "SETTINGS", 6, 5,  // dos selectores: tarjeta con nombre y valor (recortado del juego) y flechas a sus lados
     {{6, 14, 34, 54, "", 2, 1, 13, 43, 0}, {222, 14, 250, 54, "", 3, 1, 241, 43, 0},
      {6, 86, 34, 126, "", 2, 1, 13, 107, 0}, {222, 86, 250, 126, "", 3, 1, 241, 107, 0},
      {0, 0, 0, 0, "OK", 4, 1, 128, 163, 0}, VOLVER(18, 178)}, NULL, sel_ajustes, 2},
    {0, "EDIT PROFILE", 3, 0,
     {{32, 30, 225, 63, "NICKNAME", 0, 1, 128, 48, 0}, {32, 94, 225, 127, "EMBLEM", 0, 1, 128, 112, 0},
      VOLVER(18, 178)}},
    {0, "ONLINE", 4, 0,
     {{32, 14, 225, 47, "FIND MATCH", 0, 1, 156, 39, 0}, {32, 62, 225, 95, "FRIEND CODE", 0, 1, 156, 87, 0},
      {32, 110, 225, 143, "ONLINE SETTINGS", 0, 1, 156, 135, 0}, VOLVER(18, 178)}},
    {0, "ONLINE", 2, 0,  // la pregunta de conectarse: SÍ / NO
     {{24, 100, 118, 140, "YES", 0, 1, 67, 143, 0}, {138, 100, 232, 140, "NO", 0, 1, 187, 143, 0}},
     "Connect to the online service?"},
    {0, "MATCH SETTINGS", 5, 5,  // las condiciones de la partida
     {{120, 4, 250, 34, "FRIEND ROSTER", 0, 1, 188, 19, 0}, {6, 62, 34, 102, "", 2, 1, 14, 86, 0},
      {222, 62, 250, 102, "", 3, 1, 240, 86, 0}, {0, 0, 0, 0, "OK", 4, 1, 128, 165, 0}, VOLVER(18, 178)},
     NULL, sel_partida, 1},
    {0, "TIME TRIALS", 4, 0,  // misma rejilla que la del online: se distinguen por lo elegido en el menú principal
     {{32, 14, 225, 47, "TIME TRIALS", 0, 1, 156, 39, 0}, {32, 62, 225, 95, "SEND GHOST", 0, 1, 156, 87, 0},
      {32, 110, 225, 143, "RECEIVE GHOST", 0, 1, 156, 135, 0}, VOLVER(18, 178)}},
    {0, "GRAND PRIX", 2, 0,  // copa elegida (Grand Prix): arriba sus pistas; OK / BACK
     {{40, 40, 216, 76, "START", 0, 1, 128, 71, 0}, {40, 92, 216, 128, "BACK", 0, 1, 128, 119, 0}}, NULL, NULL, 0, 1},
    {0, "TIME TRIALS", 2, 0,  // pista elegida (contrarreloj): arriba la pista y los récords; OK / BACK
     {{40, 40, 216, 76, "START", 0, 1, 128, 71, 0}, {40, 92, 216, 128, "BACK", 0, 1, 128, 119, 0}}, NULL, NULL, 0, 1},
    {0, "BATTLE", 2, 0,  // solo Shine Runners (los globos se inflan con el micrófono)
     {{32, 50, 225, 90, "SHINE RUNNERS", 0, 1, 140, 113, 0}, VOLVER(18, 178)}},
    {0, "MISSIONS", 8, 0,
     {{8, 6, 81, 51, "LEVEL 1", 0, 0, 0, 0, 0}, {92, 6, 165, 51, "LEVEL 2", 0, 0, 0, 0, 0}, {176, 6, 249, 51, "LEVEL 3", 0, 0, 0, 0, 0},
      {8, 58, 81, 103, "LEVEL 4", 0, 0, 0, 0, 0}, {92, 58, 165, 103, "LEVEL 5", 0, 0, 0, 0, 0}, {176, 58, 249, 103, "LEVEL 6", 0, 0, 0, 0, 0},
      {92, 110, 165, 155, "LEVEL 7", 0, 0, 0, 0, 0}, VOLVER(18, 178)}},
    {0, "MISSION", 10, 0,  // las misiones del nivel: arriba la descripción
     {{8, 6, 81, 51, "1", 0, 0, 0, 0, 0}, {92, 6, 165, 51, "2", 0, 0, 0, 0, 0}, {176, 6, 249, 51, "3", 0, 0, 0, 0, 0},
      {8, 58, 81, 103, "4", 0, 0, 0, 0, 0}, {92, 58, 165, 103, "BOSS", 0, 0, 0, 0, 0}, {176, 58, 249, 103, "5", 0, 0, 0, 0, 0},
      {8, 110, 81, 155, "6", 0, 0, 0, 0, 0}, {92, 110, 165, 155, "7", 0, 0, 0, 0, 0}, {176, 110, 249, 155, "8", 0, 0, 0, 0, 0},
      VOLVER(18, 178)}, NULL, NULL, 0, 1},
    {0, "MISSION", 2, 0,  // misión elegida: arriba la descripción; OK / BACK
     {{40, 40, 216, 76, "START", 0, 1, 128, 71, 0}, {40, 92, 216, 128, "BACK", 0, 1, 128, 119, 0}}, NULL, NULL, 0, 1},
    {0, "PAUSE", 2, 0,  // la pausa en carrera (START): sobre la carrera parada
     {{40, 40, 216, 80, "CONTINUE", 0, 1, 126, 78, 0}, {40, 96, 216, 136, "QUIT", 0, 1, 126, 112, 0}}},
    {0, "ONLINE", 0, 0, {{0}}, "Connecting... please wait."},  // conectando (sin rejilla)
    {0, "SEARCHING", 1, 6, {VOLVER(18, 178)}, "Searching for racers..."},  // buscando rivales
    {0, "PRIVATE ROOM", 4, 0,  // salas privadas: en internet con código (wifi.c) o en la misma wifi (sin código); hasta 8
     {{30, 72, 226, 102, "CREATE ONLINE ROOM", 0, 7, 0, 0, 0}, {30, 110, 226, 140, "JOIN ONLINE ROOM", 0, 8, 0, 0, 0},
      {30, 148, 226, 178, "SAME WI-FI ROOM", 0, 10, 128, 70, 0}, {0, 0, 0, 0, "BACK", 1, 9, 0, 0, 0}},
     "Up to 8 players.\nOnline with a code, or the same Wi-Fi."},
    {0, "YOUR ROOM", 2, 0,  // la sala creada: el código y cuántos hay; START = el multijugador del juego
     {{0, 0, 0, 0, "START", 4, 10, 128, 70, 0}, {0, 0, 0, 0, "BACK", 1, 9, 0, 0, 0}}, ""},
    {0, "JOIN ROOM", 4, 7,  // unirse: el código con nuestro teclado de píxeles (o el teclado del ordenador)
     {{11, 156, 62, 176, "DEL", 0, 14, 0, 0, 0}, {70, 156, 245, 176, "JOIN", 0, 11, 0, 0, 0},
      {0, 0, 0, 0, "START", 4, 10, 128, 70, 0}, {0, 0, 0, 0, "BACK", 1, 9, 0, 0, 0}}, ""},
    {0, "", 1, 0,  // la portada del juego (abajo en blanco, sin 3D): la nuestra; START para empezar
     {{0, 0, 0, 0, "PRESS START", 4, 2, 0, 0, 0}}},
    // ---- el multijugador del juego (al que llevan las salas): sus grupos, con lo nuestro ----
    {0, "MULTIPLAYER", 6, 0,  // 27: los grupos que hay (tocar uno para entrar) o crear uno
     {{8, 4, 248, 34, "", 0, 1, 135, 24, 0}, {8, 38, 248, 68, "", 0, 1, 135, 56, 0}, {8, 72, 248, 102, "", 0, 1, 135, 88, 0},
      {8, 106, 248, 136, "", 0, 1, 135, 120, 0}, {40, 146, 216, 176, "CREATE GROUP", 0, 1, 150, 157, 0}, VOLVER(18, 178)},
     NULL, NULL, 0, 0, rec_grupos, 4},
    {0, "NEW GROUP", 2, 0,  // 28: NORMAL (cada uno con su juego); la descarga (SIMPLE) aquí no sirve
     {{30, 110, 226, 140, "CREATE GROUP", 0, 1, 128, 40, 0}, VOLVER(18, 178)}, "Everyone plays with their own copy."},
    {0, "WAITING FOR RACERS", 2, 0,  // 29: el grupo, esperando (el anfitrión solo, o un invitado)
     {{40, 0, 216, 150, "", 5, 15, 0, 0, 0}, VOLVER(18, 178)}, NULL, NULL, 0, 0, rec_lista, 1},
    {0, "WAITING FOR RACERS", 3, 0,  // 30: el anfitrión con alguien más: START RACE (el CUT OFF del juego)
     {{40, 0, 216, 150, "", 5, 15, 0, 0, 0}, {0, 0, 0, 0, "START RACE", 4, 1, 148, 167, 0}, VOLVER(18, 178)},
     NULL, NULL, 0, 0, rec_lista, 1},
    {0, "MULTIPLAYER", 0, 0, {{0}}, "Please wait..."},  // 31: "un momento", "guardando"
    {0, "SELECT GAME", 3, 0,  // 32: el anfitrión elige el juego (sin globos: se inflan con el micrófono)
     {{24, 30, 232, 70, "VS RACE", 0, 1, 140, 30, 0}, {24, 90, 232, 130, "SHINE RUNNERS", 0, 1, 140, 130, 0}, VOLVER(18, 178)}},
    {0, "SELECT GAME", 0, 0, {{0}}, "The host is choosing the game."},  // 33: los invitados esperan
    {0, "GAME SETTINGS", 16, 0,  // 34: las reglas (el anfitrión): cada fila con sus flechas y el valor del juego
     {FILA_REGLA(0, 16, 121), FILA_REGLA(1, 43, 121), FILA_REGLA(2, 71, 121), FILA_REGLA(3, 114, 12), FILA_REGLA(4, 143, 121),
      {0, 0, 0, 0, "OK", 4, 1, 128, 172, 0}}, NULL, NULL, 0, 0, rec_reglas, 5},
    {0, "WAITING FOR HOST", 5, 0,  // 35: las reglas (un invitado): las ve, las pone el anfitrión
     {FILA_VER(0), FILA_VER(1), FILA_VER(2), FILA_VER(3), FILA_VER(4)}, NULL, NULL, 0, 0, rec_reglas_inv, 5},
    {0, "WELCOME", 1, 0,  // 36: la primera vez, "¿el apodo está bien?": SÍ (se cambia luego en OPTIONS)
     {{0, 0, 0, 0, "OK", 4, 1, 67, 143, 0}}, "Welcome to Action Kart!\nRename your racer in OPTIONS."},
};
#define PANT_GRUPOS 27
#define PANT_ESPERA_MP 31
#define PANT_JUEGO_INV 33
#define NPANT (int)(sizeof pantallas_p / sizeof pantallas_p[0])
// Las rejillas de cada pantalla de pantallas_p (la pantalla de abajo: qué puntos tienen algo, 64 x 48), apuntadas con
// MKDS_HUELLAS=1 (las líneas [mascara]). La de las pistas no tiene: se llega desde la de las copas.
static const uint64_t mascaras_p[][48] = {
    {  // menú principal
     0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffffffe0ull, 0xfffffffffffffff0ull,
     0xfffffffffffffff0ull, 0xfffffffffffffff0ull, 0xfffffffffffffff0ull, 0xfffffffffffffff0ull,
     0xfffffffffffffff0ull, 0xfffffffffffffff0ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0xffffffffffffffc0ull, 0xffffffffffffffe0ull, 0xfffffffffffffff0ull,
     0xfffffffffffffff0ull, 0xfffffffffffffff0ull, 0xfffffffffffffff0ull, 0xfffffffffffffff0ull,
     0xfffffffffffffff0ull, 0xfffffffffffffff0ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0xffffffffffffffe0ull, 0xfffffffffffffff0ull, 0xfffffffffffffff0ull,
     0xfffffffffffffff0ull, 0xfffffffffffffff0ull, 0xfffffffffffffff0ull, 0xfffffffffffffff0ull,
     0xfffffffffffffff0ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x7ffffff8fffffff0ull, 0x7ffffffcfffffff0ull,
     0x7ffffffcfffffff0ull, 0x7ffffffcfffffff0ull, 0x7ffffffcfffffff0ull, 0x7ffffffcfffffff0ull,
     0x3ffffff8fffffff0ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    },
    {  // modo
     0x0000000000000000ull, 0xffffffffffff0000ull, 0xffffffffffff8000ull, 0xffffffffffffc000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull,
     0xffffffffffffc000ull, 0x0000000000000000ull, 0xffffffffffff8000ull, 0xffffffffffffc000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull,
     0xffffffffffffc000ull, 0x0000000000000000ull, 0xffffffffffff0000ull, 0xffffffffffff8000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull,
     0xffffffffffffc000ull, 0xffffffffffff8000ull, 0x0000000000000000ull, 0xffffffffffff8000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0x0000000000000000ull, 0xffffffffffff0000ull,
     0xffffffffffff8000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc1ffull,
     0xffffffffffffc3ffull, 0xffffffffffffc7ffull, 0xffffffffffff87ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // categoría
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x079e000000000f80ull,
     0x1fff000000001fe0ull, 0xfffffffc1fffffffull, 0xfffffffc3fffffffull, 0xfffffffc7fffffffull,
     0xfffffffc7fffffffull, 0xfffffffc7fffffffull, 0xfffffffc7fffffffull, 0xfffffffc7fffffffull,
     0xfffffffc7fffffffull, 0xfffffffc7fffffffull, 0xfffffffc7fffffffull, 0xfffffffc7fffffffull,
     0xfffffffc7fffffffull, 0x000000003fffffffull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x7fff8000000f81f8ull, 0x7fff8000001ffffcull, 0xfffffffc3fffffffull,
     0xfffffffc7fffffffull, 0xfffffffc7fffffffull, 0xfffffffc7fffffffull, 0xfffffffc7fffffffull,
     0xfffffffc7fffffffull, 0xfffffffc7fffffffull, 0xfffffffc7fffffffull, 0xfffffffc7fffffffull,
     0xfffffffc7fffffffull, 0xfffffffc7fffffffull, 0xfffffffc7fffffffull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // personaje
     0x0000000000000000ull, 0x000000000000c018ull, 0x0ffc1ff83ff0ffe8ull, 0x0ffc1ff83ff07fe0ull,
     0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull,
     0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff0ffe8ull,
     0x0ffc1ff83ff0fff8ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0ffc1ff83ff07fe0ull,
     0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull,
     0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull,
     0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull,
     0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull,
     0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x0ffc1ff83ff07fe0ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // kart
     0x0000000000000000ull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xfffffffffffff000ull, 0xffffffffffffe000ull, 0xfffe0000000001ffull,
     0xfffc00ffff0003ffull, 0x000000ffff0007ffull, 0x000000ffff0007ffull, 0x000000ffff0007ffull,
     0x000000ffff0007ffull, 0x000000ffff07ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // copas
     0x0000000000000000ull, 0x0000ffffffffffffull, 0x0001ffffffffffffull, 0x0003ffffffffffffull,
     0x0007ffffffffffffull, 0x000fffffffffffffull, 0x001fffffffffffffull, 0x003fffffffffffffull,
     0x003fffffffffffffull, 0x003fffffffffffffull, 0x003fffffffffffffull, 0x003fffffffffffffull,
     0x003fffffffffffffull, 0x003fffffffffffffull, 0x003fffffffffffffull, 0x003fffffffffffffull,
     0x003fffffffffffffull, 0x003fffffffffffffull, 0x003fffffffffffffull, 0x001fffffffffffffull,
     0x000fffffffffffffull, 0xffffffffffff8000ull, 0xffffffffffffc000ull, 0xffffffffffffe000ull,
     0xfffffffffffff000ull, 0xfffffffffffff800ull, 0xfffffffffffffc00ull, 0xfffffffffffffe00ull,
     0xfffffffffffffe00ull, 0xfffffffffffffe00ull, 0xfffffffffffffe00ull, 0xfffffffffffffe00ull,
     0xfffffffffffffe00ull, 0xfffffffffffffe00ull, 0xfffffffffffffe00ull, 0xfffffffffffffe00ull,
     0xfffffffffffffe00ull, 0xfffffffffffffe00ull, 0xfffffffffffffc00ull, 0xfffffffffffff9ffull,
     0xfffffffffffff3ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // opciones
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x00fffffffffffc00ull, 0x00fffffffffffe00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull,
     0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x007fffffffffff00ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x00fffffffffffc00ull, 0x00fffffffffffe00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull,
     0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x007fffffffffff00ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x00fffffffffffc00ull, 0x00fffffffffffe00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull,
     0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x007fffffffffff00ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // ajustes
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x01fffffffffffe00ull, 0x01ffffffffffff00ull,
     0x09ffffffffffff10ull, 0x19ffffffffffff18ull, 0x39ffffffffffff1cull, 0x39ffffffffffff1cull,
     0x19ffffffffffff18ull, 0x09ffffffffffff10ull, 0x01ffffffffffff00ull, 0x00ffffffffffff00ull,
     0x007fffffffffff00ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x01fffffffffffe00ull, 0x01ffffffffffff00ull,
     0x09ffffffffffff10ull, 0x19ffffffffffff18ull, 0x39ffffffffffff1cull, 0x39ffffffffffff1cull,
     0x19ffffffffffff18ull, 0x09ffffffffffff10ull, 0x01ffffffffffff00ull, 0x00ffffffffffff00ull,
     0x007fffffffffff00ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x000000ffff000000ull, 0x000000ffff0001ffull,
     0x000000ffff0003ffull, 0x000000ffff0007ffull, 0x000000ffff0007ffull, 0x000000ffff0007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // editar perfil
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x00fffffffffffc00ull, 0x00fffffffffffe00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull,
     0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x007fffffffffff00ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x00fffffffffffc00ull, 0x00fffffffffffe00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull,
     0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x007fffffffffff00ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // online
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffff0000ull, 0xffffffffffff8000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffff0000ull, 0xffffffffffff8000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffff0000ull, 0xffffffffffff8000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull, 0xffffffffffffc000ull,
     0xffffffffffffc000ull, 0xffffffffffffc000ull, 0x0000000000000000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // online: ¿conectar?
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x03fffff00fffffc0ull, 0x02ffffd00bffff40ull, 0x03fffff00fffffc0ull, 0x03fffff00fffffc0ull,
     0x03fffff00fffffc0ull, 0x03fffff00fffffc0ull, 0x02ffffd00bffff40ull, 0x03fffff00fffffc0ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    },
    {  // online: condiciones
     0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffc0000000ull, 0xffffffffc0000000ull,
     0xffffffffc0000000ull, 0xffffffffc0000000ull, 0xffffffffc0000000ull, 0xffffffff80000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x00fffffffffffe00ull, 0x0cffffffffffff10ull,
     0x1cffffffffffff18ull, 0x3cffffffffffff1cull, 0x1cffffffffffff1cull, 0x0cffffffffffff18ull,
     0x047fffffffffff10ull, 0x003fffffffffff00ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x000000ffff000000ull, 0x000000ffff0001ffull,
     0x000000ffff0003ffull, 0x000000ffff0007ffull, 0x000000ffff0007ffull, 0x000000ffff0007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // gran premio: copa elegida
     0x0000000000000000ull, 0x0000ffffffffffffull, 0x0001ffffffffffffull, 0x0003ffffffffffffull,
     0x0007ffffffffffffull, 0x000fffffffffffffull, 0x001fffffffffffffull, 0x003fffffffffffffull,
     0x003fffffffffffffull, 0x003fffffffffffffull, 0x003fffffffffffffull, 0x003fffffffffffffull,
     0x003fffffffffffffull, 0x003fffffffffffffull, 0x003fffffffffffffull, 0x1fffffffffffffffull,
     0x3fffffffffffffffull, 0x3fffffffffffffffull, 0x3fffffffffffffffull, 0x3fffffffffffffffull,
     0x0fffffffffffffffull, 0xffffffffffff8000ull, 0xffffffffffffc000ull, 0xffffffffffffe000ull,
     0xfffffffffffff000ull, 0xfffffffffffff800ull, 0xfffffffffffffc00ull, 0xfffffffffffffff8ull,
     0xfffffffffffffffcull, 0xfffffffffffffffcull, 0xfffffffffffffffcull, 0xfffffffffffffffcull,
     0xfffffffffffffff8ull, 0xfffffffffffffe00ull, 0xfffffffffffffe00ull, 0xfffffffffffffe00ull,
     0xfffffffffffffe00ull, 0xfffffffffffffe00ull, 0xfffffffffffffc00ull, 0xfffffffffffff9ffull,
     0xfffffffffffff3ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // contrarreloj: pista elegida
     0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffe000000000ull, 0xffffffc000000000ull, 0xffffff8000000000ull,
     0xffffff0000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // batalla
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0xffffffffffffff80ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffff80ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffffff00ull,
     0xffffffffffffff80ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // misiones: niveles
     0x0000000000000000ull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x0000000000000000ull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x0000000000000000ull, 0x000001ffff800000ull,
     0x000001ffff800000ull, 0x000001ffff800000ull, 0x000001ffff800000ull, 0x000001ffff800000ull,
     0x000001ffff800000ull, 0x000001ffff800000ull, 0x000001ffff800000ull, 0x000001ffff800000ull,
     0x000001ffff800000ull, 0x000001ffff800000ull, 0x000001ffff800000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // misiones del nivel
     0x0000000000000000ull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x0000000000000000ull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x0000000000000000ull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // misión elegida
     0x0000000000000000ull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x3ffff1ffff8ffffcull, 0x3ffffffffffffffcull,
     0x3ffffffffffffffcull, 0x3ffffffffffffffcull, 0x3ffffffffffffffcull, 0x3ffffffffffffffcull,
     0x3ffffffffffffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x0000000000000000ull, 0x0000000000000000ull, 0x3ffffffffffffffcull,
     0x3ffffffffffffffcull, 0x3ffffffffffffffcull, 0x3ffffffffffffffcull, 0x3ffffffffffffffcull,
     0x3ffffffffffffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull,
     0x3ffff1ffff8ffffcull, 0x3ffff1ffff8ffffcull, 0x0000000000000000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // pausa
     0x0000000000000000ull, 0x0000000000003fffull, 0x3f5eb1fe00003fffull, 0x3f5eb1fa00003fffull,
     0x351e21fa00003fffull, 0x0000000000003fffull, 0x0000000000000007ull, 0x0000000000003fffull,
     0x0000000000003fffull, 0x0000000000003fffull, 0x0000000000003fffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // online: buscando rivales
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0x00000000000001ffull,
     0x2fffffe0000003ffull, 0x1d6ffff0000007ffull, 0x7d3d73a0000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // 123ee249
     0x0000000000000000ull, 0x003ffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull,
     0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull,
     0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull,
     0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull,
     0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull,
     0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull,
     0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull,
     0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull, 0x7ffffffffffffffeull,
     0x7ffffffffffffffeull, 0x7ffffffffffff800ull, 0x7ffffffffffff000ull, 0x7fffffffffffe000ull,
     0x7fffffffffffe000ull, 0x7fffffffffffe000ull, 0x7fffffffffffe000ull, 0x7fffffffffffe1ffull,
     0x7fffffffffffe3ffull, 0x7fffffffffffe7ffull, 0x7fffffffffffe7ffull, 0x7fffffffffffe7ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // a8d79200
     0x07fffffffffffff0ull, 0x0ffffffffffffff8ull, 0x1ffffffffffffffcull, 0x3ffffffffffffffcull,
     0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull,
     0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull,
     0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull,
     0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x3ffffffffffffff8ull, 0x0000000000000000ull,
     0x0ffffffffffffff0ull, 0x1ffffffffffffff8ull, 0x3ffffffffffffffcull, 0x7ffffffffffffffcull,
     0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull,
     0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull,
     0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull, 0x7ffffffffffffffcull,
     0x7ffffffffffffffcull, 0x3ffffffffffffff8ull, 0x0000000000000000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // 77de066c
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007ffffffffffe00ull, 0x0000000000000000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // eb8d6ba0
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007ffffffffffe00ull, 0x0000000000000000ull, 0x003ffffffff001ffull,
     0x003ffffffff003ffull, 0x003ffffffff007ffull, 0x003ffffffff007ffull, 0x003ffffffff007ffull,
     0x003ffffffff007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // c4804ab4
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull, 0x00ffffffffffff00ull,
     0x00ffffffffffff00ull, 0x00fffffffffffe00ull, 0x00ffc00000000000ull, 0x00fffffffff00000ull,
     0x003ffffffff00000ull, 0x003ffffffff00000ull, 0x003ffffffff00000ull, 0x003ffffffff00000ull,
     0x003ffffffff00000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    },
    {  // c7d8214c
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007ffffffffffe00ull, 0x0000000000000000ull, 0x003ffffffff00000ull,
     0x003ffffffff00000ull, 0x003ffffffff00000ull, 0x003ffffffff00000ull, 0x003ffffffff00000ull,
     0x003ffffffff00000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    },
    {  // 331554e8
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull, 0x007fffffffffff00ull,
     0x007fffffffffff00ull, 0x007ffffffffffe00ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    },
    {  // b2e8b8b9
     0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffffff00ull, 0xffffffffffffff80ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffff80ull, 0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffffff80ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffff80ull, 0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffffff00ull,
     0xffffffffffffff80ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffff80ull, 0x0000000000000000ull, 0x00000000000001ffull,
     0x00000000000003ffull, 0x00000000000007ffull, 0x00000000000007ffull, 0x00000000000007ffull,
     0x00000000000007ffull, 0x000000000007ffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
    },
    {  // 63062205
     0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffffff00ull, 0xffffffffffffff80ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffff80ull, 0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffffff80ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffff80ull, 0x0000000000000000ull, 0x0000000000000000ull, 0xffffffffffffff00ull,
     0xffffffffffffff80ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull, 0xffffffffffffffc0ull,
     0xffffffffffffffc0ull, 0xffffffffffffff80ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    },
    {  // 1b5e9199
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffe00000ull, 0xffffffffffc00000ull, 0xffffffffff800000ull, 0xffffffffff000000ull,
     0x000000ffff000000ull, 0x000000ffff000000ull, 0x000000ffff000000ull, 0x000000ffff000000ull,
     0x000000ffff000000ull, 0x000000ffff000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    },
    {  // 96596c8d
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull, 0xffffffffffffffffull,
     0xffffffffffe00000ull, 0xffffffffffc00000ull, 0xffffffffff8007feull, 0xffffffffff0007feull,
     0x00000000000007feull, 0x00000000000007feull, 0x000004000054e7feull, 0x00007f5ff7d7e7feull,
     0x0003ff5ff7d747feull, 0x00000000000007feull, 0x0000000000000000ull, 0x0000000000000000ull,
    },
    {  // 4afc1655 (la primera vez: el apodo)
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull,
     0x03ffffffffffffc0ull, 0x03ffffffffffffc0ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x03fffff00fffffc0ull, 0x02ffffd00bffff40ull, 0x03fffff00fffffc0ull, 0x03fffff00fffffc0ull,
     0x03fffff00fffffc0ull, 0x03fffff00fffffc0ull, 0x02ffffd00bffff40ull, 0x03fffff00fffffc0ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
     0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull, 0x0000000000000000ull,
    },
};
#define NMASC (int)(sizeof mascaras_p / sizeof mascaras_p[0])
static const int masc_pant[NMASC] = {0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 14, 15, 16, 17, 18, 19, 20, 22,
                                     27, 28, 29, 30, 31, 31, 31, 32, 33, 34, 35,  // (el multijugador)
                                     36};
#define PANT_ONLINE 10
#define PANT_CONECTAR 11
#define PANT_TITULO 26      // la portada (sin rejilla: abajo en blanco)
#define PANT_SALA 23        // salas privadas: el menú (+1 la creada, +2 unirse); se ven encima del menú principal
#define PANT_CONECTANDO 21  // tras el SÍ: la pantalla de abajo en blanco, "conectando" (no tiene rejilla)
#define PANT_BUSCANDO 22
#define PANT_CONTRARRELOJ 13
static volatile int sala_vista;  // 0 nada; 1 el menú de salas, 2 la creada, 3 unirse (sobre el menú principal)
static char codigo_sala[8];      // el que se va escribiendo para unirse
void online_crear(void);         // wifi.c
void online_unirse(const char *codigo);
int online_estado(char *codigo, int *jugadores);
static volatile int toque_principal = -1;  // el botón del menú principal tocado (un toque no mueve el marcado)
static volatile uint32_t toque_principal_t;
static volatile int marcado_p[32];  // lo último marcado en cada página (el menú principal dice si se va al online)  // de qué página de pantallas_p es cada rejilla

// cuántos puntos de la rejilla cambian entre dos pantallas (lo marcado mueve unos 20; otra pantalla, más de 500)
static int distancia(const uint64_t *a, const uint64_t *b) {
    int d = 0;
    for (int i = 0; i < 48; i++) d += __builtin_popcountll(a[i] ^ b[i]);
    return d;
}
#define PANT_COPAS 5
#define PANT_PISTAS 6
static SDL_Texture *tex_letras;  // los textos del juego en nuestros botones (Recorte), solo las letras
static SDL_Rect recorte_src[16];
static SDL_FRect recorte_dst[16];
static int nrecorte_d;
static volatile int pantalla_p = -1;  // la de pantallas_p que se ve ahora; -1 ninguna (la del juego)
static volatile int tapar_juego;      // transición entre pantallas: no se enseña la de abajo del juego
static volatile int sel_personaje;    // el retrato marcado en la de elegir personaje (0-11)
static volatile int pulsa_a;          // imágenes que queda pulsado A (acción 2)
static volatile int copa_vista = -1;  // paso 2 de las copas: cuál (0-7); -1 en el paso 1
// Botones de la cruceta pulsados por nosotros, uno detrás de otro (4 imágenes pulsado y 4 suelto): en la de elegir
// personaje, tocar un retrato lo elige, así que las flechas mueven el cursor con la cruceta
static volatile int cola_bits[8], cola_n, cola_i, cola_fase;
// el retrato al que hay que llevar el cursor (-1 nada): un paso de cruceta cada vez y se mira si ha llegado (el juego se
// salta pulsaciones mientras el cursor se mueve, y a final de fila la derecha no hace nada)
static volatile int objetivo_p = -1, espera_p, intentos_p, sel_antes = -1;
#define PANT_PERSONAJE 3
static void cruceta(const int *bits, int n) {
    if (cola_i < cola_n) return;  // aún moviendo
    for (int i = 0; i < n && i < 8; i++) cola_bits[i] = bits[i];
    cola_fase = 0; cola_i = 0; cola_n = n;
}
static const int ret_x[4] = {40, 100, 160, 220}, ret_y[3] = {30, 82, 134};  // centros de los 12 retratos

static int ov_x, ov_y, ov_w, ov_h;  // (definidos más abajo)
static char nombres_mandos[8][64];   // los mandos conectados (lee_mandos)
static volatile int n_mandos;
static volatile uint32_t mando_visto;  // cuándo se pulsó algo en un mando (SDL_GetTicks), para la luz de "funciona"
static volatile int demo_portada;  // la demo de la portada está en marcha (se tapa con la nuestra y se sale)
static volatile int pausa_propia, pulsa_start;  // pausa con nuestro botón (o START): enseña nuestra página de pausa
// tras QUIT: el juego pregunta en su pantalla de abajo; mientras, se enseña entera en el centro y se puede tocar
static volatile int saliendo;
#define PANT_PAUSA 20
// el botón de pausa de la carrera: arriba a la derecha, encima del mapa
static void rect_pausa(float *x0, float *y0, float *x1, float *y1) {
    float H = (float)dst_h, m = H * 0.03f, h = H * 0.1f, w = H * 0.16f;
    *x1 = dst_x + dst_w - m; *x0 = *x1 - w; *y0 = dst_y + m; *y1 = *y0 + h;
}
// dónde va el botón k de la pantalla q, en puntos de la ventana. Los de la pantalla (barras, categorías, copas) van en
// su sitio de la de abajo, encogidos debajo de la banda del título; los demás, en sitios fijos de la imagen 16:9
#define BANDA 0.17f  // alto de la banda del título (parte del alto de la imagen)
static void rect_p(int q, int k, float *x0, float *y0, float *x1, float *y1) {
    const BotonP *b = &pantallas_p[q].b[k];
    float H = (float)dst_h, m = H * 0.03f;
    if (b->tipo == 1) {  // volver: abajo a la derecha, una barra como las demás
        float h = H * 0.11f, w = H * 0.36f;
        *x1 = dst_x + dst_w - m; *x0 = *x1 - w; *y1 = dst_y + dst_h - m; *y0 = *y1 - h;
    } else if ((b->tipo == 2 || b->tipo == 3) && b->x1 == 0) {  // flechas: a los lados del botón de acción, "◀ OK ▶"
        float r = H * 0.075f, cy = dst_y + dst_h - m - H * 0.055f, cx = dst_x + dst_w / 2 + (b->tipo == 2 ? -1 : 1) * (H * 0.25f + r * 1.5f);
        if (q == PANT_PISTAS) { cx = dst_x + dst_w * 0.06f; }  // "a las copas": abajo a la izquierda
        *x0 = cx - r; *x1 = cx + r; *y0 = cy - r; *y1 = cy + r;
    } else if (b->tipo == 4) {  // acción: abajo en el centro
        float w = H * 0.5f, h = H * 0.11f;
        *x0 = dst_x + (dst_w - w) / 2; *x1 = *x0 + w; *y1 = dst_y + dst_h - m; *y0 = *y1 - h;
    } else {
        int con_volver = 0;
        for (int j = 0; j < pantallas_p[q].n; j++) con_volver |= pantallas_p[q].b[j].tipo == 1;
        // (el menú principal deja abajo sitio para el mando y el aviso)
        float top = dst_y + H * BANDA, alto = H * ((con_volver ? 0.84f : q == 0 ? 0.80f : 0.97f) - BANDA);
        float bw = dst_w * 256.0f / 342, bx = dst_x + (dst_w - bw) / 2;  // el centro 4:3 (en carrera el recuadro de abajo es el mapa)
        if (pantallas_p[q].arriba) { bx = dst_x + dst_w * 0.03f; bw = dst_w * 0.55f; }  // a la izquierda: a la derecha va lo de arriba
        *x0 = bx + b->x0 * bw / 256.0f; *x1 = bx + (b->x1 + 1) * bw / 256.0f;
        *y0 = top + b->y0 * alto / 192.0f; *y1 = top + (b->y1 + 1) * alto / 192.0f;
    }
}

// el teclado del código de sala (panel 7): la tecla k (0-35: A-Z y 0-9), 9 por fila, en puntos de la ventana
static const char teclas_sala[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
static void rect_tecla(int k, float *x0, float *y0, float *x1, float *y1) {
    float H = (float)dst_h, top = dst_y + H * BANDA, alto = H * (0.84f - BANDA);
    float bw = dst_w * 256.0f / 342, bx = dst_x + (dst_w - bw) / 2;
    int col = k % 9, fila = k / 9, dx = 11 + col * 26, dy = 72 + fila * 21;
    *x0 = bx + dx * bw / 256.0f; *x1 = bx + (dx + 24) * bw / 256.0f;
    *y0 = top + dy * alto / 192.0f; *y1 = top + (dy + 19) * alto / 192.0f;
}

// la tarjeta de la derecha: 1 el personaje, 2 el kart, 3 las pistas de una copa (en el centro)
static void rect_tarjeta(int panel, float *x0, float *y0, float *x1, float *y1) {
    float H = (float)dst_h, m = H * 0.03f;
    if (panel == 6 || panel == 7) { *x0 = *y0 = *x1 = *y1 = 0; return; }  // (estas se colocan solas)
    float w = panel == 1 ? H * 0.36f : panel == 2 ? H * 0.66f : H * 0.95f;
    float h = panel == 1 ? H * 0.5f : panel == 2 ? H * 0.64f : H * 0.62f;
    if (panel == 3) { *x0 = dst_x + (dst_w - w) / 2; *y0 = dst_y + H * BANDA; }
    else { *x0 = dst_x + dst_w - m - w; *y0 = dst_y + m; }  // arriba del todo a la derecha
    *x1 = *x0 + w; *y1 = *y0 + h;
}
int gx_polis(void);
extern volatile int g_3d_arriba;
static int ov_x, ov_y, ov_w, ov_h;
static volatile int pide_disp = -1;  // el menú la cambia: se hace en ventana_mensajes
int ventana_disposicion(void) { return disposicion; }
void ventana_pon_disposicion(int d) { pide_disp = d >= 0 && d <= 3 ? d : 0; }

static void calcula_destino(void) {
    int w, h;
    SDL_GetWindowSize(ventana, &w, &h);
    if (w <= 0 || h <= 0) return;
    int iw = pantalla_completa ? 512 : 256, ih = pantalla_completa ? 192 : 384;
    if (disposicion == 1) iw = 1024, ih = 576;  // 256x192 y 85x64 al lado: 16:9
    if (disposicion >= 2) iw = 256, ih = 192;
    if (disposicion == 3) iw = 342;
    EnterCriticalSection(&cs_destino);
    if (w * ih > h * iw) { dst_h = h; dst_w = h * iw / ih; }   // sobra ancho: bandas a los lados
    else { dst_w = w; dst_h = w * ih / iw; }                   // sobra alto: bandas arriba y abajo
    dst_x = (w - dst_w) / 2;
    dst_y = (h - dst_h) / 2;
    if (disposicion == 3 && en_carrera && !saliendo) {  // en carrera: solo el mapa (sin el tiempo ni la clasificación), arriba a la
        ov_h = dst_h * 30 / 100;            // derecha, debajo del botón de pausa
        ov_w = ov_h * 192 / 146;
        ov_x = dst_x + dst_w - ov_w - dst_h / 50;
        ov_y = dst_y + dst_h * 16 / 100;
    } else if (disposicion == 3) {  // una sola pantalla a 16:9: la de arriba en el centro (con los lados del 3D ancho) y
        ov_h = dst_h;                 // la de abajo, sin su fondo, encima de ella y del mismo tamaño
        ov_w = dst_w * 256 / 342;
        ov_x = dst_x + (dst_w - ov_w) / 2;
        ov_y = dst_y;
    } else if (disposicion == 2) {  // superpuesta: un tercio del ancho, con un pequeño margen
        int m = dst_w / 80;
        ov_w = dst_w / 3;
        ov_h = ov_w * 3 / 4;
        ov_x = dst_x + dst_w - ov_w - m;
        ov_y = dst_y + dst_h - ov_h - m;
    } else {  // al lado: la de arriba, a todo el alto; la táctil, en lo que queda a la derecha y abajo
        ov_x = dst_x + dst_h * 4 / 3;
        ov_w = dst_x + dst_w - ov_x;
        ov_h = ov_w * 3 / 4;
        ov_y = dst_y + dst_h - ov_h;
    }
    LeaveCriticalSection(&cs_destino);
}

// La siguiente imagen pendiente cuya hora ya ha llegado (y se la queda el pintor); -1 si no hay ninguna. En cola: "la
// más nueva" tiraba una imagen cada vez que un pequeño temblor juntaba dos en un refresco (con 240 FPS en un monitor
// de 240 Hz salían 212-231). Solo se salta si hay 3 o más esperando, para no ir acumulando retraso.
static int elige_marco(void) {
    static int64_t visto;
    int64_t ya = reloj();
    EnterCriticalSection(&cs_marcos);
    int mejor = -1, primero = -1, pendientes = 0;
    for (int i = 0; i < NM; i++) {
        if (!marco_ts[i] || marco_ts[i] > ya) continue;
        if (mejor < 0 || marco_ts[i] > marco_ts[mejor]) mejor = i;
        if (marco_ts[i] <= visto) continue;
        pendientes++;
        if (primero < 0 || marco_ts[i] < marco_ts[primero]) primero = i;
    }
    int elegido = pendientes >= 3 || primero < 0 ? mejor : primero;
    if (elegido >= 0) { i_viendo = elegido; visto = marco_ts[elegido]; }
    LeaveCriticalSection(&cs_marcos);
    return elegido;
}

static SDL_Texture *tex_juego;
static SDL_Texture *tex_abajo;  // DISPOSICION=3: la de abajo con el fondo transparente
static uint32_t *px_abajo;
static int abajo_w, abajo_h;
extern volatile int g_abajo_sin_fondo;

// la de abajo con alfa: la CLAVE de video.c, transparente; lo demás, opaco
static void sube_abajo(const uint32_t *px, int iw, int ih) {
    if (abajo_w != iw || abajo_h != ih) {
        if (tex_abajo) SDL_DestroyTexture(tex_abajo);
        tex_abajo = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, iw, ih);
        SDL_SetTextureBlendMode(tex_abajo, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex_abajo, iw > 256 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        free(px_abajo);
        px_abajo = malloc((size_t)iw * ih * 4);
        abajo_w = iw; abajo_h = ih;
    }
    for (size_t i = 0, n = (size_t)iw * ih; i < n; i++) px_abajo[i] = px[i] == 0x00010203u ? 0 : px[i] | 0xFF000000u;
    // Botones cerrados como RECORDS: las barras que se salen por la derecha (las de los menús) se cierran con su propio
    // extremo izquierdo dado la vuelta. Cada barra es un tramo de filas con el borde derecho opaco.
    int e = iw / 256, W = 9 * e;
    for (int y0 = 0; y0 < ih;) {
        if (!px_abajo[(size_t)y0 * iw + iw - 1]) { y0++; continue; }
        int y1 = y0, smin = iw;
        int ini[1536];  // dónde empieza el tramo que llega al borde, por fila
        for (; y1 < ih && px_abajo[(size_t)y1 * iw + iw - 1]; y1++) {
            int x = iw - 1;
            while (x > 0 && px_abajo[(size_t)y1 * iw + x - 1]) x--;
            ini[y1 - y0] = x;
            if (x < smin) smin = x;
        }
        if (smin < iw / 2)  // una barra de verdad (no un adorno pegado al borde)
            for (int y = y0; y < y1; y++) {
                uint32_t *f = px_abajo + (size_t)y * iw;
                for (int k = 0; k < W; k++) {
                    int src = smin + k;
                    f[iw - 1 - k] = src < ini[y - y0] ? 0 : f[src];
                }
            }
        y0 = y1;
    }
    SDL_UpdateTexture(tex_abajo, NULL, px_abajo, iw * 4);
}
static int tex_w, tex_h;

static void gl_rect(float x, float y, float w, float h, float u0, float v0, float u1, float v1) {
    SDL_Rect src = {(int)(u0 * tex_w), (int)(v0 * tex_h), (int)((u1 - u0) * tex_w), (int)((v1 - v0) * tex_h)};
    SDL_FRect dst = {x, y, w, h};
    if (src.w > 0 && src.h > 0) SDL_RenderCopyF(pintor, tex_juego, &src, &dst);
    else SDL_RenderFillRectF(pintor, &dst);  // sin textura: el borde negro
}

static void raton(int mx, int my, int pulsado);

// ---- controles táctiles: iOS (y en el Mac con MKDS_TACTIL=1, el ratón hace de dedo) ---------------------------------
// En carrera (DISPOSICION=3), con el estilo de la interfaz: la cruceta abajo a la izquierda; A, B, X, Y en rombo abajo a
// la derecha; ITEM (L) encima, debajo del mapa; R (derrape) a su izquierda; y otro ITEM y R encima de la cruceta, para
// la mano izquierda. Arriba a la derecha van la pausa y el mapa.
// Dos toques seguidos a izquierda o derecha (también con el teclado o el mando) = R: salto, y mantenido, derrape.
// Un dedo que no cae en un botón es el lápiz (fuera de carrera, la táctil del juego).
static int tactil;
typedef struct { float x, y, r; int bit, xy; const char *texto; int forma; } Boton;  // en alturas de ventana; x < 0: desde la derecha
static const Boton botones[] = {
    {-0.12f, 0.73f, 0.085f, 0, -1, "A", 10}, {-0.26f, 0.86f, 0.07f, 1, -1, "B", 10},
    {-0.26f, 0.60f, 0.06f, -1, 0, "X", 10},  {-0.40f, 0.73f, 0.06f, -1, 1, "Y", 10},
    {-0.11f, 0.535f, 0.065f, 9, -1, "", 12}, {-0.41f, 0.53f, 0.055f, 8, -1, "R", 10},
    {0.12f, 0.47f, 0.065f, 9, -1, "", 12},   {0.30f, 0.475f, 0.055f, 8, -1, "R", 10},  // otro ITEM y R, encima de la cruceta
};
#define NB (int)(sizeof botones / sizeof botones[0])
#define CRUZ_X 0.22f
#define CRUZ_Y 0.76f
#define CRUZ_R 0.17f
typedef struct { SDL_FingerID id; float x, y; int activo; } Dedo;
static Dedo dedos[10];
static volatile int b_pulsado[NB + 1];  // + la cruceta (para pintarlos)
static uint16_t tactil_in = 0x3FF, tactil_xy = 3;

// en la imagen del juego (dst: en el iPhone, sin las bandas de los lados, donde está la isla), no en la ventana
static void sitio(const Boton *b, int w, int h, float *x, float *y, float *r) {
    (void)w; (void)h;
    float H = (float)dst_h;
    *x = b->x < 0 ? dst_x + dst_w + b->x * H : dst_x + b->x * H;
    *y = dst_y + b->y * H;
    *r = b->r * H;
}
static int boton_en(float x, float y, int w, int h) {
    for (int i = 0; i < NB; i++) {
        float bx, by, r;
        sitio(&botones[i], w, h, &bx, &by, &r);
        if ((x - bx) * (x - bx) + (y - by) * (y - by) < r * r * 1.6f) return i;  // un poco de holgura
    }
    return -1;
}

// los botones y el lápiz según los dedos que hay ahora mismo (en ventana_mensajes, tras los eventos)
static void lee_dedos(void) {
    int w, h;
    SDL_GetWindowSize(ventana, &w, &h);
    uint16_t in = 0x3FF, xy = 3;
    int lapiz = 0, lx = 0, ly = 0, p[NB + 1] = {0};
    float cx = dst_x + CRUZ_X * dst_h, cy = dst_y + CRUZ_Y * dst_h, cr = CRUZ_R * dst_h;
    for (int d = 0; d < 10; d++) {
        if (!dedos[d].activo) continue;
        float x = dedos[d].x, y = dedos[d].y, dx = x - cx, dy = y - cy, dist = sqrtf(dx * dx + dy * dy);
        int b = boton_en(x, y, w, h);
        if (b >= 0) {
            p[b] = 1;
            if (botones[b].bit >= 0) in &= (uint16_t)~(1u << botones[b].bit);
            if (botones[b].xy >= 0) xy &= (uint16_t)~(1u << botones[b].xy);
        } else if (dist < cr * 1.4f) {  // cruceta: 8 direcciones (diagonales a partir de 22,5°)
            p[NB] = 1;
            if (dist > cr * 0.2f) {
                if (dx > 0.38f * dist) in &= ~(1u << 4);
                if (dx < -0.38f * dist) in &= ~(1u << 5);
                if (dy < -0.38f * dist) in &= ~(1u << 6);
                if (dy > 0.38f * dist) in &= ~(1u << 7);
            }
        } else if (!lapiz) {
            lapiz = 1; lx = (int)x; ly = (int)y;
        }
    }
    for (int i = 0; i <= NB; i++) b_pulsado[i] = p[i];
    tactil_in = in;
    tactil_xy = xy;
    raton(lx, ly, lapiz);
}

static void dedo(SDL_FingerID id, float x, float y, int abajo) {
    int libre = -1, i;
    for (i = 0; i < 10; i++) {
        if (dedos[i].activo && dedos[i].id == id) break;
        if (!dedos[i].activo && libre < 0) libre = i;
    }
    if (i == 10) { if (!abajo || libre < 0) return; i = libre; }
    int w, h;
    SDL_GetWindowSize(ventana, &w, &h);
    int nuevo = !dedos[i].activo && abajo;
    dedos[i] = (Dedo){id, x, y, abajo};
    (void)nuevo;
    if (menu_abierto()) { menu_raton(x, y); menu_boton(0, abajo); dedos[i].activo = 0; }  // el menú se lo queda
}

static void circulo_de(float cx, float cy, float r, SDL_Color c);
static void circulo(float cx, float cy, float r, Uint8 a) {
    circulo_de(cx, cy, r * 1.08f, (SDL_Color){0, 0, 0, 70});  // aro oscuro: que se vea también sobre fondo claro
    circulo_de(cx, cy, r, (SDL_Color){255, 255, 255, a});
}
static void circulo_de(float cx, float cy, float r, SDL_Color c) {
    SDL_Vertex v[26];
    int idx[72];
    v[0] = (SDL_Vertex){{cx, cy}, c, {0, 0}};
    for (int i = 0; i <= 24; i++) {
        float t = i * 6.2831853f / 24;
        v[i + 1] = (SDL_Vertex){{cx + r * cosf(t), cy + r * sinf(t)}, c, {0, 0}};
    }
    for (int i = 0; i < 24; i++) { idx[i * 3] = 0; idx[i * 3 + 1] = i + 1; idx[i * 3 + 2] = i + 2; }
    SDL_RenderGeometry(pintor, NULL, v, 26, idx, 72);
}

// los mandos táctiles para la capa de píxeles (en puntos de la ventana); cuántos ha puesto en mb
static int botones_tactiles(MenuBoton *mb, int w, int h) {
    int n = 0;
    for (int i = 0; i < NB; i++) {
        float x, y, r;
        sitio(&botones[i], w, h, &x, &y, &r);
        mb[n++] = (MenuBoton){x - r, y - r, x + r, y + r, botones[i].texto, b_pulsado[i], botones[i].forma, 0, 0};
    }
    float cr = CRUZ_R * dst_h;
    int dirs = 0;  // los brazos pulsados (der, izq, arriba, abajo), según lo que manda la táctil
    if (!(tactil_in & (1u << 4))) dirs |= 1;
    if (!(tactil_in & (1u << 5))) dirs |= 2;
    if (!(tactil_in & (1u << 6))) dirs |= 4;
    if (!(tactil_in & (1u << 7))) dirs |= 8;
    float ccx = dst_x + CRUZ_X * dst_h, ccy = dst_y + CRUZ_Y * dst_h;
    mb[n++] = (MenuBoton){ccx - cr, ccy - cr, ccx + cr, ccy + cr, "", 0, 11, dirs, 0};
    return n;
}

// La capa propia, en píxeles: se pinta a unos 270 de alto (ampliación entera) y se amplía sin suavizar
static void capa_pixel(MenuBoton *mb, int n, int ww, int wh, int vw, int vh) {
    static SDL_Texture *tex_ui;
    static int uw, uh;
    int fct = (wh + 135) / 270;  // ampliación entera (2, 3...): todos los píxeles iguales, nada borroso
    if (fct < 2) fct = 2;
    int nuh = (wh + fct - 1) / fct, nuw = (ww + fct - 1) / fct;
    if (uw != nuw || uh != nuh) {
        if (tex_ui) SDL_DestroyTexture(tex_ui);
        tex_ui = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, nuw, nuh);
        SDL_SetTextureBlendMode(tex_ui, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(tex_ui, SDL_ScaleModeNearest);
        uw = nuw; uh = nuh;
    }
    float k = 1.0f / fct;
    for (int j = 0; j < n; j++) { mb[j].x0 *= k; mb[j].x1 *= k; mb[j].y0 *= k; mb[j].y1 *= k; }
    menu_pon_botones(mb, n);
    if (tex_ui) {
        SDL_SetRenderTarget(pintor, tex_ui);
        SDL_RenderSetScale(pintor, 1, 1);
        SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColor(pintor, 0, 0, 0, 0);
        SDL_RenderClear(pintor);
        menu_pinta_botones(nuw, nuh);
        SDL_SetRenderTarget(pintor, NULL);
        SDL_RenderSetScale(pintor, (float)vw / ww, (float)vh / wh);
        SDL_FRect todo = {0, 0, (float)(nuw * fct), (float)(nuh * fct)};
        SDL_RenderCopyF(pintor, tex_ui, NULL, &todo);
    }
}

static void gl_pinta(int i, int subir) {
    int vw, vh, ww, wh;
    SDL_GetRendererOutputSize(pintor, &vw, &vh);  // en píxeles (con pantallas HiDPI no es lo mismo que los puntos)
    SDL_GetWindowSize(ventana, &ww, &wh);
    if (ww <= 0 || wh <= 0) return;
    int e = video_escala(), iw = 256 * e, ih = 384 * e;
    const uint32_t *px = e > 1 ? marcos[i].hd : marcos[i].baja;
    if (tex_w != iw || tex_h != ih) {
        if (tex_juego) SDL_DestroyTexture(tex_juego);
        tex_juego = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, iw, ih);
        SDL_SetTextureScaleMode(tex_juego, e > 1 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        tex_w = iw; tex_h = ih;
        subir = 1;
    }
    if (subir) SDL_UpdateTexture(tex_juego, NULL, px, iw * 4);
    SDL_RenderSetScale(pintor, (float)vw / ww, (float)vh / wh);  // en puntos de la ventana, como el ratón
    SDL_SetRenderDrawColor(pintor, 0, 0, 0, 255);
    SDL_RenderClear(pintor);
    if (disposicion == 3) {  // fuera de la imagen 16:9 (las bandas del iPhone, de una ventana más ancha): nuestros cuadros,
        // en línea con los de dentro, en vez de negro
        float c = dst_h * 16.0f / 192, x0 = dst_x - ceilf(dst_x / c) * c, y0 = dst_y - ceilf(dst_y / c) * c;
        SDL_SetRenderDrawColor(pintor, 0x14, 0x1F, 0x8E, 255);
        SDL_RenderClear(pintor);
        SDL_SetRenderDrawColor(pintor, 0x1B, 0x2A, 0xAE, 255);
        for (float y = y0; y < wh; y += c)
            for (float x = x0; x < ww; x += c) {
                int ix = (int)lroundf((x - dst_x) / c), iy = (int)lroundf((y - dst_y) / c);
                if (((ix + iy) & 1) == 0) continue;
                SDL_FRect r = {x, y, c, c};
                SDL_RenderFillRectF(pintor, &r);
            }
    }
    EnterCriticalSection(&cs_destino);
    float x = (float)dst_x, y = (float)dst_y, w = (float)dst_w, h = (float)dst_h;
    float ox = (float)ov_x, oy = (float)ov_y, ow = (float)ov_w, oh = (float)ov_h;
    int lado = pantalla_completa, al_lado = disposicion;
    LeaveCriticalSection(&cs_destino);
    if (al_lado == 3) {  // una sola pantalla
        int aw = marcos[i].ancho_w;
        if (aw) {  // cámara ancha: la de arriba entera a 16:9
            static SDL_Texture *tex_ancho;
            static int tw;
            if (tw != aw) {
                if (tex_ancho) SDL_DestroyTexture(tex_ancho);
                tex_ancho = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, aw, 192 * e);
                SDL_SetTextureScaleMode(tex_ancho, e > 1 ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
                tw = aw;
            }
            if (subir) SDL_UpdateTexture(tex_ancho, NULL, marcos[i].ancho, aw * 4);
            SDL_FRect da = {x, y, w, h};
            SDL_RenderCopyF(pintor, tex_ancho, NULL, &da);
        } else {
            gl_rect(ox, oy, ow, oh, 0, 0, 1, 0.5f);
        }
        int q = pantalla_p;
        if (q < 0) {
            if (subir) sube_abajo(px + (size_t)iw * ih / 2, iw, ih / 2);
            SDL_FRect d = {ox, oy, ow, oh};
            extern volatile int g_presentacion;
            // en la presentación del circuito, solo la de arriba; corriendo, el mapa pequeño arriba a la derecha (el
            // trozo del mapa: sin la franja del tiempo de arriba ni la clasificación de la izquierda) y el botón de pausa
            if (en_carrera && !saliendo && !g_presentacion && tex_abajo) {
                int e2 = iw / 256;
                // el mapa del juego tiene dos modos: de cerca (la clasificación es una columna a la izquierda, hasta x 57)
                // y el circuito entero (fondo azul marino con estrellas; los paneles con los nombres llegan hasta x 145).
                // Se mira el fondo para saber cuál y se recorta lo que es solo mapa. (Por color de los paneles no: el agua
                // de algunos circuitos es del mismo azul.)
                const uint32_t *ab = marcos[i].baja + 256 * 192;
                int marino = 0, total = 0;
                for (int y = 50; y < 190; y += 4)
                    for (int x = 150; x < 236; x += 4) {
                        uint32_t c = ab[y * 256 + x];
                        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                        marino += b > 90 && b < 190 && r < 70 && g < 90;
                        total++;
                    }
                int corte = marino * 10 > total * 3 ? 148 : 62;
                SDL_Rect src = {corte * e2, 46 * e2, (238 - corte) * e2, 146 * e2};  // sin clasificación, puestos ni tiempo
                float nw = d.h * (238 - corte) / 146.0f;  // el recuadro, del ancho del trozo (pegado a la derecha)
                d.x = d.x + d.w - nw; d.w = nw;
                SDL_SetRenderDrawColor(pintor, 0x9F, 0xE6, 0xFF, 255);
                float g = dst_h / 180.0f + 1;
                SDL_FRect borde = {d.x - g, d.y - g, d.w + 2 * g, d.h + 2 * g};
                SDL_RenderFillRectF(pintor, &borde);
                SDL_RenderCopyF(pintor, tex_abajo, &src, &d);
                MenuBoton pb[16];
                rect_pausa(&pb[0].x0, &pb[0].y0, &pb[0].x1, &pb[0].y1);
                pb[0].texto = "II"; pb[0].tipo = 4; pb[0].marcado = 0; pb[0].icono = 0; pb[0].valor = 0;
                int np = 1;
                if (tactil && !pausa_propia) np += botones_tactiles(pb + 1, ww, wh);  // los mandos táctiles
                capa_pixel(pb, np, ww, wh, vw, vh);
            } else if (tex_abajo && ((!tapar_juego && !g_presentacion && !en_carrera && !g_3d_arriba) || saliendo))
                SDL_RenderCopyF(pintor, tex_abajo, NULL, &d);  // (con 3D arriba sin ser de los nuestros: nunca la grande)
            menu_pon_botones(NULL, 0);
        } else {  // botones propios; el marcado es el del juego más claro (su pantalla de abajo, aunque no se vea)
            const uint32_t *ab = marcos[i].baja + 256 * 192;
            if (subir) sube_abajo(px + (size_t)iw * ih / 2, iw, ih / 2);  // (para recortar el retrato o el nombre)
            MenuBoton mb[64];
            int n = pantallas_p[q].n, mejor = -1, panel = pantallas_p[q].panel;
            float luz[16] = {0}, max = 0, seg = 0;  // (una por botón: hasta 16 en BotonP b[16])
            for (int k = 0; k < n; k++) {
                const BotonP *b = &pantallas_p[q].b[k];
                rect_p(q, k, &mb[k].x0, &mb[k].y0, &mb[k].x1, &mb[k].y1);
                mb[k].texto = b->texto; mb[k].tipo = b->tipo; mb[k].marcado = 0; mb[k].icono = b->icono; mb[k].valor = 0;
                if ((b->tipo != 0 && b->tipo != 8) || b->accion >= 7) continue;
                int cuenta = 0;
                int sx0 = b->x0 + 20, sx1 = b->x1 - 4, sy0 = b->y0 + 2, sy1 = b->y1 - 1;
                if (b->accion == 1) {  // el botón del juego al que va el toque (el nuestro puede estar en otro sitio)
                    sx0 = b->tx - 60; sx1 = b->tx + 60; sy0 = b->ty - 13; sy1 = b->ty - 8;  // la franja de arriba, sin letras
                    if (sx0 < 0) sx0 = 0;
                    if (sx1 > 255) sx1 = 255;
                    if (sy0 < 0) sy0 = 0;
                    if (sy1 > 191) sy1 = 191;
                }
                for (int y = sy0; y < sy1; y += 3)
                    for (int x = sx0; x < sx1; x += 5) {
                        uint32_t c = ab[y * 256 + x];
                        if (c == 0x00010203u) continue;
                        luz[k] += (float)(((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255));
                        cuenta++;
                    }
                if (cuenta) luz[k] /= cuenta;
                if (luz[k] > max) { seg = max; max = luz[k]; mejor = k; } else if (luz[k] > seg) seg = luz[k];
            }
            if (mejor >= 0 && max > seg + 40) { mb[mejor].marcado = 1; marcado_p[q] = mejor; }
            if (panel == 1 || panel == 2) {  // el nombre del personaje, con nuestra letra (el del juego se ha quitado)
                static const char *nombres_p[12] = {"MARIO", "LUIGI", "PEACH", "YOSHI", "TOAD", "DONKEY KONG",
                                                   "WARIO", "BOWSER", "DAISY", "DRY BONES", "WALUIGI", "R.O.B."};
                float H = (float)dst_h;
                int sn = objetivo_p >= 0 ? objetivo_p : sel_personaje;
                mb[n++] = (MenuBoton){dst_x + H * 0.05f, dst_y + H * 0.16f, dst_x + dst_w * 0.5f, dst_y + H * 0.25f,
                                      nombres_p[sn % 12], 0, 7, 0, 0};
            }
            if (pantallas_p[q].arriba) {  // lo de arriba del juego (récords, pistas, la misión), en una tarjeta a la derecha
                static SDL_Texture *tex_arr;
                static uint32_t *px_arr;
                static int aw2, ah2;
                int iw2 = iw, ih2 = ih / 2;
                if (aw2 != iw2 || ah2 != ih2) {
                    if (tex_arr) SDL_DestroyTexture(tex_arr);
                    tex_arr = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, iw2, ih2);
                    SDL_SetTextureBlendMode(tex_arr, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(tex_arr, SDL_ScaleModeNearest);
                    free(px_arr);
                    px_arr = malloc((size_t)iw2 * ih2 * 4);
                    aw2 = iw2; ah2 = ih2;
                }
                if (subir && px_arr) {
                    for (size_t k = 0, nn = (size_t)iw2 * ih2; k < nn; k++) px_arr[k] = px[k] == 0x00010203u ? 0 : px[k] | 0xFF000000u;
                    SDL_UpdateTexture(tex_arr, NULL, px_arr, iw2 * 4);
                }
                float H = (float)dst_h, cw = dst_w * 0.38f, ch = cw * 0.75f;
                float cx0 = dst_x + dst_w * 0.6f, cy0 = dst_y + H * BANDA;
                SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(pintor, 0x14, 0x1F, 0x8E, 235);
                SDL_FRect fr = {cx0, cy0, cw, ch};
                SDL_RenderFillRectF(pintor, &fr);
                SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_NONE);
                if (tex_arr) SDL_RenderCopyF(pintor, tex_arr, NULL, &fr);
                mb[n++] = (MenuBoton){cx0, cy0, cx0 + cw, cy0 + ch, "", 0, 5, 0, 0};
            }
            static char msg_sala[96];
            if (q == PANT_SALA + 1 || q == PANT_SALA + 2) {  // la sala: el código y cuántos hay (o cómo va)
                char cod[8] = "";
                int jug = 0, e = online_estado(cod, &jug);
                if (q == PANT_SALA + 1)
                    snprintf(msg_sala, sizeof msg_sala, e == 7 ? "Room code: %s   Players: %d" : e == 1 ? "Creating room..." :
                             "Could not create the room. Check your internet.", cod, jug + 1);
                else
                    snprintf(msg_sala, sizeof msg_sala, e == 8 ? "Joined room %s. Press START." : e == 1 ? "Joining..." :
                             e == 4 ? "No room with code %s." : e == 6 ? "That room is full." : "Code: %s_", e == 4 ? codigo_sala :
                             e == 8 ? cod : codigo_sala);
            }
            if (pantallas_p[q].mensaje) {  // el mensaje de la pantalla, en una tarjeta arriba en el centro
                float H = (float)dst_h, w = H * 1.1f, x0m = dst_x + (dst_w - w) / 2, y0m = dst_y + H * 0.2f, y1m = y0m + H * 0.2f;
                SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(pintor, 0x14, 0x1F, 0x8E, 235);
                SDL_FRect fr = {x0m, y0m, w, y1m - y0m};
                SDL_RenderFillRectF(pintor, &fr);
                SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_NONE);
                mb[n++] = (MenuBoton){x0m, y0m, x0m + w, y1m, "", 0, 5, 0, 0};
                const char *msg = (q == PANT_SALA + 1 || q == PANT_SALA + 2) ? msg_sala : pantallas_p[q].mensaje;
                float my0 = strchr(msg, '\n') ? y0m + H * 0.03f : y0m + H * 0.065f;  // dos líneas: empieza más arriba
                mb[n++] = (MenuBoton){x0m + H * 0.05f, my0, x0m + w - H * 0.05f, my0 + H * 0.07f, msg, 0, 7, 0, 0};
                if (q == PANT_CONECTANDO || q == PANT_BUSCANDO || q == PANT_ESPERA_MP || q == PANT_JUEGO_INV)  // un círculo de puntos que gira: está trabajando
                    mb[n++] = (MenuBoton){x0m + w - H * 0.16f, y0m + H * 0.04f, x0m + w - H * 0.04f, y0m + H * 0.16f, "", 0, 14, 0, 0};
            }
            if (!tactil && (q == 0 || q == 7)) {  // abajo a la izquierda (menú principal y opciones): qué mando hay y si
                                                  // funciona (la luz se enciende al pulsar)
                static char nombre_chip[48];
                float H = (float)dst_h, y1 = dst_y + dst_h - H * (q == 0 ? 0.075f : 0.03f), y0 = y1 - H * 0.09f;
                int hay = n_mandos > 0;
                snprintf(nombre_chip, sizeof nombre_chip, "%.40s", hay ? nombres_mandos[0] : "Keyboard");
                int activo = hay && SDL_GetTicks() - mando_visto < 250;
                mb[n++] = (MenuBoton){dst_x + H * 0.03f, y0, dst_x + H * 0.03f + H * 1.05f, y1, nombre_chip,
                                      activo, 13, hay, 0};
            }
            if (q == 0) {  // en el menú principal, el aviso: proyecto no oficial, el juego es de Nintendo
                float H = (float)dst_h;
                mb[n++] = (MenuBoton){dst_x + H * 0.04f, dst_y + dst_h - H * 0.06f, dst_x + dst_w, dst_y + dst_h - H * 0.02f,
                                      "Unofficial fan project. Game (c) Nintendo.", 0, 7, 0, 0};
            }
            if (q == PANT_TITULO) {  // la portada: el nombre grande en el centro y START parpadeando
                float H = (float)dst_h;
                mb[n++] = (MenuBoton){dst_x, dst_y + H * 0.28f, dst_x + dst_w, dst_y + H * 0.5f, "ACTION KART", 0, 15, 0, 0};
                if ((SDL_GetTicks() / 600) % 2) mb[0].texto = "";  // (START parpadea)
            }
            nrecorte_d = 0;
            if (pantallas_p[q].nrec) {  // textos del juego (nombres, valores) en nuestros botones: solo sus letras, en blanco
                static uint32_t letras[256 * 192];
                static char vacio[] = "EMPTY";
                if (!tex_letras) {
                    tex_letras = SDL_CreateTexture(pintor, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 192);
                    SDL_SetTextureBlendMode(tex_letras, SDL_BLENDMODE_BLEND);
                    SDL_SetTextureScaleMode(tex_letras, SDL_ScaleModeNearest);
                }
                memset(letras, 0, sizeof letras);
                for (int r = 0; r < pantallas_p[q].nrec && nrecorte_d < 16; r++) {
                    const Recorte *rc = &pantallas_p[q].rec[r];
                    MenuBoton *bt = &mb[rc->boton];
                    if (pantallas_p[q].b[rc->boton].tipo == 5) {  // tarjeta: su fondo, aquí (el menú pone el borde)
                        SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_BLEND);
                        SDL_SetRenderDrawColor(pintor, 0x14, 0x1F, 0x8E, 235);
                        SDL_FRect fr = {bt->x0, bt->y0, bt->x1 - bt->x0, bt->y1 - bt->y0};
                        SDL_RenderFillRectF(pintor, &fr);
                        SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_NONE);
                    }
                    uint32_t fondo = ab[(rc->y + 1) * 256 + rc->x + 1];
                    int x0 = 256, y0 = 192, x1 = -1, y1 = -1, por_fila[192] = {0};
                    for (int y = rc->y; y < rc->y + rc->h && y < 192; y++)
                        for (int x = rc->x; x < rc->x + rc->w && x < 256; x++) {
                            uint32_t c = ab[y * 256 + x];
                            if (c == 0x00010203u) continue;
                            int dr = abs((int)((c >> 16) & 255) - (int)((fondo >> 16) & 255));
                            int dg = abs((int)((c >> 8) & 255) - (int)((fondo >> 8) & 255));
                            int db = abs((int)(c & 255) - (int)(fondo & 255));
                            int m2 = dr > dg ? dr : dg;
                            if (db > m2) m2 = db;
                            if (m2 < 110) continue;
                            letras[y * 256 + x] = 0xFFFFFFFFu;
                            por_fila[y]++;
                        }
                    // lo que ocupan las letras: filas con al menos 2 puntos (un punto suelto no cuenta)
                    for (int y = rc->y; y < rc->y + rc->h && y < 192; y++)
                        if (por_fila[y] >= 2) { if (y < y0) y0 = y; y1 = y; }
                    for (int y = y0; y <= y1; y++)
                        for (int x = rc->x; x < rc->x + rc->w && x < 256; x++)
                            if (letras[y * 256 + x]) { if (x < x0) x0 = x; if (x > x1) x1 = x; }
                    float bx0 = bt->x0, bx1 = bt->x1, by0 = bt->y0, by1 = bt->y1;
                    if (rc->etiqueta) {  // la etiqueta a la izquierda, el valor a la derecha
                        float hh = by1 - by0;
                        mb[n++] = (MenuBoton){bx0 + hh * 0.3f, by0 + hh * 0.22f, bx0 + (bx1 - bx0) * 0.45f, by1 - hh * 0.22f, rc->etiqueta, 0, 7, 0, 0};
                        bx0 += (bx1 - bx0) * 0.42f;
                    }
                    if (y1 - y0 < 4 || x1 < x0) {  // sin letras (la raya de un hueco vacío)
                        if (pantallas_p[q].b[rc->boton].tipo == 0) bt->texto = vacio;
                        continue;
                    }
                    float sw = (float)(x1 - x0 + 1), sh = (float)(y1 - y0 + 1), dw = bx1 - bx0, dh = by1 - by0;
                    float e = 0.9f * dw / sw;
                    if (0.85f * dh / sh < e) e = 0.85f * dh / sh;
                    if (dh * 0.5f / 11 < e) e = dh * 0.5f / 11;  // una línea: del alto de nuestras letras
                    recorte_src[nrecorte_d] = (SDL_Rect){x0, y0, x1 - x0 + 1, y1 - y0 + 1};
                    recorte_dst[nrecorte_d++] = (SDL_FRect){(bx0 + bx1 - sw * e) / 2, (by0 + by1 - sh * e) / 2, sw * e, sh * e};
                }
                SDL_UpdateTexture(tex_letras, NULL, letras, 256 * 4);
            }
            {  // el título de la página (en el paso 2 de las copas, el nombre de la copa)
                const char *tit = q == PANT_PISTAS && copa_vista >= 0 ? pantallas_p[PANT_COPAS].b[copa_vista].texto : pantallas_p[q].titulo;
                float H = (float)dst_h;
                mb[n++] = (MenuBoton){dst_x + H * 0.04f, dst_y + H * 0.025f, dst_x + dst_w * 0.6f, dst_y + H * 0.135f, tit, 0, 7, 0, 0};
            }
            if (panel) {  // tarjeta arriba a la derecha: fondo aquí (debajo de la imagen), borde y textos en el menú
                float t0, t1, t2, t3;
                rect_tarjeta(panel, &t0, &t1, &t2, &t3);
                SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_BLEND);
                SDL_SetRenderDrawColor(pintor, 0x14, 0x1F, 0x8E, 235);
                SDL_FRect fondo = {t0, t1, t2 - t0, t3 - t1};
                if (panel != 5) SDL_RenderFillRectF(pintor, &fondo);  // (los selectores llevan sus propias tarjetas)
                SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_NONE);
                float W = t2 - t0, Hh = t3 - t1;
                static char titulo[16];
                if (panel == 1) {  // el retrato marcado (el del marco naranja), en grande
                    int sel = objetivo_p >= 0 ? objetivo_p : sel_personaje;  // yendo a otro: ya el de destino
                    int e2 = iw / 256;
                    SDL_Rect src = {(ret_x[sel % 4] - 18) * e2, (ret_y[sel / 4] - 18) * e2, 36 * e2, 36 * e2};  // sin el marco
                    float lado = W * 0.76f;
                    SDL_FRect dst2 = {t0 + (W - lado) / 2, t1 + W * 0.12f, lado, lado};
                    if (tex_abajo) SDL_RenderCopyF(pintor, tex_abajo, &src, &dst2);
                    snprintf(titulo, sizeof titulo, "%d / 12", sel + 1);
                    mb[n++] = (MenuBoton){t0, t1, t2, t3, "", 0, 5, 0, 0};
                    float ty0 = t1 + W * 0.12f + lado + Hh * 0.03f;  // el número, debajo del retrato, como título pequeño
                    mb[n++] = (MenuBoton){t0 + (W - W * 0.5f) / 2, ty0, t2, ty0 + Hh * 0.13f, titulo, 0, 7, 0, 0};
                } else if (panel == 5) {  // selectores: el valor de cada uno es el recuadro del juego, recortado
                    int e2 = iw / 256;
                    for (int r = 0; r < pantallas_p[q].nsel; r++) {
                        const Selector *sl = &pantallas_p[q].sel[r];
                        float a0, b0, a1, b1;  // la tarjeta, entre las dos flechas de su fila
                        float top = dst_y + dst_h * BANDA, alto = dst_h * (0.84f - BANDA);
                        a0 = ov_x + 40 * ov_w / 256.0f; a1 = ov_x + 217 * ov_w / 256.0f;
                        b0 = top + sl->fy * alto / 192.0f; b1 = top + (sl->fy + 49) * alto / 192.0f;
                        SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_BLEND);
                        SDL_SetRenderDrawColor(pintor, 0x14, 0x1F, 0x8E, 235);
                        SDL_FRect fr = {a0, b0, a1 - a0, b1 - b0};
                        SDL_RenderFillRectF(pintor, &fr);
                        SDL_SetRenderDrawBlendMode(pintor, SDL_BLENDMODE_NONE);
                        SDL_Rect src = {sl->vx * e2, sl->vy * e2, sl->vw * e2, sl->vh * e2};
                        float vw2 = (a1 - a0) * 0.86f, vh2 = vw2 * sl->vh / sl->vw;
                        if (vh2 > (b1 - b0) * 0.46f) { vh2 = (b1 - b0) * 0.46f; vw2 = vh2 * sl->vw / sl->vh; }  // sin pisar el nombre
                        SDL_FRect dv = {a0 + (a1 - a0 - vw2) / 2, b1 - vh2 - (b1 - b0) * 0.1f, vw2, vh2};
                        if (tex_abajo) SDL_RenderCopyF(pintor, tex_abajo, &src, &dv);
                        mb[n++] = (MenuBoton){a0, b0, a1, b1, "", 0, 5, 0, 0};
                        float lh = (b1 - b0) * 0.34f;
                        mb[n++] = (MenuBoton){a0 + (a1 - a0) * 0.05f, b0 + (b1 - b0) * 0.06f, a1, b0 + (b1 - b0) * 0.06f + lh, sl->etiqueta, 0, 7, 0, 0};
                    }
                } else if (panel == 7) {  // el teclado del código de sala
                    static char letra[36][2];
                    for (int k = 0; k < 36 && n < 60; k++) {
                        letra[k][0] = teclas_sala[k]; letra[k][1] = 0;
                        float a0, b0, a1, b1;
                        rect_tecla(k, &a0, &b0, &a1, &b1);
                        mb[n++] = (MenuBoton){a0, b0, a1, b1, letra[k], 0, 16, 0, 0};
                    }
                } else if (panel == 6) {  // los rivales que va encontrando el juego (su lista de abajo), en una tarjeta
                    int e2 = iw / 256;
                    SDL_Rect src = {0, 0, 256 * e2, 156 * e2};
                    float H = (float)dst_h, ch = H * 0.42f, cw = ch * 256 / 156;
                    SDL_FRect dc = {dst_x + (dst_w - cw) / 2, dst_y + H * 0.43f, cw, ch};
                    if (tex_abajo) SDL_RenderCopyF(pintor, tex_abajo, &src, &dc);
                    mb[n++] = (MenuBoton){dc.x, dc.y, dc.x + cw, dc.y + ch, "", 0, 5, 0, 0};
                } else if (panel == 3) {  // las pistas de la copa: la lista que el juego enseña arriba al marcarla
                    int e2 = iw / 256;
                    SDL_Rect src = {0, 74 * e2, 176 * e2, 78 * e2};
                    float lw = W * 0.9f, lh = lw * 78 / 176;
                    if (lh > Hh * 0.8f) { lh = Hh * 0.8f; lw = lh * 176 / 78; }
                    SDL_FRect dst2 = {t0 + (W - lw) / 2, t1 + (Hh - lh) / 2, lw, lh};
                    SDL_RenderCopyF(pintor, tex_juego, &src, &dst2);
                    mb[n++] = (MenuBoton){t0, t1, t2, t3, "", 0, 5, 0, 0};
                } else {  // el kart: su nombre (recortado de la del juego) y sus 6 estadísticas (lo largo de cada barra)
                    int e2 = iw / 256;
                    SDL_Rect src = {34 * e2, 11 * e2, 178 * e2, 21 * e2};
                    float nw = W * 0.9f, nh = nw * 21 / 178;
                    SDL_FRect dst2 = {t0 + (W - nw) / 2, t1 + Hh * 0.05f, nw, nh};
                    if (tex_abajo) SDL_RenderCopyF(pintor, tex_abajo, &src, &dst2);
                    mb[n++] = (MenuBoton){t0, t1, t2, t3, "", 0, 5, 0, 0};
                    static const char *nombres[6] = {"SPEED", "ACCEL", "WEIGHT", "HANDLING", "DRIFT", "ITEMS"};
                    for (int k = 0; k < 6; k++) {
                        int y = 46 + 16 * k, fin = 0;
                        for (int x = 117; x < 256; x++) {
                            uint32_t c2 = ab[y * 256 + x];
                            if (((c2 >> 16) & 255) > 180 && ((c2 >> 8) & 255) > 200 && (c2 & 255) < 120) fin = x;
                        }
                        float v = fin ? (fin - 117) / 123.0f : 0;
                        float y0 = t1 + Hh * (0.2f + k * 0.13f);
                        mb[n++] = (MenuBoton){t0 + W * 0.06f, y0, t2 - W * 0.06f, y0 + Hh * 0.1f, nombres[k], 0, 6, 0, v > 1 ? 1 : v};
                    }
                }
            }
            capa_pixel(mb, n, ww, wh, vw, vh);
            menu_pon_botones(NULL, 0);
        }
    } else if (al_lado) {  // la de arriba a todo el alto; la táctil a su derecha, abajo, o superpuesta con un borde negro
        gl_rect(x, y, al_lado == 2 ? w : ox - x, h, 0, 0, 1, 0.5f);
        if (al_lado == 2) {
            float bo = ow / 100 + 2;
            gl_rect(ox - bo, oy - bo, ow + 2 * bo, oh + 2 * bo, 0, 0, 0, 0);
        }
        gl_rect(ox, oy, ow, oh, 0, 0.5f, 1, 1);
    } else if (lado) {  // pantalla de arriba a la izquierda, táctil a la derecha
        gl_rect(x, y, w / 2, h, 0, 0, 1, 0.5f);
        gl_rect(x + w / 2, y, w / 2, h, 0, 0.5f, 1, 1);
    } else {
        gl_rect(x, y, w, h, 0, 0, 1, 1);
    }
    {  // la portada de Skyendo tapa la del arranque del juego: hasta la imagen 137 entera y se va en la 155, ya en el
       // título (contado en imágenes del juego, no en tiempo: así da igual lo que tarde en arrancar)
        // (se queda hasta la imagen 162, cuando ya está nuestra portada debajo, y se va encima de ella)
        uint32_t ms = g_imagenes < 162 ? (SDL_GetTicks() < 2199 ? SDL_GetTicks() : 2199) : 2200 + (g_imagenes - 162) * 1000 / 60;
        skyendo_pinta(pintor, ww, wh, ms);
    }
    menu_pinta(ww, wh, vw, vh);  // el menú de ajustes y el contador de FPS, si toca
    for (int r = 0; r < nrecorte_d && tex_letras; r++)  // los textos del juego, encima de nuestros botones
        SDL_RenderCopyF(pintor, tex_letras, &recorte_src[r], &recorte_dst[r]);
    nrecorte_d = 0;  // (se vuelven a poner en cada imagen, solo en nuestras páginas)
    {  // pruebas: MKDS_FOTO="450,800" guarda la ventana tal cual en esas imágenes (foto_450.bmp...) en MKDS_FOTO_DIR
        static const char *fotos;
        static int mirado;
        if (!mirado) { fotos = getenv("MKDS_FOTO"); mirado = 1; }
        for (const char *p = fotos; p && *p;) {
            unsigned n = (unsigned)strtoul(p, (char **)&p, 10);
            if (*p == ',') p++;
            static unsigned hecha;
            if (n != g_imagenes || n == hecha) continue;
            hecha = n;
            SDL_Surface *f = SDL_CreateRGBSurfaceWithFormat(0, vw, vh, 32, SDL_PIXELFORMAT_ARGB8888);
            SDL_RenderSetScale(pintor, 1, 1);  // leer en píxeles, no en puntos
            int leido = f && !SDL_RenderReadPixels(pintor, NULL, SDL_PIXELFORMAT_ARGB8888, f->pixels, f->pitch);
            SDL_RenderSetScale(pintor, (float)vw / ww, (float)vh / wh);
            if (leido) {
                char ruta[600];
                snprintf(ruta, sizeof ruta, "%s/foto_%u.bmp", getenv("MKDS_FOTO_DIR") ? getenv("MKDS_FOTO_DIR") : ".", n);
                SDL_SaveBMP(f, ruta);
            }
            SDL_FreeSurface(f);
        }
    }
    SDL_RenderPresent(pintor);  // espera al refresco del monitor
}

static DWORD WINAPI hilo_pinta(LPVOID p) {
    (void)p;
    if (!menu_inicia()) fprintf(stderr, "[ventana] no se pudo preparar el menú\n");
    int subido = -1;
    int64_t ts_subido = 0;
    for (;;) {
        int i = elige_marco();
        if (i < 0) { Sleep(1); continue; }
        int nuevo = i != subido || marco_ts[i] != ts_subido;
        gl_pinta(i, nuevo);
        if (nuevo) InterlockedIncrement(&enseñadas);
        subido = i;
        ts_subido = marco_ts[i];
    }
    return 0;
}

static volatile int pide_pc;  // el menú pide pantalla completa (1) o ventana (2): se hace en ventana_mensajes
int ventana_pantalla_completa(void) { return pantalla_completa; }
void ventana_pide_pantalla_completa(int si) { pide_pc = si ? 1 : 2; }

static void cambia_pantalla_completa(void) {
    pantalla_completa = !pantalla_completa;
    SDL_SetWindowFullscreen(ventana, pantalla_completa ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    calcula_destino();
}

// Al lado: la ventana en 16:9 con el mismo alto; superpuesta, 512x384 (en las dos, la de arriba al doble); las dos
// iguales, como al principio
static int ancho_ventana(void) {
    return disposicion == 1 || disposicion == 3 ? (384 * ESCALA * 16 + 8) / 9 : (disposicion >= 2 ? 512 : 256) * ESCALA;
}
static void cambia_disposicion(int d) {
    disposicion = d;
    g_abajo_sin_fondo = d == 3;
    if (!pantalla_completa) SDL_SetWindowSize(ventana, ancho_ventana(), 384 * ESCALA);
    calcula_destino();
}

static SDL_Window *ventana3d;
static SDL_GLContext contexto3d;
int ventana_gl3d(void) { return contexto3d && !SDL_GL_MakeCurrent(ventana3d, contexto3d); }

void ventana_inicia(void) {
    InitializeCriticalSection(&cs_marcos);
    InitializeCriticalSection(&cs_destino);
    if (getenv("MKDS_DISPOSICION") && atoi(getenv("MKDS_DISPOSICION")) == 3) { disposicion = 3; g_abajo_sin_fondo = 1; }
    if (getenv("MKDS_OCULTO")) { oculto = 1; return; }
    tactil = TARGET_OS_IPHONE || getenv("MKDS_TACTIL");
#if TARGET_OS_IPHONE
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");  // los dedos van por SDL_FINGER*, no como ratón
    if (!getenv("MKDS_DISPOSICION")) _putenv_s("MKDS_DISPOSICION", "3");  // una sola pantalla 16:9, la interfaz propia
#endif
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");  // los botones por su sitio en todos los mandos
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER)) {
        fprintf(stderr, "[ventana] SDL: %s\n", SDL_GetError());
        exit(1);
    }
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    disposicion = getenv("MKDS_DISPOSICION") ? atoi(getenv("MKDS_DISPOSICION")) : 0;
    if (disposicion < 0 || disposicion > 3) disposicion = 0;
    ventana = SDL_CreateWindow("DS Kart Recompiled", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               ancho_ventana(), 384 * ESCALA,
                               SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!ventana) { fprintf(stderr, "[ventana] no se pudo abrir: %s\n", SDL_GetError()); exit(1); }
    pintor = SDL_CreateRenderer(ventana, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!pintor) { fprintf(stderr, "[ventana] sin SDL_Renderer: %s\n", SDL_GetError()); exit(1); }
    menu_renderer = pintor;  // el menú (imgui) pinta con el mismo
    // El 3D por la GPU (gpu3d_gl.c) va en su propio contexto, del hilo de pintar el 3D, pero la ventana se crea aquí: en
    // macOS solo se puede en el hilo principal
#if !TARGET_OS_IPHONE  // (en iOS no hay 3D por la GPU: ni ventana oculta)
    ventana3d = SDL_CreateWindow("", 0, 0, 16, 16, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
#endif
    if (ventana3d) {
        contexto3d = SDL_GL_CreateContext(ventana3d);
        SDL_GL_MakeCurrent(ventana3d, NULL);
    }
    g_abajo_sin_fondo = disposicion == 3;
    calcula_destino();
    if (getenv("MKDS_PANTALLA_COMPLETA") && atoi(getenv("MKDS_PANTALLA_COMPLETA"))) cambia_pantalla_completa();
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i)) SDL_GameControllerOpen(i);
    CreateThread(NULL, 0, hilo_pinta, NULL, 0, NULL);
}

// MKDS_GRABA=portada.mp4: graba el vídeo de la portada del launcher (ver ventana.c)
static void graba_portada(const uint32_t *hd, const uint32_t *baja, int e) {
    static int listo, carreras_max, carrera, fotogramas, fuera = 1000;
    static FILE *ff;
    if (!listo) {
        listo = 1;
        const char *ruta = getenv("MKDS_GRABA");
        if (!ruta) return;
        carreras_max = getenv("MKDS_GRABA_CARRERAS") ? atoi(getenv("MKDS_GRABA_CARRERAS")) : 7;
        char orden[1024];
        snprintf(orden, sizeof orden,
                 "ffmpeg -v error -y -f rawvideo -pixel_format bgra -video_size %dx%d -framerate 60 -i - "
                 "-vf \"scale=1152:864:flags=lanczos,format=yuv420p\" -c:v libx264 -preset veryfast -crf 20 "
                 "-movflags +faststart -an \"%s\"", 256 * e, 192 * e, ruta);
        ff = popen(orden, "w");
    }
    if (!ff) return;
    if (gx_polis() <= 50) { fuera++; return; }
    if (fuera >= 120) { carrera++; fotogramas = 0; }
    fuera = 0;
    fotogramas++;
    if (fotogramas > 300 && fotogramas <= 300 + 900)
        fwrite(e > 1 ? hd : baja, 4, (size_t)256 * e * 192 * e, ff);
    if (carrera >= carreras_max && fotogramas == 300 + 900) {
        pclose(ff);
        fprintf(stderr, "[mkds] portada grabada: %d carreras\n", carrera);
        ExitProcess(0);
    }
}

void ventana_guarda_bmp_de(const char *ruta, const uint32_t *px, int e);
static void guarda_intermedia(void) {
    static int listo, n, desde, hasta = 1 << 30;  // MKDS_CAPTURA_TODO_RANGO="desde,hasta" (nº de imagen)
    static const char *dir;
    if (!listo) {
        listo = 1;
        dir = getenv("MKDS_CAPTURA_TODO");
        if (getenv("MKDS_CAPTURA_TODO_RANGO")) sscanf(getenv("MKDS_CAPTURA_TODO_RANGO"), "%d,%d", &desde, &hasta);
    }
    if (!dir) return;
    if (n < desde || n > hasta) { n++; return; }
    char ruta[512];
    snprintf(ruta, sizeof ruta, "%s/t_%06d.bmp", dir, n++);
    int e = video_escala();
    ventana_guarda_bmp_de(ruta, e > 1 ? marcos[i_reservada].hd : marcos[i_reservada].baja, e);
}

// una imagen libre de la cola (nunca la que está enseñando el pintor)
static Marco *reserva(void) {
    EnterCriticalSection(&cs_marcos);
    int i = i_escribe;
    if (i == i_viendo) i = (i + 1) % NM;
    i_escribe = (i + 1) % NM;
    marco_ts[i] = 0;
    i_reservada = i;
    LeaveCriticalSection(&cs_marcos);
    return &marcos[i];
}

static void publica(int64_t ts) {
    EnterCriticalSection(&cs_marcos);
    marco_ts[i_reservada] = ts ? ts : 1;
    LeaveCriticalSection(&cs_marcos);
}

uint32_t *ventana_reserva_intermedia(void) {
    Marco *m = reserva();
    return video_escala() > 1 ? m->hd : m->baja;
}
void ventana_publica_intermedia(int64_t ts) {
    if (oculto) { guarda_intermedia(); return; }
    publica(ts);
}

void ventana_muestra_en(const uint32_t *arriba, const uint32_t *abajo, int64_t ts);
void ventana_muestra(const uint32_t *arriba, const uint32_t *abajo) { ventana_muestra_en(arriba, abajo, 0); }

void ventana_muestra_en(const uint32_t *arriba, const uint32_t *abajo, int64_t ts) {
    Marco *m = reserva();
    memcpy(m->baja, arriba, 256 * 192 * 4);
    memcpy(m->baja + 256 * 192, abajo, 256 * 192 * 4);
    int e = video_escala();
    if (e > 1) {
        size_t n = (size_t)256 * e * 192 * e;
        memcpy(m->hd, video_hd(0), n * 4);
        memcpy(m->hd + n, video_hd(1), n * 4);
    }
    g_imagenes++;
    if (disposicion == 3) {  // la pantalla de abajo como rejilla de 64 x 48: qué puntos tienen algo (sin su fondo), cada 4.
        // Sirve para saber en qué menú se está (los botones propios): se compara con las de cada menú dejando un margen
        // (lo marcado, que se mueve, cambia unos pocos puntos); h, una huella exacta, para saber si está quieta
        uint32_t h = 2166136261u;
        uint64_t masc[48];
        const uint32_t *b = m->baja + 256 * 192;
        for (int y = 2, fy = 0; y < 192; y += 4, fy++) {
            masc[fy] = 0;
            for (int x = 2, fx = 0; x < 256; x += 4, fx++) {
                int hay = b[y * 256 + x] != 0x00010203u;
                masc[fy] |= (uint64_t)hay << fx;
                h = (h ^ (uint32_t)hay) * 16777619u;
            }
        }
        memcpy(mascara_actual, masc, sizeof masc);
        if (pantalla_p == PANT_PERSONAJE) {  // el retrato marcado: el del marco naranja (si no se ve, el de antes)
            int sel = -1, mejor_n = 0;
            for (int c = 0; c < 12; c++) {
                int cx = ret_x[c % 4], cy = ret_y[c / 4], nn = 0;
                for (int y = cy - 26; y <= cy + 26; y++)
                    for (int x = cx - 26; x <= cx + 26; x++) {
                        if (x > cx - 22 && x < cx + 22 && y > cy - 22 && y < cy + 22) continue;  // solo el borde
                        if (x < 0 || x > 255 || y < 0 || y > 191) continue;
                        uint32_t c2 = b[y * 256 + x] & 0xFFFFFF;
                        int r = (c2 >> 16) & 255, g = (c2 >> 8) & 255, bb = c2 & 255;
                        nn += r > 180 && g < 120 && bb < 70;  // rojo o naranja (el marco; fuera del retrato no hay más)
                    }
                if (nn > mejor_n) { mejor_n = nn; sel = c; }
            }
            if (sel >= 0 && mejor_n >= 8) sel_personaje = sel;
        }
        static int igual;
        if (h != g_huella) { g_huella = h; igual = 0; if (getenv("MKDS_HUELLAS")) fprintf(stderr, "[pantalla] imagen %u huella %08x\n", g_imagenes, h); }
        else igual++;
        if (igual == 30 && getenv("MKDS_HUELLAS")) {  // para apuntar una pantalla nueva en pantallas_p
            fprintf(stderr, "[mascara] %08x", h);
            for (int i = 0; i < 48; i++) fprintf(stderr, " 0x%016llxull,", (unsigned long long)masc[i]);
            fprintf(stderr, "\n");
        }
        // reconocida en cuanto aparece su huella (la huella solo sale cuando la pantalla ya está quieta). En las
        // transiciones (toque, deslizar) la huella cambia: se siguen viendo los botones de la última reconocida mientras
        // no haya otra pantalla quieta 90 imágenes, y una desconocida no enseña la del juego hasta que lleva 20 quieta
        static int ultima = -1, sin = 0;
        int q = -1;
        int mejor_d = 120;  // la pantalla conocida más parecida, si se parece lo bastante
        for (int k = 0; k < NMASC && !getenv("MKDS_SIN_BOTONES"); k++) {
            int dd = distancia(masc, mascaras_p[k]);
            if (dd < mejor_d) { mejor_d = dd; q = masc_pant[k]; }
        }
        {  // "conectando": la de abajo en blanco justo después de la pregunta de conectarse
            int vacia = 1;
            for (int i = 0; i < 48 && vacia; i++) vacia = !masc[i];
            static uint32_t vio_conectar;  // la última imagen con la pregunta de conectarse (o con "conectando")
            static int la_vio;             // (solo si de verdad se ha visto: al arrancar también está en blanco)
            if (q < 0 && vacia && la_vio && g_imagenes - vio_conectar < 900) q = PANT_CONECTANDO;
            if (q == PANT_CONECTAR || q == PANT_CONECTANDO) { vio_conectar = g_imagenes; la_vio = 1; }
            // la portada: abajo en blanco un rato, sin 3D ni carrera, pasada la de Skyendo y no tras "conectar"
            if (q < 0 && vacia && igual >= 8 && g_imagenes > 157 && !en_carrera && !g_presentacion)  // (con 3D también: el logo)
                q = PANT_TITULO;
        }
        {  // el multijugador: elegir personaje y kart tapan toda la de abajo (su fondo no se quita), así que no tienen
           // rejilla (y con su 3D arriba parecen una carrera). Tras una página del multijugador, por lo que tienen: la de
           // los karts, el OK blanco abajo y el recuadro blanco del nombre; la de los personajes, el borde claro de los
           // 12 retratos (las mismas que las de un jugador: sus páginas)
            static int mp_vista;
            if (q >= PANT_GRUPOS) mp_vista = 1;
            else if (q == 0) mp_vista = 0;
            if (q < 0 && mp_vista && !g_presentacion) {
                int llenos = 0;
                for (int i = 0; i < 48; i++) llenos += __builtin_popcountll(masc[i]);
#define CLARO(x, y, v) (((b[(y) * 256 + (x)] >> 16) & 255) > (v) && ((b[(y) * 256 + (x)] >> 8) & 255) > (v) && (b[(y) * 256 + (x)] & 255) > (v))
                if (llenos > 2600) {
                    int retratos = 0;
                    for (int c = 0; c < 12; c++) retratos += CLARO(ret_x[c % 4] - 19, ret_y[c / 4], 170);
                    if (CLARO(104, 166, 200) && CLARO(40, 20, 200)) q = PANT_PERSONAJE + 1;
                    else if (retratos >= 8) q = PANT_PERSONAJE;  // (los cursores de los demás tapan alguno)
                    else if (ultima == PANT_PERSONAJE || ultima == PANT_PERSONAJE + 1) q = ultima;  // (un momento sin verse)
                }
#undef CLARO
            }
        }
        if (q >= 0) { ultima = q; sin = 0; }
        else if (g_presentacion || en_carrera) ultima = -1;  // la presentación del circuito o la carrera: nada de menús
        // animación o paso a otra: seguimos con la nuestra (menos la pregunta de conectarse: tras el SÍ, lo que va
        // pasando, "conectando..." o un error, es del juego y se tiene que ver)
        else if (ultima >= 0 && ultima != PANT_CONECTAR && igual < 90) { q = ultima; sin++; }
        else ultima = -1;  // una pantalla quieta 1,5 s que no conocemos: la del juego
        {  // la del online y la del contrarreloj son iguales: lo elegido en el menú principal (tocado hace poco, o marcado)
            int elegido = toque_principal >= 0 && SDL_GetTicks() - toque_principal_t < 10000 ? toque_principal : marcado_p[0];
            if (q == PANT_ONLINE && elegido != 1) q = PANT_CONTRARRELOJ;
        }
        if (q != PANT_COPAS && q != PANT_PISTAS) copa_vista = -1;
        if (!en_carrera) { pausa_propia = 0; saliendo = 0; }
        {  // la demo de la portada: una "carrera" sin marcador (ni vuelta, ni puesto, ni objeto: casi ningún sprite
           // arriba). Se sale sola pulsando A, y mientras tanto se ve nuestra portada, no la carrera
            static int sin_marcador;
            const uint8_t *spr = video_sprites_arriba();
            int nspr = 0;
            for (int i = 0; i < 192 * 256; i += 7) nspr += spr[i];
            int corre = g_3d_arriba && gx_polis() > 100 && (pantalla_p < 0 || pantalla_p == PANT_TITULO || pantalla_p == 0);
            sin_marcador = corre && !g_presentacion && nspr < 6 ? sin_marcador + 1 : 0;
            demo_portada = sin_marcador > 12;
            if (demo_portada && sin_marcador % 40 == 13) pulsa_a = 6;
        }
        if (demo_portada) q = PANT_TITULO;
        if (pausa_propia) q = PANT_PAUSA;
        if (q == 0 && sala_vista) q = PANT_SALA + sala_vista - 1;
        else if (q >= 0 && q < PANT_SALA) sala_vista = 0;  // (otra página del juego: se acabaron las salas)
        pantalla_p = q;
        { extern volatile int g_pagina_propia; g_pagina_propia = q >= 0; }
        g_menu_propio = q >= 0 && q != PANT_PAUSA && q != PANT_COPAS && q != PANT_PISTAS && !pantallas_p[q].arriba;
        g_ver_arriba = q >= 0 && pantallas_p[q].arriba;
        tapar_juego = q < 0 && igual < 20 && !en_carrera;
    }
    m->ancho_w = 0;
    int aw = gx_ancho();
    extern volatile int g_3d_arriba;
    if (disposicion == 3) {  // la de arriba a 16:9. En carrera: el centro, la imagen de siempre, y los lados del 3D
        // ancho. En los menús: nuestro fondo (cuadros azules de Action Kart) y encima lo de la pantalla de arriba sin su
        // fondo (CLAVE, video.c). ponytail: nuestro fondo no hace los fundidos del juego (se ven solo en lo suyo)
        int con3d = aw && g_3d_arriba;
        if (!aw) aw = 342 * e;
        int qp = pantalla_p, propia = qp >= 0 && qp != PANT_PAUSA, panel = propia ? pantallas_p[qp].panel : 0;
        int oscuro = qp == PANT_PAUSA;  // pausa: la carrera parada, oscurecida, detrás de nuestros botones
        int solo_fondo = propia && !con3d;  // páginas propias sin 3D: solo nuestro fondo (lo de arriba, si vale, va en tarjeta)
        if (demo_portada || qp == PANT_TITULO) { solo_fondo = 1; con3d = 0; }  // la demo y el logo del juego no se ven
        if (qp == PANT_PERSONAJE && objetivo_p >= 0) solo_fondo = 1;  // camino de otro personaje: los de en medio no se ven
        const uint16_t *c3 = con3d ? gx_ancho_color() : NULL;
        const uint8_t *a3 = con3d ? gx_ancho_alfa() : NULL;
        const uint32_t *top = e > 1 ? m->hd : m->baja;
        int an = 256 * e, x0 = (aw - an) / 2, al = 192 * e, cuadro = 16 * e;
        int y0 = 0, peana = 0;
        float pcx = 0, pcy = 0;  // centro de la peana (en puntos de la DS sobre los 342 de ancho)
        if (panel == 1 || panel == 2) {  // con tarjeta: el 3D (lo único que queda arriba) centrado en el sitio libre
            int libre = panel == 1 ? 267 : 210;    // hasta dónde llega el sitio libre (en puntos de la DS, de 342)
            x0 = (libre / 2 - 170) * e;              // el centro del kart (170, 135 en la de la DS) en su centro
            int medio = panel == 1 ? 108 : 122;     // y a media altura (el del kart es más alto: menos arriba)
            y0 = (medio - 135) * e;
            peana = 1; pcx = libre / 2.0f; pcy = medio + 21.0f;  // justo debajo de las ruedas
        }
        for (int y = 0; y < al; y++) {
            uint32_t *d = m->ancho + (size_t)y * aw;
            const uint32_t *t = top + (size_t)y * an;
            int sy = y - y0;
            const uint32_t *ts = top + (size_t)(sy < 0 ? 0 : sy >= al ? al - 1 : sy) * an;
            for (int x = 0; x < aw; x++) {
                int cen = x >= x0 && x < x0 + an && sy >= 0 && sy < al;
                if (con3d && !propia) {  // carrera (o un menú con 3D que no es nuestro): los lados del 3D ancho
                    int i = y * aw + x;
                    d[x] = cen ? t[x - x0] : g_presentacion ? 0u : a3[i] ? (uint32_t)((c3[i] & 31) * 255 / 31) << 16 | (uint32_t)(((c3[i] >> 5) & 31) * 255 / 31) << 8 | (uint32_t)(((c3[i] >> 10) & 31) * 255 / 31) : t[x < x0 ? 0 : an - 1];
                } else {  // nuestro fondo de cuadros y encima lo que quede de la de arriba
                    uint32_t f = ((x / cuadro + y / cuadro) & 1) ? 0x1B2AAEu : 0x141F8Eu;
                    if (peana) {  // la peana propia debajo del kart, en píxeles de la DS: sombra, lado, cara, borde, aro
                        float px = (float)(x / e) - pcx, py = (float)(y / e);
                        float d0 = px * px / (78.0f * 78) + (py - pcy - 6) * (py - pcy - 6) / (19.0f * 19);
                        float d1 = px * px / (76.0f * 76) + (py - pcy - 4) * (py - pcy - 4) / (17.0f * 17);
                        float d2 = px * px / (76.0f * 76) + (py - pcy) * (py - pcy) / (17.0f * 17);
                        float d3 = px * px / (50.0f * 50) + (py - pcy) * (py - pcy) / (11.0f * 11);
                        if (d0 <= 1) f = 0x0E1650u;
                        if (d1 <= 1) f = 0x1B2AAEu;
                        if (d2 <= 1) f = d2 > 0.86f ? 0x9FE6FFu : (d3 <= 1 && d3 > 0.8f) ? 0xF7CF3Au : 0x3A73DEu;
                    }
                    uint32_t c = cen && !solo_fondo ? ts[x - x0] : 0x00010203u;
                    d[x] = c != 0x00010203u ? c : (!propia && g_presentacion ? 0u : f);  // presentación: bandas oscuras fijas
                }
            }
        }
        if (con3d && !propia && en_carrera && !g_presentacion) {
            // la posición (8th...) del juego, de abajo a la izquierda a la esquina de arriba a la izquierda (a la izquierda
            // del cuadro del objeto): se copia lo que no es 3D de su recuadro y, donde estaba, se pone el 3D limpio de debajo
            #define RGB3(c) ((uint32_t)(((c) & 31) * 255 / 31) << 16 | (uint32_t)((((c) >> 5) & 31) * 255 / 31) << 8 | (uint32_t)((((c) >> 10) & 31) * 255 / 31))
            const uint8_t *spr = video_sprites_arriba();  // (qué puntos son de un sprite: el número de la posición lo es)
            int sx0 = x0 + 2 * e, sy0 = 148 * e, rw = 48 * e, rh = 42 * e;  // origen (en la imagen ancha)
            int dx0 = 2 * e, dy0 = 2 * e;                                   // destino: la esquina de arriba a la izquierda
            for (int y = 0; y < rh; y++)
                for (int x = 0; x < rw; x++) {
                    int sx = sx0 + x, sy = sy0 + y;
                    if (sx < 0 || sx >= aw || sy >= al) continue;
                    int i = sy * aw + sx;
                    if (!spr[(sy / e) * 256 + (sx - x0) / e]) continue;  // solo lo del sprite; el fondo se queda
                    m->ancho[(size_t)(dy0 + y) * aw + dx0 + x] = top[(size_t)sy * an + (sx - x0)];
                    if (a3[i]) m->ancho[i] = RGB3(c3[i]);  // donde estaba: el 3D de debajo
                }
            #undef RGB3
        }
        if (oscuro)
            for (size_t i = 0, nn = (size_t)aw * al; i < nn; i++) m->ancho[i] = (m->ancho[i] >> 1) & 0x7F7F7Fu;
        m->ancho_w = aw;
    }
    ultimo = m;
    publica(ts ? ts : reloj());
    graba_portada(m->hd, m->baja, e);
    if (oculto) { guarda_intermedia(); return; }
    repe_frame(arriba, abajo);
}

static void tecla(SDL_Keycode k, int pulsada) {
    int bit = -1, xy = -1;
    switch (k) {
    case SDLK_x: bit = 0; break;          // A
    case SDLK_z: bit = 1; break;          // B
    case SDLK_BACKSPACE: bit = 2; break;  // Select
    case SDLK_RETURN: bit = 3; break;     // Start
    case SDLK_RIGHT: bit = 4; break;
    case SDLK_LEFT: bit = 5; break;
    case SDLK_UP: bit = 6; break;
    case SDLK_DOWN: bit = 7; break;
    case SDLK_w: bit = 8; break;          // R
    case SDLK_q: bit = 9; break;          // L
    case SDLK_s: xy = 0; break;           // X
    case SDLK_a: xy = 1; break;           // Y
    }
    if (bit >= 0) {
        if (pulsada) teclado_in &= (uint16_t)~(1u << bit);
        else teclado_in |= (uint16_t)(1u << bit);
    }
    if (xy >= 0) {
        if (pulsada) teclado_xy &= (uint16_t)~(1u << xy);
        else teclado_xy |= (uint16_t)(1u << xy);
    }
}

static void raton(int mx, int my, int pulsado) {
    int q = pantalla_p;
    if (disposicion == 3 && q < 0 && en_carrera && !saliendo) {  // en carrera: el botón de pausa y el mapa
        static int antes_p, en_mapa;
        float x0, y0, x1, y1;
        EnterCriticalSection(&cs_destino);
        rect_pausa(&x0, &y0, &x1, &y1);
        int mx0 = ov_x, my0 = ov_y, mx1 = ov_x + ov_w, my1 = ov_y + ov_h;
        LeaveCriticalSection(&cs_destino);
        if (pulsado && !antes_p && mx >= x0 && mx < x1 && my >= y0 && my < y1) { pulsa_start = 6; pausa_propia = 1; }
        // tocar el mapa: un toque en el mapa del juego (así cambia entre el de cerca y el circuito entero)
        if (pulsado && !antes_p) en_mapa = mx >= mx0 && mx < mx1 && my >= my0 && my < my1;
        if (!pulsado) en_mapa = 0;
        antes_p = pulsado;
        g_lapiz = en_mapa;
        g_lapiz_x = 150; g_lapiz_y = 120;
        return;
    }
    if (disposicion == 3 && q >= 0) {  // botones propios: el toque va a su sitio del juego (o pulsa A, o cambia de personaje)
        static int antes, tx_, ty_, toca, salir_pausa;
        if (!pulsado) { g_lapiz = 0; antes = 0; toca = 0; if (salir_pausa) { pausa_propia = 0; salir_pausa = 0; } return; }
        if (!antes && q == PANT_SALA + 2) {  // el teclado del código: una tecla, una letra
            for (int k = 0; k < 36; k++) {
                float x0, y0, x1, y1;
                EnterCriticalSection(&cs_destino);
                rect_tecla(k, &x0, &y0, &x1, &y1);
                LeaveCriticalSection(&cs_destino);
                if (mx < x0 || mx >= x1 || my < y0 || my >= y1) continue;
                size_t l = strlen(codigo_sala);
                if (l < 6) { codigo_sala[l] = teclas_sala[k]; codigo_sala[l + 1] = 0; }
                antes = 1; toca = 0;
                return;
            }
        }
        if (!antes) {  // al apoyar: qué botón y qué hace
            antes = 1; toca = 0;
            for (int k = 0; k < pantallas_p[q].n; k++) {
                float x0, y0, x1, y1;
                EnterCriticalSection(&cs_destino);
                rect_p(q, k, &x0, &y0, &x1, &y1);
                LeaveCriticalSection(&cs_destino);
                if (mx < x0 || mx >= x1 || my < y0 || my >= y1) continue;
                const BotonP *b = &pantallas_p[q].b[k];
                if (q == 0) { toque_principal = k; toque_principal_t = SDL_GetTicks(); }
                if (q == PANT_PAUSA) salir_pausa = 1;  // CONTINUE / QUIT: al soltar se quita nuestra pausa
                if (q == PANT_PAUSA && k == 1) saliendo = 1;  // QUIT: lo que pregunte el juego se ve y se toca
                if (b->accion == 15) break;  // solo se enseña (una tarjeta)
                if (b->accion == 2) { pulsa_a = 6; break; }
                if (b->accion == 12) { sala_vista = 1; break; }                              // PRIVATE ROOM
                if (b->accion == 7) { online_crear(); sala_vista = 2; break; }               // CREATE ROOM
                if (b->accion == 8) { codigo_sala[0] = 0; sala_vista = 3; break; }  // JOIN ROOM (nuestro teclado; nada del sistema)
                if (b->accion == 9) { sala_vista = sala_vista > 1 ? 1 : 0; break; }  // BACK
                if (b->accion == 14) { size_t l = strlen(codigo_sala); if (l) codigo_sala[l - 1] = 0; break; }  // DEL
                if (b->accion == 11) { if (strlen(codigo_sala) == 6) online_unirse(codigo_sala); break; }  // JOIN
                if (b->accion == 10) sala_vista = 0;                                          // START: al multijugador
                if (b->accion == 6) { copa_vista = -1; break; }  // del paso 2 de las copas al 1 (solo lo nuestro)
                if (b->accion == 5) copa_vista = k;
                toca = 1;
                if (b->accion == 0) { tx_ = (b->x0 + b->x1) / 2; ty_ = (b->y0 + b->y1) / 2; }
                else if (b->accion == 1 || b->accion == 5 || b->accion == 10) { tx_ = b->tx; ty_ = b->ty; }
                else {  // personaje anterior / siguiente: a ese retrato con la cruceta, mirando dónde está (ventana_mensajes)
                    toca = 0;
                    int base = objetivo_p >= 0 ? objetivo_p : sel_personaje;  // (varios toques seguidos: desde el último)
                    objetivo_p = (base + (b->accion == 4 ? 1 : 11)) % 12;
                    intentos_p = 0; espera_p = 0;
                }
                break;
            }
        }
        g_lapiz_x = tx_; g_lapiz_y = ty_; g_lapiz = toca;
        return;
    }
    EnterCriticalSection(&cs_destino);
    int x0 = dst_x, y0 = dst_y, w = dst_w, h = dst_h;
    int ox = ov_x, oy = ov_y, ow = ov_w, oh = ov_h;
    LeaveCriticalSection(&cs_destino);
    if (w <= 0 || h <= 0) return;
    int x, y;
    if (disposicion) {  // la táctil es su recuadro
        if (ow <= 0 || oh <= 0) return;
        x = mx < ox ? -1 : (mx - ox) * 256 / ow;
        y = my < oy ? -1 : (my - oy) * 192 / oh;
    } else if (pantalla_completa) {  // la táctil es la mitad derecha
        x = (mx - x0) * 512 / w - 256;
        y = (my - y0) * 192 / h;
    } else {
        x = (mx - x0) * 256 / w;
        y = (my - y0) * 384 / h - 192;
    }
    if (pulsado && x >= 0 && x < 256 && y >= 0 && y < 192) {
        g_lapiz_x = x;
        g_lapiz_y = y;
        g_lapiz = 1;
    } else {
        g_lapiz = 0;
    }
}

// los mandos conectados (para el menú): se apuntan aquí, en el hilo de la ventana (SDL no es de varios hilos)
int ventana_mandos(char nombres[][64], int max) {
    int n = n_mandos < max ? n_mandos : max;
    for (int i = 0; i < n; i++) memcpy(nombres[i], nombres_mandos[i], 64);
    return n;
}

static int tecla_menu(SDL_Keycode k) {
    switch (k) {
    case SDLK_TAB: return MT_TAB;
    case SDLK_LEFT: return MT_IZQ;
    case SDLK_RIGHT: return MT_DER;
    case SDLK_UP: return MT_ARRIBA;
    case SDLK_DOWN: return MT_ABAJO;
    case SDLK_HOME: return MT_INICIO;
    case SDLK_END: return MT_FIN;
    case SDLK_BACKSPACE: return MT_BORRAR;
    case SDLK_DELETE: return MT_SUPR;
    case SDLK_RETURN: case SDLK_KP_ENTER: return MT_ENTER;
    case SDLK_ESCAPE: return MT_ESC;
    case SDLK_LCTRL: case SDLK_RCTRL: return MT_CTRL;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return MT_SHIFT;
    }
    return 0;
}

// texto escrito (UTF-8) letra a letra para el menú
static void texto_menu(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned c = *p++;
        int mas = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        c &= mas == 3 ? 0x07 : mas == 2 ? 0x0F : mas == 1 ? 0x1F : 0x7F;
        while (mas-- && (*p & 0xC0) == 0x80) c = c << 6 | (*p++ & 0x3F);
        menu_caracter(c);
    }
}

// Mandos: se suman a lo que se pulse en el teclado
static void lee_mandos(uint16_t *in, uint16_t *xy, int *combo) {
    *in = 0x3FF;
    *xy = 3;
    *combo = 0;
    int n = 0;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        SDL_GameController *m = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
        if (!m) continue;
        if (n < 8) snprintf(nombres_mandos[n++], 64, "%s", SDL_GameControllerName(m) ? SDL_GameControllerName(m) : "?");
        const int zona = 12000;
        int lx = SDL_GameControllerGetAxis(m, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(m, SDL_CONTROLLER_AXIS_LEFTY);
#define B(b) SDL_GameControllerGetButton(m, SDL_CONTROLLER_BUTTON_##b)
        struct { int pulsado, bit; } t[] = {
            // como en un mando de Nintendo, por su sitio: A el de la derecha, B el de abajo, X arriba, Y a la izquierda
            {B(B), 0}, {B(A), 1}, {B(BACK), 2}, {B(START), 3},
            {B(DPAD_RIGHT) || lx > zona, 4}, {B(DPAD_LEFT) || lx < -zona, 5},
            {B(DPAD_UP) || ly < -zona, 6}, {B(DPAD_DOWN) || ly > zona, 7},
            {B(RIGHTSHOULDER) || SDL_GameControllerGetAxis(m, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000, 8},
            {B(LEFTSHOULDER) || SDL_GameControllerGetAxis(m, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000, 9},
        };
        for (int k = 0; k < 10; k++)
            if (t[k].pulsado) *in &= (uint16_t)~(1u << t[k].bit);
        if (B(Y)) *xy &= ~1;  // el de arriba: X de la DS
        if (B(X)) *xy &= ~2;  // el de la izquierda: Y de la DS
        if (B(LEFTSTICK) && B(RIGHTSTICK)) *combo = 1;  // L3 + R3: menú de ajustes
#undef B
    }
    n_mandos = n;
}

void ventana_mensajes(void) {
    uint16_t mando_in = 0x3FF, mando_xy = 3;
    int combo = 0;
    if (!oculto) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            int menu = menu_abierto();
            switch (e.type) {
            case SDL_QUIT: ExitProcess(0);
            case SDL_KEYDOWN:
                if (e.key.keysym.sym == SDLK_F1) { if (!e.key.repeat) menu_alterna(); break; }
                if (menu) { if (tecla_menu(e.key.keysym.sym)) menu_tecla(tecla_menu(e.key.keysym.sym), 1); break; }
                if (sala_vista == 3) {  // escribiendo el código: borrar, y Enter = JOIN; el juego no ve las teclas
                    size_t l = strlen(codigo_sala);
                    if (e.key.keysym.sym == SDLK_BACKSPACE && l) codigo_sala[l - 1] = 0;
                    if ((e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) && l == 6) online_unirse(codigo_sala);
                    break;
                }
                if (e.key.repeat) break;
                if (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_ESCAPE && pantalla_completa))
                    cambia_pantalla_completa();
                else if (e.key.keysym.sym == SDLK_F10) repe_guarda();
                else tecla(e.key.keysym.sym, 1);
                break;
            case SDL_KEYUP:
                if (tecla_menu(e.key.keysym.sym)) menu_tecla(tecla_menu(e.key.keysym.sym), 0);
                tecla(e.key.keysym.sym, 0);  // soltar también para el juego
                break;
            case SDL_TEXTINPUT:
                if (menu) texto_menu(e.text.text);
                else if (sala_vista == 3)  // el código de la sala: 6 letras o números, en mayúsculas
                    for (const char *c = e.text.text; *c; c++) {
                        char k = *c >= 'a' && *c <= 'z' ? (char)(*c - 32) : *c;
                        size_t l = strlen(codigo_sala);
                        if (l < 6 && ((k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9'))) { codigo_sala[l] = k; codigo_sala[l + 1] = 0; }
                    }
                break;
            case SDL_MOUSEWHEEL: menu_rueda((float)e.wheel.y); break;
            case SDL_FINGERDOWN: case SDL_FINGERMOTION: case SDL_FINGERUP: {
                int w, h;
                SDL_GetWindowSize(ventana, &w, &h);
                dedo(e.tfinger.fingerId, e.tfinger.x * w, e.tfinger.y * h, e.type != SDL_FINGERUP);
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
                if (tactil && e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT) { dedo(-1, (float)e.button.x, (float)e.button.y, 1); break; }
                if (menu) { menu_raton((float)e.button.x, (float)e.button.y); menu_boton(e.button.button == SDL_BUTTON_LEFT ? 0 : 1, 1); }
                else if (e.button.button == SDL_BUTTON_LEFT) raton(e.button.x, e.button.y, 1);
                break;
            case SDL_MOUSEMOTION:
                if (tactil && e.motion.which != SDL_TOUCH_MOUSEID && (e.motion.state & SDL_BUTTON_LMASK) && !menu) { dedo(-1, (float)e.motion.x, (float)e.motion.y, 1); break; }
                if (menu) menu_raton((float)e.motion.x, (float)e.motion.y);
                else if (e.motion.state & SDL_BUTTON_LMASK) raton(e.motion.x, e.motion.y, 1);
                break;
            case SDL_MOUSEBUTTONUP:
                if (tactil && e.button.which != SDL_TOUCH_MOUSEID && e.button.button == SDL_BUTTON_LEFT && !menu) { dedo(-1, (float)e.button.x, (float)e.button.y, 0); break; }
                menu_boton(e.button.button == SDL_BUTTON_LEFT ? 0 : 1, 0);
                if (e.button.button == SDL_BUTTON_LEFT) raton(e.button.x, e.button.y, 0);
                break;
            case SDL_WINDOWEVENT: if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) calcula_destino(); break;
            case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
            }
        }
        if (titulo_nuevo) { titulo_nuevo = 0; SDL_SetWindowTitle(ventana, titulo); }
        lee_mandos(&mando_in, &mando_xy, &combo);
        if (mando_in != 0x3FF || mando_xy != 3) mando_visto = SDL_GetTicks();  // algo pulsado en un mando
        if (tactil && !menu_abierto()) lee_dedos();
    }
    static int combo_antes;  // L3 + R3: abre o cierra el menú
    if (combo && !combo_antes) menu_alterna();
    combo_antes = combo;
    if (pide_pc) {
        if ((pide_pc == 1) != (pantalla_completa != 0)) cambia_pantalla_completa();
        pide_pc = 0;
    }
    if (disposicion == 3) {  // ¿carrera? 3D arriba en una pantalla que no es de las nuestras (los menús con 3D, el de
        // personaje y el de kart, sí lo son) ni la presentación, y algo de 3D (MKDS_POLIS_CARRERA, 100); un rato seguido
        // para entrar o salir. (Con 400 polígonos, circuitos sencillos como el desierto no contaban como carrera.)
        static int umbral = -1, cuenta;
        if (umbral < 0) umbral = getenv("MKDS_POLIS_CARRERA") ? atoi(getenv("MKDS_POLIS_CARRERA")) : 100;
        int ahora = g_3d_arriba && gx_polis() > umbral && (pantalla_p < 0 || pantalla_p == PANT_PAUSA) && !g_presentacion;
        if (en_carrera && g_presentacion) ahora = 1;  // (una ceremonia a mitad no cambia nada)
        cuenta = ahora != en_carrera ? cuenta + 1 : 0;
        if (cuenta > 20) { en_carrera = ahora; g_carrera = ahora; cuenta = 0; calcula_destino(); }
    }
    if (pide_disp >= 0) {
        if (pide_disp != disposicion) cambia_disposicion(pide_disp);
        pide_disp = -1;
    }
    if (menu_abierto()) {  // el menú se queda los botones; el juego no ve nada pulsado
        menu_mando(mando_in);
        g_keyinput = g_guion_in;
        g_keyxy = g_guion_xy;
        g_lapiz = 0;
        return;
    }
    uint16_t propio = 0x3FF;
    if (pulsa_a > 0) { pulsa_a--; propio &= (uint16_t)~1u; }  // el botón de acción de los menús propios (A)
    if (pulsa_start > 0) { pulsa_start--; propio &= (uint16_t)~8u; }  // nuestro botón de pausa (START)
    {  // START del teclado o del mando en carrera: nuestra pausa sigue a la del juego
        static int start_antes;
        int start = !((teclado_in & mando_in & g_guion_in) & 8);
        if (start && !start_antes && en_carrera && disposicion == 3) { pausa_propia = !pausa_propia; saliendo = 0; }
        start_antes = start;
    }
    {  // pruebas: MKDS_PRUEBA_SALA="350:1,500:2" abre esa página de salas en esa imagen (2 crea la sala de verdad)
        static const char *ps;
        static int mirado;
        static uint32_t hecha;
        if (!mirado) { ps = getenv("MKDS_PRUEBA_SALA"); mirado = 1; }
        for (const char *p = ps; p && *p;) {
            unsigned f = (unsigned)strtoul(p, (char **)&p, 10), v = 0;
            if (*p == ':') v = (unsigned)strtoul(p + 1, (char **)&p, 10);
            if (*p == ',') p++;
            if (f == g_imagenes && f != hecha) { hecha = f; sala_vista = (int)v; if (v == 2) online_crear(); }
        }
    }
    {  // pruebas: MKDS_PRUEBA_SIG="1200,1300" pulsa nuestro "siguiente" en esas imágenes
        static const char *ps;
        static int mirado;
        static uint32_t hecha;
        if (!mirado) { ps = getenv("MKDS_PRUEBA_SIG"); mirado = 1; }
        for (const char *p = ps; p && *p;) {
            unsigned f = (unsigned)strtoul(p, (char **)&p, 10);
            if (*p == ',') p++;
            if (f == g_imagenes && f != hecha) {
                hecha = f;
                int base = objetivo_p >= 0 ? objetivo_p : sel_personaje;
                objetivo_p = (base + 1) % 12; intentos_p = 0; espera_p = 0; sel_antes = -1;
            }
        }
    }
    if (objetivo_p >= 0 && cola_i >= cola_n) {
        // un paso de cruceta; luego se espera a que el cursor llegue de verdad (cambia lo detectado) y se asiente, o
        // como mucho 40 imágenes; y entonces el siguiente paso. Así nunca se adelanta ni se pasa
        int sp = sel_personaje, op = objetivo_p;
        if (sel_antes >= 0 && sp == sel_antes && espera_p > 0) espera_p--;
        else if (sel_antes >= 0 && sp != sel_antes) { sel_antes = -1; espera_p = 8; }  // ha llegado: que se asiente
        else if (espera_p > 0) espera_p--;
        else if (sp == op || ++intentos_p > 20 || pantalla_p != PANT_PERSONAJE) { objetivo_p = -1; sel_antes = -1; }
        else {
            int fr = sp / 4, fc = sp % 4, tr = op / 4, tc = op % 4;
            int paso[1] = {fr < tr ? 7 : fr > tr ? 6 : fc < tc ? 4 : 5};  // primero la fila, luego la columna
            cruceta(paso, 1);
            sel_antes = sp;
            espera_p = 40;
        }
    }
    if (cola_i < cola_n) {  // la cruceta de los menús propios
        if (cola_fase < 4) propio &= (uint16_t)~(1u << cola_bits[cola_i]);
        if (++cola_fase >= 8) { cola_fase = 0; cola_i++; }
    }
    {  // dos toques seguidos a la izquierda o a la derecha (menos de 0,3 s): R mientras se mantenga esa dirección = salto
        // y derrape. Con la táctil, el teclado o el mando, solo en carrera
        static uint32_t ult[2];
        static int antes[2], salto = -1;
        uint16_t todo = teclado_in & mando_in & (tactil ? tactil_in : 0x3FF);
        uint32_t t = SDL_GetTicks();
        for (int k = 0; k < 2; k++) {  // 0 derecha (bit 4), 1 izquierda (bit 5)
            int ahora = !(todo & (1u << (4 + k)));
            if (ahora && !antes[k]) { if (t - ult[k] < 300 && en_carrera) salto = k; ult[k] = t; }
            if (!ahora && salto == k) salto = -1;
            antes[k] = ahora;
        }
        if (salto >= 0) propio &= (uint16_t)~(1u << 8);
    }
    g_keyinput = teclado_in & mando_in & g_guion_in & (tactil ? tactil_in : 0x3FF) & propio;
    g_keyxy = (uint16_t)(teclado_xy & mando_xy & g_guion_xy & (tactil ? tactil_xy : 3));
}

static void escribe_bmp(const char *ruta, const uint32_t *px, int e) {
    FILE *f = fopen(ruta, "wb");
    if (!f) return;
    uint32_t tam = 54 + 256 * 384 * 4 * (uint32_t)(e * e);
    uint8_t cab[54] = {'B', 'M'};
    memcpy(cab + 2, &tam, 4);
    uint32_t off = 54, cab_tam = 40, ancho = 256 * (uint32_t)e, planos_bits = 1 | (32 << 16);
    int32_t alto = -384 * e;
    memcpy(cab + 10, &off, 4);
    memcpy(cab + 14, &cab_tam, 4);
    memcpy(cab + 18, &ancho, 4);
    memcpy(cab + 22, &alto, 4);
    memcpy(cab + 26, &planos_bits, 4);
    fwrite(cab, 1, 54, f);
    fwrite(px, 4, (size_t)256 * 384 * e * e, f);
    fclose(f);
}

// Para guardar una captura (MKDS_CAPTURA=archivo.bmp)
void ventana_guarda_bmp(const char *ruta) {
    int e = video_escala();  // con reescalado, a tamaño grande
    escribe_bmp(ruta, e > 1 ? ultimo->hd : ultimo->baja, e);
}

void ventana_guarda_bmp_de(const char *ruta, const uint32_t *px, int e) { escribe_bmp(ruta, px, e); }

// Contador de FPS en el título de la ventana (lo pone ventana_mensajes: SDL quiere el hilo de la ventana)
void ventana_titulo_fps(int fps, int fps3d) {
    int pantalla = (int)InterlockedExchange(&enseñadas, 0);
    menu_pon_fps(fps, fps3d, pantalla);
    if (oculto || !ventana) return;
    const char *av = repe_aviso();
    if (av) snprintf(titulo, sizeof titulo, "DS Kart Recompiled  -  %s", av);
    else snprintf(titulo, sizeof titulo, "DS Kart Recompiled  -  %d FPS  (3D: %d)  pantalla: %d", fps, fps3d, pantalla);
    titulo_nuevo = 1;
}
