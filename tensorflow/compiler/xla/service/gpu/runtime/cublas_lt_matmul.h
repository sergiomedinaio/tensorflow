/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_GPU_RUNTIME_CUBLAS_LT_MATMUL_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_GPU_RUNTIME_CUBLAS_LT_MATMUL_H_

#include "tensorflow/compiler/xla/runtime/custom_call.h"
#include "tensorflow/compiler/xla/runtime/custom_call_registry.h"

#if GOOGLE_CUDA
#include "tensorflow/compiler/xla/stream_executor/cuda/cuda_blas_lt.h"
#endif  // GOOGLE_CUDA

namespace xla {
namespace gpu {

// Registers XLA Gpu runtime kernel launch custom calls.
void RegisterMatmulCustomCalls(runtime::DirectCustomCallRegistry& registry);

}  // namespace gpu
}  // namespace xla

namespace xla {
namespace runtime {

#if GOOGLE_CUDA
XLA_RUNTIME_REGISTER_ENUM_ATTR_DECODING(
    stream_executor::cuda::BlasLt::Epilogue);
#endif  // GOOGLE_CUDA

}  // namespace runtime
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_GPU_RUNTIME_CUBLAS_LT_MATMUL_H_
