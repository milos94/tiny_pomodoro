// Shared timer + session types for tiny_pomodoro_pro / _pro_plus.

#pragma once

#include <array>
#include <cstddef>
#include <string_view>
#include <thread>

namespace pomodoro {

constexpr int kDefaultFocusMin = 25;
constexpr int kDefaultShortMin = 5;
constexpr int kDefaultLongMin = 15;
constexpr int kLongBreakEvery = 4;

enum class Session : std::size_t { Focus = 0, ShortBreak = 1, LongBreak = 2 };

inline constexpr std::array<std::string_view, 3> kSessionNames{
    "Focus", "Short Break", "Long Break"};

[[nodiscard]] constexpr std::string_view name_of(Session s) noexcept {
  return kSessionNames[static_cast<std::size_t>(s)];
}

// No std::atomic on the shared fields: every worker iteration crosses
// std::this_thread::sleep_for (opaque to the optimiser, so it can't hoist
// loads), x86-64 TSO covers naturally-aligned int/bool/enum access, and
// mutators (set_duration, reset) are only invoked from the main thread while
// the timer is stopped. Revisit if porting to a weakly-ordered ISA.
class Timer {
public:
  enum class State { Stopped, Running, Paused };

  explicit Timer(int seconds = kDefaultFocusMin * 60) noexcept
      : initial_(seconds), remaining_(seconds) {}

  ~Timer() {
    stop_flag_ = true;
    if (worker_.joinable())
      worker_.join();
  }

  Timer(const Timer &) = delete;
  Timer &operator=(const Timer &) = delete;

  void start() {
    if (state_ == State::Running)
      return;
    state_ = State::Running;
    if (!worker_.joinable())
      worker_ = std::thread(&Timer::run, this);
  }

  void pause() noexcept { state_ = State::Paused; }

  void reset() noexcept {
    remaining_ = initial_;
    state_ = State::Stopped;
  }

  void set_duration(int seconds) noexcept {
    initial_ = seconds;
    remaining_ = seconds;
  }

  [[nodiscard]] int seconds_left() const noexcept { return remaining_; }
  [[nodiscard]] bool is_running() const noexcept {
    return state_ == State::Running;
  }

private:
  void run() noexcept {
    using namespace std::chrono_literals;
    while (!stop_flag_) {
      if (state_ != State::Running) {
        std::this_thread::sleep_for(100ms);
        continue;
      }
      std::this_thread::sleep_for(1s);
      if (state_ != State::Running)
        continue;
      if (remaining_ > 0 && --remaining_ == 0)
        state_ = State::Stopped;
    }
  }

  int initial_;
  int remaining_;
  State state_{State::Stopped};
  bool stop_flag_{false};
  std::thread worker_;
};

} // namespace pomodoro
