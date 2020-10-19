#include "rewrites/type_conversion.hpp"

#include <mlir/Transforms/DialectConversion.h>

namespace
{
mlir::LogicalResult setBlockSig(
    mlir::Block& block, const mlir::TypeConverter::SignatureConversion& conversion)
{
    if (conversion.getConvertedTypes().size() != block.getNumArguments())
    {
        return mlir::failure();
    }
    for (auto it : llvm::zip(block.getArguments(), conversion.getConvertedTypes()))
    {
        auto arg = std::get<0>(it);
        auto type = std::get<1>(it);
        arg.setType(type);
    }
    return mlir::success();
}

mlir::LogicalResult convertRegionTypes(
    mlir::Region *region, mlir::TypeConverter &converter, bool apply)
{
    if (region->empty())
    {
        return mlir::failure();
    }

    // Convert the arguments of each block within the region.
    auto sig = converter.convertBlockSignature(&region->front());
    assert(static_cast<bool>(sig));
    if (apply)
    {
        auto res = setBlockSig(region->front(), *sig);
        assert(mlir::succeeded(res));
    }
    for (auto &block : llvm::make_early_inc_range(llvm::drop_begin(*region, 1)))
    {
        sig = converter.convertBlockSignature(&block);
        if (!sig)
        {
            return mlir::failure();
        }
        if (apply)
        {
            if (mlir::failed(setBlockSig(block, *sig)))
            {
                return mlir::failure();
            }
        }
    }
    return mlir::success();
}
}

FuncOpSignatureConversion::FuncOpSignatureConversion(mlir::TypeConverter& conv,
    mlir::MLIRContext* ctx)
    : OpRewritePattern(ctx), converter(conv) {}

mlir::LogicalResult FuncOpSignatureConversion::matchAndRewrite(
    mlir::FuncOp funcOp, mlir::PatternRewriter& rewriter) const
{
    auto type = funcOp.getType();

    // Convert the original function types.
    mlir::TypeConverter::SignatureConversion result(type.getNumInputs());
    llvm::SmallVector<mlir::Type, 1> newResults;
    if (mlir::failed(converter.convertSignatureArgs(type.getInputs(), result)) ||
            mlir::failed(converter.convertTypes(type.getResults(), newResults)) ||
            mlir::failed(convertRegionTypes(&funcOp.getBody(), converter, false)))
    {
        return mlir::failure();
    }

    // Update the function signature in-place.
    rewriter.updateRootInPlace(funcOp, [&] {
        funcOp.setType(mlir::FunctionType::get(result.getConvertedTypes(), newResults,
                                               funcOp.getContext()));
        auto res = convertRegionTypes(&funcOp.getBody(), converter, true);
        assert(mlir::succeeded(res));
    });
    return mlir::success();
}

OpTypeConversion::OpTypeConversion(mlir::MLIRContext*, mlir::TypeConverter& conv):
    RewritePattern(0, mlir::Pattern::MatchAnyOpTypeTag()),
    converter(conv) {}

mlir::LogicalResult OpTypeConversion::matchAndRewrite(mlir::Operation* op, mlir::PatternRewriter& rewriter) const
{
    bool changed = false;
    llvm::SmallVector<mlir::Type, 8> new_types;
    for (auto type : op->getResultTypes())
    {
        if (auto new_type = converter.convertType(type))
        {
            changed = changed || (new_type != type);
            new_types.push_back(new_type);
        }
        else
        {
            new_types.push_back(type);
        }
    }

    if (changed)
    {
        rewriter.updateRootInPlace(op, [&]
        {
            for (unsigned i = 0; i < static_cast<unsigned>(new_types.size()); ++i)
            {
                op->getResult(i).setType(new_types[i]);
            }
        });
    }
    return mlir::success(changed);
}
