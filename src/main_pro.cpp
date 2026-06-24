// Tiny X11 UI for the "raw" pomodoro. Timer + Session live in timer.hpp,
// shared with tiny_pomodoro_pro_plus.

#include "timer.hpp"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <alsa/asoundlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numbers>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using namespace pomodoro;

// Helper: format seconds as MM:SS
[[nodiscard]] std::string format_time(int seconds) {
  if (seconds < 0)
    seconds = 0;
  std::array<char, 16> buf{};
  std::snprintf(buf.data(), buf.size(), "%02d:%02d", seconds / 60,
                seconds % 60);
  return std::string(buf.data());
}

// ── RAII helpers for C resources ─────────────────────────────────────────────

struct FileCloser {
  void operator()(FILE *f) const noexcept {
    if (f)
      std::fclose(f);
  }
};
struct PcmCloser {
  void operator()(snd_pcm_t *p) const noexcept {
    if (p)
      snd_pcm_close(p);
  }
};

using FileHandle = std::unique_ptr<FILE, FileCloser>;
using PcmHandle = std::unique_ptr<snd_pcm_t, PcmCloser>;

[[nodiscard]] PcmHandle open_pcm() noexcept {
  snd_pcm_t *pcm = nullptr;
  if (snd_pcm_open(&pcm, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0)
    return {};
  return PcmHandle{pcm};
}

// ── Sound
// ─────────────────────────────────────────────────────────────────────

constexpr unsigned kSampleRate = 44100;

// Synthesised fallback beep
void play_beep(double freq = 880.0, int duration_ms = 200) {
  auto pcm = open_pcm();
  if (!pcm)
    return;
  snd_pcm_set_params(pcm.get(), SND_PCM_FORMAT_S16_LE,
                     SND_PCM_ACCESS_RW_INTERLEAVED, 1, kSampleRate, 1, 50000);
  const auto frames = static_cast<std::size_t>(static_cast<int>(kSampleRate) *
                                               duration_ms / 1000);
  std::vector<short> buf(frames);
  for (std::size_t i = 0; i < frames; ++i) {
    const double t = static_cast<double>(i) / static_cast<double>(kSampleRate);
    const double envelope =
        1.0 - static_cast<double>(i) / static_cast<double>(frames);
    buf[i] = static_cast<short>(
        32767.0 * std::sin(2.0 * std::numbers::pi * freq * t) * envelope);
  }
  snd_pcm_writei(pcm.get(), buf.data(), static_cast<snd_pcm_uframes_t>(frames));
  snd_pcm_drain(pcm.get());
}

// WAV helpers (little-endian reads). Not [[nodiscard]] — callers also use these
// to advance the file position past header fields they don't need.
std::uint16_t wav_u16(FILE *f) {
  std::array<unsigned char, 2> b{};
  std::fread(b.data(), 1, b.size(), f);
  return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
}
std::uint32_t wav_u32(FILE *f) {
  std::array<unsigned char, 4> b{};
  std::fread(b.data(), 1, b.size(), f);
  return static_cast<std::uint32_t>(b[0] | (b[1] << 8) | (b[2] << 16) |
                                    (b[3] << 24));
}

// Play a WAV file; falls back to synthesised beep if the file can't be loaded
void play_wav(const char *path) {
  FileHandle f{std::fopen(path, "rb")};
  if (!f) {
    play_beep();
    return;
  }

  std::array<char, 4> tag{};
  if (std::fread(tag.data(), 1, tag.size(), f.get()) != tag.size() ||
      std::strncmp(tag.data(), "RIFF", 4) != 0) {
    play_beep();
    return;
  }
  wav_u32(f.get()); // file size
  if (std::fread(tag.data(), 1, tag.size(), f.get()) != tag.size() ||
      std::strncmp(tag.data(), "WAVE", 4) != 0) {
    play_beep();
    return;
  }

  std::uint16_t channels = 1, bits = 16;
  std::uint32_t sample_rate = kSampleRate, data_size = 0;
  bool got_fmt = false, got_data = false;
  while (std::fread(tag.data(), 1, tag.size(), f.get()) == tag.size()) {
    const std::uint32_t sz = wav_u32(f.get());
    if (std::strncmp(tag.data(), "fmt ", 4) == 0) {
      const std::uint16_t audio_fmt = wav_u16(f.get());
      channels = wav_u16(f.get());
      sample_rate = wav_u32(f.get());
      wav_u32(f.get());
      wav_u16(f.get()); // byte rate, block align
      bits = wav_u16(f.get());
      if (sz > 16)
        std::fseek(f.get(), static_cast<long>(sz - 16), SEEK_CUR);
      got_fmt = (audio_fmt == 1);
    } else if (std::strncmp(tag.data(), "data", 4) == 0) {
      data_size = sz;
      got_data = true;
      break;
    } else {
      std::fseek(f.get(), static_cast<long>(sz), SEEK_CUR);
    }
  }
  if (!got_fmt || !got_data || data_size == 0) {
    play_beep();
    return;
  }

  std::vector<std::byte> pcm_buf(data_size);
  const auto read_bytes =
      std::fread(pcm_buf.data(), 1, pcm_buf.size(), f.get());

  auto pcm = open_pcm();
  if (!pcm) {
    play_beep();
    return;
  }
  const snd_pcm_format_t fmt =
      (bits == 8) ? SND_PCM_FORMAT_U8 : SND_PCM_FORMAT_S16_LE;
  snd_pcm_set_params(pcm.get(), fmt, SND_PCM_ACCESS_RW_INTERLEAVED, channels,
                     sample_rate, 1, 50000);
  const auto frame_size = static_cast<snd_pcm_uframes_t>(channels * (bits / 8));
  snd_pcm_writei(pcm.get(), pcm_buf.data(),
                 static_cast<snd_pcm_uframes_t>(read_bytes) / frame_size);
  snd_pcm_drain(pcm.get());
}

// ── Window layout
// ───────────────────────────────────────────────────────────── Layout:
//   y=6..26:   session name  (small font, centred)
//   y=30..82:  timer MM:SS   (big font, centred)
//   y=90:      separator line
//   y=92..110: duration column headers
//   y=117..139: duration +/- controls (3 columns × 120 px)
//   y=152..186: Start/Pause + Reset buttons
constexpr unsigned kWinW = 360;
constexpr unsigned kWinH = 210;

struct Button {
  int x;
  int y;
  unsigned int w;
  unsigned int h;
  std::string label;
};

} // namespace

int main() {
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    std::fputs("Unable to open X display\n", stderr);
    return 1;
  }

  const int screen = DefaultScreen(dpy);
  const unsigned long black = BlackPixel(dpy, screen);
  const unsigned long white = WhitePixel(dpy, screen);

  Window win = XCreateSimpleWindow(dpy, RootWindow(dpy, screen), 100, 100,
                                   kWinW, kWinH, 1, black, white);

  XSelectInput(dpy, win,
               ExposureMask | KeyPressMask | ButtonPressMask |
                   StructureNotifyMask);
  Atom wmDelete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
  XSetWMProtocols(dpy, win, &wmDelete, 1);

  if (XClassHint *class_hint = XAllocClassHint()) {
    class_hint->res_name = const_cast<char *>("tiny_pomodoro_pro");
    class_hint->res_class = const_cast<char *>("TinyPomodoroPro");
    XSetClassHint(dpy, win, class_hint);
    XFree(class_hint);
  }
  XMapWindow(dpy, win);

  GC gc = XCreateGC(dpy, win, 0, nullptr);

  // Big font for the timer, small font for everything else
  XFontStruct *font_big =
      XLoadQueryFont(dpy, "-*-helvetica-bold-r-*-*-24-*-*-*-*-*-*-*");
  if (!font_big)
    font_big = XLoadQueryFont(dpy, "fixed");
  XFontStruct *font_small =
      XLoadQueryFont(dpy, "-*-helvetica-medium-r-*-*-12-*-*-*-*-*-*-*");
  if (!font_small)
    font_small = XLoadQueryFont(dpy, "fixed");
  if (!font_small)
    font_small = font_big;

  // Proper gray via colormap (falls back to white)
  unsigned long gray = white;
  {
    XColor xcol{};
    if (XAllocNamedColor(dpy, DefaultColormap(dpy, screen), "gray85", &xcol,
                         &xcol))
      gray = xcol.pixel;
  }

  // Duration controls: 3 columns of 120 px
  // Within each column: [-22-][gap][val 28][gap][+22+], centred → [-] at
  // col+24, [+] at col+74
  int focus_min = kDefaultFocusMin;
  int short_min = kDefaultShortMin;
  int long_min = kDefaultLongMin;

  Button focus_minus{.x = 24, .y = 117, .w = 22, .h = 22, .label = "-"};
  Button focus_plus{.x = 74, .y = 117, .w = 22, .h = 22, .label = "+"};
  Button short_minus{.x = 144, .y = 117, .w = 22, .h = 22, .label = "-"};
  Button short_plus{.x = 194, .y = 117, .w = 22, .h = 22, .label = "+"};
  Button long_minus{.x = 264, .y = 117, .w = 22, .h = 22, .label = "-"};
  Button long_plus{.x = 314, .y = 117, .w = 22, .h = 22, .label = "+"};

  // Action buttons: two 110×34 buttons centred in 360 px with 20 px gap
  Button btn_start{.x = 60, .y = 152, .w = 110, .h = 34, .label = "Start"};
  Button btn_reset{.x = 190, .y = 152, .w = 110, .h = 34, .label = "Reset"};

  Session session = Session::Focus;
  int pomodoro_count = 0;

  Timer timer(focus_min * 60);
  bool running = true;

  // Draw text centred horizontally and vertically inside a bounding box
  auto draw_centered = [&](XFontStruct *fnt, int bx, int by, unsigned int bw,
                           unsigned int bh, std::string_view text) {
    if (!fnt || text.empty())
      return;
    const int len = static_cast<int>(text.size());
    const int tw = XTextWidth(fnt, text.data(), len);
    const int tx = bx + (static_cast<int>(bw) - tw) / 2;
    const int ty =
        by + static_cast<int>(bh) / 2 + (fnt->ascent - fnt->descent) / 2;
    XSetFont(dpy, gc, fnt->fid);
    XDrawString(dpy, win, gc, tx, ty, text.data(), len);
  };

  auto draw = [&](int secs) {
    XClearWindow(dpy, win);
    XSetForeground(dpy, gc, black);

    // ── Session name (centred at top) ─────────────────────────────────
    draw_centered(font_small, 0, 6, kWinW, 20, name_of(session));

    // ── Timer MM:SS (centred, big font) ───────────────────────────────
    const std::string timestr = format_time(secs);
    draw_centered(font_big, 0, 30, kWinW, 52, timestr);

    // ── Separator ─────────────────────────────────────────────────────
    XDrawLine(dpy, win, gc, 10, 90, static_cast<int>(kWinW) - 10, 90);

    // ── Duration column headers ───────────────────────────────────────
    draw_centered(font_small, 0, 92, 120, 18, "Focus");
    draw_centered(font_small, 120, 92, 120, 18, "Short");
    draw_centered(font_small, 240, 92, 120, 18, "Long");

    // ── Duration +/- buttons and values ──────────────────────────────
    auto draw_btn = [&](const Button &b) {
      XSetForeground(dpy, gc, gray);
      XFillRectangle(dpy, win, gc, b.x, b.y, b.w, b.h);
      XSetForeground(dpy, gc, black);
      XDrawRectangle(dpy, win, gc, b.x, b.y, b.w, b.h);
      draw_centered(font_small, b.x, b.y, b.w, b.h, b.label);
    };
    draw_btn(focus_minus);
    draw_btn(focus_plus);
    draw_btn(short_minus);
    draw_btn(short_plus);
    draw_btn(long_minus);
    draw_btn(long_plus);

    // Values in the gap between each pair of buttons (col_base+46, width 28)
    XSetForeground(dpy, gc, black);
    draw_centered(font_small, 46, 117, 28, 22, std::to_string(focus_min));
    draw_centered(font_small, 166, 117, 28, 22, std::to_string(short_min));
    draw_centered(font_small, 286, 117, 28, 22, std::to_string(long_min));

    // ── Action buttons ────────────────────────────────────────────────
    btn_start.label = timer.is_running() ? "Pause" : "Start";
    draw_btn(btn_start);
    draw_btn(btn_reset);
  };

  auto advance_session = [&]() {
    XBell(dpy, 100);
    play_wav(SOUND_PATH);
    if (session == Session::Focus) {
      ++pomodoro_count;
      if (pomodoro_count % kLongBreakEvery == 0) {
        session = Session::LongBreak;
        timer.set_duration(long_min * 60);
      } else {
        session = Session::ShortBreak;
        timer.set_duration(short_min * 60);
      }
    } else {
      session = Session::Focus;
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
        if (static_cast<Atom>(ev.xclient.data.l[0]) == wmDelete)
          running = false;
      } else if (ev.type == ButtonPress) {
        const int mx = ev.xbutton.x;
        const int my = ev.xbutton.y;
        auto hit = [&](const Button &b) noexcept {
          return mx >= b.x && mx <= b.x + static_cast<int>(b.w) && my >= b.y &&
                 my <= b.y + static_cast<int>(b.h);
        };
        // duration +/- buttons
        if (hit(focus_minus)) {
          focus_min = std::max(1, focus_min - 1);
          if (!timer.is_running())
            timer.set_duration(focus_min * 60);
          draw(timer.seconds_left());
        } else if (hit(focus_plus)) {
          focus_min = std::min(240, focus_min + 1);
          if (!timer.is_running())
            timer.set_duration(focus_min * 60);
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
          if (timer.is_running())
            timer.pause();
          else
            timer.start();
          draw(timer.seconds_left());
        } else if (hit(btn_reset)) {
          timer.reset();
          draw(timer.seconds_left());
        }
      } else if (ev.type == KeyPress) {
        std::array<char, 16> buf{};
        KeySym keysym;
        const int len =
            XLookupString(&ev.xkey, buf.data(), static_cast<int>(buf.size()),
                          &keysym, nullptr);
        if (len > 0 && (buf[0] == 'q' || buf[0] == 'Q' || buf[0] == '\x1b'))
          running = false;
      }
    }

    const int cur = timer.seconds_left();
    if (was_running && !timer.is_running() && cur == 0)
      advance_session();
    was_running = timer.is_running();
    if (cur != last_seconds) {
      last_seconds = cur;
      draw(last_seconds);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));
  }

  if (font_big)
    XFreeFont(dpy, font_big);
  if (font_small && font_small != font_big)
    XFreeFont(dpy, font_small);
  XFreeGC(dpy, gc);
  XDestroyWindow(dpy, win);
  XCloseDisplay(dpy);

  return 0;
}
