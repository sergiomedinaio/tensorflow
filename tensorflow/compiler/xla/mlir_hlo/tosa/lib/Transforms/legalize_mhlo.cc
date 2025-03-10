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

#include <utility>

#include "mhlo_tosa/Transforms/passes.h"
#include "mlir-hlo/Dialect/mhlo/IR/hlo_ops.h"
#include "mlir/Dialect/Quant/QuantTypes.h"
#include "mlir/Dialect/Tosa/IR/TosaOps.h"
#include "mlir/IR/BuiltinAttributes.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#define GEN_PASS_DEF_TOSALEGALIZEMHLOPASS
#include "mhlo_tosa/Transforms/passes.h.inc"

#define PASS_NAME "tosa-legalize-tf"
#define DEBUG_TYPE PASS_NAME

#include "mhlo_tosa/Transforms/legalize_mhlo.pdll.h.inc"

namespace mlir {
namespace tosa {
namespace {

struct LegalizeMhlo : ::impl::TosaLegalizeMhloPassBase<LegalizeMhlo> {
  void runOnOperation() final;

  LogicalResult initialize(MLIRContext* ctx) override;

 private:
  FrozenRewritePatternSet patterns;
};

struct ConvertMhloCompareOp : public OpRewritePattern<mhlo::CompareOp> {
  using OpRewritePattern<mhlo::CompareOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mhlo::CompareOp op,
                                PatternRewriter& rewriter) const override {
    auto direction = op.getComparisonDirection();
    auto resultType = op->getResultTypes().front();

    switch (direction) {
      case mlir::mhlo::ComparisonDirection::EQ: {
        rewriter.replaceOpWithNewOp<tosa::EqualOp>(op, resultType, op.getLhs(),
                                                   op.getRhs());
        break;
      }
      case mlir::mhlo::ComparisonDirection::NE: {
        auto equalOp = rewriter.create<tosa::EqualOp>(op->getLoc(), resultType,
                                                      op.getLhs(), op.getRhs());
        rewriter.replaceOpWithNewOp<tosa::LogicalNotOp>(op, resultType,
                                                        equalOp);
        break;
      }
      default: {
        return rewriter.notifyMatchFailure(
            op, "comparison direction not yet implemented");
      }
    }
    return success();
  }
};

// TODO(jennik): Move this lowering to PDLL when variadic tensors are supported.
struct ConvertMhloConcatenateOp : public OpRewritePattern<mhlo::ConcatenateOp> {
  using OpRewritePattern<mhlo::ConcatenateOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mhlo::ConcatenateOp op,
                                PatternRewriter& rewriter) const override {
    rewriter.replaceOpWithNewOp<tosa::ConcatOp>(op, op.getResult().getType(),
                                                op.getVal(), op.getDimension());
    return success();
  }
};

struct ConvertMhloDotOp : public OpRewritePattern<mhlo::DotOp> {
  using OpRewritePattern<mhlo::DotOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mhlo::DotOp op,
                                PatternRewriter& rewriter) const override {
    auto lhsType = op.getLhs().getType().dyn_cast<RankedTensorType>();
    auto rhsType = op.getRhs().getType().dyn_cast<RankedTensorType>();
    if (!lhsType | !rhsType) {
      return rewriter.notifyMatchFailure(op, "input tensors are not ranked");
    }

    auto resultType = op.getResult().getType().dyn_cast<ShapedType>();
    if (!resultType) {
      return rewriter.notifyMatchFailure(op,
                                         "result tensor does not have shape");
    }

    if (lhsType.getElementType() != rhsType.getElementType()) {
      return rewriter.notifyMatchFailure(
          op, "lhs and rhs element types must match");
    }

    auto lhsShape = lhsType.getShape();
    auto rhsShape = rhsType.getShape();
    auto resultShape = resultType.getShape();
    llvm::SmallVector<int64_t, 3> lhsReshape;
    llvm::SmallVector<int64_t, 3> rhsReshape;
    llvm::SmallVector<int64_t, 3> matMulShape;

    // tosa.matmul requires input tensors to have a rank of 3, so lhs and rhs
    // need to be reshaped first.
    if (lhsType.getRank() == 1) {
      // Reshape lhs to [1, 1, N].
      lhsReshape = {1, 1, lhsShape[0]};
      if (rhsType.getRank() == 1) {
        // Reshape rhs to [1, N, 1].
        rhsReshape = {1, rhsShape[0], 1};
        // MatMul shape is [1, 1, N].
        matMulShape = {1, 1, lhsShape[0]};
      } else if (rhsType.getRank() == 2) {
        // Reshape rhs to [1, N, K].
        rhsReshape = {1, rhsShape[0], rhsShape[1]};
        // MatMul shape is [1, 1, K].
        matMulShape = {1, 1, rhsShape[1]};
      } else {
        return rewriter.notifyMatchFailure(op, "rhs must have rank of 1 or 2");
      }
    } else if (lhsType.getRank() == 2) {
      // Reshape lhs to [1, M, K].
      lhsReshape = {1, lhsShape[0], lhsShape[1]};
      if (rhsType.getRank() == 1) {
        // Reshape rhs to [1, K, 1].
        rhsReshape = {1, rhsShape[0], 1};
        // MatMul shape is [1, M, 1].
        matMulShape = {1, lhsShape[0], 1};
      } else if (rhsType.getRank() == 2) {
        // Reshape rhs to [1, K, N].
        rhsReshape = {1, rhsShape[0], rhsShape[1]};
        // MatMul shape is [1, M, N].
        matMulShape = {1, lhsShape[0], rhsShape[1]};
      } else {
        return rewriter.notifyMatchFailure(op, "rhs must have rank of 1 or 2");
      }
    } else {
      return rewriter.notifyMatchFailure(op, "lhs must have rank of 1 or 2");
    }

    auto lhsReshapeType =
        RankedTensorType::get(lhsReshape, lhsType.getElementType());
    auto lhsReshapeOp = rewriter.create<tosa::ReshapeOp>(
        op->getLoc(), lhsReshapeType, op.getLhs(),
        rewriter.getI64ArrayAttr(lhsReshape));

    auto rhsReshapeType =
        RankedTensorType::get(rhsReshape, rhsType.getElementType());
    auto rhsReshapeOp = rewriter.create<tosa::ReshapeOp>(
        op->getLoc(), rhsReshapeType, op.getRhs(),
        rewriter.getI64ArrayAttr(rhsReshape));

    auto matMulType =
        RankedTensorType::get(matMulShape, lhsType.getElementType());
    auto matMulOp = rewriter.create<tosa::MatMulOp>(op->getLoc(), matMulType,
                                                    lhsReshapeOp, rhsReshapeOp);

    // Reshape the matmul result back to the original result shape.
    rewriter.replaceOpWithNewOp<tosa::ReshapeOp>(
        op, resultType, matMulOp, rewriter.getI64ArrayAttr(resultShape));
    return success();
  }
};

// TODO(jennik): Consider the case of a non-constant expansion.
struct ConvertMhloIotaOp : public OpRewritePattern<mhlo::IotaOp> {
  using OpRewritePattern<mhlo::IotaOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mhlo::IotaOp op,
                                PatternRewriter& rewriter) const override {
    auto resultType = op.getResult().getType();
    auto elementType = resultType.cast<ShapedType>().getElementType();
    auto resultRankedType = resultType.dyn_cast<RankedTensorType>();

    if (!resultRankedType) {
      return rewriter.notifyMatchFailure(op, "result tensor must be ranked");
    }
    if (!resultRankedType.hasStaticShape()) {
      return rewriter.notifyMatchFailure(op, "result tensor must be static");
    }

    auto resultShape = resultRankedType.getShape();
    auto iotaDimension = op.getIotaDimension();
    int64_t iotaArrayLength = resultShape[iotaDimension];

    // Create a const op of [0, 1, 2...iotaArrayLength - 1] to be tiled.
    llvm::SmallVector<mlir::Attribute, 4> constValues;
    constValues.resize(iotaArrayLength);
    for (int i = 0; i < iotaArrayLength; i++) {
      if (elementType.isa<FloatType>()) {
        constValues[i] = rewriter.getFloatAttr(elementType, i);
      } else {
        constValues[i] = rewriter.getIntegerAttr(elementType, i);
      }
    }

    RankedTensorType constType =
        RankedTensorType::get(iotaArrayLength, elementType);
    auto constOp = rewriter.create<tosa::ConstOp>(
        op.getLoc(), constType, DenseElementsAttr::get(constType, constValues));

    // Create the multiples attr for the tile op, where all dimensions except
    // the iota dimension are multiplied.
    llvm::SmallVector<int64_t, 4> tileMultiples;
    size_t tileMultiplesSize = resultShape.size();
    tileMultiples.resize(tileMultiplesSize);

    for (size_t i = 0; i < tileMultiplesSize; i++) {
      if (i == iotaDimension) {
        tileMultiples[i] = 1;
      } else {
        tileMultiples[i] = resultShape[i];
      }
    }

    // Tile the const array to the result shape of the iota op.
    rewriter.replaceOpWithNewOp<tosa::TileOp>(
        op, resultType, constOp, rewriter.getI64ArrayAttr(tileMultiples));
    return success();
  }
};

struct ConvertMhloReduceOp : public OpRewritePattern<mhlo::ReduceOp> {
  using OpRewritePattern<mhlo::ReduceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mhlo::ReduceOp op,
                                PatternRewriter& rewriter) const override {
    Block& bodyBlock = op.getBody().front();

    // To lower to a tosa.reduce_* op, the body should contain the reduce op and
    // a return op.
    if (bodyBlock.getOperations().size() != 2) {
      return rewriter.notifyMatchFailure(op, "body required to contain 2 ops");
    }

    auto operands = op.getInputs().front();
    ShapedType inputType = operands.getType().cast<ShapedType>();
    uint64_t dimension = op.getDimensions().getValues<uint64_t>().begin()[0];
    Operation& innerOp = bodyBlock.front();
    Value reduceOpResult;

    if (isa<mhlo::AddOp>(innerOp)) {
      reduceOpResult =
          rewriter
              .create<tosa::ReduceSumOp>(op->getLoc(), inputType, operands,
                                         rewriter.getI64IntegerAttr(dimension))
              .getResult();
    } else if (isa<mhlo::MaxOp>(innerOp)) {
      reduceOpResult =
          rewriter
              .create<tosa::ReduceMaxOp>(op->getLoc(), inputType, operands,
                                         rewriter.getI64IntegerAttr(dimension))
              .getResult();
    } else {
      return rewriter.notifyMatchFailure(
          op, "reducing along a " + innerOp.getName().getStringRef().str() +
                  " op not supported");
    }

    // TOSA reduce ops do not remove the dimension being reduced, so reshape the
    // reduced output and remove the reduction dimension.
    ArrayRef<int64_t> innerShape = inputType.getShape();
    llvm::SmallVector<int64_t, 2> outputShape;
    int outputShapeLength = innerShape.size() - 1;
    outputShape.resize(outputShapeLength);
    for (int64_t i = 0; i < outputShapeLength; i++) {
      if (i < static_cast<int64_t>(dimension)) {
        outputShape[i] = innerShape[i];
      } else {
        outputShape[i] = innerShape[i + 1];
      }
    }

    rewriter
        .replaceOpWithNewOp<tosa::ReshapeOp>(
            op, op.getResultTypes().front(), reduceOpResult,
            rewriter.getI64ArrayAttr(outputShape))
        .getResult();

    return success();
  }
};

struct ConvertMhloSliceOp : public OpRewritePattern<mhlo::SliceOp> {
  using OpRewritePattern<mhlo::SliceOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mhlo::SliceOp op,
                                PatternRewriter& rewriter) const override {
    auto rank = op.getOperand().getType().getRank();
    if (rank < 1 || rank > 6) {
      return rewriter.notifyMatchFailure(
          op, "tosa.slice only supports 1D to 6D tensors");
    }

    auto strides = op.getStrides().getValues<int64_t>();
    for (auto stride : strides) {
      if (stride != 1) {
        return rewriter.notifyMatchFailure(
            op, "tosa.slice only supports strides of 1");
      }
    }

    auto startIndices = op.getStartIndices().getValues<int64_t>();
    auto endIndices = op.getLimitIndices().getValues<int64_t>();

    llvm::SmallVector<int64_t, 2> size;
    size.resize(startIndices.size());
    llvm::SmallVector<int64_t, 2> startIndicesI64;
    startIndicesI64.resize(startIndices.size());

    for (int64_t i = 0; i < static_cast<int64_t>(startIndices.size()); i++) {
      size[i] = endIndices[i] - startIndices[i];
      startIndicesI64[i] = startIndices[i];
    }

    rewriter.replaceOpWithNewOp<tosa::SliceOp>(
        op, op.getResult().getType(), op.getOperand(),
        rewriter.getI64ArrayAttr(startIndicesI64),
        rewriter.getI64ArrayAttr(size));
    return success();
  }
};

struct ConvertMhloTransposeOp : public OpRewritePattern<mhlo::TransposeOp> {
  using OpRewritePattern<mhlo::TransposeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(mhlo::TransposeOp op,
                                PatternRewriter& rewriter) const override {
    auto rank = op.getOperand().getType().getRank();
    if (rank < 1 || rank > 6) {
      return rewriter.notifyMatchFailure(
          op, "tosa.transpose only supports 1D to 6D tensors");
    }

    auto perms = op.getPermutation();
    auto constOp = rewriter.create<tosa::ConstOp>(
        op->getLoc(),
        RankedTensorType::get({perms.size()}, rewriter.getI64Type()), perms);
    rewriter.replaceOpWithNewOp<tosa::TransposeOp>(op, op.getResult().getType(),
                                                   op.getOperand(), constOp);
    return success();
  }
};

LogicalResult LegalizeMhlo::initialize(MLIRContext* ctx) {
  RewritePatternSet patternList(ctx);
  populateGeneratedPDLLPatterns(patternList);
  patternList.addWithLabel<ConvertMhloCompareOp>({"MhloCompare"}, ctx);
  patternList.addWithLabel<ConvertMhloConcatenateOp>({"MhloConcatenate"}, ctx);
  patternList.addWithLabel<ConvertMhloDotOp>({"MhloDot"}, ctx);
  patternList.addWithLabel<ConvertMhloIotaOp>({"MhloIota"}, ctx);
  patternList.addWithLabel<ConvertMhloReduceOp>({"MhloReduce"}, ctx);
  patternList.addWithLabel<ConvertMhloSliceOp>({"MhloSlice"}, ctx);
  patternList.addWithLabel<ConvertMhloTransposeOp>({"MhloTranspose"}, ctx);
  patterns = std::move(patternList);
  return success();
}

void LegalizeMhlo::runOnOperation() {
  (void)applyPatternsAndFoldGreedily(getOperation(), patterns);
}

}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>> createLegalizeMhloPass() {
  return std::make_unique<LegalizeMhlo>();
}

}  // namespace tosa
}  // namespace mlir
