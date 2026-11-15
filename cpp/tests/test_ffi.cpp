// Tests for the C ABI, driven THROUGH the C API only.
//
// Deliberately no C++ types from the project appear here: if a test needs
// hc::Controller to set something up, the ABI has a hole. Everything below
// uses a synthetic frame source and no sink, so these run with no camera, no
// display and nothing injected -- the same configuration a UI uses to bring
// its own plumbing up before any hardware exists.
//
// What these DO NOT cover, on purpose: whether a gesture means the right
// thing. That is test_controller's and test_gesture_recognizer's job, through
// a scripted tracker the C API has no business exposing. These cover the
// boundary -- layout, lifetimes, truncation, error reporting, threading.

#include "hc/hand_controller.h"

#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {

// Big enough for any message this library produces; the point of the test is
// the content, not the truncation (which has its own case).
constexpr int kErrorCapacity = 1024;

hc_config SyntheticConfig() {
  hc_config config{};
  hc_config_defaults(&config);
  config.source = HC_SOURCE_SYNTHETIC;
  config.synthetic_width = 160;
  config.synthetic_height = 120;
  // One thread and one hand: these tests measure plumbing, and the palm
  // detector re-running for an empty second slot would only make them slower.
  config.max_hands = 1;
  config.threads = 1;
  return config;
}

// Owns a session and makes the common failure legible: a creation failure
// reports the library's own message rather than a null dereference later.
class Session {
 public:
  bool Open(const hc_config& config) {
    char error[kErrorCapacity] = {0};
    handle_ = hc_session_create(&config, error, kErrorCapacity);
    reason_ = error;
    return handle_ != nullptr;
  }

  ~Session() { hc_session_destroy(handle_); }

  hc_session* get() const { return handle_; }
  const std::string& reason() const { return reason_; }

 private:
  hc_session* handle_ = nullptr;
  std::string reason_;
};

// Polls until a frame appears, up to `timeout_ms`. Returns the frame index, or
// 0 if none arrived -- which on a synthetic source means something is wrong
// rather than merely slow.
std::uint64_t WaitForFrame(hc_session* session, int timeout_ms = 4000) {
  std::vector<hc_hand> hands(4);
  std::vector<hc_event> events(8);
  std::vector<hc_command> commands(8);

  for (int waited = 0; waited < timeout_ms; waited += 10) {
    hc_snapshot snapshot{};
    snapshot.hands = hands.data();
    snapshot.hand_capacity = static_cast<int32_t>(hands.size());
    snapshot.events = events.data();
    snapshot.event_capacity = static_cast<int32_t>(events.size());
    snapshot.commands = commands.data();
    snapshot.command_capacity = static_cast<int32_t>(commands.size());

    if (hc_session_poll(session, &snapshot) == 1 && snapshot.frame_index > 0) {
      return snapshot.frame_index;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return 0;
}

}  // namespace

// --- the ABI itself ------------------------------------------------------

// A mismatched .so is otherwise silent until a field lands in the wrong
// place, which is the worst possible way to find out.
TEST(Ffi, AbiVersionMatchesTheHeaderItWasBuiltAgainst) {
  EXPECT_EQ(hc_abi_version(), HC_ABI_VERSION);
}

// The names are the bindings file's own vocabulary. A UI rendering a gesture
// must show the same word a user would type into the configuration, so these
// come from the library rather than from a table in Dart.
TEST(Ffi, NameTablesSpeakTheBindingsVocabulary) {
  EXPECT_STREQ(hc_gesture_name(HC_GESTURE_POINT_INDEX), "point_index");
  EXPECT_STREQ(hc_gesture_name(HC_GESTURE_TWO_FINGERS), "two_fingers");
  EXPECT_STREQ(hc_gesture_name(HC_GESTURE_FOUR_FINGERS), "four_fingers");
  EXPECT_STREQ(hc_phase_name(HC_PHASE_HELD), "held");
  EXPECT_STREQ(hc_command_name(HC_COMMAND_MOVE_CURSOR), "move_cursor");
  EXPECT_STREQ(hc_command_name(HC_COMMAND_KEY_RELEASE), "key_release");
  EXPECT_STREQ(hc_button_name(HC_BUTTON_MIDDLE), "middle");
  EXPECT_STREQ(hc_key_name(HC_KEY_PRINT_SCREEN), "print_screen");
  EXPECT_STREQ(hc_handedness_name(HC_HANDEDNESS_LEFT), "left");
  EXPECT_STREQ(hc_sink_name(HC_SINK_UINPUT), "uinput");
}

// An out-of-range value must not index a table. A UI passing a value from a
// newer library would otherwise read whatever followed it in memory.
TEST(Ffi, NameTablesAreSafeForValuesOutOfRange) {
  EXPECT_NE(hc_gesture_name(-1), nullptr);
  EXPECT_NE(hc_gesture_name(9999), nullptr);
  EXPECT_NE(hc_phase_name(42), nullptr);
  EXPECT_NE(hc_command_name(-7), nullptr);
  EXPECT_NE(hc_key_name(1000), nullptr);
  EXPECT_NE(hc_button_name(-2), nullptr);
}

// The default must not touch the user's desktop. A library that injects input
// on creation is hostile, and this is the line that guarantees it does not.
TEST(Ffi, DefaultsInjectNothing) {
  hc_config config{};
  hc_config_defaults(&config);
  EXPECT_EQ(config.sink, HC_SINK_NONE);
  EXPECT_EQ(config.source, HC_SOURCE_CAMERA);
  EXPECT_EQ(config.max_hands, 2);
  EXPECT_EQ(config.bindings_text, nullptr);
  // 0 means "leave the file's value", and 0 is not a legal sensitivity --
  // the parser rejects it too, so there is no ambiguity.
  EXPECT_FLOAT_EQ(config.sensitivity, 0.0f);
  EXPECT_LT(config.smoothing, 0.0f) << "0 is a legal smoothing, so the "
                                       "'unset' marker has to be negative";
}

TEST(Ffi, NullArgumentsAreToleratedEverywhere) {
  hc_config_defaults(nullptr);
  hc_session_destroy(nullptr);
  hc_session_stop(nullptr);
  EXPECT_EQ(hc_session_start(nullptr), 0);
  EXPECT_EQ(hc_session_running(nullptr), 0);
  EXPECT_EQ(hc_session_poll(nullptr, nullptr), 0);
  EXPECT_EQ(hc_session_paused(nullptr), 0);
  EXPECT_EQ(hc_session_sink_available(nullptr), 0);
  EXPECT_EQ(hc_session_copy_frame(nullptr, nullptr, 0, nullptr, nullptr), 0);
  EXPECT_EQ(hc_session_bindings_text(nullptr, nullptr, 0), 0);
  EXPECT_EQ(hc_session_last_error(nullptr, nullptr, 0), 0);

  char error[kErrorCapacity] = {0};
  EXPECT_EQ(hc_session_create(nullptr, error, kErrorCapacity), nullptr);
  EXPECT_GT(std::strlen(error), 0u);
}

TEST(Ffi, DefaultBindingsTextIsTheShippedConfiguration) {
  const char* text = hc_default_bindings_text();
  ASSERT_NE(text, nullptr);
  const std::string bindings = text;
  EXPECT_NE(bindings.find("set sensitivity"), std::string::npos);
  EXPECT_NE(bindings.find("right point_index  held  -> move_cursor"),
            std::string::npos);
}

// --- creation ------------------------------------------------------------

// Bindings are validated BEFORE a camera is touched or a model is loaded, so
// a typo costs a millisecond rather than a camera open.
TEST(Ffi, CreateRejectsBadBindingsAndSaysWhichLine) {
  hc_config config = SyntheticConfig();
  config.bindings_text = "right point_index held -> fly_away\n";

  char error[kErrorCapacity] = {0};
  EXPECT_EQ(hc_session_create(&config, error, kErrorCapacity), nullptr);
  const std::string message = error;
  EXPECT_NE(message.find("line 1"), std::string::npos) << message;
  EXPECT_NE(message.find("fly_away"), std::string::npos) << message;
}

TEST(Ffi, CreateWithASyntheticSourceAndNoSinkSucceeds) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();
  EXPECT_TRUE(session.reason().empty());

  // No sink is a supported configuration, so "available" is honestly false.
  EXPECT_EQ(hc_session_sink_available(session.get()), 0);
  EXPECT_EQ(hc_session_running(session.get()), 0);
}

// A truncated message must still be a C string. A caller handing over a tiny
// buffer gets less text, never an unterminated one.
TEST(Ffi, ErrorMessagesAreTruncatedButAlwaysTerminated) {
  hc_config config = SyntheticConfig();
  config.bindings_text = "nonsense\n";

  char tiny[8];
  std::memset(tiny, 'x', sizeof(tiny));
  EXPECT_EQ(hc_session_create(&config, tiny, static_cast<int32_t>(sizeof(tiny))),
            nullptr);
  EXPECT_EQ(tiny[sizeof(tiny) - 1], '\0');
  EXPECT_LT(std::strlen(tiny), sizeof(tiny));
}

// --- running -------------------------------------------------------------

TEST(Ffi, PollBeforeStartReportsNoFrame) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();

  hc_hand hands[2]{};
  hc_snapshot snapshot{};
  snapshot.hands = hands;
  snapshot.hand_capacity = 2;
  EXPECT_EQ(hc_session_poll(session.get(), &snapshot), 0);
  EXPECT_EQ(snapshot.hand_count, 0);
}

TEST(Ffi, StartRunsTheWorkerAndFramesAdvance) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();
  ASSERT_EQ(hc_session_start(session.get()), 1);
  EXPECT_EQ(hc_session_running(session.get()), 1);

  const std::uint64_t first = WaitForFrame(session.get());
  ASSERT_GT(first, 0u) << "no frame arrived from a synthetic source";

  // The index is monotonic, and the caller polling faster than the pipeline
  // steps sees repeats rather than gaps -- poll is not a queue.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  const std::uint64_t later = WaitForFrame(session.get());
  EXPECT_GE(later, first);

  hc_session_stop(session.get());
  EXPECT_EQ(hc_session_running(session.get()), 0);
}

TEST(Ffi, StartTwiceIsRefusedWithAReason) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();
  ASSERT_EQ(hc_session_start(session.get()), 1);
  EXPECT_EQ(hc_session_start(session.get()), 0);

  char error[kErrorCapacity] = {0};
  EXPECT_GT(hc_session_last_error(session.get(), error, kErrorCapacity), 0);
  EXPECT_GT(std::strlen(error), 0u);
  hc_session_stop(session.get());
}

TEST(Ffi, StopIsIdempotentAndSafeBeforeStart) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();
  hc_session_stop(session.get());
  hc_session_stop(session.get());
  EXPECT_EQ(hc_session_running(session.get()), 0);
}

// The caller owns the buffers, so a buffer too small must TRUNCATE visibly
// rather than overflow. A count equal to the capacity is the caller's signal.
TEST(Ffi, PollTruncatesToTheCapacityGiven) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();
  ASSERT_EQ(hc_session_start(session.get()), 1);
  ASSERT_GT(WaitForFrame(session.get()), 0u);

  // Null arrays with zero capacity: a caller that wants only the metrics.
  hc_snapshot metrics_only{};
  ASSERT_EQ(hc_session_poll(session.get(), &metrics_only), 1);
  EXPECT_EQ(metrics_only.hand_count, 0);
  EXPECT_EQ(metrics_only.event_count, 0);
  EXPECT_EQ(metrics_only.command_count, 0);
  EXPECT_GT(metrics_only.frame_index, 0u);
  EXPECT_GT(metrics_only.frame_width, 0);
  EXPECT_GT(metrics_only.frame_height, 0);

  hc_session_stop(session.get());
}

TEST(Ffi, MetricsAreReportedAndPlausible) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();
  ASSERT_EQ(hc_session_start(session.get()), 1);
  ASSERT_GT(WaitForFrame(session.get()), 0u);

  hc_snapshot snapshot{};
  ASSERT_EQ(hc_session_poll(session.get(), &snapshot), 1);
  EXPECT_GT(snapshot.fps, 0.0);
  EXPECT_LT(snapshot.fps, 10000.0);
  // Work, not wall time: the capture wait is excluded, so this cannot be
  // negative and should not be absurd.
  EXPECT_GE(snapshot.latency_ms, 0.0);
  EXPECT_LT(snapshot.latency_ms, 1000.0);
  EXPECT_EQ(snapshot.frame_width, 160);
  EXPECT_EQ(snapshot.frame_height, 120);

  hc_session_stop(session.get());
}

// --- the frame ------------------------------------------------------------

TEST(Ffi, CopyFrameReportsItsSizeThenFillsRgba) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();
  ASSERT_EQ(hc_session_start(session.get()), 1);
  ASSERT_GT(WaitForFrame(session.get()), 0u);

  int32_t width = 0;
  int32_t height = 0;
  // Asking with no buffer reports the size needed, so a caller sizes once.
  const int32_t needed =
      hc_session_copy_frame(session.get(), nullptr, 0, &width, &height);
  ASSERT_GT(needed, 0);
  EXPECT_EQ(width, 160);
  EXPECT_EQ(height, 120);
  EXPECT_EQ(needed, width * height * 4) << "RGBA, tightly packed";

  // A buffer one byte short must write nothing and still report the size.
  std::vector<std::uint8_t> small(static_cast<std::size_t>(needed) - 1, 0x5a);
  EXPECT_EQ(hc_session_copy_frame(session.get(), small.data(),
                                  needed - 1, nullptr, nullptr),
            needed);
  EXPECT_EQ(small[0], 0x5a) << "a short buffer must not be partly written";

  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(needed), 0);
  EXPECT_EQ(hc_session_copy_frame(session.get(), pixels.data(), needed,
                                  nullptr, nullptr),
            needed);

  // Alpha is opaque on every pixel: a UI uploading this must not get a
  // transparent image because the fourth channel was left at zero.
  for (std::size_t i = 3; i < pixels.size(); i += 4) {
    ASSERT_EQ(pixels[i], 255) << "alpha at byte " << i;
  }

  hc_session_stop(session.get());
}

TEST(Ffi, CopyFrameBeforeAnyFrameReportsNothing) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();
  EXPECT_EQ(hc_session_copy_frame(session.get(), nullptr, 0, nullptr, nullptr),
            0);
}

// --- pause and the sink ---------------------------------------------------

TEST(Ffi, PauseIsReflectedAndReversible) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();
  ASSERT_EQ(hc_session_start(session.get()), 1);

  EXPECT_EQ(hc_session_paused(session.get()), 0);
  hc_session_set_paused(session.get(), 1);
  EXPECT_EQ(hc_session_paused(session.get()), 1);

  // A paused session keeps stepping: the user is still watching their hand.
  const std::uint64_t during = WaitForFrame(session.get());
  EXPECT_GT(during, 0u);

  hc_session_set_paused(session.get(), 0);
  EXPECT_EQ(hc_session_paused(session.get()), 0);
  hc_session_stop(session.get());
}

TEST(Ffi, SwappingToTheLogSinkMakesItAvailable) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();
  ASSERT_EQ(hc_session_sink_available(session.get()), 0);

  char error[kErrorCapacity] = {0};
  ASSERT_EQ(hc_session_set_sink(session.get(), HC_SINK_LOG, error,
                                kErrorCapacity),
            1)
      << error;
  EXPECT_EQ(hc_session_sink_available(session.get()), 1);

  // And back to nothing, which is preview mode and a supported state.
  ASSERT_EQ(hc_session_set_sink(session.get(), HC_SINK_NONE, error,
                                kErrorCapacity),
            1);
  EXPECT_EQ(hc_session_sink_available(session.get()), 0);
}

// A refused sink must not silently downgrade the session: a UI that asked to
// inject and got preview mode would show a green light over a dead cursor.
TEST(Ffi, ARefusedSinkLeavesThePreviousOneInPlace) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();

  char error[kErrorCapacity] = {0};
  ASSERT_EQ(hc_session_set_sink(session.get(), HC_SINK_LOG, error,
                                kErrorCapacity),
            1);
  ASSERT_EQ(hc_session_sink_available(session.get()), 1);

  EXPECT_EQ(hc_session_set_sink(session.get(), 99, error, kErrorCapacity), 0);
  EXPECT_GT(std::strlen(error), 0u);
  EXPECT_EQ(hc_session_sink_available(session.get()), 1)
      << "the working sink must still be attached";
}

// --- bindings -------------------------------------------------------------

TEST(Ffi, BindingsTextRoundTripsThroughTheBoundary) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();

  const int32_t needed =
      hc_session_bindings_text(session.get(), nullptr, 0);
  ASSERT_GT(needed, 1);

  std::vector<char> text(static_cast<std::size_t>(needed), '\0');
  ASSERT_EQ(hc_session_bindings_text(session.get(), text.data(), needed),
            needed);
  const std::string serialized = text.data();
  EXPECT_NE(serialized.find("move_cursor"), std::string::npos);

  // What came out must go back in. This is the property that lets a UI edit
  // bindings without implementing the format.
  char error[kErrorCapacity] = {0};
  EXPECT_EQ(hc_session_set_bindings_text(session.get(), serialized.c_str(),
                                         error, kErrorCapacity),
            1)
      << error;
}

TEST(Ffi, AnEmptyBindingSetIsValidNotAnError) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();

  char error[kErrorCapacity] = {0};
  EXPECT_EQ(hc_session_set_bindings_text(session.get(), "", error,
                                         kErrorCapacity),
            1)
      << error;

  const int32_t needed = hc_session_bindings_text(session.get(), nullptr, 0);
  std::vector<char> text(static_cast<std::size_t>(needed), '\0');
  hc_session_bindings_text(session.get(), text.data(), needed);
  EXPECT_EQ(std::string(text.data()).find("move_cursor"), std::string::npos);
}

// A rejected edit must not disarm a working configuration. The UI's own
// validation is the first line of defence; this is the backstop behind it.
TEST(Ffi, RejectedBindingsLeaveThePreviousSetInPlace) {
  Session session;
  ASSERT_TRUE(session.Open(SyntheticConfig())) << session.reason();

  const int32_t before_size =
      hc_session_bindings_text(session.get(), nullptr, 0);
  std::vector<char> before(static_cast<std::size_t>(before_size), '\0');
  hc_session_bindings_text(session.get(), before.data(), before_size);

  char error[kErrorCapacity] = {0};
  // A key_press with no matching release: rejected, and the most dangerous
  // configuration mistake in the project.
  EXPECT_EQ(hc_session_set_bindings_text(
                session.get(), "left thumb_up began -> key_press arrow_right\n",
                error, kErrorCapacity),
            0);
  EXPECT_GT(std::strlen(error), 0u) << "a rejection must say why";

  const int32_t after_size =
      hc_session_bindings_text(session.get(), nullptr, 0);
  std::vector<char> after(static_cast<std::size_t>(after_size), '\0');
  hc_session_bindings_text(session.get(), after.data(), after_size);
  EXPECT_STREQ(before.data(), after.data());
}

// --- discovery ------------------------------------------------------------

TEST(Ffi, EnumerateCamerasReportsTheCountEvenWithoutABuffer) {
  const int32_t count = hc_enumerate_cameras(nullptr, 0);
  ASSERT_GE(count, 0);
  if (count == 0) GTEST_SKIP() << "no /dev/video* on this machine";

  std::vector<hc_camera> cameras(static_cast<std::size_t>(count));
  EXPECT_EQ(hc_enumerate_cameras(cameras.data(), count), count);
  for (const hc_camera& camera : cameras) {
    EXPECT_GE(camera.index, 0);
    EXPECT_GT(std::strlen(camera.path), 0u);
    EXPECT_GT(std::strlen(camera.name), 0u) << "a nameless camera is "
                                               "unpickable in a UI";
  }
}

TEST(Ffi, EnumerateCamerasTruncatesWithoutLosingTheCount) {
  const int32_t count = hc_enumerate_cameras(nullptr, 0);
  if (count == 0) GTEST_SKIP() << "no /dev/video* on this machine";

  hc_camera one{};
  EXPECT_EQ(hc_enumerate_cameras(&one, 1), count)
      << "the return value is how many exist, not how many were written";
}
