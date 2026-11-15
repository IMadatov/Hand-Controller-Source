#include "cli.hpp"

#include <string>
#include <vector>

#include <gtest/gtest.h>

using hc::CliOptions;
using hc::ParseCli;
using hc::SourceKind;

namespace {

// ParseCli takes the real (char**) argv, so the test has to own mutable
// storage for the strings. argv[0] is the program name, as it is in main.
struct Args {
  explicit Args(std::vector<std::string> flags) {
    storage.reserve(flags.size() + 1);
    storage.emplace_back("hand_controller");
    for (std::string& f : flags) storage.push_back(std::move(f));
    for (std::string& s : storage) pointers.push_back(s.data());
  }

  int argc() const { return static_cast<int>(pointers.size()); }
  char** argv() { return pointers.data(); }

  std::vector<std::string> storage;
  std::vector<char*> pointers;
};

// Parses and expects success, returning the options.
CliOptions ParseOk(std::vector<std::string> flags) {
  Args args(std::move(flags));
  CliOptions out;
  std::string error;
  EXPECT_TRUE(ParseCli(args.argc(), args.argv(), out, error))
      << "unexpected failure: " << error;
  EXPECT_TRUE(error.empty());
  return out;
}

// Parses and expects failure, returning the error message.
std::string ParseErr(std::vector<std::string> flags) {
  Args args(std::move(flags));
  CliOptions out;
  std::string error;
  EXPECT_FALSE(ParseCli(args.argc(), args.argv(), out, error));
  EXPECT_FALSE(error.empty()) << "a rejection must say why";
  return error;
}

// Case-insensitive substring search, so assertions about an error message
// are not also assertions about its capitalisation.
bool Mentions(const std::string& haystack, const std::string& needle) {
  const auto lower = [](std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  return lower(haystack).find(lower(needle)) != std::string::npos;
}

}  // namespace

TEST(Cli, DefaultsToTwoHands) {
  EXPECT_EQ(ParseOk({}).max_hands, 2);
}

// The rest of the no-argument defaults, pinned alongside max_hands: running
// the binary bare has to be a complete, valid configuration.
TEST(Cli, NoArgumentsIsAValidConfiguration) {
  const CliOptions opts = ParseOk({});
  EXPECT_EQ(opts.source, SourceKind::Camera);
  EXPECT_EQ(opts.camera_index, 0);
  EXPECT_TRUE(opts.want_overlay);
  EXPECT_EQ(opts.frames, 0);  // 0 == until the user quits.
  EXPECT_TRUE(opts.report_path.empty());
}

TEST(Cli, ParsesSyntheticGeometry) {
  const CliOptions opts = ParseOk({"--synthetic", "640x360"});
  EXPECT_EQ(opts.source, SourceKind::Synthetic);
  EXPECT_EQ(opts.synthetic_width, 640);
  EXPECT_EQ(opts.synthetic_height, 360);
}

TEST(Cli, RejectsMalformedSyntheticGeometry) {
  EXPECT_TRUE(Mentions(ParseErr({"--synthetic", "640"}), "synthetic"));
  EXPECT_FALSE(ParseErr({"--synthetic", "640x"}).empty());
  EXPECT_FALSE(ParseErr({"--synthetic", "x360"}).empty());
  EXPECT_FALSE(ParseErr({"--synthetic", "640x360x2"}).empty());
  EXPECT_FALSE(ParseErr({"--synthetic", "axb"}).empty());
}

// A zero or negative dimension parses as a number but cannot produce a
// frame; SyntheticFrameSource requires both to be positive.
TEST(Cli, RejectsNonPositiveSyntheticGeometry) {
  EXPECT_FALSE(ParseErr({"--synthetic", "0x360"}).empty());
  EXPECT_FALSE(ParseErr({"--synthetic", "640x0"}).empty());
  EXPECT_FALSE(ParseErr({"--synthetic", "-640x360"}).empty());
}

TEST(Cli, RejectsTwoSourcesAtOnce) {
  const std::string error = ParseErr({"--camera", "0", "--video", "a.mp4"});
  // The message has to name the conflict rather than silently preferring one
  // source, which is the failure mode this test exists to prevent.
  EXPECT_TRUE(Mentions(error, "camera"));
  EXPECT_TRUE(Mentions(error, "video"));
}

TEST(Cli, RejectsEverySourceCombination) {
  EXPECT_FALSE(ParseErr({"--camera", "0", "--synthetic", "640x360"}).empty());
  EXPECT_FALSE(ParseErr({"--video", "a.mp4", "--synthetic", "640x360"}).empty());
  EXPECT_FALSE(ParseErr({"--synthetic", "640x360", "--camera", "1"}).empty());
}

TEST(Cli, RejectsUnknownFlag) {
  EXPECT_TRUE(Mentions(ParseErr({"--nope"}), "--nope"));
}

TEST(Cli, HeadlessImpliesNoOverlay) {
  EXPECT_FALSE(ParseOk({"--headless"}).want_overlay);
}

// Every value-taking flag given as the last argument. A parser that reads
// argv[i + 1] without checking i + 1 < argc walks off the end of argv, which
// is the single most common bug in hand-written argument parsing.
TEST(Cli, RejectsFlagWithMissingValue) {
  for (const std::string& flag :
       {"--camera", "--video", "--synthetic", "--max-hands", "--frames",
        "--threads", "--report"}) {
    const std::string error = ParseErr({flag});
    EXPECT_TRUE(Mentions(error, flag)) << "error for " << flag << ": " << error;
  }
}

TEST(Cli, RejectsNonNumericNumbers) {
  EXPECT_FALSE(ParseErr({"--camera", "abc"}).empty());
  EXPECT_FALSE(ParseErr({"--max-hands", "two"}).empty());
  EXPECT_FALSE(ParseErr({"--frames", "12x"}).empty());
  EXPECT_FALSE(ParseErr({"--threads", ""}).empty());
}

// HandTrackerOnnx throws for max_hands < 1 or > 32. Catching it here turns a
// mistyped flag into a message instead of an exception out of the
// constructor.
TEST(Cli, RejectsOutOfRangeMaxHands) {
  EXPECT_FALSE(ParseErr({"--max-hands", "0"}).empty());
  EXPECT_FALSE(ParseErr({"--max-hands", "-1"}).empty());
  EXPECT_FALSE(ParseErr({"--max-hands", "33"}).empty());
  EXPECT_EQ(ParseOk({"--max-hands", "1"}).max_hands, 1);
}

TEST(Cli, RejectsNonPositiveThreads) {
  EXPECT_FALSE(ParseErr({"--threads", "0"}).empty());
  EXPECT_FALSE(ParseErr({"--threads", "-4"}).empty());
  EXPECT_EQ(ParseOk({"--threads", "4"}).threads, 4);
}

// 0 is meaningful for --frames (run until the user quits), so it must be
// accepted where it is rejected for --max-hands and --threads.
TEST(Cli, AcceptsZeroFramesAsRunUntilQuit) {
  EXPECT_EQ(ParseOk({"--frames", "0"}).frames, 0);
  EXPECT_EQ(ParseOk({"--frames", "300"}).frames, 300);
  EXPECT_FALSE(ParseErr({"--frames", "-1"}).empty());
}

TEST(Cli, ParsesPathsVerbatim) {
  const CliOptions video = ParseOk({"--video", "/tmp/a b.avi"});
  EXPECT_EQ(video.source, SourceKind::Video);
  EXPECT_EQ(video.video_path, "/tmp/a b.avi");

  EXPECT_EQ(ParseOk({"--report", "/tmp/r.md"}).report_path, "/tmp/r.md");
}

TEST(Cli, ParsesCameraIndex) {
  const CliOptions opts = ParseOk({"--camera", "2"});
  EXPECT_EQ(opts.source, SourceKind::Camera);
  EXPECT_EQ(opts.camera_index, 2);
}

// The configuration Task 14's measurement step actually runs, parsed in one
// go: if the flags interfere with each other, this is where it shows.
TEST(Cli, ParsesAFullMeasurementCommandLine) {
  const CliOptions opts = ParseOk({"--synthetic", "1280x720", "--frames", "300",
                                   "--headless", "--max-hands", "2", "--threads",
                                   "4", "--report", "/tmp/out.md"});
  EXPECT_EQ(opts.source, SourceKind::Synthetic);
  EXPECT_EQ(opts.synthetic_width, 1280);
  EXPECT_EQ(opts.synthetic_height, 720);
  EXPECT_EQ(opts.frames, 300);
  EXPECT_FALSE(opts.want_overlay);
  EXPECT_EQ(opts.max_hands, 2);
  EXPECT_EQ(opts.threads, 4);
  EXPECT_EQ(opts.report_path, "/tmp/out.md");
}

// --- Task 11: the injection flags ----------------------------------------
//
// These are the flags that decide whether the program touches the user's
// desktop, so the parser is the place a mistake has to be caught: a typo that
// silently falls back to injecting is the one failure mode worth a test each.

// --no-inject is the DEFAULT. Development happens on the same desktop this
// program would seize, and a build that grabs the cursor the moment it starts
// is hostile to the person writing it.
TEST(Cli, DefaultsToNoInjection) {
  const CliOptions opts = ParseOk({});
  EXPECT_EQ(opts.sink, hc::SinkKind::Null);
  EXPECT_FALSE(opts.no_inject) << "the flag was not given";
  EXPECT_EQ(hc::ResolveSink(opts), hc::SinkKind::Null);
}

TEST(Cli, ParsesEachSinkName) {
  EXPECT_EQ(ParseOk({"--sink", "null"}).sink, hc::SinkKind::Null);
  EXPECT_EQ(ParseOk({"--sink", "log"}).sink, hc::SinkKind::Log);
  EXPECT_EQ(ParseOk({"--sink", "xtest"}).sink, hc::SinkKind::XTest);
}

TEST(Cli, RejectsAnUnknownSinkNamingIt) {
  const std::string error = ParseErr({"--sink", "wayland"});
  EXPECT_TRUE(Mentions(error, "wayland")) << error;
  EXPECT_TRUE(Mentions(error, "--sink")) << error;
}

// Order-independent on purpose: --no-inject is a safety override, and a
// safety override that depends on where it sits on the command line is not
// one.
TEST(Cli, NoInjectOverridesAnExplicitSink) {
  const CliOptions after = ParseOk({"--sink", "xtest", "--no-inject"});
  EXPECT_EQ(after.sink, hc::SinkKind::XTest) << "what was asked for is kept";
  EXPECT_TRUE(after.no_inject);
  EXPECT_EQ(hc::ResolveSink(after), hc::SinkKind::Null) << "what is used";

  const CliOptions before = ParseOk({"--no-inject", "--sink", "xtest"});
  EXPECT_EQ(hc::ResolveSink(before), hc::SinkKind::Null);
}

TEST(Cli, SensitivityAndSmoothingAreAbsentUntilGiven) {
  const CliOptions opts = ParseOk({});
  EXPECT_FALSE(opts.has_sensitivity);
  EXPECT_FALSE(opts.has_smoothing);
}

TEST(Cli, ParsesSensitivityAndSmoothing) {
  const CliOptions opts = ParseOk({"--sensitivity", "3.5", "--smoothing", "0"});
  EXPECT_TRUE(opts.has_sensitivity);
  EXPECT_FLOAT_EQ(opts.sensitivity, 3.5f);
  EXPECT_TRUE(opts.has_smoothing);
  EXPECT_FLOAT_EQ(opts.smoothing, 0.0f);
}

// The same ranges ParseBindings enforces on `set sensitivity` and
// `set smoothing`. Two paths to one setting must not disagree about what is
// valid, or a value accepted on the command line would be rejected the moment
// the bindings UI saved it.
TEST(Cli, RejectsNonPositiveSensitivity) {
  EXPECT_TRUE(Mentions(ParseErr({"--sensitivity", "0"}), "--sensitivity"));
  EXPECT_TRUE(Mentions(ParseErr({"--sensitivity", "-1.5"}), "--sensitivity"));
}

TEST(Cli, RejectsNonNumericSensitivity) {
  const std::string error = ParseErr({"--sensitivity", "fast"});
  EXPECT_TRUE(Mentions(error, "fast")) << error;
}

TEST(Cli, RejectsSmoothingOutsideZeroToOne) {
  EXPECT_TRUE(Mentions(ParseErr({"--smoothing", "1.5"}), "--smoothing"));
  EXPECT_TRUE(Mentions(ParseErr({"--smoothing", "-0.1"}), "--smoothing"));
}

// nan and inf parse as floating point and would otherwise sail through every
// range check: nan compares false against everything, inf passes "> 0".
TEST(Cli, RejectsNonFiniteNumbers) {
  EXPECT_FALSE(ParseErr({"--sensitivity", "nan"}).empty());
  EXPECT_FALSE(ParseErr({"--sensitivity", "inf"}).empty());
  EXPECT_FALSE(ParseErr({"--smoothing", "nan"}).empty());
}

TEST(Cli, ParsesTheBindingsPathVerbatim) {
  EXPECT_EQ(ParseOk({"--bindings", "/etc/hc/b.txt"}).bindings_path,
            "/etc/hc/b.txt");
  EXPECT_TRUE(ParseOk({}).bindings_path.empty())
      << "absent means the compiled-in default, not a path to look for";
}

// A configuration error the user cannot locate is a configuration that
// failed, so the message names the path rather than saying "could not read
// the bindings".
TEST(Cli, ReadTextFileNamesAMissingPath) {
  std::string text;
  std::string error;
  EXPECT_FALSE(hc::ReadTextFile("/nonexistent/hand-controller/bindings.txt",
                                text, error));
  EXPECT_TRUE(Mentions(error, "/nonexistent/hand-controller/bindings.txt"))
      << error;
}

TEST(Cli, ReadTextFileReadsAFileItCanOpen) {
  std::string text;
  std::string error;
  // The shipped configuration, whose path the build injects into this test.
  ASSERT_TRUE(hc::ReadTextFile(HC_DEFAULT_BINDINGS_PATH, text, error)) << error;
  EXPECT_TRUE(error.empty());
  EXPECT_NE(text.find("set sensitivity"), std::string::npos);
}
