// Terminal-only Pomodoro timer — STL + POSIX only, no GUI framework.
// All code is intentionally placed in this single file.

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <termios.h>
#include <unistd.h>

// ── Timer ─────────────────────────────────────────────────────────────────────

struct Timer {
    enum class State { Stopped, Running, Paused };

    explicit Timer(int s = 25 * 60) : initial(s), remaining(s) {}

    ~Timer() {
        stop_flag = true;
        if (worker.joinable()) worker.join();
    }

    void start() {
        if (state == State::Running) return;
        state = State::Running;
        if (!worker.joinable())
            worker = std::thread(&Timer::run, this);
    }

    void pause() { state = State::Paused; }

    void reset() {
        remaining = initial;
        state     = State::Stopped;
    }

    void set_duration(int s) { initial = remaining = s; }

    int  seconds_left() const { return remaining; }
    bool is_running()   const { return state == State::Running; }

private:
    void run() {
        using namespace std::chrono_literals;
        while (!stop_flag) {
            if (state != State::Running) { std::this_thread::sleep_for(100ms); continue; }
            std::this_thread::sleep_for(1s);
            if (state != State::Running) continue;
            if (remaining > 0 && --remaining == 0)
                state = State::Stopped;
        }
    }

    int        initial, remaining;
    State      state{State::Stopped};
    std::thread worker;
    bool        stop_flag{false};
};

// ── Terminal helpers ──────────────────────────────────────────────────────────

static termios g_orig_termios;

static void restore_terminal() {
    tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
    std::fputs("\033[?25h\033[0m\n", stdout);
    std::fflush(stdout);
}

static void setup_raw_terminal() {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    std::atexit(restore_terminal);

    termios raw      = g_orig_termios;
    raw.c_lflag     &= ~static_cast<tcflag_t>(ICANON | ECHO);
    raw.c_cc[VMIN]   = 0;
    raw.c_cc[VTIME]  = 0; // non-blocking read
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);

    std::fputs("\033[?25l", stdout); // hide cursor
}

static void handle_signal(int) {
    restore_terminal();
    _exit(0);
}

// ── Rendering ─────────────────────────────────────────────────────────────────

static constexpr const char* SESSION_NAMES[] = { "Focus", "Short Break", "Long Break" };

static void render(const Timer& t, int sess, int pomodoros,
                   int focus_min, int short_min, int long_min) {
    int secs = t.seconds_left();

    // Move to top-left without clearing to avoid flicker
    std::fputs("\033[H", stdout);

    std::fputs("\033[1m┌─────────────────────────────────┐\033[0m\n", stdout);

    // Session name + pomodoro count
    std::printf("\033[1m│  %-20s        #%-3d  │\033[0m\n", SESSION_NAMES[sess], pomodoros);

    // Timer
    std::printf("\033[1m│         \033[32m%02d:%02d\033[0m\033[1m"
                "                    │\033[0m\n", secs / 60, secs % 60);

    // Status
    const char* dot    = t.is_running() ? "\033[32m●\033[0m\033[1m" : "\033[33m■\033[0m\033[1m";
    const char* status = t.is_running() ? "Running" : "Paused ";
    std::printf("\033[1m│         %s %s"
                "                │\033[0m\n", dot, status);

    std::fputs("\033[1m├─────────────────────────────────┤\033[0m\n", stdout);

    // Duration controls
    std::printf("\033[1m│  Focus \033[0m%-3d\033[1m  Short \033[0m%-3d"
                "\033[1m  Long \033[0m%-3d\033[1m  │\033[0m\n",
                focus_min, short_min, long_min);

    std::fputs("\033[1m├─────────────────────────────────┤\033[0m\n", stdout);

    // Key reference
    std::fputs("\033[1m│\033[0m  [s] Start/Pause   [r] Reset     \033[1m│\033[0m\n", stdout);
    std::fputs("\033[1m│\033[0m  [f/F] Focus ±     [b/B] Short ± \033[1m│\033[0m\n", stdout);
    std::fputs("\033[1m│\033[0m  [l/L] Long ±      [q]   Quit    \033[1m│\033[0m\n", stdout);

    std::fputs("\033[1m└─────────────────────────────────┘\033[0m\n", stdout);

    std::fflush(stdout);
}

// ── Main ──────────────────────────────────────────────────────────────────────

int main() {
    std::signal(SIGINT,  handle_signal);
    std::signal(SIGTERM, handle_signal);

    if (!isatty(STDIN_FILENO)) {
        std::fputs("error: requires a terminal\n", stderr);
        return 1;
    }

    setup_raw_terminal();
    std::fputs("\033[2J", stdout); // clear screen once

    int focus_min = 25, short_min = 5, long_min = 15;
    int session        = 0; // 0=Focus  1=Short  2=Long
    int pomodoro_count = 0;
    bool was_running   = false;

    Timer timer(focus_min * 60);

    auto advance_session = [&]() {
        std::putchar('\a'); // terminal bell
        std::fflush(stdout);
        if (session == 0) {
            ++pomodoro_count;
            session = (pomodoro_count % 4 == 0) ? 2 : 1;
            timer.set_duration((session == 2 ? long_min : short_min) * 60);
        } else {
            session = 0;
            timer.set_duration(focus_min * 60);
        }
        timer.start();
    };

    while (true) {
        char c = 0;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            switch (c) {
                case 's': case 'S':
                    if (timer.is_running()) timer.pause(); else timer.start();
                    break;
                case 'r': case 'R':
                    timer.reset();
                    break;
                case 'f':
                    focus_min = std::max(1, focus_min - 1);
                    if (!timer.is_running()) timer.set_duration(focus_min * 60);
                    break;
                case 'F':
                    focus_min = std::min(240, focus_min + 1);
                    if (!timer.is_running()) timer.set_duration(focus_min * 60);
                    break;
                case 'b':
                    short_min = std::max(1, short_min - 1);
                    break;
                case 'B':
                    short_min = std::min(120, short_min + 1);
                    break;
                case 'l':
                    long_min = std::max(1, long_min - 1);
                    break;
                case 'L':
                    long_min = std::min(240, long_min + 1);
                    break;
                case 'q': case 'Q': case '\x1b':
                    restore_terminal();
                    return 0;
                default: break;
            }
        }

        bool now_running = timer.is_running();
        if (was_running && !now_running && timer.seconds_left() == 0)
            advance_session();
        was_running = now_running;

        render(timer, session, pomodoro_count, focus_min, short_min, long_min);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}
