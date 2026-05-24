// Minimal, self-contained Timer implementation and a tiny X11 UI for the "raw" pomodoro.
// All code is intentionally placed in this single file.

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <alsa/asoundlib.h>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstring>
#include <iostream>
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

// Synthesised fallback beep
static void play_beep(double freq = 880.0, int duration_ms = 200) {
    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) return;
    snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                       1, 44100, 1, 50000);
    const int frames = 44100 * duration_ms / 1000;
    std::vector<short> buf(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; i++)
        buf[static_cast<std::size_t>(i)] = static_cast<short>(32767.0 * std::sin(2.0 * M_PI * freq * i / 44100.0)
                                    * (1.0 - static_cast<double>(i) / frames));
    snd_pcm_writei(pcm, buf.data(), static_cast<snd_pcm_uframes_t>(frames));
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
}

// WAV helpers (little-endian reads)
static uint16_t wav_u16(FILE* f) {
    unsigned char b[2]; std::fread(b, 1, 2, f);
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
}
static uint32_t wav_u32(FILE* f) {
    unsigned char b[4]; std::fread(b, 1, 4, f);
    return static_cast<uint32_t>(b[0] | (b[1]<<8) | (b[2]<<16) | (b[3]<<24));
}
// Play a WAV file; falls back to synthesised beep if the file can't be loaded
static void play_wav(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { play_beep(); return; }

    char tag[4];
    if (std::fread(tag, 1, 4, f) != 4 || std::strncmp(tag, "RIFF", 4) != 0)
        { std::fclose(f); play_beep(); return; }
    wav_u32(f); // file size
    if (std::fread(tag, 1, 4, f) != 4 || std::strncmp(tag, "WAVE", 4) != 0)
        { std::fclose(f); play_beep(); return; }

    uint16_t channels = 1, bits = 16;
    uint32_t sample_rate = 44100, data_size = 0;
    bool got_fmt = false, got_data = false;
    while (std::fread(tag, 1, 4, f) == 4) {
        uint32_t sz = wav_u32(f);
        if (std::strncmp(tag, "fmt ", 4) == 0) {
            uint16_t audio_fmt = wav_u16(f);
            channels    = wav_u16(f);
            sample_rate = wav_u32(f);
            wav_u32(f); wav_u16(f); // byte rate, block align
            bits        = wav_u16(f);
            if (sz > 16) std::fseek(f, static_cast<long>(sz - 16), SEEK_CUR);
            got_fmt = (audio_fmt == 1);
        } else if (std::strncmp(tag, "data", 4) == 0) {
            data_size = sz; got_data = true; break;
        } else {
            std::fseek(f, static_cast<long>(sz), SEEK_CUR);
        }
    }
    if (!got_fmt || !got_data || data_size == 0)
        { std::fclose(f); play_beep(); return; }

    void* pcm_buf = std::malloc(data_size);
    if (!pcm_buf) { std::fclose(f); play_beep(); return; }
    uint32_t read_bytes = static_cast<uint32_t>(std::fread(pcm_buf, 1, data_size, f));
    std::fclose(f);

    snd_pcm_t* pcm = nullptr;
    if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
        { std::free(pcm_buf); play_beep(); return; }
    snd_pcm_format_t fmt = (bits == 8) ? SND_PCM_FORMAT_U8 : SND_PCM_FORMAT_S16_LE;
    snd_pcm_set_params(pcm, fmt, SND_PCM_ACCESS_RW_INTERLEAVED,
                       channels, sample_rate, 1, 50000);
    snd_pcm_writei(pcm, pcm_buf, read_bytes / (channels * (bits / 8)));
    snd_pcm_drain(pcm);
    snd_pcm_close(pcm);
    std::free(pcm_buf);
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

    // Layout:
    //   y=6..26:   session name  (small font, centred)
    //   y=30..82:  timer MM:SS   (big font, centred)
    //   y=90:      separator line
    //   y=92..110: duration column headers
    //   y=117..139: duration +/- controls (3 columns × 120 px)
    //   y=152..186: Start/Pause + Reset buttons
    const unsigned int win_w = 360, win_h = 210;
    Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 100, 100, win_w, win_h, 1, black, white);

    XSelectInput(dpy, win, ExposureMask | KeyPressMask | ButtonPressMask | StructureNotifyMask);
    Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wmDelete, 1);
    XClassHint* class_hint = XAllocClassHint();
    if (class_hint) {
        class_hint->res_name  = const_cast<char*>("tiny_pomodoro_pro");
        class_hint->res_class = const_cast<char*>("TinyPomodoroPro");
        XSetClassHint(dpy, win, class_hint);
        XFree(class_hint);
    }
    XMapWindow(dpy, win);

    GC gc = XCreateGC(dpy, win, 0, nullptr);

    // Big font for the timer, small font for everything else
    XFontStruct* font_big = XLoadQueryFont(dpy, "-*-helvetica-bold-r-*-*-24-*-*-*-*-*-*-*");
    if (!font_big) font_big = XLoadQueryFont(dpy, "fixed");
    XFontStruct* font_small = XLoadQueryFont(dpy, "-*-helvetica-medium-r-*-*-12-*-*-*-*-*-*-*");
    if (!font_small) font_small = XLoadQueryFont(dpy, "fixed");
    if (!font_small) font_small = font_big;

    // Proper gray via colormap (falls back to white)
    unsigned long gray = white;
    {
        XColor xcol{};
        if (XAllocNamedColor(dpy, DefaultColormap(dpy, screen), "gray85", &xcol, &xcol))
            gray = xcol.pixel;
    }

    struct Button { int x, y; unsigned int w, h; std::string label; };

    // Duration controls: 3 columns of 120 px
    // Within each column: [-22-][gap][val 28][gap][+22+], centred → [-] at col+24, [+] at col+74
    int focus_min = 25, short_min = 5, long_min = 15;

    Button focus_minus{  24, 117, 22, 22, "-"};
    Button focus_plus {  74, 117, 22, 22, "+"};
    Button short_minus{ 144, 117, 22, 22, "-"};
    Button short_plus { 194, 117, 22, 22, "+"};
    Button long_minus { 264, 117, 22, 22, "-"};
    Button long_plus  { 314, 117, 22, 22, "+"};

    // Action buttons: two 110×34 buttons centred in 360 px with 20 px gap
    Button btn_start{60,  152, 110, 34, "Start"};
    Button btn_reset{190, 152, 110, 34, "Reset"};

    enum SessionType { Focus, ShortBreak, LongBreak };
    SessionType session = Focus;
    int pomodoro_count = 0;

    Timer timer(focus_min * 60);
    bool running = true;

    // Draw text centred horizontally and vertically inside a bounding box
    auto draw_centered = [&](XFontStruct* fnt, int bx, int by, unsigned int bw, unsigned int bh,
                             const char* text) {
        if (!fnt || !text) return;
        int len = (int)std::strlen(text);
        int tw  = XTextWidth(fnt, text, len);
        int tx  = bx + ((int)bw - tw) / 2;
        int ty  = by + (int)bh / 2 + (fnt->ascent - fnt->descent) / 2;
        XSetFont(dpy, gc, fnt->fid);
        XDrawString(dpy, win, gc, tx, ty, text, len);
    };

    auto draw = [&](int secs) {
        XClearWindow(dpy, win);
        XSetForeground(dpy, gc, black);

        // ── Session name (centred at top) ─────────────────────────────────
        const char* sess_name = (session == Focus)      ? "Focus"       :
                                (session == ShortBreak) ? "Short Break" : "Long Break";
        draw_centered(font_small, 0, 6, win_w, 20, sess_name);

        // ── Timer MM:SS (centred, big font) ───────────────────────────────
        std::string timestr = format_time(secs);
        draw_centered(font_big, 0, 30, win_w, 52, timestr.c_str());

        // ── Separator ─────────────────────────────────────────────────────
        XDrawLine(dpy, win, gc, 10, 90, (int)win_w - 10, 90);

        // ── Duration column headers ───────────────────────────────────────
        draw_centered(font_small,   0, 92, 120, 18, "Focus");
        draw_centered(font_small, 120, 92, 120, 18, "Short");
        draw_centered(font_small, 240, 92, 120, 18, "Long");

        // ── Duration +/- buttons and values ──────────────────────────────
        auto draw_btn = [&](const Button& b) {
            XSetForeground(dpy, gc, gray);
            XFillRectangle(dpy, win, gc, b.x, b.y, b.w, b.h);
            XSetForeground(dpy, gc, black);
            XDrawRectangle(dpy, win, gc, b.x, b.y, b.w, b.h);
            draw_centered(font_small, b.x, b.y, b.w, b.h, b.label.c_str());
        };
        draw_btn(focus_minus); draw_btn(focus_plus);
        draw_btn(short_minus); draw_btn(short_plus);
        draw_btn(long_minus);  draw_btn(long_plus);

        // Values in the gap between each pair of buttons (col_base+46, width 28)
        XSetForeground(dpy, gc, black);
        draw_centered(font_small,  46, 117, 28, 22, std::to_string(focus_min).c_str());
        draw_centered(font_small, 166, 117, 28, 22, std::to_string(short_min).c_str());
        draw_centered(font_small, 286, 117, 28, 22, std::to_string(long_min).c_str());

        // ── Action buttons ────────────────────────────────────────────────
        btn_start.label = timer.is_running() ? "Pause" : "Start";
        draw_btn(btn_start);
        draw_btn(btn_reset);
    };

    auto advance_session = [&]() {
        XBell(dpy, 100);
        play_wav(SOUND_PATH);
        if (session == Focus) {
            pomodoro_count++;
            if (pomodoro_count % 4 == 0) {
                session = LongBreak;
                timer.set_duration(long_min * 60);
            } else {
                session = ShortBreak;
                timer.set_duration(short_min * 60);
            }
        } else {
            session = Focus;
            timer.set_duration(focus_min * 60);
        }
        timer.start();
    };

    int last_seconds = timer.seconds_left();
    bool was_running = false;
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
        if (was_running && !timer.is_running() && cur == 0) {
            advance_session();
        }
        was_running = timer.is_running();
        if (cur != last_seconds) {
            last_seconds = cur;
            draw(last_seconds);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    if (font_big) XFreeFont(dpy, font_big);
    if (font_small && font_small != font_big) XFreeFont(dpy, font_small);
    XFreeGC(dpy, gc);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    return 0;
}
