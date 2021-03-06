#include <type_traits>

#include "larq_compute_engine/core/bitpacking/utils.h"
#include "ruy/profiler/instrumentation.h"
#include "tensorflow/lite/c/common.h"
#include "tensorflow/lite/kernels/internal/cppmath.h"
#include "tensorflow/lite/kernels/internal/tensor.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/portable_type_to_tflitetype.h"

using namespace tflite;

namespace compute_engine {
namespace tflite {

using namespace core::bitpacking;
using core::TBitpacked;

TfLiteStatus QuantizePrepare(TfLiteContext* context, TfLiteNode* node) {
  TF_LITE_ENSURE_EQ(context, NumInputs(node), 1);
  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 1);

  const TfLiteTensor* input = GetInput(context, node, 0);
  TfLiteTensor* output = GetOutput(context, node, 0);

  TF_LITE_ENSURE(context, input->type == kTfLiteFloat32 ||
                              input->type == kTfLiteInt8 ||
                              input->type == kTfLiteBool);
  TF_LITE_ENSURE_EQ(context, output->type, kTfLiteInt32);

  int num_dims = NumDimensions(input);
  TF_LITE_ENSURE_EQ(context, num_dims, NumDimensions(output));

  TfLiteIntArray* output_dims = TfLiteIntArrayCopy(input->dims);

  // The last dimension is bitpacked
  output_dims->data[num_dims - 1] =
      GetBitpackedSize(SizeOfDimension(input, num_dims - 1));

  return context->ResizeTensor(context, output, output_dims);
}

TfLiteStatus DequantizePrepare(TfLiteContext* context, TfLiteNode* node) {
  TF_LITE_ENSURE_EQ(context, NumInputs(node), 1);
  TF_LITE_ENSURE_EQ(context, NumOutputs(node), 1);

  const TfLiteTensor* input = GetInput(context, node, 0);
  TfLiteTensor* output = GetOutput(context, node, 0);

  TF_LITE_ENSURE_EQ(context, input->type, kTfLiteInt32);
  TF_LITE_ENSURE(context, output->type == kTfLiteFloat32 ||
                              output->type == kTfLiteInt8 ||
                              output->type == kTfLiteBool);

  int num_dims = NumDimensions(input);

  TF_LITE_ENSURE_EQ(context, num_dims, NumDimensions(output));

  // The first n-1 dimensions are equal
  for (int i = 0; i < num_dims - 1; ++i) {
    TF_LITE_ENSURE_EQ(context, SizeOfDimension(output, i),
                      SizeOfDimension(input, i));
  }
  // The last dimension is bitpacked
  int packed_channels = SizeOfDimension(input, num_dims - 1);
  int unpacked_channels = SizeOfDimension(output, num_dims - 1);
  TF_LITE_ENSURE_EQ(context, packed_channels,
                    GetBitpackedSize(unpacked_channels));

  // We don't support resizing here, because we can not know the number of
  // output channels based on the number of input channels

  return kTfLiteOk;
}

TfLiteStatus QuantizeEval(TfLiteContext* context, TfLiteNode* node) {
  ruy::profiler::ScopeLabel label("Binary Quantize");

  const TfLiteTensor* input = GetInput(context, node, 0);
  TfLiteTensor* output = GetOutput(context, node, 0);

  if (input->type == kTfLiteFloat32) {
    bitpack_tensor(GetTensorShape(input), GetTensorData<float>(input), 0,
                   GetTensorData<TBitpacked>(output));
  } else if (input->type == kTfLiteInt8) {
    bitpack_tensor(GetTensorShape(input), GetTensorData<std::int8_t>(input),
                   input->params.zero_point, GetTensorData<TBitpacked>(output));
  } else if (input->type == kTfLiteBool) {
    // The strategy here is to interpret the input data as an unsigned integer
    // (of the same width as the bool type for the target). We then call
    // bitpacking, with a 'zero point' of 1. This means that the value with all
    // zero bits will be bitpacked as bit 1, and all other values will be
    // bitpacked as bit 0. Assuming that `false` is represented by a value with
    // all zero bits, this gives the correct result of bitpacking `false` as bit
    // 1 and `true` as bit 0.

    static_assert(std::is_same<::tflite::TfLiteTypeToType<kTfLiteBool>::Type,
                               bool>::value,
                  "");
    using BOOL_UINT = std::conditional<
        sizeof(bool) == 1, std::uint8_t,
        std::conditional<sizeof(bool) == 2, std::uint16_t,
                         std::conditional<sizeof(bool) == 4, std::uint32_t,
                                          std::uint64_t>::type>::type>::type;
    static_assert(sizeof(bool) == sizeof(BOOL_UINT), "");

    bitpack_tensor(GetTensorShape(input), GetTensorData<BOOL_UINT>(input),
                   BOOL_UINT(1), GetTensorData<TBitpacked>(output));
  } else {
    return kTfLiteError;
  }

  return kTfLiteOk;
}

TfLiteStatus DequantizeEval(TfLiteContext* context, TfLiteNode* node) {
  ruy::profiler::ScopeLabel label("Binary Dequantize");

  const TfLiteTensor* input = GetInput(context, node, 0);
  TfLiteTensor* output = GetOutput(context, node, 0);

  auto out_shape = GetTensorShape(output);
  int dims = out_shape.DimensionsCount();
  int num_rows = FlatSizeSkipDim(out_shape, dims - 1);
  int num_cols = out_shape.Dims(dims - 1);

  if (output->type == kTfLiteFloat32) {
    unpack_matrix(GetTensorData<TBitpacked>(input), num_rows, num_cols,
                  GetTensorData<float>(output));
  } else if (output->type == kTfLiteInt8) {
    int offset = TfLiteRound(1.0f / output->params.scale);
    std::int8_t zero_bit_result =
        std::min(127, output->params.zero_point + offset);
    std::int8_t one_bit_result =
        std::max(-128, output->params.zero_point - offset);
    unpack_matrix(GetTensorData<TBitpacked>(input), num_rows, num_cols,
                  GetTensorData<std::int8_t>(output), zero_bit_result,
                  one_bit_result);
  } else if (output->type == kTfLiteBool) {
    unpack_matrix(GetTensorData<TBitpacked>(input), num_rows, num_cols,
                  GetTensorData<bool>(output), true, false);
  } else {
    return kTfLiteError;
  }

  return kTfLiteOk;
}

TfLiteRegistration* Register_QUANTIZE() {
  static TfLiteRegistration r = {nullptr, nullptr, QuantizePrepare,
                                 QuantizeEval};
  return &r;
}

TfLiteRegistration* Register_DEQUANTIZE() {
  static TfLiteRegistration r = {nullptr, nullptr, DequantizePrepare,
                                 DequantizeEval};
  return &r;
}

}  // namespace tflite
}  // namespace compute_engine
