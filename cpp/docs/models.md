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
  SHA256 (OBSERVED, computed locally after downloading the official GitHub
  release asset):
  `a0994512ec1e1debc00c18bfc7a5f16249f6ebd6a6128ff2034464cc380ea21`
- **Windows archive:** `onnxruntime-win-x64-1.18.1.zip`
  SHA256 (OBSERVED, computed locally after downloading the official GitHub
  release asset; **NOT runtime-tested** — this sandbox is Linux-only, so the
  Windows archive was hash-verified but never linked/run):
  `07d64003c5ee714cadc0c433cdbed3a8462af8c0dc9340357daafec791ab9512`
- Both archives were downloaded directly from
  `https://github.com/microsoft/onnxruntime/releases/download/v1.18.1/...`
  (the official release, not a mirror).

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
  `palm_detection_cpu.pbtxt`. `validate_models.cpp`'s own preprocessing does a
  plain aspect-preserving resize for the palm path and does NOT implement
  MediaPipe's exact letterbox border semantics; that exact resampler is
  Task 4's job, not this task's.

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

- **Resulting anchor count: 2016** — OBSERVED twice: (a) directly as the
  middle dimension of both ONNX output tensors reported by
  `validate_models` (`[1, 2016, 18]`, `[1, 2016, 1]`); (b) as the literal
  `num_boxes: 2016` field in the same pbtxt's
  `TensorsToDetectionsCalculatorOptions`. Both sources agree, so this value
  needs no further confirmation in Task 5 beyond reproducing the anchor
  *generation* logic that yields 2016 for these parameters.
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
- Best raw score = `1.414808`; sigmoid(score) = `0.804523` at anchor index
  1683. 0.80 clears the 0.5 `min_score_thresh` comfortably and is not noise —
  this is a real detection, on a fixture with two crossed, clearly-visible
  palms.
- Raw box row at that index (18 raw values, pre-decode, anchor-relative):
  `[4.7002, 5.6171, 92.7093, 92.7408, -2.2925, 50.9371, -8.6756, -38.5467, 11.1860, -29.8626, 27.7332, -17.5594, 42.9212, -2.5921, -21.7281, 22.7313, -34.4161, -11.6298]`

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

**Caveat (NOT DETERMINED):** whether `Identity_1` / `Identity_2` are raw
logits needing a sigmoid or already-squashed probabilities was not
conclusively settled from the sources read. MediaPipe's pbtxt pipes
`hand_flag_tensor` through a bare `TensorsToFloatsCalculator` (no sigmoid
option visible in that node) before thresholding at `0.5` — suggesting the
TFLite model's own output head already applies sigmoid internally. Consistent
with that, the live run below produced values already inside `[0, 1]`
(0.744 and 0.935), which would be a very unlikely coincidence for a raw
logit. Treat these as presence/handedness probabilities already in `[0, 1]`
unless Task 7/8's end-to-end test shows otherwise.

### Thresholds — OBSERVED verbatim from `hand_landmark_cpu.pbtxt`

- **Presence threshold** (`ThresholdingCalculatorOptions.threshold`): `0.5`.
- Handedness threshold: NOT DETERMINED — no equivalent `ThresholdingCalculator`
  for handedness was found in `hand_landmark_cpu.pbtxt`; the Python solutions
  wrapper may apply its own cutoff (`> 0.5` = one label, `<= 0.5` = the other)
  but that was not located in a file this task fetched directly, so it is not
  recorded as observed. Task 9 (`Handedness`) should confirm.

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
- `shift_x`: NOT PRESENT in the pbtxt node → defaults to the
  `RectTransformationCalculatorOptions` proto default. The proto default was
  NOT DETERMINED in this task (the `.proto` file defining that default was
  not fetched); treat `shift_x` as `0.0` only provisionally. Task 7 must
  confirm this against the calculator's `.proto` default or the end-to-end
  test before relying on it.
- `shift_y: -0.5`
- `square_long: true`

### Landmarks -> ROI (used once a hand is already tracked)

OBSERVED verbatim from
`mediapipe/modules/hand_landmark/hand_landmark_landmarks_to_roi.pbtxt` and
`mediapipe/modules/hand_landmark/calculators/hand_landmarks_to_rect_calculator.cc`
(`google-ai-edge/mediapipe`, master, fetched directly):

- **Rotation axis is NOT a simple two-keypoint pair.** The rotation angle is
  computed from (a) the wrist (full 21-point landmark index **0**), and (b)
  the average position of three PIP-row joints: full-landmark indices
  **5, 9, 13** (index, middle, and ring finger MCP/PIP row — the calculator's
  own internal names are `kIndexFingerPIPJoint`, `kMiddleFingerPIPJoint`,
  `kRingFingerPIPJoint`, derived from a 12-point partial-landmark subset
  `{0,1,2,3,5,6,9,10,13,14,17,18}` of the full 21; mapped back to full-landmark
  indices by hand in this task: partial index 0 → full 0, partial index 4 →
  full 5, partial index 6 → full 9, partial index 8 → full 13). Target angle
  is `M_PI * 0.5` (90 degrees), same convention as the palm case. This is
  recorded exactly as found rather than forced into the brief's "two
  keypoint indices" phrasing, because the real calculator averages three
  points on one side of the axis, not one.
- `scale_x: 2.0`
- `scale_y: 2.0`
- `shift_x`: NOT PRESENT in the pbtxt node → same caveat as above, NOT
  DETERMINED, provisionally `0.0`.
- `shift_y: -0.1`
- `square_long: true`

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
  this fixture was sigmoid 0.805 (see above), well above the 0.5 threshold.

## What was NOT verified (explicit gaps)

1. **Original TFLite models were not re-inspected.** There is no TFLite
   tooling in this environment (no Python `tensorflow`/`mediapipe`), so the
   claim that `palm_detection_mediapipe_2023feb.onnx` /
   `handpose_estimation_mediapipe_2023feb.onnx` are faithful conversions of
   MediaPipe's official `palm_detection_full.tflite` /
   `hand_landmark_full.tflite` rests entirely on the opencv_zoo project's own
   documentation and is not independently cross-checked bit-for-bit against
   the original TFLite graphs in this task.
2. **`shift_x` default for `RectTransformationCalculatorOptions`** — see
   above, not located in a fetched `.proto` file.
3. **Handedness threshold / sign convention** — not located in a fetched
   MediaPipe file; Python solutions-layer behavior was not independently
   confirmed.
4. **Whether presence/handedness outputs are pre- or post-sigmoid** — inferred
   from observed value ranges, not confirmed from a source that states it
   explicitly.
5. **Windows ONNX Runtime archive** was hash-pinned but never actually linked
   or executed (this sandbox has no Windows target); only the Linux path was
   build- and run-verified.

None of the above gaps are "models are unusable" findings — the hard gate in
Step 8 of the brief does not apply. Both sessions created without throwing,
every tensor shape printed, the palm detector's best score (sigmoid 0.805) is
a real detection rather than noise, and the 21 printed landmarks are neither
all identical nor all zero. The gaps above are either forwarded explicitly to
the later task responsible for confirming them (as the brief's Step 7
instructs) or are inherent limits of having no TFLite/Python tooling in this
sandbox.
