/* main_raw.cpp – Pomodoro timer using only Xlib, no external libraries.
   Same functionality as main_fltk.cpp; optimised for small binary size. */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <sys/select.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const int WIN_W = 320, WIN_H = 420;

/* ── X globals ────────────────────────────────────────────────────────────── */
static Display*    dpy;
static Window      win;
static GC          gc;
static XFontStruct *fn, *fn_big;   /* regular and large (timer) font */
static Atom        atom_wm_del;

/* ── app state ────────────────────────────────────────────────────────────── */
static int    cur_session  = 0;    /* 0=Focus  1=Short Break  2=Long Break */
static int    focus_count  = 0;    /* completed focus sessions (for long-break cycle) */
static int    running      = 0;
static int    secs_left   = 25 * 60;
static time_t end_time    = 0;

/* editable duration inputs – [0]=focus [1]=short [2]=long */
static char ib[3][8] = {"25", "5", "15"};   /* input buffers */
static int  il[3]    = {2, 1, 2};           /* buffer lengths */
static int  ifocus   = -1;                  /* focused input index, -1=none */

/* ── helpers ──────────────────────────────────────────────────────────────── */

static const int DEF_MINS[3] = {25, 5, 15};

static int get_mins(int i) {
    int v = il[i] ? atoi(ib[i]) : 0;
    return v > 0 ? v : DEF_MINS[i];
}

static int in_rect(int px, int py, int rx, int ry, int rw, int rh) {
    return px >= rx && px < rx + rw && py >= ry && py < ry + rh;
}

static void stop_timer(void) { running = 0; end_time = 0; }

static void reset_session(int s) {
    stop_timer();
    cur_session = s;
    secs_left   = get_mins(s) * 60;
}

/* ── drawing ──────────────────────────────────────────────────────────────── */

static void draw_button(int x, int y, int w, int h,
                        const char* lbl, unsigned long bg) {
    XSetForeground(dpy, gc, bg);
    XFillRectangle(dpy, win, gc, x, y, (unsigned)w, (unsigned)h);
    XSetForeground(dpy, gc, 0x999999);
    XDrawRectangle(dpy, win, gc, x, y, (unsigned)(w - 1), (unsigned)(h - 1));
    XSetForeground(dpy, gc, 0x000000);
    XSetFont(dpy, gc, fn->fid);
    int lw = XTextWidth(fn, lbl, (int)strlen(lbl));
    int ty = y + (h + fn->ascent - fn->descent) / 2;
    XDrawString(dpy, win, gc, x + (w - lw) / 2, ty, lbl, (int)strlen(lbl));
}

static void draw_input(int i) {
    int x = 138, y = 288 + i * 34, iw = 60, ih = 24;
    XSetForeground(dpy, gc, 0xFFFFFF);
    XFillRectangle(dpy, win, gc, x, y, (unsigned)iw, (unsigned)ih);
    XSetForeground(dpy, gc, ifocus == i ? 0x0044DD : 0x999999);
    XDrawRectangle(dpy, win, gc, x, y, (unsigned)(iw - 1), (unsigned)(ih - 1));
    XSetForeground(dpy, gc, 0x000000);
    XSetFont(dpy, gc, fn->fid);
    int tw = XTextWidth(fn, ib[i], il[i]);
    int ty = y + (ih + fn->ascent - fn->descent) / 2;
    XDrawString(dpy, win, gc, x + (iw - tw) / 2, ty, ib[i], il[i]);
}

static void redraw(void) {
    static const char* snames[] = {"Focus", "Short Break", "Long Break"};
    static const int   sx[]     = {14, 116, 218};
    static const char* rlbl[]   = {"Focus:", "Short Break:", "Long Break:"};

    /* background */
    XSetForeground(dpy, gc, 0xFFFFFF);
    XFillRectangle(dpy, win, gc, 0, 0, WIN_W, WIN_H);

    /* session label – centred in the 18px-tall band at the top */
    XSetForeground(dpy, gc, 0x000000);
    XSetFont(dpy, gc, fn->fid);
    {
        const char* s = snames[cur_session];
        int sw = XTextWidth(fn, s, (int)strlen(s));
        XDrawString(dpy, win, gc, (WIN_W - sw) / 2, 18 + fn->ascent,
                    s, (int)strlen(s));
    }

    /* timer – large font, centred in the 90 px band starting at y=54 */
    {
        char buf[6];
        snprintf(buf, sizeof(buf), "%02d:%02d", secs_left / 60, secs_left % 60);
        XSetFont(dpy, gc, fn_big->fid);
        int tw = XTextWidth(fn_big, buf, 5);
        int ty = 54 + (90 + fn_big->ascent - fn_big->descent) / 2;
        XDrawString(dpy, win, gc, (WIN_W - tw) / 2, ty, buf, 5);
    }

    /* session selector buttons (y=158, h=28) */
    for (int i = 0; i < 3; i++)
        draw_button(sx[i], 158, 88, 28, snames[i],
                    cur_session == i ? 0xCCCCCC : 0xE8E8E8);

    /* start/stop button (y=202, h=36) */
    draw_button(110, 202, 100, 36, running ? "Stop" : "Start", 0xE8E8E8);

    /* separator label */
    {
        static const char sep[] = "Durations (minutes)";
        XSetForeground(dpy, gc, 0x777777);
        XSetFont(dpy, gc, fn->fid);
        int tw = XTextWidth(fn, sep, (int)(sizeof(sep) - 1));
        XDrawString(dpy, win, gc, (WIN_W - tw) / 2, 258 + fn->ascent,
                    sep, (int)(sizeof(sep) - 1));
        XSetForeground(dpy, gc, 0x000000);
    }

    /* duration input rows */
    for (int i = 0; i < 3; i++) {
        const char* lbl = rlbl[i];
        int lw = XTextWidth(fn, lbl, (int)strlen(lbl));
        int iy = 288 + i * 34;
        int ty = iy + (24 + fn->ascent - fn->descent) / 2;
        XSetFont(dpy, gc, fn->fid);
        /* right-align label with right edge at x=132 */
        XDrawString(dpy, win, gc, 132 - lw, ty, lbl, (int)strlen(lbl));
        draw_input(i);
    }

    XFlush(dpy);
}

/* ── event handlers ───────────────────────────────────────────────────────── */

static void on_click(int x, int y) {
    static const int sx[] = {14, 116, 218};

    /* session selector buttons */
    for (int i = 0; i < 3; i++) {
        if (in_rect(x, y, sx[i], 158, 88, 28)) {
            reset_session(i);
            ifocus = -1;
            redraw();
            return;
        }
    }

    /* start/stop button */
    if (in_rect(x, y, 110, 202, 100, 36)) {
        if (running) {
            stop_timer();
        } else {
            if (secs_left == 0) reset_session(cur_session);
            running  = 1;
            end_time = time(NULL) + secs_left;
        }
        ifocus = -1;
        redraw();
        return;
    }

    /* click on a duration input field */
    ifocus = -1;
    for (int i = 0; i < 3; i++) {
        if (in_rect(x, y, 138, 288 + i * 34, 60, 24)) {
            ifocus = i;
            break;
        }
    }
    redraw();
}

static void on_key(KeySym ks, const char* txt) {
    if (ifocus < 0) return;
    int i = ifocus;
    if (ks == XK_BackSpace) {
        if (il[i] > 0) ib[i][--il[i]] = '\0';
    } else if (ks == XK_Tab || ks == XK_Return) {
        ifocus = (ifocus + 1) % 3;
    } else if (txt[0] >= '0' && txt[0] <= '9' && il[i] < 3) {
        ib[i][il[i]++] = txt[0];
        ib[i][il[i]]   = '\0';
    }
    redraw();
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void) {
    dpy = XOpenDisplay(NULL);
    if (!dpy) return 1;

    int scr = DefaultScreen(dpy);
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr),
                              100, 100, WIN_W, WIN_H, 0,
                              BlackPixel(dpy, scr), WhitePixel(dpy, scr));
    XStoreName(dpy, win, "Tiny Pomodoro");

    /* prevent resizing */
    XSizeHints sh = {};
    sh.flags      = PMinSize | PMaxSize;
    sh.min_width  = sh.max_width  = WIN_W;
    sh.min_height = sh.max_height = WIN_H;
    XSetWMNormalHints(dpy, win, &sh);

    /* handle window-close from the WM */
    atom_wm_del = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &atom_wm_del, 1);

    XSelectInput(dpy, win, ExposureMask | ButtonPressMask | KeyPressMask);

    /* load fonts – prefer larger variants, fall back gracefully */
    fn_big = XLoadQueryFont(dpy, "12x24");
    if (!fn_big) fn_big = XLoadQueryFont(dpy, "10x20");
    fn     = XLoadQueryFont(dpy, "9x15");
    if (!fn)     fn     = XLoadQueryFont(dpy, "fixed");
    if (!fn_big) fn_big = fn;

    gc = XCreateGC(dpy, win, 0, NULL);

    XMapWindow(dpy, win);
    XFlush(dpy);
    reset_session(0);

    int fd   = ConnectionNumber(dpy);
    int quit = 0;

    while (!quit) {
        /* block (or poll with 100 ms timeout when running) until X events arrive */
        if (!XPending(dpy)) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd, &fds);
            if (running) {
                struct timeval tv = {0, 100000};   /* 100 ms */
                select(fd + 1, &fds, NULL, NULL, &tv);
            } else {
                select(fd + 1, &fds, NULL, NULL, NULL);   /* block */
            }
        }

        /* advance the countdown using wall-clock time for accuracy */
        if (running) {
            int nl = (int)(end_time - time(NULL));
            if (nl < 0) nl = 0;
            if (nl != secs_left) {
                secs_left = nl;
                if (!secs_left) {
                    XBell(dpy, 100);
                    /* auto-advance: Focus→Break, Break→Focus */
                    int next;
                    if (cur_session == 0) {
                        focus_count++;
                        next = (focus_count % 4 == 0) ? 2 : 1;
                    } else {
                        next = 0;
                    }
                    reset_session(next);
                    running  = 1;
                    end_time = time(NULL) + secs_left;
                }
                redraw();
            }
        }

        /* drain the pending event queue */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose && ev.xexpose.count == 0) {
                redraw();
            } else if (ev.type == ButtonPress && ev.xbutton.button == Button1) {
                on_click(ev.xbutton.x, ev.xbutton.y);
            } else if (ev.type == KeyPress) {
                char   txt[4] = {0};
                KeySym ks     = 0;
                XLookupString(&ev.xkey, txt, sizeof(txt), &ks, NULL);
                on_key(ks, txt);
            } else if (ev.type == ClientMessage &&
                       (Atom)ev.xclient.data.l[0] == atom_wm_del) {
                quit = 1;
            }
        }
    }

    XFreeFont(dpy, fn);
    if (fn_big != fn) XFreeFont(dpy, fn_big);
    XFreeGC(dpy, gc);
    XCloseDisplay(dpy);
    return 0;
}
