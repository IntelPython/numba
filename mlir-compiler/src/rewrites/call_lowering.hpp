#pragma once

#include <functional>

#include "plier/dialect.hpp"

#include <mlir/IR/PatternMatch.h>

namespace mlir
{
class TypeConverter;
}

struct CallOpLowering : public mlir::OpRewritePattern<plier::PyCallOp>
{
    using resolver_t = std::function<mlir::LogicalResult(plier::PyCallOp, llvm::StringRef, llvm::ArrayRef<mlir::Value>, mlir::PatternRewriter&)>;

    CallOpLowering(mlir::TypeConverter &typeConverter,
                   mlir::MLIRContext *context,
                   resolver_t resolver);

    mlir::LogicalResult matchAndRewrite(
        plier::PyCallOp op, mlir::PatternRewriter &rewriter) const override;

private:
    resolver_t resolver;
};
