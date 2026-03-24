#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static void print_usage(const char* prog) {
    std::printf(
        "Usage: %s [options]\n"
        "Options:\n"
        "  --focus-duration <minutes>    Set focus duration (default: 25)\n"
        "  --short-break <minutes>       Set short break duration (default: 5)\n"
        "  --long-break <minutes>        Set long break duration (default: 15)\n"
        "  --long-break-interval <n>     Sessions before a long break (default: 4)\n"
        "  --help                        Show this help message\n",
        prog);
}

static void run_timer(int duration_min, const char* label) {
    using namespace std::chrono;
    std::printf("%s for %d minutes.\n", label, duration_min);
    const auto deadline = steady_clock::now() + minutes(duration_min);
    while (true) {
        const auto left = duration_cast<seconds>(deadline - steady_clock::now());
        if (left.count() <= 0) break;
        std::printf("\r%02lld:%02lld remaining...  ",
                    static_cast<long long>(left.count() / 60),
                    static_cast<long long>(left.count() % 60));
        std::fflush(stdout);
        std::this_thread::sleep_for(seconds(1));
    }
    std::puts("\nTime's up!");
}

// Parse a positive integer from s; returns -1 on failure.
static int parse_pos_int(const char* s) {
    char* end;
    const long v = std::strtol(s, &end, 10);
    return (*end == '\0' && v > 0 && v <= 999) ? static_cast<int>(v) : -1;
}

int main(int argc, char* argv[]) {
    int focus_duration    = 25;
    int short_break       = 5;
    int long_break        = 15;
    int long_break_every  = 4;

    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        const bool has_next = (i + 1 < argc);

        if (std::strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (std::strcmp(arg, "--focus-duration") == 0 && has_next) {
            if ((focus_duration = parse_pos_int(argv[++i])) < 0) {
                std::fprintf(stderr, "Invalid value for --focus-duration\n"); return 1;
            }
        } else if (std::strcmp(arg, "--short-break") == 0 && has_next) {
            if ((short_break = parse_pos_int(argv[++i])) < 0) {
                std::fprintf(stderr, "Invalid value for --short-break\n"); return 1;
            }
        } else if (std::strcmp(arg, "--long-break") == 0 && has_next) {
            if ((long_break = parse_pos_int(argv[++i])) < 0) {
                std::fprintf(stderr, "Invalid value for --long-break\n"); return 1;
            }
        } else if (std::strcmp(arg, "--long-break-interval") == 0 && has_next) {
            if ((long_break_every = parse_pos_int(argv[++i])) < 0) {
                std::fprintf(stderr, "Invalid value for --long-break-interval\n"); return 1;
            }
        }
    }

    for (int session = 1; ; ++session) {
        run_timer(focus_duration, "Focus session");
        run_timer(session % long_break_every == 0 ? long_break : short_break,
                  session % long_break_every == 0 ? "Long break" : "Short break");
    }
}
