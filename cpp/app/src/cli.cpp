#include "cli.hpp"

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdlib>
#include <string>

namespace hc {
namespace {

// The tracker's own ceiling: past this, 2*max_hands - 1 exceeds
// kMaxAssociationEntries and the association layer starts ignoring slots
// instead of matching them. Rejecting it here keeps that from becoming a
// constructor exception.
constexpr int kMaxHandsCeiling = 32;

// Parses a complete string as an integer. Stricter than atoi on purpose:
// trailing garbage ("12x") and an empty string are errors, not silently
// truncated successes.
bool ParseInt(const std::string& text, int& out) {
  if (text.empty()) return false;

  errno = 0;
  char* end = nullptr;
  const long value = std::strtol(text.c_str(), &end, 10);

  if (errno == ERANGE) return false;
  if (end != text.c_str() + text.size()) return false;  // Trailing garbage.
  if (value < INT_MIN || value > INT_MAX) return false;

  out = static_cast<int>(value);
  return true;
}

// Parses "WxH" into two positive dimensions.
bool ParseGeometry(const std::string& text, int& width, int& height) {
  const std::size_t x = text.find('x');
  if (x == std::string::npos) return false;
  // A second 'x' means something like "640x360x2", which is not a geometry.
  if (text.find('x', x + 1) != std::string::npos) return false;

  int w = 0;
  int h = 0;
  if (!ParseInt(text.substr(0, x), w)) return false;
  if (!ParseInt(text.substr(x + 1), h)) return false;
  if (w <= 0 || h <= 0) return false;

  width = w;
  height = h;
  return true;
}

}  // namespace

std::string CliUsage() {
  return
      "usage: hand_controller [options]\n"
      "\n"
      "  Source (at most one):\n"
      "    --camera N        capture from camera index N (default: 0)\n"
      "    --video PATH      read frames from a video file\n"
      "    --synthetic WxH   generate frames, no camera needed\n"
      "\n"
      "  Pipeline:\n"
      "    --max-hands N     hands to track, 1..32 (default: 2)\n"
      "    --frames N        stop after N frames, 0 = until quit (default: 0)\n"
      "    --threads N       ONNX Runtime intra-op threads (default: 1)\n"
      "\n"
      "  Output:\n"
      "    --headless        no preview window\n"
      "    --report PATH     also write the timing report to PATH\n"
      "    --help            print this message\n";
}

bool ParseCli(int argc, char** argv, CliOptions& out, std::string& error) {
  CliOptions opts;
  error.clear();

  // Which source flag was seen, so a conflict can name both sides. Not
  // derivable from opts.source, whose default is Camera whether or not
  // --camera was actually given.
  std::string source_flag;

  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];

    // --headless and --help are the only flags that take no value; every
    // other branch below needs one, so the bounds check is shared here
    // rather than repeated (and forgotten) in each branch.
    const bool wants_value = (flag != "--headless" && flag != "--help");
    std::string value;
    if (wants_value) {
      if (i + 1 >= argc) {
        error = "flag " + flag + " requires a value";
        return false;
      }
      value = argv[++i];
    }

    const auto set_source = [&](SourceKind kind) {
      if (!source_flag.empty() && source_flag != flag) {
        error = "cannot use " + source_flag + " and " + flag +
                " together: pick one frame source";
        return false;
      }
      source_flag = flag;
      opts.source = kind;
      return true;
    };

    if (flag == "--camera") {
      if (!set_source(SourceKind::Camera)) return false;
      if (!ParseInt(value, opts.camera_index) || opts.camera_index < 0) {
        error = "--camera needs a non-negative integer, got \"" + value + "\"";
        return false;
      }
    } else if (flag == "--video") {
      if (!set_source(SourceKind::Video)) return false;
      opts.video_path = value;
    } else if (flag == "--synthetic") {
      if (!set_source(SourceKind::Synthetic)) return false;
      if (!ParseGeometry(value, opts.synthetic_width, opts.synthetic_height)) {
        error = "--synthetic needs WxH with positive dimensions, got \"" +
                value + "\"";
        return false;
      }
    } else if (flag == "--max-hands") {
      if (!ParseInt(value, opts.max_hands) || opts.max_hands < 1 ||
          opts.max_hands > kMaxHandsCeiling) {
        error = "--max-hands needs an integer in 1.." +
                std::to_string(kMaxHandsCeiling) + ", got \"" + value + "\"";
        return false;
      }
    } else if (flag == "--frames") {
      // 0 is legal here and means "until the user quits".
      if (!ParseInt(value, opts.frames) || opts.frames < 0) {
        error = "--frames needs a non-negative integer, got \"" + value + "\"";
        return false;
      }
    } else if (flag == "--threads") {
      if (!ParseInt(value, opts.threads) || opts.threads < 1) {
        error = "--threads needs a positive integer, got \"" + value + "\"";
        return false;
      }
    } else if (flag == "--report") {
      opts.report_path = value;
    } else if (flag == "--headless") {
      opts.want_overlay = false;
    } else if (flag == "--help") {
      // Not an error: asking for help is a successful run that prints usage
      // and exits 0. Reporting it as a parse failure would mean a non-zero
      // exit code for a correct command line.
      opts.want_help = true;
    } else {
      error = "unknown flag " + flag;
      return false;
    }
  }

  out = opts;
  return true;
}

}  // namespace hc
