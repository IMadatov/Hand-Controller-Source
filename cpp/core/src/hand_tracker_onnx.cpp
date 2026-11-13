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
// Wraps an axis-aligned Rect as a RotatedRect with zero rotation, so that
// AxisAlignedBounds() returns it back unchanged.
RotatedRect Unrotated(const Rect& r) {
  return RotatedRect{Point2{r.x + r.w * 0.5f, r.y + r.h * 0.5f}, r.w, r.h,
                     0.0f};
}

// The rect used to decide "is this detection the hand this slot is already
// tracking", derived from the slot's current landmarks.
//
// MEASURED, and the reason this is NOT simply the slot's crop ROI. The
// association layer compares AxisAlignedBounds(slot.roi) against the palm
// detector's box, so the two must be commensurable extents. The crop ROI is
// not: LandmarkRoiOptions scales by 2.0 and squares on the long side, which
// on the committed fixture inflates it to ~13x the area of the palm box it
// would be compared against (0.206 -> 2.06 vs the box's 0.152). IoU is
// bounded above by the area ratio, so the TRUE pair measured 0.074 -- below
// the 0.3 association threshold, i.e. a correct match was structurally
// impossible and every re-detection stacked a second slot onto a hand that
// was already tracked.
//
// Using the tight bounding box of the same 12-point landmark subset
// MediaPipe's ROI calculator uses (fingertips excluded) makes the comparison
// like-for-like. Measured on the two-hand composite: 0.557 / 0.597 for the
// true pairs and 0.000 / 0.000 for the false pairs -- a clean separation
// either side of 0.3. (Comparing inflated ROI to inflated ROI also clears the
// threshold, but at 0.887 true vs 0.239 false it leaves only 0.06 of margin
// on the false side, because two 2.6x-scaled ROIs overlap heavily even for
// different hands. The tight boxes are the safer signal.)
Rect TightLandmarkBox(const std::array<Point2, kNumLandmarks>& lm,
                      const LandmarkRoiOptions& o) {
  std::array<Point2, kMaxBboxIndices> subset{};
  for (int i = 0; i < o.bbox_index_count; ++i) {
    subset[static_cast<std::size_t>(i)] =
        lm[static_cast<std::size_t>(o.bbox_indices[static_cast<std::size_t>(i)])];
  }
  return BoundingBox(subset.data(), static_cast<std::size_t>(o.bbox_index_count));
}

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
    // Task 8's guard ("max_hands > 1 is not implemented") is DELIBERATELY
    // GONE: Task 10 is the task that was required to lift it consciously.
    // max_hands is now honoured for real, one TrackSlot per hand.
    if (config.max_hands < 1) {
      throw std::invalid_argument("HandTrackerOnnx: max_hands must be >= 1");
    }
    // The upper bound is not arbitrary and not a policy choice: it is derived
    // from the association layer. AssignDetectionsToSlots considers only the
    // first kMaxAssociationEntries slots AND the first kMaxAssociationEntries
    // detections, silently ignoring the rest. Since max_results below asks
    // the decoder for up to 2*max_hands - 1 detections, keeping
    // 2*max_hands - 1 <= kMaxAssociationEntries is what guarantees no slot
    // and no detection is ever dropped unseen. Rejecting loudly beats
    // misassociating quietly.
    if (static_cast<std::size_t>(2 * config.max_hands - 1) >
        kMaxAssociationEntries) {
      throw std::invalid_argument(
          "HandTrackerOnnx: max_hands exceeds what the association layer can "
          "handle (2*max_hands-1 must be <= " +
          std::to_string(kMaxAssociationEntries) + ")");
    }

    slots_.assign(static_cast<std::size_t>(config.max_hands), Slot{});
    assoc_slots_.reserve(slots_.size());

    palm_decode_opts_ = PalmDecodeDefaults();
    // How many detections the decoder is asked for, and why it is NOT just
    // max_hands. The detector runs over the whole frame, so it re-finds the
    // hands that are ALREADY tracked; those detections are matched to their
    // live slots and discarded, and only the leftovers can fill free slots.
    // In the worst case max_hands - 1 slots are live and each consumes one
    // detection, while max_hands - (max_hands - 1) = 1 slot still needs
    // filling, and the free slot's hand may rank below all of them. Asking
    // for max_hands would therefore let a frame end with a free slot and an
    // untracked hand in view. 2*max_hands - 1 is the smallest cap that cannot
    // starve a free slot this way; it is a tracking-policy number, not a model
    // parameter (models.md records no max_num_detections).
    palm_decode_opts_.max_results = 2 * config.max_hands - 1;

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

    // ORDER MATTERS, and it is Task 8's order, generalized: the decision to
    // run the detector is made from the slot state at frame ENTRY, before any
    // landmark model runs. So a slot that dies to low presence during THIS
    // frame re-triggers detection on the NEXT frame, not on this one
    // (ruling T10-F; HandTracker.ReDetectsAfterHandDisappears pins the same
    // ordering for the single-hand case).
    //
    // Ruling T10-D, a performance fact the measurement task needs: this fires
    // the palm detector on EVERY frame for as long as any slot is empty. A
    // scene with one hand visible and max_hands = 2 therefore pays for palm
    // detection every single frame, indefinitely. That matches MediaPipe and
    // is deliberate in phase 1 -- there is no throttle here -- but it means a
    // mean "PalmDetect stage time" is a misleading summary. Task 14 must
    // report PalmDetect CALLS PER FRAME alongside it.
    if (AnySlotInactive()) DetectAndFillFreeSlots(frame, aspect);

    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (!slots_[i].t.active) continue;
      RunLandmarksForSlot(i, frame, aspect, timestamp_us, out);
    }
  }

  void Reset() {
    for (Slot& s : slots_) s = Slot{};
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

  bool AnySlotInactive() const {
    for (const Slot& s : slots_) {
      if (!s.t.active) return true;
    }
    return false;
  }

  // Index of the first inactive slot, or -1 if every slot is taken. Returning
  // -1 is exactly the max_hands cap: once all slots are live, any remaining
  // unmatched detection is dropped.
  int FirstFreeSlot() const {
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      if (!slots_[i].t.active) return static_cast<int>(i);
    }
    return -1;
  }

  // Runs the palm detector ONCE over the whole frame and seeds every free
  // slot from the detections that do not already belong to a live slot.
  //
  // Ruling T10-C: "once over the whole frame" is the literal behaviour. This
  // is not scoped to the region where a hand went missing and it is not run
  // once per free slot -- the detector has no notion of which hand is
  // missing. Rejecting the hands we are already tracking is done afterwards,
  // by the association layer, which is the only part of the pipeline that
  // knows where the live slots are.
  void DetectAndFillFreeSlots(const ImageView& frame, float aspect) {
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

    if (detections.empty()) return;

    // Association runs entirely in FRAME-normalized space (association.hpp,
    // ruling T9-A); DecodePalmDetections reports the detector's 192x192
    // input-square space. Mapping first is mandatory, not cosmetic: on this
    // path the frame is never square, so feeding detector-space boxes to
    // AssignDetectionsToSlots would misassociate silently.
    framed_.clear();
    for (const PalmDetection& d : detections) {
      framed_.push_back(PalmToFrameSpace(d, square, aspect));
    }

    // The association layer takes a flat std::vector<TrackSlot>, so mirror
    // the slots' association halves into one. Rebuilt from slots_ on every
    // detect frame rather than kept in sync incrementally: there is exactly
    // one place the mirror can be wrong, and it is this loop.
    assoc_slots_.clear();
    for (const Slot& s : slots_) assoc_slots_.push_back(s.t);

    std::vector<int> assignment;
    {
      ScopedStage s(metrics_, Stage::Associate);
      // The REAL frame aspect, never a hardcoded constant (association.hpp).
      //
      // HONEST NOTE, so nobody "proves" this argument is dead: replacing
      // `aspect` with 1.0f here leaves the entire suite green, and that is a
      // mathematical identity rather than a gap in the tests. Every rect we
      // put in assoc_slots_ carries rotation 0, and AxisAlignedBounds scales
      // the x half-extent by `aspect` and then divides it back out again, so
      // for an unrotated rect the parameter cancels exactly. It stops
      // cancelling the moment a rotated rect is passed -- which is precisely
      // what a future change to the association rect would do -- and these
      // frames are 2.67:1, nowhere near square. Pass the real value.
      assignment = AssignDetectionsToSlots(assoc_slots_, framed_, aspect,
                                           kDefaultAssociationIou);
    }

    for (std::size_t i = 0; i < assignment.size(); ++i) {
      // Matched to a slot that is already tracking this hand: DISCARD. Using
      // it would stack a second slot onto a hand we already have, and the
      // genuinely untracked hand would never get a slot.
      if (assignment[i] >= 0) continue;

      const int free_slot = FirstFreeSlot();
      if (free_slot < 0) break;  // max_hands reached.

      Slot& slot = slots_[static_cast<std::size_t>(free_slot)];
      slot.crop_roi = RoiFromPalm(framed_[i], aspect, palm_roi_opts_);
      // Seed the association rect from the detection box, which is already
      // the right KIND of rect (tight and axis-aligned) to compare against
      // future detections. This is belt-and-braces rather than load-bearing
      // today: the landmark pass further down this same frame overwrites it
      // with the tight landmark box, and if that pass instead deactivates the
      // slot the value is never read. It exists so a slot is never
      // simultaneously `active` and carrying a stale or zero rect, which is
      // what the association layer would silently match against if the order
      // of the two passes ever changed.
      slot.t.roi = Unrotated(framed_[i].box);
      // Ruling T9-C: track ids come from the process-global, atomic
      // NextTrackId() (association.hpp), so tracks from different slots and
      // different tracker instances never collide on id.
      slot.t.id = NextTrackId();
      slot.t.handedness = Handedness::Unknown;
      slot.t.misses = 0;
      slot.t.active = true;
    }
  }

  void RunLandmarksForSlot(std::size_t slot_index, const ImageView& frame,
                           float aspect, std::int64_t timestamp_us,
                           std::vector<HandObservation>& out) {
    Slot& slot = slots_[slot_index];
    {
      ScopedStage s(metrics_, Stage::Preprocess);
      WarpToTensorNhwc(frame, slot.crop_roi, ModelWarpOptions(landmark_side_),
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
            CropToFrame(slot.crop_roi, aspect, in_crop);
      }
    }

    if (presence < config_.min_presence) {
      // THIS hand is gone (or this slot's ROI drifted off it). Only this
      // slot is dropped -- the other slots' tracks are independent and must
      // survive. The freed slot makes the next frame run the detector again.
      slot.t.active = false;
      ++slot.t.misses;
      return;
    }

    HandObservation obs{};
    obs.track_id = slot.t.id;
    // Ruling T8-E: which side of the 0.5 split means Left and which means
    // Right is NOT DETERMINED -- MediaPipe's handedness.txt label map has not
    // been fetched, and the convention additionally flips under input
    // mirroring (cpp/docs/models.md, "Handedness label order"). This
    // assignment is therefore a placeholder ordering: it guarantees a side is
    // assigned (never Unknown) but must not be trusted as Left-vs-Right until
    // Tasks 9/10 resolve the label map.
    obs.handedness =
        (handedness_score >= 0.5f) ? Handedness::Right : Handedness::Left;
    slot.t.handedness = obs.handedness;
    // Confidence in the chosen class, i.e. the distance of the binary score
    // from the 0.5 split, expressed as a probability in [0.5, 1].
    obs.handedness_score = (handedness_score >= 0.5f) ? handedness_score
                                                      : 1.0f - handedness_score;
    obs.presence = presence;
    obs.landmarks = landmarks;
    obs.timestamp_us = timestamp_us;
    out.push_back(obs);

    // Seed THIS slot's next-frame crop ROI from its own landmarks, which is
    // what lets the detector be skipped entirely while every slot holds, and
    // refresh the (tight, differently-shaped) rect the association layer
    // compares against palm boxes. See TightLandmarkBox for why these two are
    // not the same rectangle.
    slot.crop_roi = RoiFromLandmarks(landmarks, aspect, landmark_roi_opts_);
    slot.t.roi = Unrotated(TightLandmarkBox(landmarks, landmark_roi_opts_));
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

  // One slot per tracked hand. The two rectangles are deliberately distinct
  // and must not be collapsed into one:
  //   - `t.roi` (the TrackSlot the association layer sees) is a TIGHT,
  //     unrotated extent, commensurable with a palm detection box.
  //   - `crop_roi` is MediaPipe's inflated, rotated ROI, which is what the
  //     landmark model is actually cropped from.
  // See TightLandmarkBox for the measurements behind the split.
  struct Slot {
    TrackSlot t{};
    RotatedRect crop_roi{};
  };

  // Tracking state: exactly one slot per hand, sized once from
  // config.max_hands and never resized, so the cap is structural rather than
  // a comparison someone can forget.
  std::vector<Slot> slots_;
  // Scratch buffers, members so the detection path allocates nothing after
  // the first frame.
  std::vector<PalmDetection> framed_;
  std::vector<TrackSlot> assoc_slots_;
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
