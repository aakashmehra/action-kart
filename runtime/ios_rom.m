// iOS: pedir la ROM al abrir la app por primera vez (como Delta). La app no la lleva dentro: se elige con el selector
// de archivos de iOS (Archivos, iCloud...) y se copia a Documents/rom/mkds.nds. Tiene que ser la misma con la que se
// construyó la app (ROM_CRC, lo pone ios/construir.sh): el código del juego ya va traducido de ella.
// Solo se compila para iOS (ios/construir.sh); en el Mac la ROM va en rom/mkds.nds.
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <SDL2/SDL.h>
#include <zlib.h>
#include <stdio.h>

int skyendo_pinta(SDL_Renderer *r, int ancho, int alto, uint32_t ms);

static volatile int listo;  // 0 esperando, 1 copiada, -1 cancelado o fallo

@interface Elegidor : NSObject <UIDocumentPickerDelegate>
@end
@implementation Elegidor
- (void)documentPicker:(UIDocumentPickerViewController *)c didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *u = urls.firstObject;
    BOOL acceso = [u startAccessingSecurityScopedResource];
    NSString *dst = [NSHomeDirectory() stringByAppendingPathComponent:@"Documents/rom/mkds.nds"];
    [[NSFileManager defaultManager] removeItemAtPath:dst error:nil];
    NSError *e = nil;
    [[NSFileManager defaultManager] copyItemAtURL:u toURL:[NSURL fileURLWithPath:dst] error:&e];
    if (acceso) [u stopAccessingSecurityScopedResource];
    listo = e ? -1 : 1;
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController *)c {
    listo = -1;
}
@end

static uint32_t crc_archivo(const char *ruta) {
    FILE *f = fopen(ruta, "rb");
    if (!f) return 0;
    static unsigned char buf[1 << 16];
    uLong c = crc32(0L, Z_NULL, 0);
    for (size_t n; (n = fread(buf, 1, sizeof buf, f)) > 0;) c = crc32(c, buf, (uInt)n);
    fclose(f);
    return (uint32_t)c;
}

static int pregunta(const char *titulo, const char *texto, const char *boton) {
    SDL_MessageBoxButtonData b[1] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, boton}};
    SDL_MessageBoxData m = {SDL_MESSAGEBOX_INFORMATION, NULL, titulo, texto, 1, b, NULL};
    int r = 0;
    SDL_ShowMessageBox(&m, &r);
    return r;
}

// Hasta que haya en Documents/rom/mkds.nds una ROM que valga (ve_a_la_carpeta_del_juego ya ha hecho chdir a Documents)
void ios_pide_rom(void) {
#ifdef ROM_CRC
    FILE *hay = fopen("rom/mkds.nds", "rb");
    if (hay) { fclose(hay); return; }  // (ya elegida otra vez: comprobada al copiarla)
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window *v = SDL_CreateWindow("Action Kart", 0, 0, 0, 0, SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_Renderer *r = SDL_CreateRenderer(v, -1, SDL_RENDERER_ACCELERATED);
    static Elegidor *el;
    el = [Elegidor new];
    const char *aviso = "To start, choose your own copy of the game: a Mario Kart DS (Europe) ROM, the same one this "
                        "app was built from. It is copied into the app and never leaves your device.";
    for (;;) {
        int w, h;
        SDL_GetRendererOutputSize(r, &w, &h);  // en píxeles (la pantalla del iPhone es de alta resolución)
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        skyendo_pinta(r, w, h, 1000);  // de fondo, la portada (sin fundidos)
        SDL_RenderPresent(r);
        pregunta("Welcome to Action Kart", aviso, "Choose ROM");
        listo = 0;
        UIDocumentPickerViewController *p =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeData ] asCopy:YES];
        p.delegate = el;
        UIViewController *vc = nil;
        for (UIScene *s in UIApplication.sharedApplication.connectedScenes)
            if ([s isKindOfClass:[UIWindowScene class]])
                for (UIWindow *win in ((UIWindowScene *)s).windows)
                    if (win.isKeyWindow) vc = win.rootViewController;
        if (!vc) vc = UIApplication.sharedApplication.windows.firstObject.rootViewController;
        [vc presentViewController:p animated:YES completion:nil];
        while (!listo) {
            SDL_PumpEvents();
            [[NSRunLoop currentRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
        if (listo < 0) continue;
        if (crc_archivo("rom/mkds.nds") == (uint32_t)ROM_CRC) break;
        remove("rom/mkds.nds");
        aviso = "That file is not the ROM this app was built from. Choose the same Mario Kart DS (Europe) ROM that was "
                "used to build it.";
    }
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(v);
#endif
}
