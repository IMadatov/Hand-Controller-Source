# Model record (Task 1)

This file is a data record, not prose. Every value below is marked **OBSERVED**
(read directly off the ONNX model file via ONNX Runtime, or off a file fetched
by hand from MediaPipe's own repository and quoted verbatim) or **NOT
DETERMINED** (could not be established from any primary source available in
this task). Nothing here is a guess dressed up as a fact.

Produced by running `cpp/build/tools/validate_models` (built from
`cpp/tools/validate_models.cpp`) against the fixture
`cpp/tests/fixtures/hand_rgb.{raw,txt}` (640x480 RGB8, two crossed open palms,
CC BY-SA 4.0, source: Wikimedia Commons, see "Fixture image" below).

## Model provenance

- **Source repository:** https://github.com/opencv/opencv_zoo
- **Pinned commit:** `48b10fa505aeb51fbd127fc60a5c660040e823b7` — OBSERVED via
  `git ls-remote https://github.com/opencv/opencv_zoo.git HEAD main` at the
  time this task ran (2026-11-12).
- **License:** Apache 2.0 (per `models/palm_detection_mediapipe/README.md` and
  `models/handpose_estimation_mediapipe/README.md` in that repo) — OBSERVED
  by fetching those READMEs.
- **Conversion:** both models were converted by the opencv_zoo maintainers
  from the official MediaPipe TFLite models (`palm_detection_full.tflite`,
  `hand_landmark_full.tflite`) to ONNX via tf2onnx + onnx-simplifier.
  OBSERVED from the READMEs; not re-verified against the original TFLite
  files (no TFLite tooling available in this environment — see "What was NOT
  verified" below).
- **Fetch mechanism:** `cpp/cmake/FetchModels.cmake`, via
  `github.com/opencv/opencv_zoo/raw/<pinned-commit>/...` (this resolves the
  repo's Git-LFS-stored binary directly; `raw.githubusercontent.com` on the
  same path serves only the LFS pointer file and was rejected after
  observing it return a 132-byte text pointer, not the binary).
- **Files are NOT committed to git.** CMake fetches them at configure time
  into `HC_PALM_MODEL` / `HC_LANDMARK_MODEL` (absolute paths, CACHE
  variables), with `URL_HASH SHA256` pinned.

| File | SHA256 (OBSERVED, computed locally with `sha256sum` / Python `hashlib` after download, cross-checked against the file's own Git LFS pointer `oid`) | Size |
|---|---|---|
| `palm_detection_mediapipe_2023feb.onnx` | `78ff51c38496b7fc8b8ebdb6cc8c1abb02fa6c38427c6848254cdaba57fcce7c` | 3,905,734 bytes |
| `handpose_estimation_mediapipe_2023feb.onnx` | `db0898ae717b76b075d9bf563af315b29562e11f8df5027a1ef07b02bef6d81c` | 4,099,621 bytes |

## ONNX Runtime

- **Version pinned:** 1.18.1 — chosen because it is a recent stable tagged
  release with published Linux x64 and Windows x64 archives.
- **Linux archive:** `onnxruntime-linux-x64-1.18.1.tgz`
  SHA256 (OBSERVED, re-computed from the archive actually present in the build
  tree at `build/_deps/onnxruntime_release-subbuild/.../src/`, i.e. the bytes
  CMake really downloaded and hash-checked):
  `a0994512ec1e1debc00c18bfc7a5f16249f6ebd6a6128ff2034464cc380ea211`
- **Windows archive:** `onnxruntime-win-x64-1.18.1.zip`, 60,979,938 bytes
  SHA256 (OBSERVED, see correction note below):
  `53fb7226fe3cf16001afd1eae79e35f891a20e80bd686185d62ea878e6f9b1a6`
  **NOT runtime-tested** — this sandbox is Linux-only, so the Windows archive
  was hash-verified and its contents listed, but never linked or run. Structural
  check only: the zip contains `onnxruntime-win-x64-1.18.1/lib/onnxruntime.dll`
  and `.../lib/onnxruntime.lib`, which is what `FetchOnnxRuntime.cmake` expects
  at `${SOURCE_DIR}/lib/` after FetchContent strips the top-level directory.
- Both archives were downloaded directly from
  `https://github.com/microsoft/onnxruntime/releases/download/v1.18.1/...`
  (the official release, not a mirror).

**Correction note (both ONNX Runtime hashes were wrong in the first revision).**
The first revision of this document recorded the Linux hash with only 63 hex
digits (a truncated transcription) and recorded the Windows hash as
`07d64003...ab9512`, which is simply not the hash of that asset. Both have been
re-derived from bytes and replaced:

- Linux: re-hashed from the archive in the build tree — the exact bytes CMake
  downloaded and accepted against `URL_HASH`. 64 digits, ends `...380ea211`.
- Windows: re-downloaded twice, independently, streaming to a SHA256 accumulator;
  both passes agreed on `53fb7226...f9b1a6`, and the payload was confirmed to be
  a real 28-entry zip holding `onnxruntime.dll`/`onnxruntime.lib`.

The Windows error was **not** cosmetic and not confined to this document: the
same wrong value was in `cmake/FetchOnnxRuntime.cmake`, so **every Windows
configure would have failed** the `URL_HASH` check. This invalidates the first
revision's reasoning that "CMake's `URL_HASH` is authoritative so a typo in
`models.md` is harmless" — the `.cmake` file was wrong too. Every hash in this
document has now been re-counted for length and re-checked against either real
local bytes or two agreeing downloads.

## Palm detection model

File: `palm_detection_mediapipe_2023feb.onnx`

### Input tensor — OBSERVED from `validate_models` output

| Name | Shape | Dtype |
|---|---|---|
| `input_1` | `[1, 192, 192, 3]` (NHWC) | float32 |

- **Value range:** `[0.0, 1.0]` — OBSERVED from MediaPipe's own
  `palm_detection_cpu.pbtxt` (`ImageToTensorCalculatorOptions.output_tensor_float_range { min: 0.0 max: 1.0 }`),
  and independently from opencv_zoo's `mp_palmdet.py` preprocessing
  (`image.astype(np.float32) / 255.0`). Channel order RGB (not BGR) per the
  same preprocessing code.
- **Resize/letterbox:** MediaPipe's graph uses `keep_aspect_ratio: true` with
  `BORDER_ZERO` padding (i.e. letterbox, not plain stretch) — OBSERVED from
  `palm_detection_cpu.pbtxt`.
  `validate_models.cpp`'s `LetterboxToNhwc()` reproduces that shape: it scales
  the 640x480 fixture by a single factor `min(192/640, 192/480)`, centres the
  result, and zero-fills the remaining border, so the aspect ratio IS preserved.
  It uses nearest-neighbour sampling, whereas MediaPipe interpolates, and it
  does not reproduce MediaPipe's exact border/rounding semantics — the
  production-grade resampler is Task 4's job. For Task 4's reference, the
  letterbox for this fixture is scale `0.3`, content `192x144`, with 24 rows of
  zero padding top and bottom and no horizontal padding.

### Output tensors — OBSERVED from `validate_models` output, cross-checked by
hand-parsing the ONNX file's `GraphProto.output` field order (field 12) with a
minimal protobuf walker, independent of ONNX Runtime's own reporting (both
agree):

| Name | Shape | Dtype | Meaning |
|---|---|---|---|
| `Identity` | `[1, 2016, 18]` | float32 | Per-anchor raw box + 7 keypoints. Columns 0-3 = box (OBSERVED layout matches MediaPipe's `box_coord_offset: 0`, i.e. first 4 of the 18 coords are the box); columns 4-17 = 7 keypoints x 2 coords each (`keypoint_coord_offset: 4`, `num_keypoints: 7`, `num_values_per_keypoint: 2`), all OBSERVED from MediaPipe's own `palm_detection_cpu.pbtxt` `TensorsToDetectionsCalculatorOptions`. Values are in raw/pre-decode anchor-relative form (not yet multiplied by `x_scale`/`y_scale`/`w_scale`/`h_scale`, not yet added to anchor center) — decoding into image-space boxes is Task 6's job. |
| `Identity_1` | `[1, 2016, 1]` | float32 | Per-anchor raw (pre-sigmoid) detection score. `sigmoid_score: true` per MediaPipe's options, so the real confidence is `1/(1+exp(-raw))`. |

Output order (`Identity` = boxes, `Identity_1` = score) was OBSERVED three
independent ways: (1) ONNX Runtime's `GetOutputNameAllocated` order, (2) a
hand-written protobuf field walker over the raw `.onnx` bytes reading
`GraphProto.output` directly (bypassing both ONNX Runtime and any Python ONNX
package), (3) opencv_zoo's `mp_palmdet.py`, which indexes
`output_blob[0][..., 0:4]` as box and `output_blob[1][0, :, 0]` as score.

### SSD anchor parameters — OBSERVED verbatim from MediaPipe's own graph
configuration file, fetched directly:
`mediapipe/modules/palm_detection/palm_detection_cpu.pbtxt` at
`google-ai-edge/mediapipe` master, `SsdAnchorsCalculatorOptions` node:

```
num_layers: 4
min_scale: 0.1484375
max_scale: 0.75
input_size_width: 192
input_size_height: 192
anchor_offset_x: 0.5
anchor_offset_y: 0.5
strides: 8
strides: 16
strides: 16
strides: 16
aspect_ratios: 1.0
fixed_anchor_size: true
```

**Plus one parameter that is NOT in the pbtxt but is load-bearing:**

```
interpolated_scale_aspect_ratio: 1.0   # proto2 default, NOT written in the pbtxt
```

- OBSERVED from `mediapipe/calculators/tflite/ssd_anchors_calculator.proto`:
  `optional float interpolated_scale_aspect_ratio = 13 [default = 1.0];`
  Because the pbtxt omits the field, the default applies — and the default is
  what makes the count come out right. Task 5 must set it explicitly to 1.0
  rather than leaving a zero-initialised struct member.
- **Resulting anchor count: 2016** — OBSERVED three ways: (a) directly as the
  middle dimension of both ONNX output tensors reported by
  `validate_models` (`[1, 2016, 18]`, `[1, 2016, 1]`); (b) as the literal
  `num_boxes: 2016` field in the same pbtxt's
  `TensorsToDetectionsCalculatorOptions`; (c) by reimplementing MediaPipe's
  `SsdAnchorsCalculator::GenerateAnchors` (including `CalculateScale`, the
  same-stride inner coalescing loop, and the `interpolated_scale_aspect_ratio`
  branch) and running it on exactly the parameters above.

  The arithmetic, for Task 5 to check itself against:

  | Stride | Feature map | Anchors per cell | Subtotal |
  |---|---|---|---|
  | 8 | 192/8 = 24 x 24 = 576 | 2 | 1152 |
  | 16, 16, 16 (coalesced) | 192/16 = 12 x 12 = 144 | 6 | 864 |
  | | | **Total** | **2016** |

  Two anchors per cell at stride 8 because `aspect_ratios: 1.0` contributes one
  and `interpolated_scale_aspect_ratio: 1.0` contributes a second; the three
  consecutive `strides: 16` layers are merged by the calculator's inner
  `while (last_same_stride_layer < strides.size() && strides[...] == stride)`
  loop into a single 12x12 map with 3 x 2 = 6 anchors per cell.

  **Sensitivity check (why this parameter matters):** with
  `interpolated_scale_aspect_ratio` left at 0.0 the same code yields
  576 + 432 = **1008** anchors, which contradicts the model's actual output
  shape. So the first revision's parameter list was genuinely insufficient to
  reproduce 2016 — recording it was necessary, not pedantic.
- `num_coords: 18`, `box_coord_offset: 0`, `keypoint_coord_offset: 4`,
  `num_keypoints: 7`, `num_values_per_keypoint: 2`, `reverse_output_order: true`
  — all OBSERVED verbatim from the same pbtxt node
  (`TensorsToDetectionsCalculatorOptions`).
- `x_scale: 192.0`, `y_scale: 192.0`, `w_scale: 192.0`, `h_scale: 192.0` — box
  decode scale factors, OBSERVED verbatim from the same node.

### Thresholds — OBSERVED verbatim from the same pbtxt file

- **Score threshold** (`min_score_thresh`): `0.5` (from
  `TensorsToDetectionsCalculatorOptions`).
- **Score clipping threshold** (`score_clipping_thresh`): `100.0` (from
  `TensorsToDetectionsCalculatorOptions`; raw scores are clamped to
  `[-100, 100]` before the sigmoid).
- **NMS IoU threshold** (`min_suppression_threshold`): `0.3`, with
  `overlap_type: INTERSECTION_OVER_UNION` and `algorithm: WEIGHTED` (from
  `NonMaxSuppressionCalculatorOptions` in the same pbtxt).

### Live run against the fixture — OBSERVED, `validate_models` output

- num_anchors confirmed = 2016
- Best raw score = `2.275857`; sigmoid(score) = `0.906858` at anchor index
  **1682**. 0.91 clears the 0.5 `min_score_thresh` comfortably and is not noise
  — this is a real detection, on a fixture with two crossed, clearly-visible
  palms.
- Raw box row at that index (18 raw values, pre-decode, anchor-relative):
  `[6.2701, -1.5299, 79.9564, 79.9760, -16.0717, 37.5384, 3.1172, -42.6803, 17.2026, -32.1031, 30.3280, -18.2734, 41.8974, -3.0467, -23.5989, 17.4689, -28.9847, -15.1278]`

  These numbers changed from the first revision (`1.414808` / `0.804523` /
  index 1683) because the tool's palm preprocessing was switched from a
  stretch to a true letterbox. The score **improved**, 0.805 -> 0.907, which is
  the expected direction: the model was trained on aspect-preserved input, so
  removing the 4:3 -> 1:1 distortion makes the hand easier to detect. Treat the
  current values as the authoritative ones.

- **The score threshold is a real gate, not decoration** — OBSERVED by running
  the tool on deliberately hand-free input:

  | Fixture | sigmoid(best score) | Tool exit |
  |---|---|---|
  | real hand (committed fixture) | 0.906858 | 0 (pass) |
  | all-black 640x480 | 0.025053 | 1 (FATAL, below 0.5) |
  | uniform mid-grey 640x480 | 0.056953 | 1 (FATAL, below 0.5) |

  A ~16x separation between hand and no-hand means the check discriminates;
  it is not a tautology that passes on anything. (Those negative fixtures were
  temporary and are not committed; the committed fixture was restored and
  re-verified byte-identical by SHA256 afterwards.)
  Incidentally this also settles the pre/post-sigmoid question for this tensor
  empirically: raw scores of `-3.661387` and `-2.806886` are **negative**, so
  `Identity_1` carries pre-sigmoid logits, consistent with
  `sigmoid_score: true` in the pbtxt.

## Hand landmark model

File: `handpose_estimation_mediapipe_2023feb.onnx`

### Input tensor — OBSERVED from `validate_models` output

| Name | Shape | Dtype |
|---|---|---|
| `input_1` | `[1, 224, 224, 3]` (NHWC) | float32 |

- **Value range:** `[0.0, 1.0]`, RGB channel order — OBSERVED from
  MediaPipe's `hand_landmark_cpu.pbtxt`
  (`output_tensor_float_range { min: 0.0 max: 1.0 }`) and from opencv_zoo's
  `mp_handpose.py` (`blob / 255.`, with an explicit `cv.cvtColor(..., COLOR_BGR2RGB)`
  earlier in the same preprocessing function).
- The real preprocessing is "crop ROI from the rotated-aligned source image,
  then resize to 224x224 with `keep_aspect_ratio: true` (letterbox)" — this
  tool instead resizes the *whole* fixture to 224x224 with no ROI crop or
  rotation, per the brief's Step 5 instruction ("runs the landmark model on
  the full image resized to its input size"). That is a deliberate
  simplification for this validation tool only; the real ROI-aware crop is
  Task 4/7/8's job.

### Output tensors — OBSERVED from `validate_models` output, cross-checked by
hand-parsing the ONNX file's `GraphProto.output` field order the same way as
the palm model (both agree):

| Index | Name | Shape | Dtype | Meaning |
|---|---|---|---|---|
| 0 | `Identity` | `[1, 63]` | float32 | 21 screen-space landmarks x 3 (x, y in pixels relative to the 224x224 input, z = relative depth). |
| 1 | `Identity_1` | `[1, 1]` | float32 | Hand-presence confidence score (scalar, pre- or post-sigmoid — see caveat below). |
| 2 | `Identity_2` | `[1, 1]` | float32 | Handedness score (scalar). |
| 3 | `Identity_3` | `[1, 63]` | float32 | 21 world-space landmarks x 3 (meters, origin at ROI center). |

This mapping (index 0 = landmarks, 1 = presence, 2 = handedness, 3 =
world landmarks) was OBSERVED two independent ways: (a) MediaPipe's own
`hand_landmark_cpu.pbtxt`, `SplitTensorVectorCalculator` node, whose
`ranges` field splits the model's raw output-tensor vector into
`landmark_tensors` (range 0-1), `hand_flag_tensor` (range 1-2),
`handedness_tensor` (range 2-3), `world_landmark_tensor` (range 3-4), in
that order; (b) opencv_zoo's `mp_handpose.py`, which unpacks
`landmarks, conf, handedness, landmarks_word = blob` from
`self.model.forward(self.model.getUnconnectedOutLayersNames())`, and
`getUnconnectedOutLayersNames()` returns outputs in graph-declaration order,
which is the same order confirmed by the protobuf field walk
(`Identity, Identity_1, Identity_2, Identity_3`).

**Activation — OBSERVED (this was NOT DETERMINED in the first revision; now
settled from source).** `Identity_1` / `Identity_2` are **already
probabilities** — the model's own head applies the sigmoid, and the consumer
must NOT apply another one. Chain of evidence:

1. `hand_landmark_cpu.pbtxt` pipes `hand_flag_tensor` into a
   `TensorsToFloatsCalculator` node that carries **no options block at all**.
2. `tensors_to_floats_calculator.proto` declares
   `optional Activation activation = 1 [default = NONE];` with
   `enum Activation { NONE = 0; SIGMOID = 1; }`. With no options, activation is
   `NONE` — MediaPipe applies no sigmoid outside the model.
3. The immediately following node thresholds that float at `0.5`. A `0.5`
   cutoff is only meaningful against a value already in `[0, 1]`.
4. Handedness goes to `TensorsToClassificationCalculator` with
   `binary_classification: true`, which likewise interprets its input as a
   probability.
5. Consistent with the live run: `0.744082` and `0.934898`, both inside
   `[0, 1]`.

Contrast with the palm detector, whose `Identity_1` *is* a raw logit
(`sigmoid_score: true`, and observed negative values on hand-free input).
Applying a sigmoid twice here would be a silent accuracy bug, so this
distinction between the two models matters.

### Thresholds — OBSERVED verbatim from `hand_landmark_cpu.pbtxt`

- **Presence threshold** (`ThresholdingCalculatorOptions.threshold`): `0.5`.
- Handedness threshold: there is no `ThresholdingCalculator` for handedness in
  `hand_landmark_cpu.pbtxt`. Instead `TensorsToClassificationCalculator` with
  `binary_classification: true` and `top_k: 1` turns the single score into one
  labelled class, which is an implicit `0.5` split between the two labels.
- **Handedness label order: NOT DETERMINED.** Which side of that split means
  "Left" and which means "Right" is set by
  `label_map_path: "mediapipe/modules/hand_landmark/handedness.txt"`, a file
  this task did not fetch. Task 9/10 must read that label map rather than
  guessing; also note MediaPipe's own docs warn handedness assumes a
  mirrored (selfie-view) input image, so the convention flips with input
  mirroring. Recorded as a gap, not an assumption.

### Live run against the fixture — OBSERVED, `validate_models` output

21 landmark triples (screen-space, pixels in the 224x224 input space, z =
relative depth) — not degenerate, not all zero, not all identical:

```
[ 0] (81.3362, 179.1960, -0.0000)
[ 1] (69.1005, 138.6646, -1.6724)
[ 2] (70.4149, 97.0504, -8.3447)
[ 3] (77.1990, 65.2371, -14.7811)
[ 4] (75.4569, 38.8582, -21.8480)
[ 5] (96.7060, 90.2701, -22.8793)
[ 6] (125.0851, 57.3036, -32.5769)
[ 7] (144.9369, 36.3865, -37.3813)
[ 8] (161.5953, 21.2400, -40.8258)
[ 9] (111.2011, 111.6159, -26.8916)
[10] (150.0325, 78.6307, -37.1039)
[11] (178.9241, 55.0880, -40.1180)
[12] (203.7531, 37.7851, -42.8701)
[13] (122.0466, 138.7535, -29.6749)
[14] (159.8362, 109.7631, -36.8216)
[15] (186.2565, 85.7382, -36.8403)
[16] (208.7812, 64.7229, -37.6398)
[17] (130.9692, 167.5221, -32.4270)
[18] (161.5952, 149.0013, -37.5948)
[19] (177.5726, 132.1696, -38.1014)
[20] (191.0818, 116.3174, -39.0009)
```

Presence (`Identity_1`): `0.744082`. Handedness (`Identity_2`): `0.934898`.

## ROI transform constants

### Palm detection -> ROI

OBSERVED verbatim from
`mediapipe/modules/hand_landmark/palm_detection_detection_to_roi.pbtxt`
(`google-ai-edge/mediapipe`, master, fetched directly):

- **Rotation axis keypoints:** `rotation_vector_start_keypoint_index: 0`
  (center of wrist), `rotation_vector_end_keypoint_index: 2` (MCP of middle
  finger) — these are indices into the palm detector's 7 keypoints (the
  `Identity` tensor's columns 4-17, two floats per keypoint), NOT into the
  21-point hand landmark topology.
- **Target angle:** `rotation_vector_target_angle_degrees: 90`.
- `scale_x: 2.6`
- `scale_y: 2.6`
- `shift_x: 0.0` — OBSERVED (upgraded from NOT DETERMINED in the first
  revision). The field is absent from the pbtxt node, so the proto2 default
  applies, and `mediapipe/calculators/util/rect_transformation_calculator.proto`
  declares it verbatim as:
  ```
  optional float shift_x = 5;
  ```
  with **no** `[default = ...]` clause. For a proto2 `optional float` with no
  explicit default the implicit default is `0.0`, so `shift_x` is `0.0` — this
  is now a determined value, not a provisional one. (Contrast `scale_x`/`scale_y`
  in the same file, which *do* carry `[default = 1.0]`, confirming the file does
  spell out defaults when they are non-zero.)
- `shift_y: -0.5`
- `square_long: true`

### Landmarks -> ROI (used once a hand is already tracked)

OBSERVED verbatim from
`mediapipe/modules/hand_landmark/hand_landmark_landmarks_to_roi.pbtxt` and
`mediapipe/modules/hand_landmark/calculators/hand_landmarks_to_rect_calculator.cc`
(`google-ai-edge/mediapipe`, master, fetched directly):

- **Rotation axis is NOT a simple two-keypoint pair, and the three far-side
  points are NOT equally weighted.** The axis runs from the wrist to a
  *nested* average of three points. Verbatim from `ComputeRotation`:

  ```cpp
  const float x0 = landmarks.landmark(kWristJoint).x() * image_size.first;
  const float y0 = landmarks.landmark(kWristJoint).y() * image_size.second;

  float x1 = (landmarks.landmark(kIndexFingerPIPJoint).x() +
              landmarks.landmark(kRingFingerPIPJoint).x()) /
             2.f;
  float y1 = (landmarks.landmark(kIndexFingerPIPJoint).y() +
              landmarks.landmark(kRingFingerPIPJoint).y()) /
             2.f;
  x1 = (x1 + landmarks.landmark(kMiddleFingerPIPJoint).x()) / 2.f *
       image_size.first;
  y1 = (y1 + landmarks.landmark(kMiddleFingerPIPJoint).y()) / 2.f *
       image_size.second;

  const float rotation =
      NormalizeRadians(kTargetAngle - std::atan2(-(y1 - y0), x1 - x0));
  ```

  Averaging index+ring first, then averaging *that* with middle, gives
  **0.25 / 0.5 / 0.25**, not a flat one-third each. The first revision of this
  document wrongly described it as "the average of three"; that is corrected
  here.

- **Index mapping — the constants are partial-subset indices and must be
  remapped.** The calculator's constants are declared under an explicit comment:

  ```cpp
  // Indices within the partial landmarks.
  constexpr int kWristJoint = 0;
  constexpr int kMiddleFingerPIPJoint = 6;
  constexpr int kIndexFingerPIPJoint = 4;
  constexpr int kRingFingerPIPJoint = 8;
  constexpr int kNumLandmarks = 21;
  constexpr float kTargetAngle = M_PI * 0.5f;
  ```

  `Process()` does not pass the raw 21-point list through — it unconditionally
  calls `GetPartialLandmarks(cc)`, which, when it receives the full 21
  landmarks (exactly what this model emits), rebuilds a 12-point list:

  ```cpp
  if (landmarks.landmark_size() == kNumLandmarks) {
    static constexpr int kPartialLandmarkIndices[]{0, 1,  2,  3,  5,  6,
                                                   9, 10, 13, 14, 17, 18};
    NormalizedLandmarkList partial_landmarks;
    for (int i : kPartialLandmarkIndices) {
      *partial_landmarks.add_landmark() = landmarks.landmark(i);
    }
    return partial_landmarks;
  }
  ```

  So the subset path is the **live** path, not dead legacy code; the `else`
  branch is the legacy one, taken only if a caller feeds 12 points directly.
  Remapping subset position -> full landmark index:

  | Constant | Subset index | **Full landmark index** | Anatomy |
  |---|---|---|---|
  | `kWristJoint` | 0 | **0** | wrist |
  | `kIndexFingerPIPJoint` | 4 | **5** | index finger MCP |
  | `kMiddleFingerPIPJoint` | 6 | **9** | middle finger MCP |
  | `kRingFingerPIPJoint` | 8 | **13** | ring finger MCP |

  **In full-landmark terms:** axis from `L[0]` to
  `0.25*L[5] + 0.5*L[9] + 0.25*L[13]`; target angle `kTargetAngle = M_PI * 0.5`.

  Cross-check that this remapping is the right reading: full indices 5/9/13 are
  the index/middle/ring **MCP** joints, which matches the calculator's own
  description of using the index, middle and ring fingers. Reading the constants
  as full-landmark indices instead would give 4/6/8 = thumb **tip**, index PIP,
  index **tip** — two points on the index finger plus the thumb, which does not
  match the stated intent and would put the "far" end of the palm axis out at
  the fingertips. The `PIPJoint` names are a known misnomer for what are
  actually MCP joints (MediaPipe issue #1787); the names are wrong, the
  remapped positions are right.

- **Bounding box uses only those 12 points, not all 21** — easily missed.
  `NormalizedLandmarkListToRect` receives the *partial* list, and its min/max
  loops run `for (int i = 0; i < landmarks.landmark_size(); ++i)`, i.e. over 12
  landmarks. Fingertips (4, 8, 12, 16, 20) are therefore excluded from the ROI
  extent before `scale_x`/`scale_y` are applied. Task 7 must crop to the subset
  before computing the box, or the ROI will come out too large.

- `scale_x: 2.0`
- `scale_y: 2.0`
- `shift_x: 0.0` — OBSERVED, same proto2 reasoning as the palm case above
  (`optional float shift_x = 5;`, no explicit default).
- `shift_y: -0.1`
- `square_long: true`

**Note for Task 7's `LandmarkRoiOptions`.** Per the controller's ruling this
splits into one origin index plus weighted rotation targets. The values to fill
in are: origin index `0`, and
`rotation_target_weights = {{5, 0.25f}, {9, 0.5f}, {13, 0.25f}}`
(weights sum to 1.0), target angle `M_PI * 0.5`.

## Fixture image

- `cpp/tests/fixtures/hand_rgb.raw` + `cpp/tests/fixtures/hand_rgb.txt`
  (`640 480 3`), interleaved RGB8, 921,600 bytes.
- **Source:** downloaded, not synthesized. Wikimedia Commons,
  "Open Palm of the Left Hand, Fingers.jpg" by Eyefive45, CC BY-SA 4.0,
  https://commons.wikimedia.org/wiki/File:Open_Palm_of_the_Left_Hand,_Fingers.jpg
  (original 4032x3024; resized with ImageMagick to 640x480, same aspect
  ratio so no distortion, then decoded with Python's stdlib-adjacent `PIL`
  to raw interleaved RGB8 bytes — no OpenCV/mediapipe/numpy involved in
  producing the fixture, consistent with the ban on importing those in this
  environment).
- **Content:** both palms, open, crossed, filling most of the frame (not a
  single isolated hand, since that was the clearest permissively-licensed
  photo found; the brief allows "any permissively-licensed hand photograph").
- **Confirmed to produce a real detection:** palm detector's best score on
  this fixture was sigmoid 0.907 (see above), well above the 0.5 threshold,
  and drops to 0.025 on an all-black frame of the same size.

## What was NOT verified (explicit gaps)

1. **Original TFLite models were not re-inspected.** There is no TFLite
   tooling in this environment (no Python `tensorflow`/`mediapipe`), so the
   claim that `palm_detection_mediapipe_2023feb.onnx` /
   `handpose_estimation_mediapipe_2023feb.onnx` are faithful conversions of
   MediaPipe's official `palm_detection_full.tflite` /
   `hand_landmark_full.tflite` rests entirely on the opencv_zoo project's own
   documentation and is not independently cross-checked bit-for-bit against
   the original TFLite graphs in this task.
2. ~~**`shift_x` default**~~ — **RESOLVED** in this revision. Fetched
   `rect_transformation_calculator.proto`; `optional float shift_x = 5;` has no
   explicit default, so proto2's implicit `0.0` applies. No longer a gap.
3. **Handedness sign convention** (which way round "left" vs "right" maps) —
   still NOT DETERMINED. `handedness.txt`, the label-map file named by
   `TensorsToClassificationCalculatorOptions.label_map_path`, was not fetched,
   so which label index corresponds to which hand is unconfirmed. The
   *threshold* is determined (`0.5`, see item 4). Task 10 must resolve the
   label order before trusting handedness, and note the Python
   `solutions`-layer additionally flips handedness for mirrored input.
4. ~~**Whether presence/handedness are pre- or post-sigmoid**~~ — **RESOLVED**
   in this revision, for both models, from source rather than from value ranges:
   - *Landmark model = **post**-sigmoid (already probabilities).* The graph
     feeds `hand_flag_tensor` into a bare `TensorsToFloatsCalculator` with **no
     options block**, and `tensors_to_floats_calculator.proto` declares
     `optional Activation activation = 1 [default = NONE];` — so MediaPipe
     applies no activation outside the model. The very next node applies
     `threshold: 0.5` directly to that value, which is only meaningful if the
     value is already in `[0, 1]`. Handedness likewise goes to
     `TensorsToClassificationCalculator` with `binary_classification: true`,
     which consumes a probability directly. Consistent with the observed
     `presence = 0.744082`, `handedness = 0.934898`.
   - *Palm model = **pre**-sigmoid (logits).* `sigmoid_score: true` in the
     pbtxt, and empirically confirmed by negative values (`-3.661387` on a
     black frame) which a post-sigmoid output could not produce.
5. **Windows ONNX Runtime archive** was hash-pinned but never actually linked
   or executed (this sandbox has no Windows target); only the Linux path was
   build- and run-verified. Its hash was corrected in this revision (see the
   ONNX Runtime correction note) and its internal layout checked, but a real
   Windows build remains unverified.

None of the above gaps are "models are unusable" findings — the hard gate in
Step 8 of the brief does not apply. Both sessions created without throwing,
every tensor shape printed, the palm detector's best score (sigmoid 0.907) is
a real detection rather than noise, and the 21 printed landmarks are neither
all identical nor all zero. The gaps above are either forwarded explicitly to
the later task responsible for confirming them (as the brief's Step 7
instructs) or are inherent limits of having no TFLite/Python tooling in this
sandbox.
