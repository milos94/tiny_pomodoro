#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Int_Input.H>
#include <FL/Fl_Window.H>
#include <alsa/asoundlib.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <numbers>
#include <vector>

namespace {

// ── Defaults ─────────────────────────────────────────────────────────────────
constexpr int kDefaultFocusMin = 25;
constexpr int kDefaultShortMin = 5;
constexpr int kDefaultLongMin = 15;
constexpr int kLongBreakEvery = 4;
constexpr unsigned kSampleRate = 44100;

enum class Session { Focus, ShortBreak, LongBreak };

struct PomodoroApp {
  Fl_Window *window = nullptr;
  Fl_Box *session_label = nullptr;
  Fl_Box *timer_label = nullptr;
  Fl_Button *start_stop_btn = nullptr;
  Fl_Int_Input *focus_input = nullptr;
  Fl_Int_Input *short_input = nullptr;
  Fl_Int_Input *long_input = nullptr;

  bool running = false;
  int seconds_left = 0;
  int pomodoro_count = 0;
  Session current = Session::Focus;
};

PomodoroApp app;

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

// ── Sound ────────────────────────────────────────────────────────────────────

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

// Play a WAV file; falls back to synthesised beep if the file can't be loaded.
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
  wav_u32(f.get());
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
      wav_u16(f.get());
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

// ── UI helpers ───────────────────────────────────────────────────────────────

void update_timer_label() {
  std::array<char, 6> buf{};
  std::snprintf(buf.data(), buf.size(), "%02d:%02d", app.seconds_left / 60,
                app.seconds_left % 60);
  app.timer_label->copy_label(buf.data());
  app.timer_label->redraw();
}

[[nodiscard]] int input_minutes(Fl_Int_Input *w, int fallback) {
  const int v = std::atoi(w->value());
  return v > 0 ? v : fallback;
}

// ── Timer logic ──────────────────────────────────────────────────────────────

void tick_cb(void *);

void stop_timer() {
  app.running = false;
  app.start_stop_btn->label("Start");
  Fl::remove_timeout(tick_cb);
}

void reset_session(Session s) {
  stop_timer();
  app.current = s;

  int minutes = 0;
  const char *name = nullptr;

  switch (s) {
  case Session::Focus:
    minutes = input_minutes(app.focus_input, kDefaultFocusMin);
    name = "Focus";
    break;
  case Session::ShortBreak:
    minutes = input_minutes(app.short_input, kDefaultShortMin);
    name = "Short Break";
    break;
  case Session::LongBreak:
    minutes = input_minutes(app.long_input, kDefaultLongMin);
    name = "Long Break";
    break;
  }

  app.seconds_left = minutes * 60;
  app.session_label->copy_label(name);
  update_timer_label();
}

void tick_cb(void *) {
  if (!app.running)
    return;
  if (app.seconds_left > 0) {
    --app.seconds_left;
    update_timer_label();
    Fl::add_timeout(1.0, tick_cb);
    return;
  }

  stop_timer();
  play_wav(SOUND_PATH);

  // advance to next session
  if (app.current == Session::Focus) {
    ++app.pomodoro_count;
    reset_session(app.pomodoro_count % kLongBreakEvery == 0
                      ? Session::LongBreak
                      : Session::ShortBreak);
  } else {
    reset_session(Session::Focus);
  }

  // auto-start the next session
  app.running = true;
  app.start_stop_btn->label("Stop");
  Fl::add_timeout(1.0, tick_cb);
}

// ── Callbacks ────────────────────────────────────────────────────────────────

void start_stop_cb(Fl_Widget *, void *) {
  if (app.running) {
    stop_timer();
    return;
  }
  if (app.seconds_left == 0)
    reset_session(app.current);
  app.running = true;
  app.start_stop_btn->label("Stop");
  Fl::add_timeout(1.0, tick_cb);
}

void session_cb(Fl_Widget *, void *data) {
  reset_session(*static_cast<Session *>(data));
}

} // namespace

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
  static Session s_focus = Session::Focus;
  static Session s_short = Session::ShortBreak;
  static Session s_long = Session::LongBreak;

  // Stack-allocate the window: its destructor runs at main's return (after
  // Fl::run() exits) and cascades through Fl_Group::~Fl_Group to delete the
  // child widgets. Children stay heap-allocated — FLTK tracks them by pointer
  // in the parent's child list and deletes them itself, so wrapping them in
  // unique_ptr would cause a double-free.
  Fl_Window window(320, 420, "Tiny Pomodoro");
  app.window = &window;
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
  auto *btn_focus = new Fl_Button(14, 158, 88, 28, "Focus");
  auto *btn_short = new Fl_Button(116, 158, 88, 28, "Short Break");
  auto *btn_long = new Fl_Button(218, 158, 88, 28, "Long Break");
  btn_focus->callback(session_cb, &s_focus);
  btn_short->callback(session_cb, &s_short);
  btn_long->callback(session_cb, &s_long);

  // Start / Stop
  app.start_stop_btn = new Fl_Button(110, 202, 100, 36, "Start");
  app.start_stop_btn->labelsize(16);
  app.start_stop_btn->labelfont(FL_BOLD);
  app.start_stop_btn->callback(start_stop_cb);

  // Divider label
  auto *sep = new Fl_Box(0, 258, 320, 20, "Durations (minutes)");
  sep->labelsize(12);
  sep->labelcolor(FL_DARK3);

  // ── Duration inputs ──────────────────────────────────────────────────────
  //  row layout: [label 100px] [input 60px]  starting x=80

  // Focus
  auto *lbl_focus = new Fl_Box(30, 288, 100, 24, "Focus:");
  lbl_focus->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
  lbl_focus->labelsize(13);
  app.focus_input = new Fl_Int_Input(138, 288, 60, 24);
  app.focus_input->value("25");

  // Short break
  auto *lbl_short = new Fl_Box(30, 322, 100, 24, "Short Break:");
  lbl_short->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
  lbl_short->labelsize(13);
  app.short_input = new Fl_Int_Input(138, 322, 60, 24);
  app.short_input->value("5");

  // Long break
  auto *lbl_long = new Fl_Box(30, 356, 100, 24, "Long Break:");
  lbl_long->align(FL_ALIGN_RIGHT | FL_ALIGN_INSIDE);
  lbl_long->labelsize(13);
  app.long_input = new Fl_Int_Input(138, 356, 60, 24);
  app.long_input->value("15");

  app.window->end();
  app.window->xclass("tiny_pomodoro");
  app.window->show();

  reset_session(Session::Focus);

  return Fl::run();
}
