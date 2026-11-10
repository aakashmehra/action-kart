// iOS (arm64): el cambio de fibra a mano (compat.h, COMPAT_FIBRA_ASM). ucontext no funciona en el iPhone de verdad.
// compat_cambia(&guardar, nueva): guarda x19-x30 y d8-d15 (lo que una función debe conservar) en la pila actual, apunta
// esa pila en *guardar, se pasa a la nueva y recupera los suyos. Una fibra nueva tiene preparado un marco con x19 = la
// fibra y x30 = compat_trampolin, que llama a su función.
#include "compat.h"
#ifdef COMPAT_FIBRA_ASM
void compat_fibra_empieza(CompatFibra *fb) {
    fb->f(fb->arg);
    exit(8);  // una fibra no debería terminar
}
__asm__(".text\n"
        ".p2align 2\n"
        ".globl _compat_cambia\n"
        "_compat_cambia:\n"
        "    sub sp, sp, #0xA0\n"
        "    stp x19, x20, [sp, #0x00]\n"
        "    stp x21, x22, [sp, #0x10]\n"
        "    stp x23, x24, [sp, #0x20]\n"
        "    stp x25, x26, [sp, #0x30]\n"
        "    stp x27, x28, [sp, #0x40]\n"
        "    stp x29, x30, [sp, #0x50]\n"
        "    stp d8, d9, [sp, #0x60]\n"
        "    stp d10, d11, [sp, #0x70]\n"
        "    stp d12, d13, [sp, #0x80]\n"
        "    stp d14, d15, [sp, #0x90]\n"
        "    mov x9, sp\n"
        "    str x9, [x0]\n"
        "    mov sp, x1\n"
        "    ldp x19, x20, [sp, #0x00]\n"
        "    ldp x21, x22, [sp, #0x10]\n"
        "    ldp x23, x24, [sp, #0x20]\n"
        "    ldp x25, x26, [sp, #0x30]\n"
        "    ldp x27, x28, [sp, #0x40]\n"
        "    ldp x29, x30, [sp, #0x50]\n"
        "    ldp d8, d9, [sp, #0x60]\n"
        "    ldp d10, d11, [sp, #0x70]\n"
        "    ldp d12, d13, [sp, #0x80]\n"
        "    ldp d14, d15, [sp, #0x90]\n"
        "    add sp, sp, #0xA0\n"
        "    ret\n"
        ".p2align 2\n"
        ".globl _compat_trampolin\n"
        "_compat_trampolin:\n"
        "    mov x0, x19\n"
        "    bl _compat_fibra_empieza\n"
        "    brk #0\n");
#endif
