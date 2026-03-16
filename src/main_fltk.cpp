#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Window.H>
#include <cstdio>
#include <cstdlib>

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
    Session current        = Session::Focus;
};

static PomodoroApp app;

// ── helpers ──────────────────────────────────────────────────────────────────

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
