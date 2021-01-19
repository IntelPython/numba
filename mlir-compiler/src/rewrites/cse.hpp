#pragma once

#include <mlir/Support/LogicalResult.h>
#include <mlir/IR/PatternMatch.h>

namespace CSE
{
namespace detail
{
mlir::LogicalResult applyCSE(mlir::Region& region, mlir::PatternRewriter& rewriter);
}
}

template<typename Op>
struct CSERewrite : public mlir::OpRewritePattern<Op>
{
    CSERewrite(mlir::MLIRContext *context):
        OpRewritePattern(context, /*benefit*/0) {}

    mlir::LogicalResult matchAndRewrite(
        Op op, mlir::PatternRewriter &rewriter) const override
    {
        return ::CSE::detail::applyCSE(op.getRegion(), rewriter);
    }
};
