#pragma once

#include <string>

namespace hc {

// Where frames come from. Exactly one is in effect per run; the parser
// rejects a command line that asks for more than one.
enum class SourceKind { Camera, Video, Synthetic };

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

// The usage text, for --help and for printing alongside a parse error.
std::string CliUsage();

}  // namespace hc
