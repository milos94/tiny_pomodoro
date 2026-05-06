#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Window.H>
#include <alsa/asoundlib.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <unistd.h>

enum class Session { Focus, ShortBreak, LongBreak };

struct PomodoroApp {
    Fl_Window*    window          = nullptr;
    Fl_Box*       session_label   = nullptr;
    Fl_Box*       timer_label     = nullptr;
    Fl_Button*    start_stop_btn  = nullptr;
    Fl_Int_Input* focus_input     = nullptr;
    Fl_Int_Input* short_input     = nullptr;
    Fl_Int_Input* long_input      = nullptr;

    bool    running        = false;
    int     seconds_left   = 0;
    int     pomodoro_count = 0;
    Session current        = Session::Focus;
};

static PomodoroApp app;

// ── helpers ──────────────────────────────────────────────────────────────────

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
static std::string exe_dir() {
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    char* slash = std::strrchr(buf, '/');
    if (slash) *slash = '\0';
    return std::string(buf);
}
// Play a WAV file; falls back to synthesised beep if the file can't be loaded
static void play_wav(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) { play_beep(); return; }

    char tag[4];
    if (std::fread(tag, 1, 4, f) != 4 || std::strncmp(tag, "RIFF", 4) != 0)
        { std::fclose(f); play_beep(); return; }
    wav_u32(f);
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
            wav_u32(f); wav_u16(f);
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

static void update_timer_label() {
    char buf[6];
    std::snprintf(buf, sizeof(buf), "%02d:%02d",
                  app.seconds_left / 60,
                  app.seconds_left % 60);
    app.timer_label->copy_label(buf);
    app.timer_label->redraw();
}

static int input_minutes(Fl_Int_Input* w, int fallback) {
    int v = std::atoi(w->value());
    return v > 0 ? v : fallback;
}

// ── timer logic ──────────────────────────────────────────────────────────────

static void tick_cb(void*);

static void stop_timer() {
    app.running = false;
    app.start_stop_btn->label("Start");
    Fl::remove_timeout(tick_cb);
}

static void reset_session(Session s) {
    stop_timer();
    app.current = s;

    int minutes = 0;
    const char* name = nullptr;

    switch (s) {
        case Session::Focus:
            minutes = input_minutes(app.focus_input, 25);
            name    = "Focus";
            break;
        case Session::ShortBreak:
            minutes = input_minutes(app.short_input, 5);
            name    = "Short Break";
            break;
        case Session::LongBreak:
            minutes = input_minutes(app.long_input, 15);
            name    = "Long Break";
            break;
    }

    app.seconds_left = minutes * 60;
    app.session_label->copy_label(name);
    update_timer_label();
}

static void tick_cb(void*) {
    if (!app.running) return;
    if (app.seconds_left > 0) {
        --app.seconds_left;
        update_timer_label();
        Fl::add_timeout(1.0, tick_cb);
    } else {
        stop_timer();
        play_wav((exe_dir() + "/sounds/beep.wav").c_str());
        // advance to next session
        if (app.current == Session::Focus) {
            app.pomodoro_count++;
            if (app.pomodoro_count % 4 == 0)
                reset_session(Session::LongBreak);
            else
                reset_session(Session::ShortBreak);
        } else {
            reset_session(Session::Focus);
        }
        // auto-start the next session
        app.running = true;
        app.start_stop_btn->label("Stop");
        Fl::add_timeout(1.0, tick_cb);
    }
}

// ── callbacks ────────────────────────────────────────────────────────────────

static void start_stop_cb(Fl_Widget*, void*) {
    if (app.running) {
        stop_timer();
    } else {
        if (app.seconds_left == 0)
            reset_session(app.current);
        app.running = true;
        app.start_stop_btn->label("Stop");
        Fl::add_timeout(1.0, tick_cb);
    }
}

static void session_cb(Fl_Widget*, void* data) {
    reset_session(*static_cast<Session*>(data));
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    static Session s_focus  = Session::Focus;
    static Session s_short  = Session::ShortBreak;
    static Session s_long   = Session::LongBreak;

    app.window = new Fl_Window(320, 420, "Tiny Pomodoro");
    app.window->color(FL_WHITE);

    // Session name
    app.session_label = new Fl_Box(0, 18, 320, 28, "Focus");
    app.session_label->labelsize(15);
    app.session_label->labelfont(FL_BOLD);

    // Countdown display
    app.timer_label = new Fl_Box(0, 54, 320, 90, "25:00");
    app.timer_label->labelsize(60);
    app.timer_label->labelfont(FL_BOLD);

    // Session selector buttons
    auto* btn_focus = new Fl_Button(14,  158, 88, 28, "Focus");
    auto* btn_short = new Fl_Button(116, 158, 88, 28, "Short Break");
    auto* btn_long  = new Fl_Button(218, 158, 88, 28, "Long Break");
    btn_focus->callback(session_cb, &s_focus);
    btn_short->callback(session_cb, &s_short);
    btn_long ->callback(session_cb, &s_long);

    // Start / Stop
    app.start_stop_btn = new Fl_Button(110, 202, 100, 36, "Start");
    app.start_stop_btn->labelsize(16);
    app.start_stop_btn->labelfont(FL_BOLD);
    app.start_stop_btn->callback(start_stop_cb);

    // Divider label
    auto* sep = new Fl_Box(0, 258, 320, 20, "Durations (minutes)");
    sep->labelsize(12);
    sep->labelcolor(FL_DARK3);

    // ── Duration inputs ──────────────────────────────────────────────────────
    //  row layout: [label 100px] [input 60px]  starting x=80

    // Focus
    auto* lbl_focus = new Fl_Box(30, 288, 100, 24, "Focus:");
    lbl_focus->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    lbl_focus->labelsize(13);
    app.focus_input = new Fl_Int_Input(138, 288, 60, 24);
    app.focus_input->value("25");

    // Short break
    auto* lbl_short = new Fl_Box(30, 322, 100, 24, "Short Break:");
    lbl_short->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    lbl_short->labelsize(13);
    app.short_input = new Fl_Int_Input(138, 322, 60, 24);
    app.short_input->value("5");

    // Long break
    auto* lbl_long = new Fl_Box(30, 356, 100, 24, "Long Break:");
    lbl_long->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
    lbl_long->labelsize(13);
    app.long_input = new Fl_Int_Input(138, 356, 60, 24);
    app.long_input->value("15");

    app.window->end();
    app.window->show();

    reset_session(Session::Focus);

    return Fl::run();
}
