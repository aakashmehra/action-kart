// iOS: las peticiones al servidor de salas del online (http.c en el resto: allí con curl, que en iOS no se puede
// lanzar). La misma función: POST, la respuesta en texto en 'r', devuelve el código HTTP o -1. Se llama desde el hilo
// del online, nunca desde el del juego: aquí se espera a la respuesta.
#import <Foundation/Foundation.h>
#include <string.h>

int http_pide(const char *url, const char *cuerpo, char *r, int max, int espera) {
    r[0] = 0;
    NSURL *u = [NSURL URLWithString:[NSString stringWithUTF8String:url]];
    if (!u) return -1;
    NSMutableURLRequest *q = [NSMutableURLRequest requestWithURL:u];
    q.HTTPMethod = @"POST";
    q.timeoutInterval = (espera + 999) / 1000 + 5;
    if (cuerpo) q.HTTPBody = [NSData dataWithBytes:cuerpo length:strlen(cuerpo)];
    __block int codigo = -1;
    __block NSData *datos = nil;
    dispatch_semaphore_t listo = dispatch_semaphore_create(0);
    [[[NSURLSession sharedSession] dataTaskWithRequest:q completionHandler:^(NSData *d, NSURLResponse *resp, NSError *e) {
        if (!e && [resp isKindOfClass:[NSHTTPURLResponse class]]) {
            codigo = (int)((NSHTTPURLResponse *)resp).statusCode;
            datos = d;
        }
        dispatch_semaphore_signal(listo);
    }] resume];
    dispatch_semaphore_wait(listo, DISPATCH_TIME_FOREVER);
    if (codigo < 0) return -1;
    size_t n = datos.length < (size_t)max - 1 ? datos.length : (size_t)max - 1;
    memcpy(r, datos.bytes, n);
    r[n] = 0;
    return codigo;
}
