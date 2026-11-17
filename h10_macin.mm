// OLA 10: raton y teclado de la ventana del Mac (P4_WINDOW=1) para el menu de ajustes original (h10_settings.cpp).
// Se compila aparte como t_window_win.mm (Cocoa choca con el prelude de compat). Los eventos se reciben con un monitor local
// durante WinPump (mismo hilo que el motor) y se pasan en fracciones 0..1 del contenido de la ventana.
#import <Cocoa/Cocoa.h>

typedef void (*H10Ev)(int kind, float fx, float fy, int key);
static H10Ev s_ev;
static id s_mon;

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
extern "C" void H10_MacInputInstall(H10Ev ev)
{
    s_ev = ev; if (s_mon) return;
    NSEventMask m = NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp | NSEventMaskScrollWheel | NSEventMaskKeyDown | NSEventMaskKeyUp;
    s_mon = [NSEvent addLocalMonitorForEventsMatchingMask:m handler:^NSEvent *(NSEvent *e) {
        float fx = 0, fy = 0;
        switch (e.type) {
        case NSEventTypeMouseMoved: case NSEventTypeLeftMouseDragged: Pos(e, &fx, &fy); s_ev(0, fx, fy, 0); break;
        case NSEventTypeLeftMouseDown: Pos(e, &fx, &fy); s_ev(1, fx, fy, 0); break;
        case NSEventTypeLeftMouseUp: Pos(e, &fx, &fy); s_ev(2, fx, fy, 0); break;
        case NSEventTypeScrollWheel: if (e.scrollingDeltaY != 0) s_ev(3, 0, 0, e.scrollingDeltaY > 0 ? 1 : -1); break;
        case NSEventTypeKeyDown: if (!e.isARepeat) s_ev(4, 0, 0, MapKey(e)); return nil;   // sin pitido
        case NSEventTypeKeyUp: s_ev(5, 0, 0, MapKey(e)); return nil;
        default: break;
        }
        return e;
    }];
    if ([NSApp keyWindow]) [[NSApp keyWindow] setAcceptsMouseMovedEvents:YES];
    for (NSWindow *w in [NSApp windows]) [w setAcceptsMouseMovedEvents:YES];
}
