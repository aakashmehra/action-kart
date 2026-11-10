// Menú de ajustes dentro del juego, con Dear ImGui (runtime/imgui, licencia MIT). Ver menu.h.
//
// Pestañas: Gráficos (pantalla completa, imágenes por segundo, resolución del 3D, mostrar FPS), Mandos (los
// conectados y qué hace cada botón), Audio (volumen, silencio, micrófono) y Juego (nombre, idioma, IP del amigo
// para el multijugador). Lo que se puede cambiar en marcha se aplica al momento; la resolución y el idioma, al
// volver a abrir el juego. Todo se guarda en ajustes.ini al cerrar el menú.
// Los textos están en los 5 idiomas del launcher (LAUNCHER_IDIOMA en ajustes.ini).
// Se compila sin la biblioteca de C++ (sin excepciones, RTTI ni estáticos con candado): basta el enlazador de C.
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "imgui/imgui.h"
#if defined(_WIN32) && !defined(MKDS_SDL)  // (la ventana de Windows de siempre; Action Kart usa la de SDL también allí)
#include "imgui/imgui_impl_opengl2.h"
#define IMPL_INIT() ImGui_ImplOpenGL2_Init()
#define IMPL_NEWFRAME() ImGui_ImplOpenGL2_NewFrame()
#define IMPL_RENDER(d) ImGui_ImplOpenGL2_RenderDrawData(d)
#else  // Linux, macOS e iOS: con el SDL_Renderer de la ventana (ventana_sdl.c)
#include "imgui/imgui_impl_sdlrenderer2.h"
#include <SDL.h>
extern "C" { void *menu_renderer; }
#define IMPL_INIT() ImGui_ImplSDLRenderer2_Init((SDL_Renderer *)menu_renderer)
#define IMPL_NEWFRAME() ImGui_ImplSDLRenderer2_NewFrame()
#define IMPL_RENDER(d) ImGui_ImplSDLRenderer2_RenderDrawData(d, (SDL_Renderer *)menu_renderer)
#endif
#include "menu.h"
#include "letra_pixel.inc"
// Jersey 10 a varios tamaños (múltiplos de 10: así sale nítida, sin escalar), con los bordes sin suavizar (el atlas
// se deja en 0/255). letra_boton apunta a la del tamaño que toca en cada momento (usa_letra)
static ImFont *letras_pixel[6];
static ImFont *letra_boton;
static float usa_letra(float t) {
    int k = (int)(t / 10 + 0.75f) - 1;  // mejor un poco más grande que más pequeña
    if (k < 0) k = 0;
    if (k > 5) k = 5;
    letra_boton = letras_pixel[k];
    return 10.0f * (k + 1);
}

extern "C" {
void gx_pon_fps(int fps);                       // gpu3d.c: imágenes por segundo (intermedias)
void gx_pon_gpu(int si);                         // gpu3d.c: 3D por la GPU
void gx_pon_suavizado(int si);                   // gpu3d.c: antialiasing (con la GPU)
int gx_polis(void);                              // gpu3d.c: polígonos de la última escena
int online_estado(char *codigo, int *jugadores); // wifi.c: la sala del online (la crea el launcher)
void sonido_pon_volumen(int volumen, int silencio);  // sonido.c
void mic_pon(int activo, int ganancia);          // microfono.c
void nombre_cambia(void);                        // main_mkds.c: vuelve a leer MKDS_NOMBRE
}

// ---- cola de lo que llega del hilo de la ventana ------------------------------------------------------
#ifdef _WIN32
static CRITICAL_SECTION cerrojo;
static void cierra_c(void) { EnterCriticalSection(&cerrojo); }
static void abre_c(void) { LeaveCriticalSection(&cerrojo); }
static void cerrojo_inicia(void) { InitializeCriticalSection(&cerrojo); }
#else
static pthread_mutex_t cerrojo = PTHREAD_MUTEX_INITIALIZER;
static void cierra_c(void) { pthread_mutex_lock(&cerrojo); }
static void abre_c(void) { pthread_mutex_unlock(&cerrojo); }
static void cerrojo_inicia(void) {}
#endif

enum { EV_TECLA, EV_CARACTER, EV_RATON, EV_BOTON, EV_RUEDA };
struct Evento { int tipo, a, b; float x, y; };
static Evento cola[512];
static int cola_n;
static volatile int abierto, listo;
static volatile int fps_juego, fps_3d, fps_pantalla;

static void mete(Evento e) {
    if (!listo || !abierto) return;
    cierra_c();
    if (cola_n < 512) cola[cola_n++] = e;
    abre_c();
}

extern "C" int menu_abierto(void) { return listo && abierto; }
extern "C" void menu_tecla(int t, int p) { Evento e = {EV_TECLA, t, p, 0, 0}; mete(e); }
extern "C" void menu_caracter(unsigned c) { Evento e = {EV_CARACTER, (int)c, 0, 0, 0}; mete(e); }
extern "C" void menu_raton(float x, float y) { Evento e = {EV_RATON, 0, 0, x, y}; mete(e); }
extern "C" void menu_boton(int b, int p) { Evento e = {EV_BOTON, b, p, 0, 0}; mete(e); }
extern "C" void menu_rueda(float dy) { Evento e = {EV_RUEDA, 0, 0, 0, dy}; mete(e); }
extern "C" void menu_pon_fps(int j, int t, int p) { fps_juego = j; fps_3d = t; fps_pantalla = p; }

// el mando: los botones de la DS que cambian se convierten en teclas de navegación del menú
extern "C" void menu_mando(unsigned short in) {
    static unsigned short antes = 0x3FF;
    static const struct { int bit, tecla; } m[] = {
        {6, MT_PAD_ARRIBA}, {7, MT_PAD_ABAJO}, {5, MT_PAD_IZQ}, {4, MT_PAD_DER},
        {0, MT_PAD_ACEPTA}, {1, MT_PAD_ATRAS}, {9, MT_PAD_L}, {8, MT_PAD_R},
    };
    for (unsigned k = 0; k < sizeof m / sizeof *m; k++) {
        int ahora = !(in & (1u << m[k].bit)), era = !(antes & (1u << m[k].bit));
        if (ahora != era) menu_tecla(m[k].tecla, ahora);
    }
    antes = in;
}

// ---- ajustes -----------------------------------------------------------------------------------------
static struct {
    int fps;          // 60, 120, 180, 240
    int escala;       // 1-4 (al reiniciar)
    int mostrar_fps;
    int volumen;      // 0-100
    int silencio;
    int micro;
    int ganancia;     // 1-16
    char nombre[32];
    int idioma;       // 1 inglés, 2 francés, 3 alemán, 4 italiano, 5 español (al reiniciar)
    char ip[64];      // la del amigo (Radmin), para el multijugador
    int gpu;          // 3D por la GPU (gpu3d_gl.c)
    int suavizado;    // antialiasing con la GPU
} aj;
static char idioma_menu[4] = "es";
static int pestaña_pedida = -1, pestaña_actual;
static int cambiado;

static int entero(const char *v, int def) { const char *e = getenv(v); return e && *e ? atoi(e) : def; }
static void texto(const char *v, char *dst, size_t n, const char *def) {
    const char *e = getenv(v);
    snprintf(dst, n, "%s", e ? e : def);
}

static void pon_env(const char *clave, const char *valor) {
    char var[64];
    snprintf(var, sizeof var, "MKDS_%s", clave);
#ifdef _WIN32
    _putenv_s(var, valor);
#else
    setenv(var, valor, 1);
#endif
}

static void lee_ajustes(void) {
    aj.fps = entero("MKDS_FPS", 60);
    aj.escala = entero("MKDS_ESCALA", 2);
    aj.mostrar_fps = entero("MKDS_MOSTRAR_FPS", 0);
    aj.volumen = entero("MKDS_VOLUMEN", 100);
    aj.silencio = entero("MKDS_SILENCIO", 0);
    aj.micro = entero("MKDS_MICRO", 1);
    aj.ganancia = entero("MKDS_MICRO_GANANCIA", 8);
    texto("MKDS_NOMBRE", aj.nombre, sizeof aj.nombre, "");
    aj.idioma = entero("MKDS_IDIOMA", 1);
    texto("MKDS_LOCAL_IP", aj.ip, sizeof aj.ip, "");
    aj.gpu = entero("MKDS_GPU", 0);
    aj.suavizado = entero("MKDS_SUAVIZADO", 1);
    texto("MKDS_LAUNCHER_IDIOMA", idioma_menu, sizeof idioma_menu, "es");
}

// ajustes.ini: se cambian las claves del menú y se deja todo lo demás (lo del launcher) como estaba
static void guarda_ajustes(void) {
    char v[16][80];
    const char *claves[] = {"FPS", "ESCALA", "MOSTRAR_FPS", "VOLUMEN", "SILENCIO", "MICRO", "MICRO_GANANCIA",
                            "NOMBRE", "IDIOMA", "LOCAL_IP", "PANTALLA_COMPLETA", "GPU", "DISPOSICION", "SUAVIZADO"};
    int n = (int)(sizeof claves / sizeof *claves);
    snprintf(v[0], 80, "%d", aj.fps); snprintf(v[1], 80, "%d", aj.escala); snprintf(v[2], 80, "%d", aj.mostrar_fps);
    snprintf(v[3], 80, "%d", aj.volumen); snprintf(v[4], 80, "%d", aj.silencio); snprintf(v[5], 80, "%d", aj.micro);
    snprintf(v[6], 80, "%d", aj.ganancia); snprintf(v[7], 80, "%s", aj.nombre); snprintf(v[8], 80, "%d", aj.idioma);
    snprintf(v[9], 80, "%s", aj.ip); snprintf(v[10], 80, "%d", ventana_pantalla_completa());
    snprintf(v[11], 80, "%d", aj.gpu);
    snprintf(v[12], 80, "%d", ventana_disposicion());
    snprintf(v[13], 80, "%d", aj.suavizado);
    static char lineas[128][256];
    int nl = 0, hecho[16] = {0};
    FILE *f = fopen("ajustes.ini", "r");
    if (f) {
        while (nl < 128 && fgets(lineas[nl], 256, f)) {
            lineas[nl][strcspn(lineas[nl], "\r\n")] = 0;
            for (int k = 0; k < n; k++) {
                size_t l = strlen(claves[k]);
                if (!strncmp(lineas[nl], claves[k], l) && lineas[nl][l] == '=') {
                    snprintf(lineas[nl], 256, "%s=%s", claves[k], v[k]);
                    hecho[k] = 1;
                }
            }
            nl++;
        }
        fclose(f);
    }
    for (int k = 0; k < n && nl < 128; k++)
        if (!hecho[k]) snprintf(lineas[nl++], 256, "%s=%s", claves[k], v[k]);
    f = fopen("ajustes.ini", "w");
    if (!f) return;
    for (int i = 0; i < nl; i++) fprintf(f, "%s\n", lineas[i]);
    fclose(f);
    for (int k = 0; k < n; k++) pon_env(claves[k], v[k]);  // y lo que lea el juego a partir de ahora
}

static void aplica(void) {
    gx_pon_fps(aj.fps);
    sonido_pon_volumen(aj.volumen, aj.silencio);
    mic_pon(aj.micro, aj.ganancia);
    pon_env("NOMBRE", aj.nombre);
    nombre_cambia();
    pon_env("LOCAL_IP", aj.ip);
    cambiado = 1;
}

extern "C" void menu_alterna(void) {
    if (!listo) return;
    abierto = !abierto;
    if (!abierto && cambiado) { guarda_ajustes(); cambiado = 0; }
}

// ---- textos en los 5 idiomas -----------------------------------------------------------------------------
struct Texto { const char *es, *en, *fr, *de, *it; };
static const Texto textos[] = {
    {"Ajustes", "Settings", "Paramètres", "Einstellungen", "Impostazioni"},
    {"Gráficos", "Graphics", "Graphismes", "Grafik", "Grafica"},
    {"Mandos", "Controllers", "Manettes", "Controller", "Controller"},
    {"Audio", "Audio", "Audio", "Audio", "Audio"},
    {"Juego", "Game", "Jeu", "Spiel", "Gioco"},
    {"Pantalla completa", "Fullscreen", "Plein écran", "Vollbild", "Schermo intero"},
    {"Las dos pantallas lado a lado. También con F11.", "Both screens side by side. Also with F11.",
     "Les deux écrans côte à côte. Aussi avec F11.", "Beide Bildschirme nebeneinander. Auch mit F11.",
     "I due schermi affiancati. Anche con F11."},
    {"Imágenes por segundo", "Frames per second", "Images par seconde", "Bilder pro Sekunde", "Fotogrammi al secondo"},
    {"El juego va a 60; por encima se pintan imágenes intermedias del 3D. Si el PC no llega, pinta las que puede.",
     "The game runs at 60; above that, in-between 3D frames are drawn. If the PC can't keep up, it draws what it can.",
     "Le jeu tourne à 60 ; au-delà, des images 3D intermédiaires sont dessinées. Si le PC ne suit pas, il en dessine moins.",
     "Das Spiel läuft mit 60; darüber werden 3D-Zwischenbilder gezeichnet. Schafft es der PC nicht, zeichnet er weniger.",
     "Il gioco va a 60; sopra si disegnano fotogrammi 3D intermedi. Se il PC non ce la fa, ne disegna meno."},
    {"Resolución del 3D", "3D resolution", "Résolution 3D", "3D-Auflösung", "Risoluzione 3D"},
    {"Original", "Original", "Originale", "Original", "Originale"},
    {"Se aplica al volver a abrir el juego.", "Applied the next time you open the game.",
     "Appliqué à la prochaine ouverture du jeu.", "Wird beim nächsten Spielstart übernommen.",
     "Si applica alla prossima apertura del gioco."},
    {"Mostrar FPS", "Show FPS", "Afficher les FPS", "FPS anzeigen", "Mostra FPS"},
    {"En una esquina, también en pantalla completa.", "In a corner, also in fullscreen.",
     "Dans un coin, aussi en plein écran.", "In einer Ecke, auch im Vollbild.", "In un angolo, anche a schermo intero."},
    {"Pantallas", "Screens", "Écrans", "Bildschirme", "Schermi"},
    {"Las dos iguales", "Both the same size", "Les deux identiques", "Beide gleich groß", "Entrambi uguali"},
    {"Una encima de otra", "One on top of the other", "L'un sur l'autre", "Übereinander", "Una sopra l'altra"},
    {"La grande y la táctil al lado", "Big one with the touch screen beside it", "La grande et l'écran tactile à côté",
     "Groß, Touchscreen daneben", "Quello grande e il touch accanto"},
    {"Las dos pantallas al mismo tamaño: una sobre otra en ventana, lado a lado en pantalla completa.",
     "Both screens the same size: stacked in a window, side by side in full screen.",
     "Les deux écrans de même taille : l'un sur l'autre en fenêtre, côte à côte en plein écran.",
     "Beide Bildschirme gleich groß: übereinander im Fenster, nebeneinander im Vollbild.",
     "Entrambi gli schermi della stessa grandezza: uno sopra l'altro in finestra, affiancati a schermo intero."},
    {"La pantalla de arriba en grande y la táctil pequeña encima de ella, en la esquina de abajo a la derecha.",
     "The top screen big and the touch screen small on top of it, in the bottom-right corner.",
     "L'écran du haut en grand et l'écran tactile en petit par-dessus, dans le coin en bas à droite.",
     "Der obere Bildschirm groß und der Touchscreen klein darüber, in der Ecke unten rechts.",
     "Lo schermo superiore grande e il touch piccolo sopra, nell'angolo in basso a destra."},
    {"La pantalla de arriba a todo el alto y la táctil pequeña pegada a su derecha, sin taparla: juntas llenan una pantalla 16:9.",
     "The top screen at full height and the touch screen small right next to it, without covering it: together they fill a 16:9 screen.",
     "L'écran du haut sur toute la hauteur et l'écran tactile en petit juste à sa droite, sans le couvrir : ensemble, ils remplissent un écran 16:9.",
     "Der obere Bildschirm in voller Höhe und der Touchscreen klein direkt rechts daneben, ohne ihn zu verdecken: Zusammen füllen sie einen 16:9-Bildschirm.",
     "Lo schermo superiore a tutta altezza e il touch piccolo subito alla sua destra, senza coprirlo: insieme riempiono uno schermo 16:9."},
    {"3D con la tarjeta gráfica (experimental)", "3D on the graphics card (experimental)",
     "3D sur la carte graphique (expérimental)", "3D auf der Grafikkarte (experimentell)",
     "3D sulla scheda grafica (sperimentale)"},
    {"Suavizar bordes", "Smooth edges", "Lisser les bords", "Kanten glätten", "Smussa i bordi"},
    {"Antialiasing, como en la DS. Si te bajan los FPS, quítalo.", "Antialiasing, like on the DS. If your FPS drop, turn it off.",
     "Anticrénelage, comme sur la DS. Si vos FPS baissent, désactivez-le.",
     "Kantenglättung wie auf dem DS. Wenn die FPS sinken, schalte sie aus.",
     "Antialiasing, come sul DS. Se gli FPS calano, disattivalo."},
    {"Gasta mucha menos CPU, sobre todo con resolución alta y más FPS. Si ves algo raro, quítalo y dínoslo.",
     "Uses much less CPU, especially with high resolution and more FPS. If something looks wrong, turn it off and tell us.",
     "Utilise beaucoup moins le processeur, surtout en haute résolution et avec plus de FPS. Si quelque chose cloche, désactive-le et dis-le-nous.",
     "Braucht viel weniger CPU, besonders bei hoher Auflösung und mehr FPS. Wenn etwas falsch aussieht, schalte es aus und sag uns Bescheid.",
     "Usa molta meno CPU, soprattutto con risoluzione alta e più FPS. Se vedi qualcosa di strano, disattivalo e diccelo."},
    {"Conectados", "Connected", "Connectées", "Verbunden", "Collegati"},
    {"Ningún mando conectado. Puedes jugar con el teclado.", "No controller connected. You can play with the keyboard.",
     "Aucune manette connectée. Tu peux jouer au clavier.", "Kein Controller verbunden. Du kannst mit der Tastatur spielen.",
     "Nessun controller collegato. Puoi giocare con la tastiera."},
    {"Botón de la DS", "DS button", "Bouton DS", "DS-Taste", "Tasto DS"},
    {"Mando", "Controller", "Manette", "Controller", "Controller"},
    {"Teclado", "Keyboard", "Clavier", "Tastatur", "Tastiera"},
    {"Cruceta", "D-pad", "Croix", "Steuerkreuz", "Croce"},
    {"cruceta o stick izquierdo", "D-pad or left stick", "croix ou stick gauche", "Steuerkreuz oder linker Stick",
     "croce o levetta sinistra"},
    {"flechas", "arrows", "flèches", "Pfeiltasten", "frecce"},
    {"Retroceso", "Backspace", "Retour arrière", "Rücktaste", "Backspace"},
    {"Lápiz", "Stylus", "Stylet", "Stift", "Pennino"},
    {"ratón en la pantalla de abajo", "mouse on the bottom screen", "souris sur l'écran du bas",
     "Maus auf dem unteren Bildschirm", "mouse sullo schermo inferiore"},
    {"Volumen", "Volume", "Volume", "Lautstärke", "Volume"},
    {"Silenciar", "Mute", "Couper le son", "Stumm", "Muto"},
    {"Micrófono (soplar en la Batalla de globos)", "Microphone (blow in Balloon Battle)",
     "Micro (souffler dans la Bataille de ballons)", "Mikrofon (pusten im Ballonkampf)",
     "Microfono (soffiare nella Battaglia palloncini)"},
    {"Sensibilidad", "Sensitivity", "Sensibilité", "Empfindlichkeit", "Sensibilità"},
    {"Si no se infla al soplar, súbela; si se infla solo, bájala.", "If it doesn't inflate when you blow, raise it; if it inflates by itself, lower it.",
     "S'il ne gonfle pas quand tu souffles, augmente-la ; s'il gonfle tout seul, baisse-la.",
     "Bläst er sich beim Pusten nicht auf, erhöhen; bläst er sich von selbst auf, senken.",
     "Se non si gonfia quando soffi, alzala; se si gonfia da solo, abbassala."},
    {"Tu nombre", "Your name", "Ton nom", "Dein Name", "Il tuo nome"},
    {"El que ven los demás en multijugador (10 letras como mucho).", "What others see in multiplayer (10 letters max).",
     "Celui que voient les autres en multijoueur (10 lettres max).", "Den sehen die anderen im Mehrspieler (max. 10 Zeichen).",
     "Quello che vedono gli altri in multigiocatore (max 10 lettere)."},
    {"Idioma del juego", "Game language", "Langue du jeu", "Spielsprache", "Lingua del gioco"},
    {"Multijugador: IP de tu amigo", "Multiplayer: your friend's IP", "Multijoueur : IP de ton ami",
     "Mehrspieler: IP deines Freundes", "Multigiocatore: IP del tuo amico"},
    {"Solo si no os veis en la lista de grupos (por ejemplo, su IP de Radmin). Vacío = buscar solo.",
     "Only if you can't see each other's group (e.g. their Radmin IP). Empty = find automatically.",
     "Seulement si vous ne vous voyez pas (par ex. son IP Radmin). Vide = recherche automatique.",
     "Nur wenn ihr euch nicht seht (z. B. seine Radmin-IP). Leer = automatisch suchen.",
     "Solo se non vi vedete (es. il suo IP Radmin). Vuoto = ricerca automatica."},
    {"Volver al juego", "Back to the game", "Retour au jeu", "Zurück zum Spiel", "Torna al gioco"},
    {"Menú abierto: el juego no recibe los botones. F1 o L3 + R3 para volver.",
     "Menu open: the game doesn't get your buttons. F1 or L3 + R3 to go back.",
     "Menu ouvert : le jeu ne reçoit pas tes boutons. F1 ou L3 + R3 pour revenir.",
     "Menü offen: das Spiel bekommt keine Tasten. F1 oder L3 + R3 zum Zurückkehren.",
     "Menu aperto: il gioco non riceve i tasti. F1 o L3 + R3 per tornare."},
    {"L / R: cambiar de pestaña", "L / R: change tab", "L / R : changer d'onglet", "L / R: Tab wechseln", "L / R: cambia scheda"},
    {"Cruz", "Cross", "Croix", "Kreuz", "Croce"},
    {"Círculo", "Circle", "Rond", "Kreis", "Cerchio"},
    {"Triángulo", "Triangle", "Triangle", "Dreieck", "Triangolo"},
    {"Cuadrado", "Square", "Carré", "Quadrat", "Quadrato"},
    {"Jugar por internet", "Play over the internet", "Jouer en ligne", "Online spielen", "Gioca su internet"},
    {"Hasta 8 jugadores, sin Radmin. Uno crea una sala y pasa el código a los demás; luego se juega con el Multijugador del juego.",
     "Up to 8 players, no Radmin needed. One creates a room and shares the code; then you play with the game's Multiplayer mode.",
     "Jusqu'à 8 joueurs, sans Radmin. L'un crée un salon et partage le code ; ensuite on joue avec le mode Multijoueur du jeu.",
     "Bis zu 8 Spieler, ohne Radmin. Einer erstellt einen Raum und teilt den Code; dann spielt ihr im Mehrspielermodus des Spiels.",
     "Fino a 8 giocatori, senza Radmin. Uno crea una stanza e condivide il codice; poi si gioca con il Multigiocatore del gioco."},
    {"Tu sala", "Your room", "Ton salon", "Dein Raum", "La tua stanza"},
    {"Estás en la sala", "You're in room", "Tu es dans le salon", "Du bist im Raum", "Sei nella stanza"},
    {"Jugadores en la sala: %d", "Players in the room: %d", "Joueurs dans le salon : %d", "Spieler im Raum: %d", "Giocatori nella stanza: %d"},
    {"Pasa el código a tus amigos. Luego, en el juego: Multijugador > Crear un grupo, y LISTO cuando estén todos.",
     "Share the code with your friends. Then, in the game: Multiplayer > Create a group, and READY when everyone is in.",
     "Partage le code avec tes amis. Puis, dans le jeu : Multijoueur > Créer un groupe, et PRÊT quand tout le monde est là.",
     "Teile den Code mit deinen Freunden. Dann im Spiel: Mehrspieler > Gruppe erstellen, und BEREIT, wenn alle da sind.",
     "Condividi il codice con i tuoi amici. Poi, nel gioco: Multigiocatore > Crea un gruppo, e PRONTO quando ci sono tutti."},
    {"Ahora, en el juego: Multijugador y elige el grupo de la lista.", "Now, in the game: Multiplayer, and pick the group from the list.",
     "Maintenant, dans le jeu : Multijoueur, et choisis le groupe dans la liste.", "Jetzt im Spiel: Mehrspieler, und wähle die Gruppe aus der Liste.",
     "Ora, nel gioco: Multigiocatore, e scegli il gruppo dalla lista."},
    {"Salir de la sala", "Leave the room", "Quitter le salon", "Raum verlassen", "Esci dalla stanza"},
    {"Conectando…", "Connecting…", "Connexion…", "Verbinden…", "Connessione…"},
    {"No se pudo conectar con el servidor de salas. ¿Hay internet?", "Couldn't reach the room server. Are you online?",
     "Impossible de joindre le serveur de salons. Es-tu connecté ?", "Raumserver nicht erreichbar. Bist du online?",
     "Impossibile raggiungere il server delle stanze. Sei connesso?"},
    {"No se pudo averiguar tu dirección en internet.", "Couldn't find out your internet address.",
     "Impossible de trouver ton adresse internet.", "Deine Internetadresse konnte nicht ermittelt werden.",
     "Impossibile trovare il tuo indirizzo internet."},
    {"Esa sala no existe (o ya se cerró).", "That room doesn't exist (or it was closed).", "Ce salon n'existe pas (ou il a été fermé).",
     "Diesen Raum gibt es nicht (oder er wurde geschlossen).", "Quella stanza non esiste (o è stata chiusa)."},
    {"Esa sala es de otra versión del juego: actualizad los dos.", "That room is from another game version: both of you, update.",
     "Ce salon vient d'une autre version du jeu : mettez-vous à jour.", "Der Raum ist von einer anderen Spielversion: beide aktualisieren.",
     "Quella stanza è di un'altra versione del gioco: aggiornate entrambi."},
    {"La sala está llena (8 jugadores).", "The room is full (8 players).", "Le salon est plein (8 joueurs).",
     "Der Raum ist voll (8 Spieler).", "La stanza è piena (8 giocatori)."},
    {"La sala se ha cerrado.", "The room has been closed.", "Le salon a été fermé.", "Der Raum wurde geschlossen.", "La stanza è stata chiusa."},
    {"Crear sala", "Create room", "Créer un salon", "Raum erstellen", "Crea stanza"},
    {"Unirse a una sala", "Join a room", "Rejoindre un salon", "Raum beitreten", "Entra in una stanza"},
    {"Unirse", "Join", "Rejoindre", "Beitreten", "Entra"},
    {"Escribe el código de 6 letras que te pase el que creó la sala.", "Type the 6-letter code from whoever created the room.",
     "Tape le code de 6 lettres de celui qui a créé le salon.", "Gib den 6-stelligen Code von dem ein, der den Raum erstellt hat.",
     "Scrivi il codice di 6 lettere di chi ha creato la stanza."},
};

static const char *T(const char *es) {
    int i = !strcmp(idioma_menu, "en") ? 1 : !strcmp(idioma_menu, "fr") ? 2 : !strcmp(idioma_menu, "de") ? 3
          : !strcmp(idioma_menu, "it") ? 4 : 0;
    if (!i) return es;
    for (unsigned k = 0; k < sizeof textos / sizeof *textos; k++)
        if (!strcmp(textos[k].es, es)) {
            const char *t[] = {textos[k].es, textos[k].en, textos[k].fr, textos[k].de, textos[k].it};
            return t[i];
        }
    return es;
}

// ---- ImGui -----------------------------------------------------------------------------------------
static double reloj_s(void) {
#ifdef _WIN32
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return (double)t.QuadPart / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

// colores del launcher: azul noche, rojo y amarillo
static void estilo(void) {
    ImGuiStyle &s = ImGui::GetStyle();
    ImGui::StyleColorsDark();
    s.WindowRounding = 14; s.FrameRounding = 7; s.TabRounding = 7; s.GrabRounding = 7; s.ChildRounding = 8;
    s.WindowPadding = ImVec2(22, 18); s.FramePadding = ImVec2(10, 6); s.ItemSpacing = ImVec2(10, 9);
    s.WindowBorderSize = 1;
    ImVec4 *c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.063f, 0.075f, 0.165f, 0.97f);   // #10132A
    c[ImGuiCol_Border] = ImVec4(0.165f, 0.188f, 0.384f, 1);          // #2A3062
    c[ImGuiCol_FrameBg] = ImVec4(0.125f, 0.145f, 0.29f, 1);          // #20254A
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.19f, 0.38f, 1);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.2f, 0.22f, 0.44f, 1);
    c[ImGuiCol_Tab] = ImVec4(0.094f, 0.11f, 0.227f, 1);
    c[ImGuiCol_TabHovered] = ImVec4(1.0f, 0.30f, 0.24f, 0.85f);
    c[ImGuiCol_TabSelected] = ImVec4(1.0f, 0.30f, 0.24f, 1);         // #FF4D3D
    c[ImGuiCol_TabSelectedOverline] = ImVec4(1.0f, 0.79f, 0.25f, 1);
    c[ImGuiCol_Button] = ImVec4(0.125f, 0.145f, 0.29f, 1);
    c[ImGuiCol_ButtonHovered] = ImVec4(1.0f, 0.30f, 0.24f, 0.8f);
    c[ImGuiCol_ButtonActive] = ImVec4(1.0f, 0.30f, 0.24f, 1);
    c[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.79f, 0.25f, 1);           // #FFC940
    c[ImGuiCol_SliderGrab] = ImVec4(1.0f, 0.79f, 0.25f, 1);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.86f, 0.45f, 1);
    c[ImGuiCol_Header] = ImVec4(0.125f, 0.145f, 0.29f, 1);
    c[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 0.30f, 0.24f, 0.6f);
    c[ImGuiCol_NavHighlight] = ImVec4(1.0f, 0.79f, 0.25f, 1);
    c[ImGuiCol_Text] = ImVec4(0.953f, 0.957f, 1, 1);                 // #F3F4FF
    c[ImGuiCol_TextDisabled] = ImVec4(0.604f, 0.639f, 0.78f, 1);     // #9AA3C7
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.125f, 0.145f, 0.29f, 1);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
}

// La letra: la del sistema (Segoe UI en Windows; DejaVu, Noto o Ubuntu en Linux), con tildes y ñ. Se carga
// grande y se escala según el tamaño de la ventana. Si no hay ninguna, la de ImGui (sin tildes).
static const float LETRA = 34;
static void carga_letra(void) {
    ImGuiIO &io = ImGui::GetIO();
    static const ImWchar latin[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0};  // Latin-1, rayas, puntos suspensivos
    const char *rutas[] = {
#ifdef _WIN32
        "C:\\Windows\\Fonts\\segoeui.ttf", "C:\\Windows\\Fonts\\arial.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf", "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/google-noto/NotoSans-Regular.ttf", "/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
#endif
    };
    for (unsigned i = 0; i < sizeof rutas / sizeof *rutas; i++) {
        FILE *f = fopen(rutas[i], "rb");
        if (!f) continue;
        fclose(f);
        if (io.Fonts->AddFontFromFileTTF(rutas[i], LETRA, NULL, latin)) return;
    }
    ImFontConfig c;
    c.SizePixels = LETRA;
    io.Fonts->AddFontDefault(&c);
}

extern "C" int menu_inicia(void) {
    if (listo) return 1;
    cerrojo_inicia();  // antes de 'listo': hasta entonces la ventana no mete nada en la cola
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;  // nada de imgui.ini: los ajustes van en ajustes.ini
    io.LogFilename = NULL;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    estilo();
    carga_letra();
    for (int k = 0; k < 6; k++) {  // la de los botones propios (la primera que se carga sigue siendo la del menú)
        ImFontConfig c;
        c.FontDataOwnedByAtlas = false;
        c.OversampleH = c.OversampleV = 1;
        c.PixelSnapH = true;
        letras_pixel[k] = io.Fonts->AddFontFromMemoryTTF((void *)letra_pixel, (int)sizeof letra_pixel, 10.0f * (k + 1), &c);
    }
    letra_boton = letras_pixel[1];
    {  // bordes duros: el atlas de letras, a 0 o 255 (lo pide el estilo de píxeles; la letra del menú apenas cambia)
        unsigned char *px;
        int aw, ah;
        io.Fonts->GetTexDataAsRGBA32(&px, &aw, &ah);
        for (int i = 0; i < aw * ah; i++) px[i * 4 + 3] = px[i * 4 + 3] >= 110 ? 255 : 0;
    }
    if (!IMPL_INIT()) return 0;
    lee_ajustes();
    aplica();
    cambiado = 0;
    listo = 1;
    if (getenv("MKDS_MENU_ABIERTO")) {  // pruebas: arranca con el menú abierto (y en esa pestaña, 0-3)
        abierto = 1;
        pestaña_pedida = atoi(getenv("MKDS_MENU_ABIERTO")) % 5;
    }
    return 1;
}

static ImGuiKey clave_imgui(int t) {
    switch (t) {
    case MT_TAB: return ImGuiKey_Tab;
    case MT_IZQ: return ImGuiKey_LeftArrow;
    case MT_DER: return ImGuiKey_RightArrow;
    case MT_ARRIBA: return ImGuiKey_UpArrow;
    case MT_ABAJO: return ImGuiKey_DownArrow;
    case MT_INICIO: return ImGuiKey_Home;
    case MT_FIN: return ImGuiKey_End;
    case MT_BORRAR: return ImGuiKey_Backspace;
    case MT_SUPR: return ImGuiKey_Delete;
    case MT_ENTER: return ImGuiKey_Enter;
    case MT_ESC: return ImGuiKey_Escape;
    case MT_CTRL: return ImGuiMod_Ctrl;
    case MT_SHIFT: return ImGuiMod_Shift;
    case MT_PAD_ARRIBA: return ImGuiKey_GamepadDpadUp;
    case MT_PAD_ABAJO: return ImGuiKey_GamepadDpadDown;
    case MT_PAD_IZQ: return ImGuiKey_GamepadDpadLeft;
    case MT_PAD_DER: return ImGuiKey_GamepadDpadRight;
    case MT_PAD_ACEPTA: return ImGuiKey_GamepadFaceDown;
    case MT_PAD_ATRAS: return ImGuiKey_GamepadFaceRight;
    }
    return ImGuiKey_None;
}

// pasa a ImGui lo que ha llegado desde el hilo de la ventana
static void vacia_cola(void) {
    ImGuiIO &io = ImGui::GetIO();
    cierra_c();
    for (int i = 0; i < cola_n; i++) {
        Evento &e = cola[i];
        switch (e.tipo) {
        case EV_TECLA:
            if (e.a == MT_PAD_L || e.a == MT_PAD_R) {  // L / R: pestaña anterior / siguiente
                if (e.b) pestaña_pedida = (pestaña_actual + (e.a == MT_PAD_R ? 1 : 4)) % 5;
            } else if (e.a == MT_PAD_ATRAS && e.b) {
                abierto = 0;  // B en el mando: volver al juego (como en los menús de la DS)
                if (cambiado) { guarda_ajustes(); cambiado = 0; }
            } else {
                io.AddKeyEvent(clave_imgui(e.a), e.b != 0);
            }
            break;
        case EV_CARACTER: io.AddInputCharacter(e.a); break;
        case EV_RATON: io.AddMousePosEvent(e.x, e.y); break;
        case EV_BOTON: io.AddMouseButtonEvent(e.a, e.b != 0); break;
        case EV_RUEDA: io.AddMouseWheelEvent(0, e.y); break;
        }
    }
    cola_n = 0;
    abre_c();
}

static void ayuda(const char *t) {  // la nota gris de debajo de cada ajuste, un poco más pequeña
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::SetWindowFontScale(0.85f);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(t);
    ImGui::PopTextWrapPos();
    ImGui::SetWindowFontScale(1);
    ImGui::PopStyleColor();
}

static void titulo(const char *t) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.79f, 0.25f, 1));
    ImGui::TextUnformatted(t);
    ImGui::PopStyleColor();
}

static int pestaña(const char *nombre, int i) {
    ImGuiTabItemFlags f = pestaña_pedida == i ? ImGuiTabItemFlags_SetSelected : 0;
    if (!ImGui::BeginTabItem(nombre, NULL, f)) return 0;
    pestaña_actual = i;
    return 1;
}

static void pestaña_graficos(void) {
    int pc = ventana_pantalla_completa();
    bool b = pc != 0;
    if (ImGui::Checkbox(T("Pantalla completa"), &b)) ventana_pide_pantalla_completa(b);
    ayuda(T("Las dos pantallas lado a lado. También con F11."));
    titulo(T("Pantallas"));
    int d = ventana_disposicion();
    if (ImGui::RadioButton(T("Las dos iguales"), d == 0)) { ventana_pon_disposicion(0); cambiado = 1; }
    ImGui::SameLine();
    if (ImGui::RadioButton(T("Una encima de otra"), d == 2)) { ventana_pon_disposicion(2); cambiado = 1; }
    ImGui::SameLine();
    if (ImGui::RadioButton(T("La grande y la táctil al lado"), d == 1)) { ventana_pon_disposicion(1); cambiado = 1; }
    ayuda(T(d == 2 ? "La pantalla de arriba en grande y la táctil pequeña encima de ella, en la esquina de abajo a la derecha."
            : d == 1 ? "La pantalla de arriba a todo el alto y la táctil pequeña pegada a su derecha, sin taparla: juntas llenan una pantalla 16:9."
                     : "Las dos pantallas al mismo tamaño: una sobre otra en ventana, lado a lado en pantalla completa."));
    titulo(T("Imágenes por segundo"));
    static const int fpss[] = {60, 120, 180, 240};
    for (int i = 0; i < 4; i++) {
        char t[16];
        snprintf(t, sizeof t, "%d##fps", fpss[i]);
        if (i) ImGui::SameLine();
        if (ImGui::RadioButton(t, aj.fps == fpss[i])) { aj.fps = fpss[i]; aplica(); }
    }
    ayuda(T("El juego va a 60; por encima se pintan imágenes intermedias del 3D. Si el PC no llega, pinta las que puede."));
    titulo(T("Resolución del 3D"));
    for (int i = 1; i <= 4; i++) {
        char t[32];
        if (i == 1) snprintf(t, sizeof t, "%s##esc", T("Original"));
        else snprintf(t, sizeof t, "%d×##esc%d", i, i);
        if (i > 1) ImGui::SameLine();
        if (ImGui::RadioButton(t, aj.escala == i)) { aj.escala = i; cambiado = 1; }
    }
    ayuda(T("Se aplica al volver a abrir el juego."));
    ImGui::Spacing();
    bool gpu = aj.gpu != 0;
    if (ImGui::Checkbox(T("3D con la tarjeta gráfica (experimental)"), &gpu)) {
        aj.gpu = gpu;
        gx_pon_gpu(aj.gpu);  // al momento
        cambiado = 1;
    }
    ayuda(T("Gasta mucha menos CPU, sobre todo con resolución alta y más FPS. Si ves algo raro, quítalo y dínoslo."));
    if (gpu) {
        bool sv = aj.suavizado != 0;
        if (ImGui::Checkbox(T("Suavizar bordes"), &sv)) {
            aj.suavizado = sv;
            gx_pon_suavizado(aj.suavizado);  // al momento
            cambiado = 1;
        }
        ayuda(T("Antialiasing, como en la DS. Si te bajan los FPS, quítalo."));
    }
    ImGui::Spacing();
    bool m = aj.mostrar_fps != 0;
    if (ImGui::Checkbox(T("Mostrar FPS"), &m)) { aj.mostrar_fps = m; cambiado = 1; }
    ayuda(T("En una esquina, también en pantalla completa."));
}

static void pestaña_mandos(void) {
    titulo(T("Conectados"));
    char nombres[8][64];
    int n = ventana_mandos(nombres, 8);
    if (!n) ayuda(T("Ningún mando conectado. Puedes jugar con el teclado."));
    for (int i = 0; i < n; i++) ImGui::BulletText("%s", nombres[i]);
    ImGui::Spacing();
    if (ImGui::BeginTable("botones", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Botón de la DS"));
        ImGui::TableSetupColumn(T("Mando"));
        ImGui::TableSetupColumn(T("Teclado"));
        ImGui::TableHeadersRow();
        char a[64], b[64], x[64], y[64];  // Xbox / PlayStation (por posición: el de arriba es la X de la DS)
        snprintf(a, sizeof a, "A / %s", T("Cruz"));
        snprintf(b, sizeof b, "B / %s", T("Círculo"));
        snprintf(x, sizeof x, "Y / %s", T("Triángulo"));
        snprintf(y, sizeof y, "X / %s", T("Cuadrado"));
        const char *filas[][3] = {
            {"A", a, "X"}, {"B", b, "Z"}, {"X", x, "S"}, {"Y", y, "A"},
            {"L", "LB · LT / L1 · L2", "Q"}, {"R", "RB · RT / R1 · R2", "W"},
            {"Start", "Start / Options", "Enter"}, {"Select", "Back / Create", T("Retroceso")},
            {T("Cruceta"), T("cruceta o stick izquierdo"), T("flechas")},
            {T("Lápiz"), "—", T("ratón en la pantalla de abajo")},
        };
        for (unsigned i = 0; i < sizeof filas / sizeof *filas; i++) {
            ImGui::TableNextRow();
            for (int k = 0; k < 3; k++) { ImGui::TableSetColumnIndex(k); ImGui::TextWrapped("%s", filas[i][k]); }
        }
        ImGui::EndTable();
    }
}

static void pestaña_audio(void) {
    titulo(T("Volumen"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##vol", &aj.volumen, 0, 100, "%d %%")) aplica();
    bool s = aj.silencio != 0;
    if (ImGui::Checkbox(T("Silenciar"), &s)) { aj.silencio = s; aplica(); }
    ImGui::Spacing();
    titulo(T("Micrófono (soplar en la Batalla de globos)"));
    bool m = aj.micro != 0;
    if (ImGui::Checkbox("##micro", &m)) { aj.micro = m; aplica(); }
    ImGui::SameLine();
    ImGui::TextUnformatted(T("Sensibilidad"));
    ImGui::SameLine();
    ImGui::BeginDisabled(!aj.micro);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderInt("##gan", &aj.ganancia, 1, 16, "x%d")) aplica();
    ImGui::EndDisabled();
    ayuda(T("Si no se infla al soplar, súbela; si se infla solo, bájala."));
}

static void pestaña_juego(void) {
    titulo(T("Tu nombre"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##nombre", aj.nombre, 11)) aplica();
    ayuda(T("El que ven los demás en multijugador (10 letras como mucho)."));
    titulo(T("Idioma del juego"));
    static const char *idiomas[] = {"English", "Français", "Deutsch", "Italiano", "Español"};
    int i = aj.idioma - 1;
    if (i < 0 || i > 4) i = 4;
    ImGui::SetNextItemWidth(-1);
    if (ImGui::Combo("##idioma", &i, idiomas, 5)) { aj.idioma = i + 1; cambiado = 1; }
    ayuda(T("Se aplica al volver a abrir el juego."));
    titulo(T("Multijugador: IP de tu amigo"));
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputText("##ip", aj.ip, sizeof aj.ip, ImGuiInputTextFlags_CharsNoBlank)) aplica();
    ayuda(T("Solo si no os veis en la lista de grupos (por ejemplo, su IP de Radmin). Vacío = buscar solo."));
}

extern "C" int wfc_ping_ms(void);  // wfc.c: el ping con los rivales del online (-1 si no hay)

// La sala (las pide el launcher; ver online_prueba en wifi.c), arriba y en el centro: el código para pasárselo a los
// demás y qué hacer. Cuando empieza la carrera se quita (una carrera son más de 500 polígonos; los menús, menos).
enum { ONL_NADA, ONL_CONECTANDO, ONL_SIN_SERVIDOR, ONL_SIN_STUN, ONL_NO_EXISTE, ONL_VERSION, ONL_LLENA, ONL_EN_SALA,
       ONL_UNIDO, ONL_CERRADA };

static int hay_aviso_sala(void) {  // (con la interfaz de Action Kart, DISPOSICION=3, la sala sale en sus páginas)
    return online_estado(NULL, NULL) != ONL_NADA && gx_polis() < 500 && ventana_disposicion() != 3;
}

static void aviso_sala(float W, float s) {
    char codigo[8] = "";
    int jugadores = 0, e = online_estado(codigo, &jugadores);
    ImGui::SetNextWindowPos(ImVec2(W / 2, 12 * s), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * s, 8 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8 * s);
    ImGui::Begin("##sala", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + W * 0.8f);
    if (e == ONL_EN_SALA || e == ONL_UNIDO) {
        ImGui::TextUnformatted(T(e == ONL_EN_SALA ? "Tu sala" : "Estás en la sala"));
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.79f, 0.25f, 1));
        ImGui::TextUnformatted(codigo);
        ImGui::PopStyleColor();
        if (e == ONL_EN_SALA) {
            ImGui::SameLine();
            ImGui::Text(T("Jugadores en la sala: %d"), jugadores + 1);
        }
        ImGui::SetWindowFontScale(0.8f);
        ImGui::TextUnformatted(T(e == ONL_EN_SALA
                                     ? "Pasa el código a tus amigos. Luego, en el juego: Multijugador > Crear un grupo, y LISTO cuando estén todos."
                                     : "Ahora, en el juego: Multijugador y elige el grupo de la lista."));
        ImGui::SetWindowFontScale(1);
    } else if (e == ONL_CONECTANDO) {
        ImGui::TextUnformatted(T("Conectando…"));
    } else {
        static const char *errores[] = {NULL, NULL, "No se pudo conectar con el servidor de salas. ¿Hay internet?",
                                        "No se pudo averiguar tu dirección en internet.", "Esa sala no existe (o ya se cerró).",
                                        "Esa sala es de otra versión del juego: actualizad los dos.", "La sala está llena (8 jugadores).",
                                        NULL, NULL, "La sala se ha cerrado."};
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.54f, 0.48f, 1));
        ImGui::TextUnformatted(T(e >= 0 && e < 10 && errores[e] ? errores[e] : "La sala se ha cerrado."));
        ImGui::PopStyleColor();
    }
    ImGui::PopTextWrapPos();
    ImGui::End();
    ImGui::PopStyleVar(2);
}

static void contador_fps(float W, float s) {
    ImGui::SetNextWindowPos(ImVec2(W - 12 * s, 12 * s), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10 * s, 5 * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6 * s);
    ImGui::Begin("##fps", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowFontScale(0.75f);
    int ping = wfc_ping_ms();
    if (aj.mostrar_fps && ping >= 0) ImGui::Text("%d FPS  |  %d ms", fps_pantalla, ping);
    else if (ping >= 0) ImGui::Text("%d ms", ping);  // jugando online: el ping aunque no se enseñen los FPS
    else ImGui::Text("%d FPS", fps_pantalla);  // lo que se ve por segundo (con las imágenes intermedias)
    ImGui::End();
    ImGui::PopStyleVar(2);
}

// ---- botones propios: barras inclinadas azules con borde celeste y letra Lilita (el diseño de Action Kart) ----
static MenuBoton botones_p[16];
static int nbotones_p;
extern "C" void menu_pon_botones(const MenuBoton *b, int n) {
    nbotones_p = n < 0 ? 0 : n > 16 ? 16 : n;
    if (nbotones_p) memcpy(botones_p, b, sizeof(MenuBoton) * (size_t)nbotones_p);
}

static void texto_contorno(ImDrawList *d, float t, ImVec2 p, ImU32 col, const char *s, float g) {
    static const float o[8][2] = {{-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};
    t = usa_letra(t);
    p = ImVec2((float)(int)p.x, (float)(int)p.y);
    if (t < 30) {  // pequeña: solo una sombra de un píxel (el contorno entero la emborrona)
        d->AddText(letra_boton, t, ImVec2(p.x + 1, p.y + 1), IM_COL32(14, 22, 80, 255), s);
        d->AddText(letra_boton, t, p, col, s);
        return;
    }
    g = 1;
    for (int i = 0; i < 8; i++) d->AddText(letra_boton, t, ImVec2(p.x + o[i][0] * g, p.y + o[i][1] * g), IM_COL32(14, 22, 80, 255), s);
    d->AddText(letra_boton, t, p, col, s);
}

static void pinta_botones_propios(void) {
    ImDrawList *d = ImGui::GetBackgroundDrawList();
    for (int i = 0; i < nbotones_p; i++) {
        const MenuBoton *b = &botones_p[i];
        float h = b->y1 - b->y0, w = b->x1 - b->x0, g = h < 40 ? 1.0f : 2.0f;  // grosor en píxeles de la capa baja
        ImU32 navy = IM_COL32(14, 22, 80, 255), celeste = IM_COL32(159, 230, 255, 255), oro = IM_COL32(255, 233, 74, 255);
        if (b->tipo == 2 || b->tipo == 3) {  // flechas: círculo azul con borde celeste y triángulo dorado
            float cx = (b->x0 + b->x1) / 2, cy = (b->y0 + b->y1) / 2, r = w / 2, f = b->tipo == 2 ? -1.0f : 1.0f;
            d->AddCircleFilled(ImVec2(cx, cy + g * 3), r, navy, 32);
            d->AddCircleFilled(ImVec2(cx, cy), r, b->marcado ? IM_COL32(78, 138, 240, 255) : IM_COL32(27, 42, 174, 255), 32);
            d->AddCircle(ImVec2(cx, cy), r, celeste, 32, g * 1.8f);
            d->AddTriangleFilled(ImVec2(cx + f * r * 0.5f, cy), ImVec2(cx - f * r * 0.3f, cy - r * 0.45f), ImVec2(cx - f * r * 0.3f, cy + r * 0.45f), oro);
            continue;
        }
        if (b->tipo == 5) {  // tarjeta: el fondo lo pone la ventana (debajo va la imagen); aquí el borde y el título
            d->AddRect(ImVec2(b->x0, b->y0), ImVec2(b->x1, b->y1), celeste, h * 0.06f, 0, g * 1.8f);
            if (b->texto && *b->texto) {
                float t = h * 0.11f;
                texto_contorno(d, t, ImVec2(b->x0 + h * 0.06f, b->y0 + h * 0.03f), oro, b->texto, g);
            }
            continue;
        }
        if (b->tipo == 15) {  // título grande centrado (la portada), dorado con contorno y sombra
            float t = usa_letra(h);
            ImVec2 tam = letra_boton->CalcTextSizeA(t, 1e9f, 0, b->texto);
            float x = (b->x0 + b->x1 - tam.x) / 2, y = (b->y0 + b->y1 - tam.y) / 2;
            d->AddText(letra_boton, t, ImVec2((float)(int)x + 3, (float)(int)y + 4), navy, b->texto);
            texto_contorno(d, t, ImVec2(x, y), IM_COL32(247, 207, 58, 255), b->texto, 2);
            continue;
        }
        if (b->tipo == 14) {  // girando: 8 puntos en círculo, uno dorado que da la vuelta (pixelado)
            float cx = (b->x0 + b->x1) / 2, cy = (b->y0 + b->y1) / 2, r = w * 0.38f, p = w * 0.09f;
            int paso = (int)(ImGui::GetTime() * 10) % 8;
            for (int k = 0; k < 8; k++) {
                float a = k * 0.785398f, x = cx + cosf(a) * r, y = cy + sinf(a) * r;
                int lejos = (paso - k + 8) % 8;
                ImU32 col = lejos == 0 ? oro : lejos < 3 ? celeste : IM_COL32(60, 80, 170, 255);
                d->AddRectFilled(ImVec2((float)(int)(x - p), (float)(int)(y - p)), ImVec2((float)(int)(x + p), (float)(int)(y + p)), col);
            }
            continue;
        }
        if (b->tipo == 13) {  // el mando conectado: pastilla con su nombre y una luz (verde: conectado; amarilla: pulsando)
            ImU32 fondo = IM_COL32(14, 22, 80, 220);
            d->AddRectFilled(ImVec2(b->x0, b->y0), ImVec2(b->x1, b->y1), fondo, h * 0.5f);
            d->AddRect(ImVec2(b->x0, b->y0), ImVec2(b->x1, b->y1), celeste, h * 0.5f, 0, 1);
            float r = h * 0.22f, cx = b->x0 + h * 0.5f, cy = (b->y0 + b->y1) / 2;
            ImU32 luz = !b->icono ? IM_COL32(150, 160, 190, 255) : b->marcado ? oro : IM_COL32(90, 220, 120, 255);
            d->AddCircleFilled(ImVec2(cx, cy), r, luz, 12);
            const char *etiqueta = b->icono ? "CONTROLLER" : "INPUT";
            float t = usa_letra(h * 0.55f);
            ImVec2 te = letra_boton->CalcTextSizeA(t, 1e9f, 0, etiqueta);
            texto_contorno(d, t, ImVec2(cx + r * 2, cy - te.y / 2), IM_COL32(169, 220, 255, 255), etiqueta, 1);
            float t2 = usa_letra(h * 0.55f);
            texto_contorno(d, t2, ImVec2(cx + r * 2 + te.x + h * 0.25f, cy - te.y / 2), IM_COL32(255, 255, 255, 255), b->texto, 1);
            continue;
        }
        if (b->tipo == 10) {  // botón redondo de los mandos táctiles (A, B, X, Y, R): letra dorada; pulsado, más claro
            float cx = (b->x0 + b->x1) / 2, cy = (b->y0 + b->y1) / 2, r = w / 2;
            d->AddCircleFilled(ImVec2(cx, cy + 2), r, IM_COL32(14, 22, 80, 200), 28);
            d->AddCircleFilled(ImVec2(cx, cy), r, b->marcado ? IM_COL32(78, 138, 240, 235) : IM_COL32(27, 42, 174, 205), 28);
            d->AddCircle(ImVec2(cx, cy), r, celeste, 28, 2);
            float t = usa_letra(r * 1.15f);
            ImVec2 tam = letra_boton->CalcTextSizeA(t, 1e9f, 0, b->texto);
            texto_contorno(d, t, ImVec2(cx - tam.x / 2, cy - tam.y / 2), b->marcado ? oro : IM_COL32(247, 207, 58, 255), b->texto, 1);
            continue;
        }
        if (b->tipo == 11) {  // cruceta: una cruz azul con borde celeste; el brazo pulsado, claro (icono: bits der/izq/arriba/abajo)
            float cx = (b->x0 + b->x1) / 2, cy = (b->y0 + b->y1) / 2, a = w / 2, bw = a * 0.34f;
            ImU32 azul = IM_COL32(27, 42, 174, 205), claro = IM_COL32(78, 138, 240, 235);
            d->AddRectFilled(ImVec2(cx - bw, cy - a + 3), ImVec2(cx + bw, cy + a + 3), IM_COL32(14, 22, 80, 200));
            d->AddRectFilled(ImVec2(cx - a, cy - bw + 3), ImVec2(cx + a, cy + bw + 3), IM_COL32(14, 22, 80, 200));
            d->AddRectFilled(ImVec2(cx - bw, cy - a), ImVec2(cx + bw, cy + a), azul);
            d->AddRectFilled(ImVec2(cx - a, cy - bw), ImVec2(cx + a, cy + bw), azul);
            struct { int bit; float x0, y0, x1, y1, tx, ty, nx, ny; } brazo[4] = {
                {1, cx + bw, cy - bw, cx + a, cy + bw, 1, 0, 0, 1}, {2, cx - a, cy - bw, cx - bw, cy + bw, -1, 0, 0, 1},
                {4, cx - bw, cy - a, cx + bw, cy - bw, 0, -1, 1, 0}, {8, cx - bw, cy + bw, cx + bw, cy + a, 0, 1, 1, 0}};
            for (int k = 0; k < 4; k++) {
                if (b->icono & brazo[k].bit) d->AddRectFilled(ImVec2(brazo[k].x0, brazo[k].y0), ImVec2(brazo[k].x1, brazo[k].y1), claro);
                float px = cx + brazo[k].tx * a * 0.72f, py = cy + brazo[k].ty * a * 0.72f, q = bw * 0.7f;
                d->AddTriangleFilled(ImVec2(px + brazo[k].tx * q, py + brazo[k].ty * q),
                                     ImVec2(px - brazo[k].tx * q * 0.4f + brazo[k].nx * q, py - brazo[k].ty * q * 0.4f + brazo[k].ny * q),
                                     ImVec2(px - brazo[k].tx * q * 0.4f - brazo[k].nx * q, py - brazo[k].ty * q * 0.4f - brazo[k].ny * q),
                                     (b->icono & brazo[k].bit) ? oro : celeste);
            }
            ImVec2 cruz[12] = {{cx - bw, cy - a}, {cx + bw, cy - a}, {cx + bw, cy - bw}, {cx + a, cy - bw}, {cx + a, cy + bw},
                               {cx + bw, cy + bw}, {cx + bw, cy + a}, {cx - bw, cy + a}, {cx - bw, cy + bw}, {cx - a, cy + bw},
                               {cx - a, cy - bw}, {cx - bw, cy - bw}};
            d->AddPolyline(cruz, 12, celeste, ImDrawFlags_Closed, 2);
            continue;
        }
        if (b->tipo == 12) {  // ITEM: cuadrado redondeado con borde dorado, un cubo y la palabra
            float cx = (b->x0 + b->x1) / 2, cy = (b->y0 + b->y1) / 2, r = w / 2;
            d->AddRectFilled(ImVec2(b->x0, b->y0 + 2), ImVec2(b->x1, b->y1 + 2), IM_COL32(14, 22, 80, 200), r * 0.4f);
            d->AddRectFilled(ImVec2(b->x0, b->y0), ImVec2(b->x1, b->y1), b->marcado ? IM_COL32(78, 138, 240, 235) : IM_COL32(27, 42, 174, 205), r * 0.4f);
            d->AddRect(ImVec2(b->x0, b->y0), ImVec2(b->x1, b->y1), oro, r * 0.4f, 0, 2);
            float c = r * 0.42f, iy = cy - r * 0.18f;
            ImVec2 hex[6] = {{cx, iy - c}, {cx + c, iy - c * 0.5f}, {cx + c, iy + c * 0.5f}, {cx, iy + c}, {cx - c, iy + c * 0.5f}, {cx - c, iy - c * 0.5f}};
            d->AddPolyline(hex, 6, oro, ImDrawFlags_Closed, 2);
            d->AddLine(ImVec2(cx - c, iy - c * 0.5f), ImVec2(cx, iy), oro, 2);
            d->AddLine(ImVec2(cx + c, iy - c * 0.5f), ImVec2(cx, iy), oro, 2);
            d->AddLine(ImVec2(cx, iy), ImVec2(cx, iy + c), oro, 2);
            float t = usa_letra(r * 0.5f);
            ImVec2 tam = letra_boton->CalcTextSizeA(t, 1e9f, 0, "ITEM");
            texto_contorno(d, t, ImVec2(cx - tam.x / 2, b->y1 - tam.y - r * 0.12f), oro, "ITEM", 1);
            continue;
        }
        if (b->tipo == 7) {  // título de la página: grande, dorado, con contorno y sombra (como en el diseño)
            float t = h;
            float tt = usa_letra(t);
            d->AddText(letra_boton, tt, ImVec2((float)(int)b->x0 + 2, (float)(int)b->y0 + 3), navy, b->texto);
            texto_contorno(d, t, ImVec2(b->x0, b->y0), IM_COL32(247, 207, 58, 255), b->texto, g * 1.6f);
            continue;
        }
        if (b->tipo == 9) {  // copa: baldosa con insignia de color y número, y el nombre debajo
            static const ImU32 col[8] = {IM_COL32(255, 106, 61, 255), IM_COL32(255, 186, 38, 255), IM_COL32(64, 200, 255, 255),
                                         IM_COL32(186, 120, 255, 255), IM_COL32(120, 200, 90, 255), IM_COL32(60, 160, 255, 255),
                                         IM_COL32(110, 210, 120, 255), IM_COL32(255, 220, 60, 255)};
            int c = (b->icono - 11) & 7;
            d->AddRectFilled(ImVec2(b->x0, b->y0 + g * 3), ImVec2(b->x1, b->y1 + g * 3), navy, h * 0.14f);
            d->AddRectFilled(ImVec2(b->x0, b->y0), ImVec2(b->x1, b->y1), b->marcado ? IM_COL32(78, 138, 240, 255) : IM_COL32(27, 42, 174, 255), h * 0.14f);
            d->AddRect(ImVec2(b->x0, b->y0), ImVec2(b->x1, b->y1), b->marcado ? oro : celeste, h * 0.14f, 0, g * 1.8f);
            float cx = (b->x0 + b->x1) / 2, cy = b->y0 + h * 0.4f, r = h * 0.24f;
            d->AddCircleFilled(ImVec2(cx, cy + g * 2), r, navy, 32);
            d->AddCircleFilled(ImVec2(cx, cy), r, col[c], 32);
            d->AddCircle(ImVec2(cx, cy), r, IM_COL32(255, 255, 255, 255), 32, g * 1.4f);
            char num[2] = {(char)('1' + (c & 3)), 0};
            float tn = r * 1.3f;
            ImVec2 tamn = (usa_letra(tn), letra_boton->CalcTextSizeA(usa_letra(tn), 1e9f, 0, num));
            texto_contorno(d, tn, ImVec2(cx - tamn.x / 2, cy - tamn.y / 2), IM_COL32(255, 255, 255, 255), num, g);
            float t = h * 0.22f;
            ImVec2 tam = (usa_letra(t), letra_boton->CalcTextSizeA(usa_letra(t), 1e9f, 0, b->texto));
            texto_contorno(d, t, ImVec2(cx - tam.x / 2, b->y1 - h * 0.08f - tam.y), b->marcado ? oro : IM_COL32(169, 220, 255, 255), b->texto, g * 0.8f);
            continue;
        }
        if (b->tipo == 6) {  // estadística: nombre y barra
            float t = h * 0.95f, xb = b->x0 + w * 0.58f;
            texto_contorno(d, t, ImVec2(b->x0, b->y0 + (h - t) / 2), IM_COL32(169, 220, 255, 255), b->texto, g * 0.7f);
            d->AddRectFilled(ImVec2(xb, b->y0 + h * 0.2f), ImVec2(b->x1, b->y1 - h * 0.2f), navy, h * 0.3f);
            d->AddRectFilled(ImVec2(xb, b->y0 + h * 0.2f), ImVec2(xb + (b->x1 - xb) * b->valor, b->y1 - h * 0.2f), oro, h * 0.3f);
            d->AddRect(ImVec2(xb, b->y0 + h * 0.2f), ImVec2(b->x1, b->y1 - h * 0.2f), celeste, h * 0.3f, 0, g);
            continue;
        }
        if (b->tipo == 1 && 0) {  // (la pestaña de antes; ahora volver es una barra más, con su triángulo)
            d->AddRectFilled(ImVec2(b->x0, b->y0 + g * 2), ImVec2(b->x1, b->y1 + g * 2), IM_COL32(14, 22, 80, 255), h * 0.3f);
            d->AddRectFilled(ImVec2(b->x0, b->y0), ImVec2(b->x1, b->y1), IM_COL32(244, 246, 252, 255), h * 0.3f);
            d->AddRect(ImVec2(b->x0, b->y0), ImVec2(b->x1, b->y1), IM_COL32(44, 73, 200, 255), h * 0.3f, 0, g * 1.5f);
            float cx = (b->x0 + b->x1) / 2, cy = (b->y0 + b->y1) / 2, r = h * 0.17f;
            ImU32 rojo = IM_COL32(240, 48, 42, 255);
            d->PathLineTo(ImVec2(cx - w * 0.22f, cy - r));
            d->PathLineTo(ImVec2(cx + w * 0.08f, cy - r));
            d->PathArcTo(ImVec2(cx + w * 0.08f, cy), r, -1.5708f, 1.5708f);
            d->PathLineTo(ImVec2(cx - w * 0.05f, cy + r));
            d->PathStroke(rojo, 0, g * 2.6f);
            d->AddTriangleFilled(ImVec2(cx - w * 0.3f, cy - r), ImVec2(cx - w * 0.16f, cy - r - h * 0.17f),
                                 ImVec2(cx - w * 0.16f, cy - r + h * 0.17f), rojo);
            continue;
        }
        float sk = h * 0.21f, ym = b->y0 + h * 0.5f;  // inclinada unos 12 grados, como en el diseño
        if (b->tipo == 1) {  // volver: triángulo dorado a la izquierda del texto
            float ax = b->x0 + sk + h * 0.42f, ay = ym, a = h * 0.2f;
            d->AddTriangleFilled(ImVec2(ax - a, ay), ImVec2(ax + a * 0.7f, ay - a), ImVec2(ax + a * 0.7f, ay + a), IM_COL32(255, 233, 74, 255));
        }
        ImVec2 p[4] = {{b->x0 + sk, b->y0}, {b->x1, b->y0}, {b->x1 - sk, b->y1}, {b->x0, b->y1}};
        ImVec2 sh[4];
        for (int k = 0; k < 4; k++) sh[k] = ImVec2(p[k].x, p[k].y + g * 2.5f);
        d->AddConvexPolyFilled(sh, 4, IM_COL32(14, 22, 80, 255));
        d->AddConvexPolyFilled(p, 4, b->marcado ? IM_COL32(78, 138, 240, 255) : IM_COL32(27, 42, 174, 255));
        ImVec2 alto[4] = {{b->x0 + sk, b->y0}, {b->x1, b->y0}, {b->x1 - sk / 2, ym}, {b->x0 + sk / 2, ym}};
        d->AddConvexPolyFilled(alto, 4, b->marcado ? IM_COL32(125, 180, 255, 255) : IM_COL32(59, 80, 224, 255));
        d->AddPolyline(p, 4, IM_COL32(159, 230, 255, 255), ImDrawFlags_Closed, g * 1.6f);
        float t = h * (b->tipo == 4 ? 0.85f : b->tipo == 8 ? 0.4f : 0.85f);
        ImVec2 tam = (usa_letra(t), letra_boton->CalcTextSizeA(usa_letra(t), 1e9f, 0, b->texto));
        float cabe = w - sk * 2 - h * (b->tipo == 1 ? 0.85f : 0.5f);  // que el texto quepa en el botón (lo que queda libre)
        while (tam.x > cabe && usa_letra(t) > 10) { t = usa_letra(t) - 10; tam = letra_boton->CalcTextSizeA(usa_letra(t), 1e9f, 0, b->texto); }
        float tx = b->tipo == 4 || b->tipo == 8 ? (b->x0 + b->x1 - tam.x) / 2 : b->x0 + sk + h * (b->tipo == 1 ? 0.8f : 0.45f);
        float ty = b->tipo == 8 ? b->y1 - h * 0.12f - tam.y : b->y0 + (h - tam.y) / 2;
        if (b->icono >= 1 && b->icono <= 3)  // flechas de velocidad (tantas como la categoría), centradas arriba
            for (int k = 0; k < b->icono; k++) {
                float a = h * 0.17f, paso = a * 1.5f;
                float ax = (b->x0 + b->x1) / 2 + (k - (b->icono - 1) / 2.0f) * paso, ay = b->y0 + h * 0.36f;
                ImVec2 v[6] = {{ax - a, ay - a}, {ax - a * 0.4f, ay - a}, {ax + a * 0.6f, ay}, {ax - a * 0.4f, ay + a}, {ax - a, ay + a}, {ax, ay}};
                d->AddTriangleFilled(v[0], v[1], v[5], oro); d->AddTriangleFilled(v[1], v[2], v[5], oro);
                d->AddTriangleFilled(v[5], v[2], v[3], oro); d->AddTriangleFilled(v[5], v[3], v[4], oro);
            }
        if (b->icono == 4) {  // espejo: dos flechas en sentidos contrarios, una sobre otra
            float ax = (b->x0 + b->x1) / 2, ay = b->y0 + h * 0.36f, a = h * 0.11f, l = h * 0.32f;
            d->AddRectFilled(ImVec2(ax - l + a, ay - a * 1.5f - a * 0.35f), ImVec2(ax + l, ay - a * 1.5f + a * 0.35f), oro);
            d->AddTriangleFilled(ImVec2(ax - l - a * 0.4f, ay - a * 1.5f), ImVec2(ax - l + a * 1.2f, ay - a * 2.6f), ImVec2(ax - l + a * 1.2f, ay - a * 0.4f), oro);
            d->AddRectFilled(ImVec2(ax - l, ay + a * 1.5f - a * 0.35f), ImVec2(ax + l - a, ay + a * 1.5f + a * 0.35f), oro);
            d->AddTriangleFilled(ImVec2(ax + l + a * 0.4f, ay + a * 1.5f), ImVec2(ax + l - a * 1.2f, ay + a * 0.4f), ImVec2(ax + l - a * 1.2f, ay + a * 2.6f), oro);
        }
        texto_contorno(d, t, ImVec2(tx, ty), b->marcado ? oro : IM_COL32(169, 220, 255, 255), b->texto, g);
    }
}

// La capa de botones propios, en su propia imagen de ImGui a baja resolución (la ventana la amplía sin suavizar: píxeles)
extern "C" void menu_pinta_botones(int ancho, int alto) {
    if (!listo || !nbotones_p || !letra_boton) return;
    ImGuiIO &io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)ancho, (float)alto);
    io.DisplayFramebufferScale = ImVec2(1, 1);
    io.DeltaTime = 1.0f / 60;
    float escala = io.FontGlobalScale;
    io.FontGlobalScale = 1;
    ImGuiStyle &st = ImGui::GetStyle();
    bool aal = st.AntiAliasedLines, aaf = st.AntiAliasedFill;
    st.AntiAliasedLines = st.AntiAliasedFill = false;
    IMPL_NEWFRAME();
    ImGui::NewFrame();
    pinta_botones_propios();
    ImGui::Render();
    IMPL_RENDER(ImGui::GetDrawData());
    st.AntiAliasedLines = aal; st.AntiAliasedFill = aaf;
    io.FontGlobalScale = escala;
}

extern "C" void menu_pinta(int ancho, int alto, int ancho_px, int alto_px) {
    if (!listo || ancho <= 0 || alto <= 0) return;
    if (!abierto && !aj.mostrar_fps && wfc_ping_ms() < 0 && !hay_aviso_sala()) return;
    ImGuiIO &io = ImGui::GetIO();
    static double antes;
    double ahora = reloj_s();
    io.DeltaTime = antes > 0 && ahora > antes ? (float)(ahora - antes) : 1.0f / 60;
    antes = ahora;
    io.DisplaySize = ImVec2((float)ancho, (float)alto);
    io.DisplayFramebufferScale = ImVec2((float)ancho_px / ancho, (float)alto_px / alto);
    float s = alto / 768.0f;  // todo crece con la ventana (768 = la ventana normal)
    if (s < 0.75f) s = 0.75f;
    io.FontGlobalScale = 21 * s / LETRA;
    vacia_cola();
    IMPL_NEWFRAME();
    ImGui::NewFrame();
    if (aj.mostrar_fps || wfc_ping_ms() >= 0) contador_fps((float)ancho, s);
    if (hay_aviso_sala()) aviso_sala((float)ancho, s);
    if (abierto) {
        ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(5, 6, 15, 165));
        float w = 640 * s, h = 560 * s;
        if (w > ancho - 20) w = (float)ancho - 20;
        if (h > alto - 20) h = (float)alto - 20;
        ImGui::SetNextWindowPos(ImVec2(ancho * 0.5f, alto * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
        ImGui::Begin("##menu", NULL, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::SetWindowFontScale(1.25f);
        ImGui::TextUnformatted("DS KART");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.30f, 0.24f, 1));
        ImGui::TextUnformatted("RECOMPILED");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextDisabled("·  %s", T("Ajustes"));
        ImGui::SetWindowFontScale(1);
        ImGui::Spacing();
        float pie = ImGui::GetFrameHeightWithSpacing() * 2.2f;
        if (ImGui::BeginTabBar("pestañas")) {
            if (pestaña(T("Gráficos"), 0)) { ImGui::BeginChild("g", ImVec2(0, -pie)); pestaña_graficos(); ImGui::EndChild(); ImGui::EndTabItem(); }
            if (pestaña(T("Mandos"), 1)) { ImGui::BeginChild("m", ImVec2(0, -pie)); pestaña_mandos(); ImGui::EndChild(); ImGui::EndTabItem(); }
            if (pestaña(T("Audio"), 2)) { ImGui::BeginChild("a", ImVec2(0, -pie)); pestaña_audio(); ImGui::EndChild(); ImGui::EndTabItem(); }
            if (pestaña(T("Juego"), 3)) { ImGui::BeginChild("j", ImVec2(0, -pie)); pestaña_juego(); ImGui::EndChild(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        pestaña_pedida = -1;
        ImGui::Separator();
        ayuda(T("Menú abierto: el juego no recibe los botones. F1 o L3 + R3 para volver."));
        ImGui::SetWindowFontScale(0.85f);
        ImGui::TextDisabled("%s", T("L / R: cambiar de pestaña"));
        ImGui::SetWindowFontScale(1);
        ImGui::SameLine(ImGui::GetWindowWidth() - 220 * s);
        if (ImGui::Button(T("Volver al juego"), ImVec2(200 * s, 0))) menu_alterna();
        ImGui::End();
    }
    ImGui::Render();
    IMPL_RENDER(ImGui::GetDrawData());
}
