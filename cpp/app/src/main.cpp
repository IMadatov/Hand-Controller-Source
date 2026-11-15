// The phase 1 application shell: wire a frame source to the tracker, draw
// the result, and report where the time went.
//
// Deliberately thin. Everything it does is composition -- every piece of
// behaviour lives in a tested unit behind it, and this file holds only the
// loop that joins them.

#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cli.hpp"
#include "opencv_frame_source.hpp"
#include "overlay.hpp"

#include "hc/frame_source.hpp"
#include "hc/hand_observation.hpp"
#include "hc/hand_tracker.hpp"
#include "hc/hand_tracker_onnx.hpp"
#include "hc/latest_frame_source.hpp"
#include "hc/metrics.hpp"
#include "hc/synthetic_frame_source.hpp"

namespace {

// Builds the frame source the options ask for, or nullptr with a message on
// stderr. The failure cases differ per source, so each one explains itself
// rather than sharing a vague "could not open source".
std::unique_ptr<hc::IFrameSource> MakeSource(const hc::CliOptions& opts) {
  switch (opts.source) {
    case hc::SourceKind::Synthetic:
      return std::make_unique<hc::SyntheticFrameSource>(
          opts.synthetic_width, opts.synthetic_height, /*frame_count=*/-1);

    case hc::SourceKind::Video: {
      auto source = hc::OpenCvFrameSource::OpenFile(opts.video_path);
      if (!source) {
        std::cerr << "error: could not open video file " << opts.video_path
                  << "\n";
      }
      return source;
    }

    case hc::SourceKind::Camera: {
      hc::CameraOptions camera;
      camera.device_index = opts.camera_index;
      auto source = hc::OpenCvFrameSource::OpenCamera(camera);
      if (!source) {
        std::cerr << "error: could not open camera " << opts.camera_index
                  << "\n";
      }
      return source;
    }
  }
  return nullptr;
}

// A one-line summary of the run, drawn into the overlay so what is on screen
// says what produced it.
std::string BuildHud(const hc::CliOptions& opts, std::uint64_t frame,
                     std::size_t hands, std::uint64_t dropped) {
  return "frame " + std::to_string(frame) + "  hands " + std::to_string(hands) +
         "/" + std::to_string(opts.max_hands) + "  dropped " +
         std::to_string(dropped);
}

}  // namespace

int main(int argc, char** argv) {
  hc::CliOptions opts;
  std::string error;
  if (!hc::ParseCli(argc, argv, opts, error)) {
    std::cerr << "error: " << error << "\n\n" << hc::CliUsage();
    return 2;
  }
  if (opts.want_help) {
    std::cout << hc::CliUsage();
    return 0;
  }

  std::unique_ptr<hc::IFrameSource> source = MakeSource(opts);
  if (!source) return 1;

  // Wrapped so capture runs on its own thread and the loop below always gets
  // the newest frame rather than a backlog of stale ones. The dropped count
  // is part of the measurement, not an error.
  auto latest = std::make_unique<hc::LatestFrameSource>(std::move(source));

  hc::Metrics metrics;

  hc::HandTrackerConfig config;
  config.palm_model_path = HC_PALM_MODEL_PATH;
  config.landmark_model_path = HC_LANDMARK_MODEL_PATH;
  config.max_hands = opts.max_hands;
  config.intra_op_threads = opts.threads;

  // The tracker loads both models in its constructor and throws if either is
  // missing or has an unexpected layout. That is a startup failure with a
  // useful message, not a crash.
  std::unique_ptr<hc::IHandTracker> tracker;
  try {
    tracker = std::make_unique<hc::HandTrackerOnnx>(config, metrics);
  } catch (const std::exception& e) {
    std::cerr << "error: could not initialise the tracker: " << e.what() << "\n";
    return 1;
  }

  std::unique_ptr<hc::Overlay> overlay;
  if (opts.want_overlay) overlay = std::make_unique<hc::Overlay>("hand_controller");

  // Held across the loop and refilled by Process, so the steady state does
  // not allocate per frame.
  std::vector<hc::HandObservation> observations;

  // Wall-clock span of the whole loop. Reported alongside the per-stage
  // table because it is the only number here that includes everything --
  // capture waits, inference, drawing and loop overhead together -- and so
  // is the one the achieved frame rate has to be derived from.
  const auto run_start = std::chrono::high_resolution_clock::now();

  std::uint64_t processed = 0;
  while (opts.frames == 0 || processed < static_cast<std::uint64_t>(opts.frames)) {
    hc::Frame frame;

    // Timed separately from the rest: CaptureWait is time the pipeline spent
    // waiting on the camera, which is not a cost our code can optimise and
    // would distort every other stage if folded into them.
    const auto wait_start = std::chrono::high_resolution_clock::now();
    const bool got = latest->Next(frame);
    const auto wait_end = std::chrono::high_resolution_clock::now();
    metrics.Add(hc::Stage::CaptureWait,
                std::chrono::duration<double, std::milli>(wait_end - wait_start)
                    .count());

    if (!got) break;  // Source ended or failed; terminal, per IFrameSource.

    // NOT timed as Stage::Total here: HandTrackerOnnx::Process already
    // records that stage itself. Adding a second sample per frame would push
    // two different quantities into one histogram and silently corrupt the
    // mean and the percentiles, which is exactly what the per-stage
    // breakdown exists to avoid.
    tracker->Process(frame.view, frame.timestamp_us, observations);

    ++processed;

    if (overlay) {
      overlay->Render(frame.view, observations,
                      BuildHud(opts, processed, observations.size(),
                               latest->dropped()));
      if (overlay->ShouldQuit()) break;
    }
  }

  const auto run_end = std::chrono::high_resolution_clock::now();
  latest->Close();

  const double elapsed_ms =
      std::chrono::duration<double, std::milli>(run_end - run_start).count();

  // Built once and written to both sinks, so the file and the terminal can
  // never disagree about what the run measured. Format() does not end in a
  // newline, hence the explicit one.
  std::ostringstream summary;
  summary << metrics.Format() << "\n"
          << "frames processed: " << processed << "\n"
          << "frames dropped:   " << latest->dropped() << "\n";
  summary << std::fixed << std::setprecision(3)
          << "wall clock:       " << elapsed_ms << " ms\n";
  if (processed > 0 && elapsed_ms > 0.0) {
    summary << "per frame:        " << elapsed_ms / static_cast<double>(processed)
            << " ms\n"
            << "frame rate:       "
            << 1000.0 * static_cast<double>(processed) / elapsed_ms << " fps\n";
  }

  std::cout << summary.str();

  if (!opts.report_path.empty()) {
    std::ofstream file(opts.report_path);
    if (!file) {
      std::cerr << "error: could not write the report to " << opts.report_path
                << "\n";
      return 1;
    }
    file << summary.str();
  }

  return 0;
}
