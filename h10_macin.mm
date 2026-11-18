// OLA 10: raton y teclado de la ventana del Mac (P4_WINDOW=1) para el menu de ajustes original (h10_settings.cpp).
// Se compila aparte como t_window_win.mm (Cocoa choca con el prelude de compat). Los eventos se reciben con un monitor local
// durante WinPump (mismo hilo que el motor) y se pasan en fracciones 0..1 del contenido de la ventana.
// OLA P1: JUGAR A MANO en el Mac. Con H10_MANUAL=1 el mismo monitor guarda el estado de teclado y raton y se instala como proveedor
// de entrada del bucle de juego (g_h5pInputProvider, h5_input.h: la MISMA ruta que los toques de la app Android, que no cambia).
// Teclas por defecto del PC original: WASD, raton para mirar, clic izq. disparar, clic dcho. apuntar, R recargar, F usar,
// V/E cuchillo, Espacio saltar, C agacharse, Ctrl/Z tumbarse, Mayus correr, G granada, Q tactica, rueda/1/2/3 cambiar de arma,
// Esc menu de pausa (libera el raton), Tab marcador. Cerrar la ventana o Cmd+Q sale del juego. H10_SENS (grados por punto, 0.11
// como sensitivity 5 * m_yaw 0.022 del original) y H10_INVERT=1 invierten la vista vertical. "Restart level" relanza el proceso.
// H10_MANUAL=2 (pruebas automaticas, con P4_NOACTIVATE=1): acepta eventos sin foco y no captura el raton.
#import <Cocoa/Cocoa.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "h5_input.h"

typedef void (*H10Ev)(int kind, float fx, float fy, int key);
static H10Ev s_ev;
static id s_mon, s_closeObs;

// teclas del Mac -> codigos del UI (ui/keycodes.h)
static int MapKey(NSEvent *e)
{
    switch (e.keyCode) {
    case 53: return 0x1B;    // Esc -> K_ESCAPE
    case 36: case 76: return 0x0D;   // Enter
    case 126: return 0x9A; case 125: return 0x9B; case 123: return 0x9C; case 124: return 0x9D;   // flechas
    case 51: return 0x7F;    // borrar
    case 48: return 0x09;    // tab
    }
    NSString *c = e.charactersIgnoringModifiers; return c.length ? (int)[c characterAtIndex:0] : 0;
}
static void Pos(NSEvent *e, float *fx, float *fy)
{
    NSView *v = e.window.contentView; NSPoint p = e.locationInWindow; NSSize s = v.bounds.size;
    *fx = s.width > 0 ? (float)(p.x / s.width) : 0; *fy = s.height > 0 ? (float)(1.0 - p.y / s.height) : 0;
}

// ---------------------------------------------------------------- OLA P1: juego a mano
extern bool g_h10MenuOpen, g_h5pPause;   // h10_settings.cpp / h5_play.cpp
void H10_SaveNow();                      // h10_settings.cpp: guarda players/config.cfg
static bool s_manual, s_bg, s_key[128], s_lmb, s_rmb, s_shift, s_ctrl, s_capt;
static double s_mdx, s_mdy, s_lastWheel;
static int s_nextReq;                    // pulsaciones de cambio de arma pendientes
// codigos de tecla virtuales del Mac (posicion fisica: valen con cualquier distribucion)
enum { KC_A = 0, KC_S = 1, KC_D = 2, KC_F = 3, KC_G = 5, KC_Z = 6, KC_C = 8, KC_V = 9, KC_Q = 12, KC_W = 13, KC_E = 14, KC_R = 15,
       KC_1 = 18, KC_2 = 19, KC_3 = 20, KC_SPACE = 49 };

static void Quit(const char *why)
{
    fprintf(stderr, "[H10] %s: salir del juego\n", why); H10_SaveNow(); fflush(stderr); exit(0);
}
// "Restart level" del menu de pausa (h10_settings.cpp): el motor no es reentrante; como el cambio de mapa (h18_maps.cpp), mismo
// binario, argumentos y entorno con execv. Solo jugando a mano: las pruebas siguen sin efecto.
extern "C" bool H10_MacRelaunch()
{
    if (!s_manual) return false;
    CGAssociateMouseAndMouseCursorPosition(true); if (s_capt) [NSCursor unhide];
    NSArray<NSString *> *a = [NSProcessInfo processInfo].arguments; if (!a.count) return false;
    char **argv = (char **)calloc(a.count + 1, sizeof(char *));
    for (NSUInteger i = 0; i < a.count; ++i) argv[i] = strdup(a[i].UTF8String);
    const char *exe = [NSBundle mainBundle].executablePath.UTF8String;
    fprintf(stderr, "[H10] partida nueva: relanzo el proceso\n"); fflush(stderr);
    execv(exe ? exe : argv[0], argv);
    fprintf(stderr, "[H10] execv fallo\n"); return false;
}
static NSWindow *GameWindow() { NSWindow *w = [NSApp keyWindow]; if (!w && [NSApp windows].count) w = [NSApp windows][0]; return w; }
// captura del raton mientras se juega: cursor oculto y desligado (llegan solo los desplazamientos), centrado en la ventana
static void SetCapture(bool on)
{
    if (on == s_capt) return;
    s_capt = on;
    if (on) {
        NSWindow *w = GameWindow();
        if (w) { NSRect r = [w convertRectToScreen:w.contentView.frame]; CGFloat top = NSScreen.screens.count ? NSMaxY(NSScreen.screens[0].frame) : 0;
                 CGWarpMouseCursorPosition(CGPointMake(NSMidX(r), top - NSMidY(r))); }
        CGAssociateMouseAndMouseCursorPosition(false); [NSCursor hide];
    } else { CGAssociateMouseAndMouseCursorPosition(true); [NSCursor unhide]; }
    s_mdx = s_mdy = 0;
}
static void MacPlayInput(H5PlayerInput *in, float dt)
{
    (void)dt;
    memset(in, 0, sizeof *in);
    const bool active = s_bg || [NSApp isActive];   // H10_MANUAL=2: pruebas con eventos enviados al proceso sin foco
    const bool play = active && !g_h10MenuOpen && !g_h5pPause;
    if (!active) { memset(s_key, 0, sizeof s_key); s_lmb = s_rmb = s_shift = s_ctrl = false; }   // al perder el foco no se quedan teclas pegadas
    if (!s_bg) SetCapture(play);
    if (!play) { s_mdx = s_mdy = 0; s_nextReq = 0; return; }
    static const float sens = getenv("H10_SENS") ? (float)atof(getenv("H10_SENS")) : 0.11f;
    static const float inv = getenv("H10_INVERT") && atoi(getenv("H10_INVERT")) ? -1.0f : 1.0f;
    float f = (s_key[KC_W] ? 1.0f : 0.0f) - (s_key[KC_S] ? 1.0f : 0.0f), r = (s_key[KC_D] ? 1.0f : 0.0f) - (s_key[KC_A] ? 1.0f : 0.0f);
    if (f != 0 && r != 0) { f *= 0.70710678f; r *= 0.70710678f; }   // diagonal = stick a tope, no mas rapido
    in->moveFwd = f; in->moveRight = r;
    in->lookYaw = (float)(-s_mdx * sens); in->lookPitch = (float)(s_mdy * sens * inv);   // izquierda +, abajo +
    s_mdx = s_mdy = 0;
    unsigned b = 0;
    if (s_lmb) b |= H5IN_FIRE;
    if (s_rmb) b |= H5IN_ADS;
    if (s_key[KC_R]) b |= H5IN_RELOAD;
    if (s_key[KC_F]) b |= H5IN_USE;
    if (s_key[KC_V] || s_key[KC_E]) b |= H5IN_MELEE;
    if (s_key[KC_SPACE]) b |= H5IN_JUMP;
    if (s_key[KC_C]) b |= H5IN_STANCE;
    if (s_ctrl || s_key[KC_Z]) b |= H5IN_PRONE;
    if (s_shift) b |= H5IN_SPRINT;
    if (s_key[KC_G]) b |= H5IN_FRAG;
    if (s_key[KC_Q]) b |= H5IN_SMOKE;
    if (s_nextReq > 0) { b |= H5IN_NEXTWEAPON; --s_nextReq; }   // una pulsacion por usercmd (el bucle la retiene hasta el siguiente)
    in->buttons = b;
}
// estado bruto del teclado/raton (todas las teclas llegan, el menu las consume aparte por s_ev)
static void Track(NSEvent *e)
{
    switch (e.type) {
    case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged:
        s_mdx += e.deltaX; s_mdy += e.deltaY; break;
    case NSEventTypeLeftMouseDown: s_lmb = true; break;
    case NSEventTypeLeftMouseUp: s_lmb = false; break;
    case NSEventTypeRightMouseDown: s_rmb = true; break;
    case NSEventTypeRightMouseUp: s_rmb = false; break;
    case NSEventTypeScrollWheel: {
        const double now = e.timestamp;   // la rueda del trackpad manda muchos eventos: uno cada 0,15 s
        if (e.scrollingDeltaY != 0 && now - s_lastWheel > 0.15) { s_lastWheel = now; if (s_nextReq < 2) ++s_nextReq; }
        break; }
    case NSEventTypeKeyDown:
        if (e.keyCode < 128) { if (!e.isARepeat && (e.keyCode == KC_1 || e.keyCode == KC_2 || e.keyCode == KC_3) && s_nextReq < 2) ++s_nextReq; s_key[e.keyCode] = true; }
        break;
    case NSEventTypeKeyUp: if (e.keyCode < 128) s_key[e.keyCode] = false; break;
    case NSEventTypeFlagsChanged: s_shift = (e.modifierFlags & NSEventModifierFlagShift) != 0; s_ctrl = (e.modifierFlags & NSEventModifierFlagControl) != 0; break;
    default: break;
    }
}

extern "C" void H10_MacInputInstall(H10Ev ev)
{
    s_ev = ev; if (s_mon) return;
    s_manual = getenv("H10_MANUAL") && atoi(getenv("H10_MANUAL")); s_bg = s_manual && atoi(getenv("H10_MANUAL")) == 2;
    NSEventMask m = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskScrollWheel | NSEventMaskKeyDown | NSEventMaskKeyUp;
    if (s_manual) m |= NSEventMaskRightMouseDown | NSEventMaskRightMouseUp | NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged | NSEventMaskFlagsChanged;
    s_mon = [NSEvent addLocalMonitorForEventsMatchingMask:m handler:^NSEvent *(NSEvent *e) {
        float fx = 0, fy = 0;
        if (s_manual) {
            Track(e);
            if (e.type == NSEventTypeKeyDown && (e.modifierFlags & NSEventModifierFlagCommand) && e.keyCode == 12) Quit("Cmd+Q");   // Cmd+Q (sin barra de menus)
        }
        switch (e.type) {
        case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: Pos(e, &fx, &fy); s_ev(0, fx, fy, 0); break;
        case NSEventTypeLeftMouseDown: Pos(e, &fx, &fy); s_ev(1, fx, fy, 0); break;
        case NSEventTypeLeftMouseUp: Pos(e, &fx, &fy); s_ev(2, fx, fy, 0); break;
        case NSEventTypeScrollWheel: if (e.scrollingDeltaY != 0) s_ev(3, 0, 0, e.scrollingDeltaY > 0 ? 1 : -1); break;
        case NSEventTypeKeyDown: if (!e.isARepeat) s_ev(4, 0, 0, MapKey(e)); return nil;   // sin pitido
        case NSEventTypeKeyUp: s_ev(5, 0, 0, MapKey(e)); return nil;
        case NSEventTypeRightMouseDown: case NSEventTypeRightMouseUp: case NSEventTypeRightMouseDragged: case NSEventTypeOtherMouseDragged: case NSEventTypeFlagsChanged: return s_manual ? nil : e;
        default: break;
        }
        return e;
    }];
    if ([NSApp keyWindow]) [[NSApp keyWindow] setAcceptsMouseMovedEvents:YES];
    for (NSWindow *w in [NSApp windows]) [w setAcceptsMouseMovedEvents:YES];
    if (s_manual) {
        g_h5pInputProvider = MacPlayInput;   // antes de que H5P_Run elija el paso de tiempo real (se llama desde H10_Install)
        const char *map = getenv("H5_MAP");
        for (NSWindow *w in [NSApp windows]) [w setTitle:[NSString stringWithFormat:@"BO1 Zombies%s%s", map && *map ? " - " : "", map ? map : ""]];
        s_closeObs = [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:nil queue:nil usingBlock:^(NSNotification *) { Quit("ventana cerrada"); }];
        fprintf(stderr, "[H10] juego a mano: teclado y raton de la ventana (sensibilidad %.3f grados/punto)\n", getenv("H10_SENS") ? atof(getenv("H10_SENS")) : 0.11);
    }
}
