#pragma once

#include <string>

namespace hc {

// Where frames come from. Exactly one is in effect per run; the parser
// rejects a command line that asks for more than one.
enum class SourceKind { Camera, Video, Synthetic };

// Where commands go. Deliberately NOT the sink classes themselves: this is a
// parsed command line, and hc_cli links nothing. main() turns one of these
// into an IOutputSink.
//
// `Null` discards, which is the default -- see CliOptions::no_inject.
// `Log` prints what would have happened without doing it. `XTest` and
// `SendInput` are the real injectors, one per platform; naming both here
// rather than one abstract "native" keeps an error message able to say which
// backend was asked for and why it is unavailable.
enum class SinkKind { Null, Log, XTest, SendInput };

// "null", "log", "xtest", "sendinput". For the startup line, so what the
// program says it is doing comes from the same table the parser read.
const char* SinkKindName(SinkKind sink);

// The parsed command line. Every field carries the default that applies when
// its flag is absent, so a default-constructed CliOptions is exactly what
// running the binary with no arguments produces.
struct CliOptions {
  SourceKind source = SourceKind::Camera;

  int camera_index = 0;     // --camera N
  std::string video_path;   // --video PATH
  int synthetic_width = 1280;   // --synthetic WxH
  int synthetic_height = 720;

  // --max-hands N. Two by default, per the spec. The tracker's own ceiling
  // is 32 (beyond that the association layer silently ignores slots), and
  // the parser enforces that range so a mistyped flag is a message rather
  // than an exception out of the tracker's constructor.
  int max_hands = 2;

  // --frames N. 0 means run until the user quits, which is why 0 is accepted
  // here and rejected for the two fields above and below.
  int frames = 0;

  int threads = 1;  // --threads N, ONNX Runtime's intra-op thread count.

  // Cleared by --headless. The pipeline must run with no overlay at all, so
  // this is the switch the measurement runs use on a machine with no display.
  bool want_overlay = true;

  std::string report_path;  // --report PATH, empty means stdout only.

  // --bindings PATH. Empty means the configuration compiled into
  // hc_control -- absence is "use the shipped default", not "look for a file
  // somewhere and silently run unbound if it is missing".
  std::string bindings_path;

  // --sink <null|log|xtest|sendinput>. What was ASKED FOR, which is not necessarily
  // what runs: see no_inject and ResolveSink.
  SinkKind sink = SinkKind::Null;

  // --no-inject. A safety override that forces the null sink regardless of
  // --sink, in either order on the command line.
  //
  // Kept separate from `sink` rather than collapsing both into one field,
  // because the startup line should be able to say "xtest requested,
  // injection disabled" rather than silently reporting null. Note the DEFAULT
  // sink is already Null, so the program does not touch the desktop unless
  // asked: this flag is for overriding an explicit --sink, e.g. in a script
  // that otherwise injects.
  bool no_inject = false;

  // --sensitivity F and --smoothing F, overriding whatever the bindings file
  // says. Absent by default, because a default VALUE here would silently
  // overwrite the file's own `set` lines on every run.
  //
  // Ranges match ParseBindings exactly: sensitivity strictly positive,
  // smoothing within 0..1 inclusive, neither one nan or inf.
  bool has_sensitivity = false;
  float sensitivity = 0.0f;
  bool has_smoothing = false;
  float smoothing = 0.0f;

  // --help. A successful parse, not an error: main prints usage and exits 0.
  bool want_help = false;
};

// Parses `argv` into `out`. Returns true on success; on failure returns false
// and writes a human-readable explanation to `error`, naming the offending
// flag.
//
// Lives in its own translation unit, separate from main(), purely so the
// tests can link it without also linking a second main().
//
// `out` is only partially written on failure and should not be used.
bool ParseCli(int argc, char** argv, CliOptions& out, std::string& error);

// The sink that will actually run, applying --no-inject over --sink.
//
// A free function rather than a field so there is exactly one place the
// override is applied; two call sites deciding it independently is how a
// safety flag ends up honoured on one path and not the other.
SinkKind ResolveSink(const CliOptions& opts);

// Reads a whole text file. Returns false with a message NAMING THE PATH on
// failure.
//
// Lives here rather than in hc_control because that layer is pure by
// assertion -- no OS, no I/O -- and its own default configuration is
// compiled in for the same reason. Parsing the text is still
// hc_control's job: this function only gets it off the disk.
bool ReadTextFile(const std::string& path, std::string& out,
                  std::string& error);

// The usage text, for --help and for printing alongside a parse error.
std::string CliUsage();

}  // namespace hc
