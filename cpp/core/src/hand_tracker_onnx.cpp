#include "hc/hand_tracker_onnx.hpp"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

#include "hc/anchors.hpp"
#include "hc/association.hpp"
#include "hc/geometry.hpp"
#include "hc/palm_decode.hpp"
#include "hc/roi.hpp"
#include "hc/warp.hpp"

namespace hc {

namespace {

// Output tensor names, pinned from cpp/docs/models.md. These are matched
// exactly rather than sniffed from shapes: a rename means the fetched model
// is not the one this code was written against, which is a hard failure, not
// something to guess around.
constexpr const char* kPalmOutputs[] = {"Identity", "Identity_1"};
// The landmark model also declares "Identity_3" (world-space landmarks in
// metres). We deliberately do not request it: nothing in this phase consumes
// world landmarks, and omitting an output lets ORT prune the subgraph that
// produces it.
constexpr const char* kLandmarkOutputs[] = {"Identity", "Identity_1",
                                            "Identity_2"};

constexpr int kNumLandmarks = 21;

// Reads a square NHWC [1,S,S,3] input spec and returns S. Throws if the model
// does not have the static shape models.md records.
int SquareInputSide(Ort::Session& session, const char* label) {
  Ort::TypeInfo type_info = session.GetInputTypeInfo(0);
  const std::vector<int64_t> shape =
      type_info.GetTensorTypeAndShapeInfo().GetShape();
  if (shape.size() != 4 || shape[0] != 1 || shape[1] <= 0 ||
      shape[1] != shape[2] || shape[3] != 3) {
    throw std::runtime_error(std::string(label) +
                             ": expected static square NHWC input [1,S,S,3]");
  }
  return static_cast<int>(shape[1]);
}

std::string InputName(Ort::Session& session) {
  Ort::AllocatorWithDefaultOptions allocator;
  return std::string(session.GetInputNameAllocated(0, allocator).get());
}

// Maps one palm detection out of the detector's input-square space into
// frame-normalized space.
//
// DecodePalmDetections reports coordinates normalized to the 192x192 square
// the detector ran on, which is the letterboxed full frame -- NOT the frame
// itself. CropToFrame with that same square ROI is the exact inverse of the
// warp that produced it, so it is what takes those coordinates back to frame
// space before RoiFromPalm (which expects frame-normalized input) sees them.
//
// The box is mapped by its two opposite corners. That is exact only because
// the letterbox square always has rotation 0, so an axis-aligned box in crop
// space stays axis-aligned in frame space; for a rotated ROI it would not be,
// which is why this helper is private to the letterbox path.
PalmDetection PalmToFrameSpace(const PalmDetection& d, const RotatedRect& square,
                               float aspect) {
  PalmDetection out = d;
  const Point2 tl = CropToFrame(square, aspect, Point2{d.box.x, d.box.y});
  const Point2 br = CropToFrame(
      square, aspect, Point2{d.box.x + d.box.w, d.box.y + d.box.h});
  out.box.x = std::fmin(tl.x, br.x);
  out.box.y = std::fmin(tl.y, br.y);
  out.box.w = std::fabs(br.x - tl.x);
  out.box.h = std::fabs(br.y - tl.y);
  for (std::size_t i = 0; i < out.keypoints.size(); ++i) {
    out.keypoints[i] = CropToFrame(square, aspect, d.keypoints[i]);
  }
  return out;
}

// Ruling T8-G: the WarpOptions defaults happen to match what both models
// want, but they are set explicitly at every call site so that a future
// change to the defaults cannot silently move the models' input range or
// channel order. [0,1] range and RGB order are both pinned in models.md.
WarpOptions ModelWarpOptions(int side) {
  WarpOptions o;
  o.side = side;
  o.scale = 1.0f / 255.0f;
  o.bias = 0.0f;
  o.output_rgb = true;
  return o;
}

}  // namespace

class HandTrackerOnnx::Impl {
 public:
  Impl(const HandTrackerConfig& config, Metrics& metrics)
      : config_(config),
        metrics_(metrics),
        env_(ORT_LOGGING_LEVEL_WARNING, "hc_hand_tracker"),
        session_options_(MakeSessionOptions(config)),
        palm_session_(env_, config.palm_model_path.c_str(), session_options_),
        landmark_session_(env_, config.landmark_model_path.c_str(),
                          session_options_),
        memory_info_(
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault)),
        palm_input_name_(InputName(palm_session_)),
        landmark_input_name_(InputName(landmark_session_)),
        palm_side_(SquareInputSide(palm_session_, "palm model")),
        landmark_side_(SquareInputSide(landmark_session_, "landmark model")),
        // Anchors are a pure function of the model's fixed geometry, so they
        // are generated exactly once here. Regenerating 2016 anchors per
        // frame would put a few thousand float ops and a heap allocation on
        // the detection path for no benefit.
        anchors_(GenerateAnchors(PalmAnchorOptions())),
        palm_roi_opts_(DefaultPalmRoiOptions()),
        landmark_roi_opts_(DefaultLandmarkRoiOptions()) {
    // Task 8 scope is ONE hand. Rejecting max_hands > 1 outright, rather than
    // accepting it and quietly returning a single observation, so that Task
    // 10 (two-hand association) must consciously lift this guard instead of
    // discovering by experiment that the field was inert. HandTrackerConfig's
    // own default is 2 -- forward-looking, per the phase-1 brief -- so today
    // a caller must pass 1 explicitly.
    if (config.max_hands > 1) {
      throw std::invalid_argument(
          "HandTrackerOnnx: max_hands > 1 is not implemented in this phase "
          "(single-hand tracking only; multi-hand association is Task 10). "
          "Pass max_hands = 1.");
    }
    if (config.max_hands < 1) {
      throw std::invalid_argument("HandTrackerOnnx: max_hands must be >= 1");
    }

    palm_decode_opts_ = PalmDecodeDefaults();
    // The decoder caps at one result because this phase consumes one hand;
    // that keeps NMS from doing work whose result is discarded. Tasks 9/10
    // raise it to config_.max_hands once association exists.
    palm_decode_opts_.max_results = 1;

    // The palm input size is PINNED at 192, not adapted to whatever model is
    // loaded. Adapting is not actually achievable: the decode scales
    // (x/y/w/h_scale, palm_decode.cpp:80-83) and the whole anchor grid
    // (PalmAnchorOptions' strides {8,16,16,16} and min/max_scale) are tied to
    // this specific network's architecture, and a differently-sized palm
    // model would need a stride list that cannot be derived from its input
    // shape. Every one of those values is pinned verbatim from
    // cpp/docs/models.md. Note that PalmDecodeOptions::input_w/input_h are
    // read by nobody -- DecodePalmDetections never touches them -- so setting
    // them from the model would have looked adaptive while changing nothing.
    //
    // So instead of faking adaptivity, this asserts the pin: a swapped model
    // fails loudly at construction rather than decoding silent nonsense.
    if (palm_side_ != palm_decode_opts_.input_w ||
        palm_side_ != palm_decode_opts_.input_h) {
      throw std::runtime_error(
          "palm model input side is " + std::to_string(palm_side_) +
          ", but the decode scales and anchor grid are pinned to " +
          std::to_string(palm_decode_opts_.input_w) +
          " (cpp/docs/models.md). This code does not support a resized palm "
          "model.");
    }

    ValidatePalmOutputs();

    palm_input_.resize(static_cast<std::size_t>(palm_side_) * palm_side_ * 3u);
    landmark_input_.resize(static_cast<std::size_t>(landmark_side_) *
                           landmark_side_ * 3u);
    palm_input_shape_ = {1, palm_side_, palm_side_, 3};
    landmark_input_shape_ = {1, landmark_side_, landmark_side_, 3};
  }

  void Process(const ImageView& frame, std::int64_t timestamp_us,
               std::vector<HandObservation>& out) {
    out.clear();
    ScopedStage total(metrics_, Stage::Total);

    const float aspect =
        static_cast<float>(frame.width) / static_cast<float>(frame.height);

    if (!tracking_) {
      if (!DetectPalmAndSeedRoi(frame, aspect)) return;
    }

    RunLandmarks(frame, aspect, timestamp_us, out);
  }

  void Reset() {
    tracking_ = false;
  }

 private:
  static Ort::SessionOptions MakeSessionOptions(
      const HandTrackerConfig& config) {
    Ort::SessionOptions o;
    // Set explicitly from config rather than left at ORT's default, so the
    // measurement task can vary it and so timings are reproducible.
    o.SetIntraOpNumThreads(std::max(1, config.intra_op_threads));
    return o;
  }

  // Checks that the palm model's anchor-count dimension agrees with the
  // anchor grid we generated. These must line up row-for-row or every decoded
  // box is nonsense, and the failure would otherwise be silent.
  void ValidatePalmOutputs() {
    Ort::TypeInfo type_info = palm_session_.GetOutputTypeInfo(0);
    const std::vector<int64_t> shape =
        type_info.GetTensorTypeAndShapeInfo().GetShape();
    if (shape.size() != 3 || shape[2] != 18 ||
        shape[1] != static_cast<int64_t>(anchors_.size())) {
      throw std::runtime_error(
          "palm model output 0 shape does not match the generated anchor grid "
          "(expected [1," +
          std::to_string(anchors_.size()) + ",18])");
    }
  }

  // Returns true if a hand was found and roi_ was seeded.
  bool DetectPalmAndSeedRoi(const ImageView& frame, float aspect) {
    // Ruling T8-B: letterbox, not stretch. A full-frame rect squared on its
    // long side makes the short axis overhang the frame; the warp zero-fills
    // those samples (BORDER_ZERO), and 0 is inside the model's [0,1] input
    // range. models.md records the palm score rising 0.805 -> 0.907 on this
    // fixture when the stretch was replaced by exactly this letterbox.
    const RotatedRect full_frame{Point2{0.5f, 0.5f}, 1.0f, 1.0f, 0.0f};
    const RotatedRect square = SquareLong(full_frame, aspect);

    {
      ScopedStage s(metrics_, Stage::Preprocess);
      WarpToTensorNhwc(frame, square, ModelWarpOptions(palm_side_),
                       palm_input_.data());
    }

    std::vector<PalmDetection> detections;
    {
      // Ruling T8-A: ScopedStage only. Its destructor calls Metrics::Add,
      // which already increments `calls`; an additional Bump here would make
      // PalmDetect.calls two per invocation.
      ScopedStage s(metrics_, Stage::PalmDetect);

      const char* input_names[] = {palm_input_name_.c_str()};
      Ort::Value input = Ort::Value::CreateTensor<float>(
          memory_info_, palm_input_.data(), palm_input_.size(),
          palm_input_shape_.data(), palm_input_shape_.size());

      std::vector<Ort::Value> outputs =
          palm_session_.Run(Ort::RunOptions{nullptr}, input_names, &input, 1,
                            kPalmOutputs, 2);

      detections = DecodePalmDetections(outputs[0].GetTensorData<float>(),
                                        outputs[1].GetTensorData<float>(),
                                        anchors_, palm_decode_opts_);
    }

    if (detections.empty()) return false;

    const PalmDetection framed =
        PalmToFrameSpace(detections[0], square, aspect);
    roi_ = RoiFromPalm(framed, aspect, palm_roi_opts_);
    // Ruling T9-C: track ids now come from the process-global, atomic
    // NextTrackId() (association.hpp) instead of an instance-local counter,
    // so single-hand and future multi-hand tracks never collide on id.
    track_id_ = NextTrackId();
    tracking_ = true;
    return true;
  }

  void RunLandmarks(const ImageView& frame, float aspect,
                    std::int64_t timestamp_us,
                    std::vector<HandObservation>& out) {
    {
      ScopedStage s(metrics_, Stage::Preprocess);
      WarpToTensorNhwc(frame, roi_, ModelWarpOptions(landmark_side_),
                       landmark_input_.data());
    }

    std::array<Point2, kNumLandmarks> landmarks{};
    float presence = 0.0f;
    float handedness_score = 0.0f;

    {
      ScopedStage s(metrics_, Stage::Landmark);

      const char* input_names[] = {landmark_input_name_.c_str()};
      Ort::Value input = Ort::Value::CreateTensor<float>(
          memory_info_, landmark_input_.data(), landmark_input_.size(),
          landmark_input_shape_.data(), landmark_input_shape_.size());

      std::vector<Ort::Value> outputs = landmark_session_.Run(
          Ort::RunOptions{nullptr}, input_names, &input, 1, kLandmarkOutputs,
          3);

      const float* raw = outputs[0].GetTensorData<float>();
      // Ruling T8-D: Identity_1 and Identity_2 are ALREADY probabilities --
      // the model's own head applies the sigmoid (models.md "Activation").
      // Applying another one here would turn 0.95 into 0.72, a silent
      // accuracy bug. Contrast the palm detector, whose scores ARE logits and
      // whose sigmoid lives inside DecodePalmDetections.
      presence = outputs[1].GetTensorData<float>()[0];
      handedness_score = outputs[2].GetTensorData<float>()[0];

      // Ruling T8-C: output 0 is 21 x (x, y, z) with x and y in PIXELS
      // relative to the model's own 224x224 input, not normalized. Dividing
      // by the input side is what makes them crop-normalized 0..1, which is
      // the space CropToFrame inverts. z is a relative depth in the same
      // pixel scale and is not part of HandObservation, so it is dropped.
      const float inv_side = 1.0f / static_cast<float>(landmark_side_);
      for (int i = 0; i < kNumLandmarks; ++i) {
        const Point2 in_crop{raw[i * 3 + 0] * inv_side,
                             raw[i * 3 + 1] * inv_side};
        landmarks[static_cast<std::size_t>(i)] =
            CropToFrame(roi_, aspect, in_crop);
      }
    }

    if (presence < config_.min_presence) {
      // The hand is gone (or the ROI drifted off it). Drop the track so the
      // next frame goes back through the detector.
      tracking_ = false;
      return;
    }

    HandObservation obs{};
    obs.track_id = track_id_;
    // Ruling T8-E: which side of the 0.5 split means Left and which means
    // Right is NOT DETERMINED -- MediaPipe's handedness.txt label map has not
    // been fetched, and the convention additionally flips under input
    // mirroring (cpp/docs/models.md, "Handedness label order"). This
    // assignment is therefore a placeholder ordering: it guarantees a side is
    // assigned (never Unknown) but must not be trusted as Left-vs-Right until
    // Tasks 9/10 resolve the label map.
    obs.handedness =
        (handedness_score >= 0.5f) ? Handedness::Right : Handedness::Left;
    // Confidence in the chosen class, i.e. the distance of the binary score
    // from the 0.5 split, expressed as a probability in [0.5, 1].
    obs.handedness_score = (handedness_score >= 0.5f) ? handedness_score
                                                      : 1.0f - handedness_score;
    obs.presence = presence;
    obs.landmarks = landmarks;
    obs.timestamp_us = timestamp_us;
    out.push_back(obs);

    // Seed the next frame's ROI from these landmarks, which is what lets the
    // detector be skipped entirely while the track holds.
    roi_ = RoiFromLandmarks(landmarks, aspect, landmark_roi_opts_);
  }

  HandTrackerConfig config_;
  Metrics& metrics_;

  Ort::Env env_;
  Ort::SessionOptions session_options_;
  Ort::Session palm_session_;
  Ort::Session landmark_session_;
  Ort::MemoryInfo memory_info_;

  std::string palm_input_name_;
  std::string landmark_input_name_;
  int palm_side_;
  int landmark_side_;

  std::vector<Anchor> anchors_;
  PalmRoiOptions palm_roi_opts_;
  LandmarkRoiOptions landmark_roi_opts_;
  PalmDecodeOptions palm_decode_opts_{};

  // Input staging buffers, sized once in the constructor so no per-frame
  // allocation happens on either model path.
  std::vector<float> palm_input_;
  std::vector<float> landmark_input_;
  std::vector<int64_t> palm_input_shape_;
  std::vector<int64_t> landmark_input_shape_;

  // Tracking state.
  bool tracking_ = false;
  RotatedRect roi_{};
  std::uint32_t track_id_ = 0;
};

HandTrackerOnnx::HandTrackerOnnx(const HandTrackerConfig& config,
                                 Metrics& metrics)
    : impl_(new Impl(config, metrics)) {}

HandTrackerOnnx::~HandTrackerOnnx() = default;

void HandTrackerOnnx::Process(const ImageView& frame,
                              std::int64_t timestamp_us,
                              std::vector<HandObservation>& out) {
  impl_->Process(frame, timestamp_us, out);
}

void HandTrackerOnnx::Reset() { impl_->Reset(); }

}  // namespace hc
