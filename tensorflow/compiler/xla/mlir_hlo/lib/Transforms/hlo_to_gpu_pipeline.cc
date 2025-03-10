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

/// This files contains a pipeline which converts HLO operations to GPU kernels
/// written in a combination of LLVM and NVVM dialects.

#include "gml_st/transforms/passes.h"
#include "mlir-hlo/Dialect/mhlo/transforms/passes.h"
#include "mlir-hlo/Transforms/gpu_passes.h"
#include "mlir-hlo/Transforms/passes.h"
#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Conversion/ShapeToStandard/ShapeToStandard.h"
#include "mlir/Dialect/Arith/Transforms/Passes.h"
#include "mlir/Dialect/Bufferization/Transforms/Passes.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/Transforms/Passes.h"
#include "mlir/Dialect/Linalg/Passes.h"
#include "mlir/Dialect/SCF/Transforms/Passes.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/Passes.h"

using namespace mlir;
using ::mlir::func::FuncOp;
using ::mlir::gpu::GPUModuleOp;

static constexpr const char* kBlockDistributionLabel = "block";
static constexpr const char* kWarpDistributionLabel = "warp";
static constexpr const char* kThreadDistributionLabel = "thread";

// TODO(b/233761238): We only want to have this pipeline temporarily, as it is
// not yet clear how exactly it will look like. The goal is to merge this with
// the unified kernel generator + autofusion + XLA Next pipeline once we have
// it, and once this code stabilizes.
void mlir::createHloToGpuPipeline(OpPassManager& pm,
                                  ArrayRef<int64_t> blockTileDim,
                                  ArrayRef<int64_t> warpTileDim,
                                  ArrayRef<int64_t> threadTileDim,
                                  bool experimentalSoftmax) {
  pm.addNestedPass<FuncOp>(hlo::createUnbufferizePass());
  pm.addPass(createCanonicalizerPass());  // Clean up get_tuple_element.
  pm.addPass(createCSEPass());  // Combine repeated subtract(broadcast).

  // HLO -> Linalg
  pm.addNestedPass<FuncOp>(mhlo::createChloLegalizeToHloPass());
  pm.addPass(createCanonicalizerPass());  // Clean up shape.assuming ops.
  pm.addNestedPass<FuncOp>(mhlo::createLegalizeHloToLinalgPass());

  // Perform tiling either for softmax or for element-wise.
  if (experimentalSoftmax) {
    // Simplify unit dimension.
    pm.addPass(mlir::createLinalgFoldUnitExtentDimsPass());

    // Tile parallel dimensions of the softmax-like patterns and distribute them
    // across warps. Warps remain independant of each other.
    pm.addNestedPass<FuncOp>(gml_st::createTilingSoftmaxPass(
        /*distribute=*/true, blockTileDim, kBlockDistributionLabel));
    pm.addNestedPass<FuncOp>(gml_st::createTilingSoftmaxPass(
        /*distribute=*/true, warpTileDim, kWarpDistributionLabel));

    // GPU-specific tiling for ops on the warp level.
    pm.addNestedPass<FuncOp>(gml_st::createTilingGpuWarpPass());
    pm.addNestedPass<FuncOp>(createScalarizationPass());

    pm.addNestedPass<FuncOp>(gml_st::createVectorizeGmlStLoopsPass(
        /*vectorizeGmlStOps=*/true, /*distributionLabels=*/{
            kWarpDistributionLabel, kThreadDistributionLabel}));
  } else {
    // TODO(b/244313563): This is a workaround to avoid temporary allocs within
    // threads. It works for as long as all of our operations are cwise.
    // Vectorize the inner loops instead.
    // TODO(frgossen): We should not have to skip this pass for softmax.
    pm.addNestedPass<FuncOp>(createLinalgElementwiseOpFusionPass());

    // Tiling
    pm.addNestedPass<FuncOp>(gml_st::createTilingCwisePass(
        /*distribute=*/true, blockTileDim, kBlockDistributionLabel));
    pm.addNestedPass<FuncOp>(gml_st::createTilingCwisePass(
        /*distribute=*/true, warpTileDim, kWarpDistributionLabel));
    pm.addNestedPass<FuncOp>(gml_st::createTilingCwisePass(
        /*distribute=*/true, threadTileDim, kThreadDistributionLabel));
    pm.addNestedPass<FuncOp>(createScalarizationPass());
  }

  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());

  // Bufferization-related passes.
  pm.addNestedPass<FuncOp>(bufferization::createEmptyTensorToAllocTensorPass());
  pm.addPass(hlo::createOneShotBufferizePass());
  // We do not deallocate buffers, since grid-level buffers get converted into
  // functions arguments, while block- (and lower-)level buffers become shared
  // memory. None of which have to be deallocated.
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createCSEPass());
  // Canonicalize away memory copies into itself
  pm.addPass(createCanonicalizerPass());

  // Linalg + GmlSt -> GPU
  pm.addNestedPass<FuncOp>(
      gml_st::createGmlStSimtfyPass(kBlockDistributionLabel));
  pm.addNestedPass<FuncOp>(
      gml_st::createGmlStToGpuPass(kWarpDistributionLabel));
  pm.addNestedPass<FuncOp>(gml_st::createGmlStToScfPass());
  pm.addNestedPass<FuncOp>(arith::createArithExpandOpsPass());
  pm.addNestedPass<FuncOp>(createConvertLinalgToLoopsPass());
  pm.addNestedPass<FuncOp>(createCanonicalizerPass());
  pm.addPass(createGpuLauchSinkIndexComputationsPass());
  constexpr llvm::StringRef kGpuDataLayoutSpec =
      "#dlti.dl_spec<#dlti.dl_entry<index,32:i32>>";
  pm.addPass(createGpuKernelOutliningPass(kGpuDataLayoutSpec));
  pm.addNestedPass<GPUModuleOp>(createForLoopSpecializationPass());
  pm.addNestedPass<GPUModuleOp>(hlo::createUnrollLoopsPass());
  pm.addNestedPass<GPUModuleOp>(createLowerAffinePass());
  pm.addNestedPass<GPUModuleOp>(createCanonicalizerPass());
  pm.addNestedPass<GPUModuleOp>(createConvertSCFToCFPass());

  // GPU -> low-level IR
#if TENSORFLOW_USE_ROCM
  pm.addNestedPass<GPUModuleOp>(createGpuKernelToRocdlPass());
#else
  pm.addNestedPass<GPUModuleOp>(createGpuKernelToNvvmPass());
#endif
  pm.addPass(createPropagateStaticShapesToKernelPass());
  // We do not do this as a nested pass in order to properly clean the host
  // side of the generated code.
  pm.addPass(createCSEPass());
  // Some instructions crash ptxas down the line if they have debug info
  // attached.
  pm.addNestedPass<GPUModuleOp>(createStripDebugInfoPass());
  pm.addNestedPass<FuncOp>(hlo::createAllocToArgPass());
}
