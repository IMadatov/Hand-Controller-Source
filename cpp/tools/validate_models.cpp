// Standalone diagnostic tool for Task 1: loads the palm-detection and
// hand-landmark ONNX models, prints every input/output tensor's name,
// shape and element type, then runs both models on the committed fixture
// image and prints the raw outputs so a human can confirm the models are
// usable before any later task depends on their shapes.
//
// This program intentionally does NOT depend on any of the (not yet
// existing) cpp/core/ library: Task 1 only creates the fetch logic and
// this tool. All image loading/resizing here is scratch code for
// diagnostics, not production preprocessing.

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

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

// Prints every input and output tensor's name, shape and element type for
// a session, as required by Task 1 step 5.
void DescribeSession(const char* label, Ort::Session& session) {
  Ort::AllocatorWithDefaultOptions allocator;
  std::printf("=== %s ===\n", label);

  size_t num_inputs = session.GetInputCount();
  for (size_t i = 0; i < num_inputs; ++i) {
    auto name = session.GetInputNameAllocated(i, allocator);
    Ort::TypeInfo type_info = session.GetInputTypeInfo(i);
    auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
    std::printf("  input[%zu]  name=%-20s shape=%-24s dtype=%s\n", i,
                name.get(), ShapeToString(tensor_info.GetShape()).c_str(),
                ElementTypeName(tensor_info.GetElementType()));
  }

  size_t num_outputs = session.GetOutputCount();
  for (size_t i = 0; i < num_outputs; ++i) {
    auto name = session.GetOutputNameAllocated(i, allocator);
    Ort::TypeInfo type_info = session.GetOutputTypeInfo(i);
    auto tensor_info = type_info.GetTensorTypeAndShapeInfo();
    std::printf("  output[%zu] name=%-20s shape=%-24s dtype=%s\n", i,
                name.get(), ShapeToString(tensor_info.GetShape()).c_str(),
                ElementTypeName(tensor_info.GetElementType()));
  }
}

struct RgbImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> data;  // interleaved RGB8, row-major
};

// Loads the committed raw fixture: <width> <height> 3 in the .txt sidecar,
// and width*height*3 interleaved RGB8 bytes in the .raw file. Uses fread
// only, matching the constraint that core (and this tool) may not link an
// image library.
RgbImage LoadRawFixture(const std::string& raw_path, const std::string& txt_path) {
  RgbImage image;
  {
    std::ifstream txt(txt_path);
    if (!txt) throw std::runtime_error("cannot open " + txt_path);
    int channels = 0;
    txt >> image.width >> image.height >> channels;
    if (channels != 3) throw std::runtime_error("fixture must have 3 channels");
  }

  std::FILE* f = std::fopen(raw_path.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open " + raw_path);
  size_t expected = static_cast<size_t>(image.width) * image.height * 3;
  image.data.resize(expected);
  size_t got = std::fread(image.data.data(), 1, expected, f);
  std::fclose(f);
  if (got != expected) {
    throw std::runtime_error("short read on " + raw_path);
  }
  return image;
}

// Nearest-neighbor resize with letterboxing (preserve aspect ratio, pad with
// zeros) to a square target size. This is throwaway diagnostic preprocessing
// for this tool only; the real resampler is Task 4's job.
std::vector<float> LetterboxToChw(const RgbImage& img, int target) {
  float scale = std::min(static_cast<float>(target) / img.width,
                          static_cast<float>(target) / img.height);
  int scaled_w = static_cast<int>(img.width * scale);
  int scaled_h = static_cast<int>(img.height * scale);
  int pad_x = (target - scaled_w) / 2;
  int pad_y = (target - scaled_h) / 2;

  std::vector<float> chw(static_cast<size_t>(3) * target * target, 0.0f);
  for (int y = 0; y < scaled_h; ++y) {
    int src_y = std::min(img.height - 1, static_cast<int>(y / scale));
    for (int x = 0; x < scaled_w; ++x) {
      int src_x = std::min(img.width - 1, static_cast<int>(x / scale));
      const uint8_t* px = &img.data[(static_cast<size_t>(src_y) * img.width + src_x) * 3];
      int dst_y = y + pad_y;
      int dst_x = x + pad_x;
      for (int c = 0; c < 3; ++c) {
        chw[static_cast<size_t>(c) * target * target + dst_y * target + dst_x] =
            px[c] / 255.0f;
      }
    }
  }
  return chw;
}

// Simple whole-image resize (no letterbox) to width x height, NHWC uint8->float/255,
// used for the landmark model which expects a square crop already (we just
// resize the full fixture into it for this diagnostic run).
std::vector<float> ResizeToNhwc(const RgbImage& img, int target_w, int target_h) {
  std::vector<float> nhwc(static_cast<size_t>(target_h) * target_w * 3);
  for (int y = 0; y < target_h; ++y) {
    int src_y = std::min(img.height - 1,
                          static_cast<int>(y * static_cast<float>(img.height) / target_h));
    for (int x = 0; x < target_w; ++x) {
      int src_x = std::min(img.width - 1,
                            static_cast<int>(x * static_cast<float>(img.width) / target_w));
      const uint8_t* px = &img.data[(static_cast<size_t>(src_y) * img.width + src_x) * 3];
      size_t dst = (static_cast<size_t>(y) * target_w + x) * 3;
      nhwc[dst + 0] = px[0] / 255.0f;
      nhwc[dst + 1] = px[1] / 255.0f;
      nhwc[dst + 2] = px[2] / 255.0f;
    }
  }
  return nhwc;
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

    RgbImage image = LoadRawFixture(fixture_dir + "/hand_rgb.raw", fixture_dir + "/hand_rgb.txt");
    std::printf("Loaded fixture: %dx%d RGB8\n\n", image.width, image.height);

    // ---- Run palm model ----
    {
      Ort::AllocatorWithDefaultOptions allocator;
      auto input_name = palm_session.GetInputNameAllocated(0, allocator);
      auto input_shape = palm_session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
      // Expect NHWC [1, H, W, 3] per opencv_zoo's mp_palmdet.py preprocessing.
      int target_h = static_cast<int>(input_shape[1]);
      int target_w = static_cast<int>(input_shape[2]);
      if (target_h <= 0 || target_w <= 0) {
        // Dynamic dims reported as -1; fall back to the known 192x192 used
        // by opencv_zoo's MPPalmDet.
        target_h = target_w = 192;
      }

      std::vector<float> input_data = ResizeToNhwc(image, target_w, target_h);
      std::vector<int64_t> shape = {1, target_h, target_w, 3};
      Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
          mem_info, input_data.data(), input_data.size(), shape.data(), shape.size());

      std::vector<const char*> input_names = {input_name.get()};
      std::vector<std::string> output_name_storage;
      std::vector<const char*> output_names;
      for (size_t i = 0; i < palm_session.GetOutputCount(); ++i) {
        auto n = palm_session.GetOutputNameAllocated(i, allocator);
        output_name_storage.emplace_back(n.get());
      }
      for (auto& s : output_name_storage) output_names.push_back(s.c_str());

      auto outputs = palm_session.Run(Ort::RunOptions{nullptr}, input_names.data(), &input_tensor,
                                       1, output_names.data(), output_names.size());

      std::printf("--- palm model raw outputs (input %dx%d) ---\n", target_w, target_h);
      // Identify score tensor (last dim == 1) vs box/landmark tensor (last dim > 4).
      int score_idx = -1;
      int box_idx = -1;
      for (size_t i = 0; i < outputs.size(); ++i) {
        auto shape_i = outputs[i].GetTensorTypeAndShapeInfo().GetShape();
        std::printf("  output[%zu] (%s) shape=%s\n", i, output_name_storage[i].c_str(),
                    ShapeToString(shape_i).c_str());
        if (!shape_i.empty() && shape_i.back() == 1) score_idx = static_cast<int>(i);
        if (!shape_i.empty() && shape_i.back() > 4) box_idx = static_cast<int>(i);
      }

      if (score_idx >= 0 && box_idx >= 0) {
        const float* scores = outputs[score_idx].GetTensorData<float>();
        const float* boxes = outputs[box_idx].GetTensorData<float>();
        auto score_shape = outputs[score_idx].GetTensorTypeAndShapeInfo().GetShape();
        auto box_shape = outputs[box_idx].GetTensorTypeAndShapeInfo().GetShape();
        size_t num_anchors = score_shape[score_shape.size() - 2];
        size_t box_stride = box_shape[box_shape.size() - 1];

        size_t best_idx = 0;
        float best_raw_score = scores[0];
        for (size_t i = 1; i < num_anchors; ++i) {
          if (scores[i] > best_raw_score) {
            best_raw_score = scores[i];
            best_idx = i;
          }
        }
        float best_sigmoid = 1.0f / (1.0f + std::exp(-best_raw_score));
        std::printf("  num_anchors=%zu\n", num_anchors);
        std::printf("  best raw score=%.6f  sigmoid(score)=%.6f  at anchor index=%zu\n",
                    best_raw_score, best_sigmoid, best_idx);
        std::printf("  raw box row at best index: [");
        for (size_t c = 0; c < box_stride; ++c) {
          std::printf("%s%.4f", c ? ", " : "", boxes[best_idx * box_stride + c]);
        }
        std::printf("]\n");
      } else {
        std::printf("  WARNING: could not identify score/box output tensors by shape heuristic\n");
      }
    }
    std::printf("\n");

    // ---- Run landmark model ----
    {
      Ort::AllocatorWithDefaultOptions allocator;
      auto input_name = landmark_session.GetInputNameAllocated(0, allocator);
      auto input_shape =
          landmark_session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
      int target_h = static_cast<int>(input_shape[1]);
      int target_w = static_cast<int>(input_shape[2]);
      if (target_h <= 0 || target_w <= 0) {
        target_h = target_w = 224;  // opencv_zoo MPHandPose default.
      }

      std::vector<float> input_data = ResizeToNhwc(image, target_w, target_h);
      std::vector<int64_t> shape = {1, target_h, target_w, 3};
      Ort::MemoryInfo mem_info = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
      Ort::Value input_tensor = Ort::Value::CreateTensor<float>(
          mem_info, input_data.data(), input_data.size(), shape.data(), shape.size());

      std::vector<const char*> input_names = {input_name.get()};
      std::vector<std::string> output_name_storage;
      std::vector<const char*> output_names;
      for (size_t i = 0; i < landmark_session.GetOutputCount(); ++i) {
        auto n = landmark_session.GetOutputNameAllocated(i, allocator);
        output_name_storage.emplace_back(n.get());
      }
      for (auto& s : output_name_storage) output_names.push_back(s.c_str());

      auto outputs = landmark_session.Run(Ort::RunOptions{nullptr}, input_names.data(),
                                           &input_tensor, 1, output_names.data(),
                                           output_names.size());

      std::printf("--- landmark model raw outputs (input %dx%d) ---\n", target_w, target_h);
      for (size_t i = 0; i < outputs.size(); ++i) {
        auto shape_i = outputs[i].GetTensorTypeAndShapeInfo().GetShape();
        size_t count = 1;
        for (auto d : shape_i) count *= static_cast<size_t>(std::max<int64_t>(d, 1));
        std::printf("  output[%zu] (%s) shape=%s\n", i, output_name_storage[i].c_str(),
                    ShapeToString(shape_i).c_str());

        const float* data = outputs[i].GetTensorData<float>();
        if (count == 63) {
          std::printf("    21 landmark triples:\n");
          for (int lm = 0; lm < 21; ++lm) {
            std::printf("      [%2d] (%.4f, %.4f, %.4f)\n", lm, data[lm * 3 + 0],
                        data[lm * 3 + 1], data[lm * 3 + 2]);
          }
        } else if (count == 1) {
          std::printf("    scalar value = %.6f\n", data[0]);
        } else {
          std::printf("    (%zu raw values, first few: ", count);
          for (size_t v = 0; v < std::min<size_t>(count, 6); ++v) {
            std::printf("%s%.4f", v ? ", " : "", data[v]);
          }
          std::printf(")\n");
        }
      }
    }

    return 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "FATAL: %s\n", ex.what());
    return 1;
  }
}
