#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Window.H>
#include <FL/fl_ask.H>
#include <cstdio>
#include <cstdlib>

enum class Session { Focus, ShortBreak, LongBreak };

struct PomodoroApp {
    Fl_Window*    window         = nullptr;
    Fl_Box*       session_label  = nullptr;
    Fl_Box*       timer_label    = nullptr;
    Fl_Button*    start_stop_btn = nullptr;
    Fl_Int_Input* focus_input    = nullptr;
    Fl_Int_Input* short_input    = nullptr;
    Fl_Int_Input* long_input     = nullptr;

    bool    running      = false;
    int     seconds_left = 0;
    int     focus_count  = 0;
    Session current      = Session::Focus;
};

static PomodoroApp app;

// ── helpers ───────────────────────────────────────────────────────────────────

static void update_timer_label() {
    char buf[6];
    std::snprintf(buf, sizeof(buf), "%02d:%02d",
                  app.seconds_left / 60, app.seconds_left % 60);
    app.timer_label->copy_label(buf);
    app.timer_label->redraw();
}

static int input_minutes(Fl_Int_Input* w, int fallback) {
    const int v = std::atoi(w->value());
    return v > 0 ? v : fallback;
}

// ── timer logic ───────────────────────────────────────────────────────────────

static void tick_cb(void*);

static void stop_timer() {
    app.running = false;
    app.start_stop_btn->label("Start");
    Fl::remove_timeout(tick_cb);
}

static void reset_session(Session s) {
    stop_timer();
    app.current = s;

    int         minutes;
    const char* name;
    switch (s) {
        case Session::Focus:      minutes = input_minutes(app.focus_input, 25); name = "Focus";       break;
        case Session::ShortBreak: minutes = input_minutes(app.short_input,  5); name = "Short Break"; break;
        case Session::LongBreak:  minutes = input_minutes(app.long_input,  15); name = "Long Break";  break;
    }

    app.seconds_left = minutes * 60;
    app.session_label->copy_label(name);
    update_timer_label();
}

static void tick_cb(void*) {
    if (!app.running) return;
    if (--app.seconds_left > 0) {
        update_timer_label();
        Fl::add_timeout(1.0, tick_cb);
    } else {
        app.seconds_left = 0;
        update_timer_label();
        fl_beep();
        // Auto-advance: Focus→Break, Break→Focus
        Session next;
        if (app.current == Session::Focus) {
            ++app.focus_count;
            next = (app.focus_count % 4 == 0) ? Session::LongBreak : Session::ShortBreak;
        } else {
            next = Session::Focus;
        }
        reset_session(next);
        app.running = true;
        app.start_stop_btn->label("Stop");
        Fl::add_timeout(1.0, tick_cb);
    }
}

// ── callbacks ─────────────────────────────────────────────────────────────────

static void start_stop_cb(Fl_Widget*, void*) {
    if (app.running) {
        stop_timer();
    } else {
        if (app.seconds_left == 0) reset_session(app.current);
        app.running = true;
        app.start_stop_btn->label("Stop");
        Fl::add_timeout(1.0, tick_cb);
    }
}

// Session is passed as the numeric enum value cast through void*.
static void session_cb(Fl_Widget*, void* data) {
    reset_session(static_cast<Session>(reinterpret_cast<long>(data)));
}

// ── main ──────────────────────────────────────────────────────────────────────

int main() {
    app.window = new Fl_Window(320, 420, "Tiny Pomodoro");
    app.window->color(FL_WHITE);

    app.session_label = new Fl_Box(0, 18, 320, 28, "Focus");
    app.session_label->labelsize(15);
    app.session_label->labelfont(FL_BOLD);

    app.timer_label = new Fl_Box(0, 54, 320, 90, "25:00");
    app.timer_label->labelsize(60);
    app.timer_label->labelfont(FL_BOLD);

    // Session selector buttons
    struct { const char* label; Session s; int x; } selectors[] = {
        {"Focus",       Session::Focus,      14 },
        {"Short Break", Session::ShortBreak, 116},
        {"Long Break",  Session::LongBreak,  218},
    };
    for (const auto& sel : selectors) {
        auto* b = new Fl_Button(sel.x, 158, 88, 28, sel.label);
        b->callback(session_cb, reinterpret_cast<void*>(static_cast<long>(sel.s)));
    }

    app.start_stop_btn = new Fl_Button(110, 202, 100, 36, "Start");
    app.start_stop_btn->labelsize(16);
    app.start_stop_btn->labelfont(FL_BOLD);
    app.start_stop_btn->callback(start_stop_cb);

    auto* sep = new Fl_Box(0, 258, 320, 20, "Durations (minutes)");
    sep->labelsize(12);
    sep->labelcolor(FL_DARK3);

    // Duration input rows: [right-aligned label] [integer input]
    struct { const char* label; Fl_Int_Input** field; const char* def; int y; } rows[] = {
        {"Focus:",       &app.focus_input, "25", 288},
        {"Short Break:", &app.short_input, "5",  322},
        {"Long Break:",  &app.long_input,  "15", 356},
    };
    for (const auto& row : rows) {
        auto* lbl = new Fl_Box(30, row.y, 100, 24, row.label);
        lbl->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
        lbl->labelsize(13);
        *row.field = new Fl_Int_Input(138, row.y, 60, 24);
        (*row.field)->value(row.def);
    }

    app.window->end();
    app.window->show();
    reset_session(Session::Focus);
    return Fl::run();
}
