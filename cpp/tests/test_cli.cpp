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
