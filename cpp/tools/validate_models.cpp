// Standalone diagnostic tool for Task 1: loads the palm-detection and
// hand-landmark ONNX models, prints every input/output tensor's name,
// shape and element type, then runs both models on the committed fixture
// image and prints the raw outputs.
//
// This is a hard gate, so it ASSERTS rather than merely printing: any
// condition that would mean "the models are unusable" (missing/renamed
// output tensor, palm score below MediaPipe's own threshold, degenerate
// landmarks) makes the program exit non-zero.
//
// This program intentionally does NOT depend on any of the (not yet
// existing) cpp/core/ library: Task 1 only creates the fetch logic and
// this tool. The image loading/resizing here is scratch diagnostic code,
// not production preprocessing.

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// MediaPipe's own min_score_thresh for palm detection, from
// palm_detection_cpu.pbtxt. A best score below this means no detection.
constexpr float kPalmScoreThreshold = 0.5f;

const char* ElementTypeName(ONNXTensorElementDataType type) {
  switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
      return "float32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8:
      return "uint8";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT8:
      return "int8";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32:
      return "int32";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
      return "int64";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_DOUBLE:
      return "double";
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
      return "bool";
    default:
      return "other";
  }
}

std::string ShapeToString(const std::vector<int64_t>& shape) {
  std::ostringstream oss;
  oss << "[";
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i) oss << ", ";
    oss << shape[i];
  }
  oss << "]";
  return oss.str();
}

std::vector<std::string> OutputNames(Ort::Session& session) {
  Ort::AllocatorWithDefaultOptions allocator;
  std::vector<std::string> names;
  for (size_t i = 0; i < session.GetOutputCount(); ++i) {
    names.emplace_back(session.GetOutputNameAllocated(i, allocator).get());
  }
  return names;
}

// Prints every input and output tensor's name, shape and element type.
void DescribeSession(const char* label, Ort::Session& session) {
  Ort::AllocatorWithDefaultOptions allocator;
  std::printf("=== %s ===\n", label);

  // NOTE: GetTensorTypeAndShapeInfo() returns a non-owning view into the
  // Ort::TypeInfo, so the TypeInfo must outlive it - keep it in a named
  // local rather than calling through a temporary.
  for (size_t i = 0; i < session.GetInputCount(); ++i) {
    auto name = session.GetInputNameAllocated(i, allocator);
    Ort::TypeInfo type_info = session.GetInputTypeInfo(i);
    auto info = type_info.GetTensorTypeAndShapeInfo();
    std::printf("  input[%zu]  name=%-20s shape=%-24s dtype=%s\n", i, name.get(),
                ShapeToString(info.GetShape()).c_str(),
                ElementTypeName(info.GetElementType()));
  }
  for (size_t i = 0; i < session.GetOutputCount(); ++i) {
    auto name = session.GetOutputNameAllocated(i, allocator);
    Ort::TypeInfo type_info = session.GetOutputTypeInfo(i);
    auto info = type_info.GetTensorTypeAndShapeInfo();
    std::printf("  output[%zu] name=%-20s shape=%-24s dtype=%s\n", i, name.get(),
                ShapeToString(info.GetShape()).c_str(),
                ElementTypeName(info.GetElementType()));
  }
}

struct RgbImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> data;  // interleaved RGB8, row-major
};

// Loads the committed raw fixture: "<width> <height> 3" in the .txt sidecar
// and width*height*3 interleaved RGB8 bytes in the .raw file. Uses fread
// only, matching the constraint that core may not link an image library.
RgbImage LoadRawFixture(const std::string& raw_path, const std::string& txt_path) {
  RgbImage image;

  std::ifstream txt(txt_path);
  if (!txt) throw std::runtime_error("cannot open " + txt_path);
  int width = 0, height = 0, channels = 0;
  if (!(txt >> width >> height >> channels)) {
    throw std::runtime_error("malformed sidecar (expected '<w> <h> <c>'): " + txt_path);
  }
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("non-positive dimensions in " + txt_path + ": " +
                             std::to_string(width) + "x" + std::to_string(height));
  }
  if (channels != 3) {
    throw std::runtime_error("fixture must have 3 channels, got " +
                             std::to_string(channels) + " in " + txt_path);
  }
  image.width = width;
  image.height = height;

  std::FILE* f = std::fopen(raw_path.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open " + raw_path);
  const size_t expected = static_cast<size_t>(width) * height * 3;
  image.data.resize(expected);
  const size_t got = std::fread(image.data.data(), 1, expected, f);
  std::fclose(f);
  if (got != expected) {
    throw std::runtime_error("short read on " + raw_path + ": expected " +
                             std::to_string(expected) + " bytes, got " +
                             std::to_string(got));
  }
  return image;
}

// Aspect-preserving resize into a centered target box, zero-padding the
// remainder (letterbox). Mirrors the intent of MediaPipe's
// ImageToTensorCalculator with keep_aspect_ratio:true + BORDER_ZERO.
// Output is NHWC float in [0,1], which is what both models take.
std::vector<float> LetterboxToNhwc(const RgbImage& img, int target_w, int target_h) {
  const float scale = std::min(static_cast<float>(target_w) / img.width,
                               static_cast<float>(target_h) / img.height);
  const int scaled_w = static_cast<int>(img.width * scale);
  const int scaled_h = static_cast<int>(img.height * scale);
  const int pad_x = (target_w - scaled_w) / 2;
  const int pad_y = (target_h - scaled_h) / 2;

  std::vector<float> nhwc(static_cast<size_t>(target_h) * target_w * 3, 0.0f);
  for (int y = 0; y < scaled_h; ++y) {
    const int src_y = std::min(img.height - 1, static_cast<int>(y / scale));
    for (int x = 0; x < scaled_w; ++x) {
      const int src_x = std::min(img.width - 1, static_cast<int>(x / scale));
      const uint8_t* px = &img.data[(static_cast<size_t>(src_y) * img.width + src_x) * 3];
      const size_t dst = (static_cast<size_t>(y + pad_y) * target_w + (x + pad_x)) * 3;
      nhwc[dst + 0] = px[0] / 255.0f;
      nhwc[dst + 1] = px[1] / 255.0f;
      nhwc[dst + 2] = px[2] / 255.0f;
    }
  }
  return nhwc;
}

// Plain stretch to target_w x target_h: maps the full source extent onto the
// target independently per axis, so aspect ratio is NOT preserved. Used only
// for the landmark path, where the brief asks for "the full image resized to
// its input size".
std::vector<float> StretchToNhwc(const RgbImage& img, int target_w, int target_h) {
  std::vector<float> nhwc(static_cast<size_t>(target_h) * target_w * 3);
  for (int y = 0; y < target_h; ++y) {
    const int src_y = std::min(
        img.height - 1, static_cast<int>(y * static_cast<float>(img.height) / target_h));
    for (int x = 0; x < target_w; ++x) {
      const int src_x = std::min(
          img.width - 1, static_cast<int>(x * static_cast<float>(img.width) / target_w));
      const uint8_t* px = &img.data[(static_cast<size_t>(src_y) * img.width + src_x) * 3];
      const size_t dst = (static_cast<size_t>(y) * target_w + x) * 3;
      nhwc[dst + 0] = px[0] / 255.0f;
      nhwc[dst + 1] = px[1] / 255.0f;
      nhwc[dst + 2] = px[2] / 255.0f;
    }
  }
  return nhwc;
}

// Reads the square input size from an NHWC [1,H,W,3] input spec. Throws if
// the model does not have the static shape this task recorded.
void InputSize(Ort::Session& session, const char* label, int* w, int* h) {
  Ort::TypeInfo type_info = session.GetInputTypeInfo(0);
  const auto shape = type_info.GetTensorTypeAndShapeInfo().GetShape();
  if (shape.size() != 4 || shape[1] <= 0 || shape[2] <= 0 || shape[3] != 3) {
    throw std::runtime_error(std::string(label) +
                             ": expected static NHWC input [1,H,W,3], got " +
                             ShapeToString(shape));
  }
  *h = static_cast<int>(shape[1]);
  *w = static_cast<int>(shape[2]);
}

Ort::Value MakeInput(const std::vector<float>& data, int h, int w,
                     std::vector<int64_t>* shape_storage) {
  *shape_storage = {1, h, w, 3};
  Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  return Ort::Value::CreateTensor<float>(mem, const_cast<float*>(data.data()), data.size(),
                                         shape_storage->data(), shape_storage->size());
}

void PrintTriples(const float* data, const char* heading) {
  std::printf("    %s\n", heading);
  for (int lm = 0; lm < 21; ++lm) {
    std::printf("      [%2d] (%.4f, %.4f, %.4f)\n", lm, data[lm * 3 + 0], data[lm * 3 + 1],
                data[lm * 3 + 2]);
  }
}

// Returns true if every triple is identical or every value is zero.
bool LandmarksDegenerate(const float* data) {
  bool all_zero = true;
  for (int i = 0; i < 63; ++i) {
    if (data[i] != 0.0f) {
      all_zero = false;
      break;
    }
  }
  if (all_zero) return true;

  bool all_same = true;
  for (int lm = 1; lm < 21; ++lm) {
    for (int c = 0; c < 3; ++c) {
      if (data[lm * 3 + c] != data[c]) {
        all_same = false;
        break;
      }
    }
    if (!all_same) break;
  }
  return all_same;
}

}  // namespace

int main() {
  try {
    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "validate_models");
    Ort::SessionOptions session_options;

    const std::string palm_path = HC_PALM_MODEL_PATH;
    const std::string landmark_path = HC_LANDMARK_MODEL_PATH;
    const std::string fixture_dir = HC_FIXTURE_DIR;

    std::printf("Palm model path:     %s\n", palm_path.c_str());
    std::printf("Landmark model path: %s\n\n", landmark_path.c_str());

    Ort::Session palm_session(env, palm_path.c_str(), session_options);
    Ort::Session landmark_session(env, landmark_path.c_str(), session_options);

    DescribeSession("palm_detection", palm_session);
    std::printf("\n");
    DescribeSession("hand_landmark", landmark_session);
    std::printf("\n");

    const RgbImage image =
        LoadRawFixture(fixture_dir + "/hand_rgb.raw", fixture_dir + "/hand_rgb.txt");
    std::printf("Loaded fixture: %dx%d RGB8\n\n", image.width, image.height);

    // ---- Palm model ----
    // Output names are matched exactly, not guessed from shapes: this task
    // recorded them, so a rename is a hard failure, not something to sniff.
    {
      const std::vector<std::string> expected = {"Identity", "Identity_1"};
      const std::vector<std::string> actual = OutputNames(palm_session);
      if (actual != expected) {
        std::fprintf(stderr,
                     "FATAL: palm model output names changed; expected [Identity, "
                     "Identity_1], got [");
        for (size_t i = 0; i < actual.size(); ++i)
          std::fprintf(stderr, "%s%s", i ? ", " : "", actual[i].c_str());
        std::fprintf(stderr, "]\n");
        return 1;
      }

      int w = 0, h = 0;
      InputSize(palm_session, "palm model", &w, &h);
      const std::vector<float> input_data = LetterboxToNhwc(image, w, h);
      std::vector<int64_t> shape;
      Ort::Value input_tensor = MakeInput(input_data, h, w, &shape);

      Ort::AllocatorWithDefaultOptions allocator;
      auto input_name = palm_session.GetInputNameAllocated(0, allocator);
      const char* input_names[] = {input_name.get()};
      const char* output_names[] = {expected[0].c_str(), expected[1].c_str()};

      auto outputs = palm_session.Run(Ort::RunOptions{nullptr}, input_names, &input_tensor, 1,
                                      output_names, 2);

      const auto box_shape = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
      const auto score_shape = outputs[1].GetTensorTypeAndShapeInfo().GetShape();
      std::printf("--- palm model raw outputs (letterboxed input %dx%d) ---\n", w, h);
      std::printf("  Identity   (boxes+keypoints) shape=%s\n",
                  ShapeToString(box_shape).c_str());
      std::printf("  Identity_1 (scores)          shape=%s\n",
                  ShapeToString(score_shape).c_str());

      if (box_shape.size() != 3 || score_shape.size() != 3 ||
          box_shape[1] != score_shape[1]) {
        std::fprintf(stderr, "FATAL: unexpected palm output ranks/anchor counts\n");
        return 1;
      }

      const float* scores = outputs[1].GetTensorData<float>();
      const float* boxes = outputs[0].GetTensorData<float>();
      const size_t num_anchors = static_cast<size_t>(score_shape[1]);
      const size_t box_stride = static_cast<size_t>(box_shape[2]);

      size_t best_idx = 0;
      float best_raw = scores[0];
      for (size_t i = 1; i < num_anchors; ++i) {
        if (scores[i] > best_raw) {
          best_raw = scores[i];
          best_idx = i;
        }
      }
      const float best_sigmoid = 1.0f / (1.0f + std::exp(-best_raw));
      std::printf("  num_anchors=%zu\n", num_anchors);
      std::printf("  best raw score=%.6f  sigmoid(score)=%.6f  at anchor index=%zu\n",
                  best_raw, best_sigmoid, best_idx);
      std::printf("  raw box row at best index: [");
      for (size_t c = 0; c < box_stride; ++c)
        std::printf("%s%.4f", c ? ", " : "", boxes[best_idx * box_stride + c]);
      std::printf("]\n");

      if (best_sigmoid < kPalmScoreThreshold) {
        std::fprintf(stderr,
                     "FATAL: best palm score %.6f is below MediaPipe's threshold %.2f "
                     "- no real detection on the fixture\n",
                     best_sigmoid, kPalmScoreThreshold);
        return 1;
      }
    }
    std::printf("\n");

    // ---- Landmark model ----
    {
      const std::vector<std::string> expected = {"Identity", "Identity_1", "Identity_2",
                                                 "Identity_3"};
      const std::vector<std::string> actual = OutputNames(landmark_session);
      if (actual != expected) {
        std::fprintf(stderr, "FATAL: landmark model output names changed; got [");
        for (size_t i = 0; i < actual.size(); ++i)
          std::fprintf(stderr, "%s%s", i ? ", " : "", actual[i].c_str());
        std::fprintf(stderr, "]\n");
        return 1;
      }

      int w = 0, h = 0;
      InputSize(landmark_session, "landmark model", &w, &h);
      // Per the brief: the full image resized to the model's input size.
      // This is a plain stretch; aspect ratio is NOT preserved.
      const std::vector<float> input_data = StretchToNhwc(image, w, h);
      std::vector<int64_t> shape;
      Ort::Value input_tensor = MakeInput(input_data, h, w, &shape);

      Ort::AllocatorWithDefaultOptions allocator;
      auto input_name = landmark_session.GetInputNameAllocated(0, allocator);
      const char* input_names[] = {input_name.get()};
      const char* output_names[] = {expected[0].c_str(), expected[1].c_str(),
                                    expected[2].c_str(), expected[3].c_str()};

      auto outputs = landmark_session.Run(Ort::RunOptions{nullptr}, input_names,
                                          &input_tensor, 1, output_names, 4);

      std::printf("--- landmark model raw outputs (stretched input %dx%d) ---\n", w, h);
      const char* meanings[] = {"screen-space landmarks", "hand presence",
                                "handedness", "world-space landmarks"};
      for (size_t i = 0; i < 4; ++i) {
        std::printf("  output[%zu] %-10s (%s) shape=%s\n", i, expected[i].c_str(),
                    meanings[i],
                    ShapeToString(outputs[i].GetTensorTypeAndShapeInfo().GetShape()).c_str());
      }

      const float* screen = outputs[0].GetTensorData<float>();
      const float* world = outputs[3].GetTensorData<float>();
      PrintTriples(screen, "21 screen-space landmark triples (x,y in input px, z rel. depth):");
      std::printf("  presence  (Identity_1) = %.6f\n", outputs[1].GetTensorData<float>()[0]);
      std::printf("  handedness(Identity_2) = %.6f\n", outputs[2].GetTensorData<float>()[0]);
      PrintTriples(world, "21 world-space landmark triples (metres, origin at ROI centre):");

      if (LandmarksDegenerate(screen)) {
        std::fprintf(stderr, "FATAL: screen-space landmarks are degenerate "
                             "(all zero or all identical)\n");
        return 1;
      }
      if (LandmarksDegenerate(world)) {
        std::fprintf(stderr, "FATAL: world-space landmarks are degenerate "
                             "(all zero or all identical)\n");
        return 1;
      }
    }

    std::printf("\nAll checks passed.\n");
    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "FATAL: %s\n", ex.what());
    return 1;
  }
}
