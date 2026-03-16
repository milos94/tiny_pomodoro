// Minimal, self-contained Timer implementation and a tiny X11 UI for the "raw" pomodoro.
// All code is intentionally placed in this single file.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

struct Timer {
    enum State {Stopped = 0, Running = 1, Paused = 2};

    explicit Timer(int seconds = 25 * 60)
        : initial_seconds(seconds), remaining_seconds(seconds), state(Stopped), stop_flag(false)
    {}

    ~Timer() {
        stop_flag = true;
        if (worker.joinable()) worker.join();
    }

    void start() {
        if (state == Running) return;
        state = Running;
        if (!worker.joinable()) worker = std::thread(&Timer::thread_func, this);
    }

    void pause() { state = Paused; }

    void reset() {
        remaining_seconds = initial_seconds;
        state = Stopped;
    }

    void set_duration(int seconds) {
        initial_seconds = seconds;
        remaining_seconds = seconds;
    }

    int seconds_left() const { return remaining_seconds; }
    bool is_running() const { return state == Running; }

private:
    void thread_func() {
        using namespace std::chrono_literals;
        while (!stop_flag) {
            if (state != Running) {
                std::this_thread::sleep_for(100ms);
                continue;
            }
            std::this_thread::sleep_for(1s);
            if (state != Running) continue;
            if (remaining_seconds > 0) {
                --remaining_seconds;
                if (remaining_seconds <= 0) {
                    remaining_seconds = 0;
                    state = Stopped;
                }
            }
        }
    }

    int initial_seconds;
    int remaining_seconds;
    State state;
    std::thread worker;
    bool stop_flag;
};

// Helper: format seconds as MM:SS
static std::string format_time(int seconds) {
    if (seconds < 0) seconds = 0;
    int m = seconds / 60;
    int s = seconds % 60;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02d:%02d", m, s);
    return std::string(buf);
}

int main() {
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        std::cerr << "Unable to open X display\n";
        return 1;
    }

    int screen = DefaultScreen(dpy);
    unsigned long black = BlackPixel(dpy, screen);
    unsigned long white = WhitePixel(dpy, screen);

    unsigned int win_w = 360, win_h = 180;
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 100, 100, win_w, win_h, 1, black, white);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmDelete, 1);
    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, nullptr);
    XFontStruct* font = XLoadQueryFont(dpy, "-*-helvetica-bold-r-*-*-24-*-*-*-*-*-*-*");
    if (!font) font = XLoadQueryFont(dpy, "fixed");
    if (font) XSetFont(dpy, gc, font->fid);

    // Buttons
    struct Button { int x,y; unsigned int w,h; std::string label; };
    Button btn_start{20, 110, 120, 40, "Start"};
    Button btn_reset{160, 110, 120, 40, "Reset"};

        // Duration controls (minutes)
        int focus_min = 25;
        int short_min = 5;
        int long_min = 15;

        // layout small +/- buttons for each duration
        Button focus_minus{20, 10, 24, 24, "-"};
        Button focus_plus{64, 10, 24, 24, "+"};
        Button short_minus{140, 10, 24, 24, "-"};
        Button short_plus{184, 10, 24, 24, "+"};
        Button long_minus{260, 10, 24, 24, "-"};
        Button long_plus{304, 10, 24, 24, "+"};

        Timer timer(focus_min*60);

    bool running = true;

    auto draw = [&](int last_seconds){
        XClearWindow(dpy, win);

        // draw time
        std::string timestr = format_time(last_seconds);
        int tx = 20;
        int ty = 60;
        XSetForeground(dpy, gc, black);
        XDrawString(dpy, win, gc, tx, ty, timestr.c_str(), (int)timestr.size());

            // draw duration labels
            XSetForeground(dpy, gc, black);
            std::string fstr = "Focus: " + std::to_string(focus_min) + "m";
            std::string sstr = "Short: " + std::to_string(short_min) + "m";
            std::string lstr = "Long: " + std::to_string(long_min) + "m";
            XDrawString(dpy, win, gc, 20, 30, fstr.c_str(), (int)fstr.size());
            XDrawString(dpy, win, gc, 140, 30, sstr.c_str(), (int)sstr.size());
            XDrawString(dpy, win, gc, 260, 30, lstr.c_str(), (int)lstr.size());
            // draw small +/- buttons
            unsigned long gray = (white ^ black) & 0x00cccccc; // best-effort
            XSetForeground(dpy, gc, gray);
            XFillRectangle(dpy, win, gc, focus_minus.x, focus_minus.y, focus_minus.w, focus_minus.h);
            XFillRectangle(dpy, win, gc, focus_plus.x, focus_plus.y, focus_plus.w, focus_plus.h);
            XFillRectangle(dpy, win, gc, short_minus.x, short_minus.y, short_minus.w, short_minus.h);
            XFillRectangle(dpy, win, gc, short_plus.x, short_plus.y, short_plus.w, short_plus.h);
            XFillRectangle(dpy, win, gc, long_minus.x, long_minus.y, long_minus.w, long_minus.h);
            XFillRectangle(dpy, win, gc, long_plus.x, long_plus.y, long_plus.w, long_plus.h);

            XSetForeground(dpy, gc, black);
            XDrawRectangle(dpy, win, gc, focus_minus.x, focus_minus.y, focus_minus.w, focus_minus.h);
            XDrawRectangle(dpy, win, gc, focus_plus.x, focus_plus.y, focus_plus.w, focus_plus.h);
            XDrawRectangle(dpy, win, gc, short_minus.x, short_minus.y, short_minus.w, short_minus.h);
            XDrawRectangle(dpy, win, gc, short_plus.x, short_plus.y, short_plus.w, short_plus.h);
            XDrawRectangle(dpy, win, gc, long_minus.x, long_minus.y, long_minus.w, long_minus.h);
            XDrawRectangle(dpy, win, gc, long_plus.x, long_plus.y, long_plus.w, long_plus.h);

            // small labels
            auto draw_small = [&](const Button& b){
                int lx = b.x + 6;
                int ly = static_cast<int>(static_cast<unsigned int>(b.y) + b.h/2 + 5);
                XDrawString(dpy, win, gc, lx, ly, b.label.c_str(), (int)b.label.size());
            };
            draw_small(focus_minus); draw_small(focus_plus);
            draw_small(short_minus); draw_small(short_plus);
            draw_small(long_minus); draw_small(long_plus);

        // draw buttons (filled rectangles)
        XSetForeground(dpy, gc, gray);
        XFillRectangle(dpy, win, gc, btn_start.x, btn_start.y, btn_start.w, btn_start.h);
        XFillRectangle(dpy, win, gc, btn_reset.x, btn_reset.y, btn_reset.w, btn_reset.h);

        // button borders and labels
        XSetForeground(dpy, gc, black);
        XDrawRectangle(dpy, win, gc, btn_start.x, btn_start.y, btn_start.w, btn_start.h);
        XDrawRectangle(dpy, win, gc, btn_reset.x, btn_reset.y, btn_reset.w, btn_reset.h);

        auto draw_label = [&](const Button& b){
            int lx = b.x + 10;
            int ly = static_cast<int>(static_cast<unsigned int>(b.y) + b.h/2 + 5);
            XDrawString(dpy, win, gc, lx, ly, b.label.c_str(), (int)b.label.size());
        };

        btn_start.label = timer.is_running() ? "Pause" : "Start";
        draw_label(btn_start);
        draw_label(btn_reset);
    };

    int last_seconds = timer.seconds_left();
    draw(last_seconds);

    while (running) {
        while (XPending(dpy) > 0) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == Expose) {
                draw(timer.seconds_left());
            } else if (ev.type == ClientMessage) {
                if ((Atom)ev.xclient.data.l[0] == wmDelete) {
                    running = false;
                }
            } else if (ev.type == ButtonPress) {
                int mx = ev.xbutton.x;
                int my = ev.xbutton.y;
                auto hit = [&](const Button& b){
                    return  mx >= b.x && 
                            mx <= b.x + static_cast<int>(b.w) &&
                            my >= b.y &&
                            my <= b.y + static_cast<int>(b.h);
                };
                // duration +/- buttons
                if (hit(focus_minus)) {
                    focus_min = std::max(1, focus_min - 1);
                    if (!timer.is_running()) timer.set_duration(focus_min * 60);
                    draw(timer.seconds_left());
                } else if (hit(focus_plus)) {
                    focus_min = std::min(240, focus_min + 1);
                    if (!timer.is_running()) timer.set_duration(focus_min * 60);
                    draw(timer.seconds_left());
                } else if (hit(short_minus)) {
                    short_min = std::max(1, short_min - 1);
                    draw(timer.seconds_left());
                } else if (hit(short_plus)) {
                    short_min = std::min(120, short_min + 1);
                    draw(timer.seconds_left());
                } else if (hit(long_minus)) {
                    long_min = std::max(1, long_min - 1);
                    draw(timer.seconds_left());
                } else if (hit(long_plus)) {
                    long_min = std::min(240, long_min + 1);
                    draw(timer.seconds_left());
                } else if (hit(btn_start)) {
                    if (timer.is_running()) timer.pause(); else timer.start();
                    draw(timer.seconds_left());
                } else if (hit(btn_reset)) {
                    timer.reset();
                    draw(timer.seconds_left());
                }
            } else if (ev.type == KeyPress) {
                char buf[16];
                KeySym keysym;
                int len = XLookupString(&ev.xkey, buf, sizeof(buf), &keysym, nullptr);
                if (len > 0) {
                    if (buf[0] == 'q' || buf[0] == 'Q' || buf[0] == '\x1b') {
                        running = false;
                    }
                }
            }
        }

        int cur = timer.seconds_left();
        if (cur != last_seconds) {
            last_seconds = cur;
            draw(last_seconds);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    if (font) XFreeFont(dpy, font);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    return 0;
}
