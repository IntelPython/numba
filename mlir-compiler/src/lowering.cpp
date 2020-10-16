#include "lowering.hpp"

#include <algorithm>
#include <array>
#include <vector>
#include <unordered_map>

#include <pybind11/pybind11.h>

#include <mlir/IR/Module.h>
#include <mlir/IR/Builders.h>
#include <mlir/IR/StandardTypes.h>
#include <mlir/Dialect/StandardOps/IR/Ops.h>

#include <mlir/Target/LLVMIR.h>

#include <llvm/Bitcode/BitcodeWriter.h>

#include "plier/dialect.hpp"

#include "compiler.hpp"
#include "pipeline_registry.hpp"
#include "utils.hpp"

#include "pipelines/base_pipeline.hpp"
#include "pipelines/plier_to_std.hpp"
#include "pipelines/plier_to_linalg.hpp"
#include "pipelines/lower_to_llvm.hpp"

namespace py = pybind11;
namespace
{

std::string serialize_mod(const llvm::Module& mod)
{
    std::string ret;
    llvm::raw_string_ostream stream(ret);
//    mod.print(stream, nullptr);
    llvm::WriteBitcodeToFile(mod, stream);
    stream.flush();
    return ret;
}

template<typename T>
std::string to_str(T& obj)
{
    std::string ret;
    llvm::raw_string_ostream stream(ret);
    obj.print(stream);
    stream.flush();
    return ret;
}

std::vector<std::pair<int, py::handle>> get_blocks(const py::object& func)
{
    std::vector<std::pair<int, py::handle>> ret;
    auto blocks = func.attr("blocks").cast<py::dict>();
    ret.reserve(blocks.size());
    for (auto it : blocks)
    {
        ret.push_back({it.first.cast<int>(), it.second});
    }
    return ret;
}

py::list get_body(const py::handle& block)
{
    return block.attr("body").cast<py::list>();
}

struct inst_handles
{
    inst_handles()
    {
        auto mod = py::module::import("numba.core.ir");
        Assign = mod.attr("Assign");
        Del = mod.attr("Del");
        Return = mod.attr("Return");
        Branch = mod.attr("Branch");
        Jump = mod.attr("Jump");

        Arg = mod.attr("Arg");
        Expr = mod.attr("Expr");
        Var = mod.attr("Var");
        Const = mod.attr("Const");
        Global = mod.attr("Global");

        auto ops = py::module::import("operator");

        for (auto elem : llvm::zip(ops_names, ops_handles))
        {
            auto name = std::get<0>(elem).name;
            std::get<1>(elem) = ops.attr(name.data());
        }
    }

    py::handle Assign;
    py::handle Del;
    py::handle Return;
    py::handle Branch;
    py::handle Jump;

    py::handle Arg;
    py::handle Expr;
    py::handle Var;
    py::handle Const;
    py::handle Global;

    struct OpId
    {
        llvm::StringRef op;
        llvm::StringRef name;
    };

    static const constexpr OpId ops_names[] = {
        {"+",  "add"},
        {"-",  "sub"},
        {"*",  "mul"},

        {">",  "gt"},
        {">=", "ge"},
        {"<",  "lt"},
        {"<=", "le"},
        {"!=", "ne"},
        {"==", "eq"},
    };

    std::array<py::handle, llvm::array_lengthof(ops_names)> ops_handles;
};

struct plier_lowerer
{
    plier_lowerer(mlir::MLIRContext& context):
        ctx(context),
        builder(&ctx)
    {
        ctx.loadDialect<mlir::StandardOpsDialect>();
        ctx.loadDialect<plier::PlierDialect>();
    }

    mlir::ModuleOp lower(const py::object& compilation_context, const py::object& func_ir)
    {
        auto mod = mlir::ModuleOp::create(builder.getUnknownLoc());
        typemap = compilation_context["typemap"];
        auto name = compilation_context["fnname"]().cast<std::string>();
        auto typ = get_func_type(compilation_context["fnargs"], compilation_context["restype"]);
        func = mlir::FuncOp::create(builder.getUnknownLoc(), name, typ);
        lower_func_body(func_ir);
        mod.push_back(func);
        return mod;
    }
private:
    mlir::MLIRContext& ctx;
    mlir::OpBuilder builder;
    std::vector<mlir::Block*> blocks;
    std::unordered_map<int, mlir::Block*> blocks_map;
    inst_handles insts;
    mlir::FuncOp func;
    std::unordered_map<std::string, mlir::Value> vars_map;
    struct BlockInfo
    {
        struct PhiDesc
        {
            mlir::Block* dest_block = nullptr;
            std::string var_name;
            unsigned arg_index = 0;
        };
        llvm::SmallVector<PhiDesc, 2> outgoing_phi_nodes;
    };
    py::handle current_instr;
    py::handle typemap;

    std::unordered_map<mlir::Block*, BlockInfo> block_infos;

    plier::PyType get_obj_type(const py::handle& obj) const
    {
        return plier::PyType::get(&ctx, py::str(obj).cast<std::string>());
    }

    plier::PyType get_type(const py::handle& inst) const
    {
        auto type = typemap(inst);
        return get_obj_type(type);
    }

    void lower_func_body(const py::object& func_ir)
    {
        auto ir_blocks = get_blocks(func_ir);
        assert(!ir_blocks.empty());
        blocks.reserve(ir_blocks.size());
        for (std::size_t i = 0; i < ir_blocks.size(); ++i)
        {
            auto block = (0 == i ? func.addEntryBlock() : func.addBlock());
            blocks.push_back(block);
            blocks_map[ir_blocks[i].first] = block;
        }

        for (std::size_t i = 0; i < ir_blocks.size(); ++i)
        {
            lower_block(blocks[i], ir_blocks[i].second);
        }
        fixup_phis();
    }

    void lower_block(mlir::Block* bb, const py::handle& ir_block)
    {
        assert(nullptr != bb);
        builder.setInsertionPointToEnd(bb);
        for (auto it : get_body(ir_block))
        {
            current_instr = it;
            lower_inst(it);
            current_instr = nullptr;
        }
    }

    void lower_inst(const py::handle& inst)
    {
        if (py::isinstance(inst, insts.Assign))
        {
            auto target = inst.attr("target");
            auto val = lower_assign(inst, target);
            storevar(val, target);
        }
        else if (py::isinstance(inst, insts.Del))
        {
            delvar(inst.attr("value"));
        }
        else if (py::isinstance(inst, insts.Return))
        {
            retvar(inst.attr("value"));
        }
        else if (py::isinstance(inst, insts.Branch))
        {
            branch(inst.attr("cond"), inst.attr("truebr"), inst.attr("falsebr"));
        }
        else if (py::isinstance(inst, insts.Jump))
        {
            jump(inst.attr("target"));
        }
        else
        {
            report_error(llvm::Twine("lower_inst not handled: \"") + py::str(inst.get_type()).cast<std::string>() + "\"");
        }
    }

    mlir::Value lower_assign(const py::handle& inst, const py::handle& target)
    {
        auto value = inst.attr("value");
        if (py::isinstance(value, insts.Arg))
        {
            auto index = value.attr("index").cast<std::size_t>();
            return builder.create<plier::ArgOp>(get_current_loc(), index,
                                                target.attr("name").cast<std::string>());
        }
        if(py::isinstance(value, insts.Expr))
        {
            return lower_expr(value);
        }
        if(py::isinstance(value, insts.Var))
        {
            return loadvar(value);
        }
        if (py::isinstance(value, insts.Const))
        {
            auto val = get_const_val(value.attr("value"));
            return builder.create<plier::ConstOp>(get_current_loc(), val);
        }
        if (py::isinstance(value, insts.Global))
        {
            auto name = value.attr("name").cast<std::string>();
            return builder.create<plier::GlobalOp>(get_current_loc(),
                                                   name);
        }

        report_error(llvm::Twine("lower_assign not handled: \"") + py::str(value.get_type()).cast<std::string>() + "\"");
    }

    mlir::Value lower_expr(const py::handle& expr)
    {
        auto op = expr.attr("op").cast<std::string>();
        using func_t = mlir::Value (plier_lowerer::*)(const py::handle&);
        const std::pair<mlir::StringRef, func_t> handlers[] = {
            {"binop", &plier_lowerer::lower_binop},
            {"cast", &plier_lowerer::lower_cast},
            {"call", &plier_lowerer::lower_call},
            {"phi", &plier_lowerer::lower_phi},
            {"build_tuple", &plier_lowerer::lower_build_tuple},
            {"static_getitem", &plier_lowerer::lower_static_getitem},
            {"getiter", &plier_lowerer::lower_simple<plier::GetiterOp>},
            {"iternext", &plier_lowerer::lower_simple_multiresult<plier::IternextOp>},
            {"pair_first", &plier_lowerer::lower_simple<plier::PairfirstOp>},
            {"pair_second", &plier_lowerer::lower_simple<plier::PairsecondOp>},
        };
        for (auto& h : handlers)
        {
            if (h.first == op)
            {
                return (this->*h.second)(expr);
            }
        }
        report_error(llvm::Twine("lower_expr not handled: \"") + op + "\"");
    }

    template <typename T>
    mlir::Value lower_simple(const py::handle& inst)
    {
        auto value = loadvar(inst.attr("value"));
        return builder.create<T>(get_current_loc(), value);
    }

    template <typename T>
    mlir::Value lower_simple_multiresult(const py::handle& inst)
    {
        auto value = loadvar(inst.attr("value"));
        auto res = builder.create<T>(get_current_loc(), value);
        assert(res.getNumResults() == 1);
        return res.getResult(0);
    }

    mlir::Value lower_cast(const py::handle& inst)
    {
        auto value = loadvar(inst.attr("value"));
        auto res_type = get_type(current_instr.attr("target"));
        return builder.create<plier::CastOp>(get_current_loc(), res_type, value);
    }

    mlir::Value lower_static_getitem(const py::handle& inst)
    {
        auto value = loadvar(inst.attr("value"));
        auto index_var = loadvar(inst.attr("index_var"));
        auto index = inst.attr("index").cast<unsigned>();
        return builder.create<plier::StaticGetItemOp>(get_current_loc(),
                                                      value, index_var, index);
    }

    mlir::Value lower_build_tuple(const py::handle& inst)
    {
        auto items = inst.attr("items").cast<py::list>();
        mlir::SmallVector<mlir::Value, 8> args;
        for (auto item : items)
        {
            args.push_back(loadvar(item));
        }
        auto res = builder.create<plier::BuildTupleOp>(get_current_loc(), args);
        assert(res.getNumResults() == 1);
        return res.getResult(0);
    }

    mlir::Value lower_phi(const py::handle& expr)
    {
        auto incoming_vals = expr.attr("incoming_values").cast<py::list>();
        auto incoming_blocks = expr.attr("incoming_blocks").cast<py::list>();
        assert(incoming_vals.size() == incoming_blocks.size());

        auto current_block = builder.getBlock();
        assert(nullptr != current_block);

        auto arg_index = current_block->getNumArguments();
        auto arg = current_block->addArgument(get_type(current_instr.attr("target")));

        auto count = incoming_vals.size();
        for (std::size_t i = 0; i < count; ++i)
        {
            auto var = incoming_vals[i].attr("name").cast<std::string>();
            auto block = blocks_map.find(incoming_blocks[i].cast<int>())->second;
            block_infos[block].outgoing_phi_nodes.push_back({current_block, std::move(var), arg_index});
        }

        return arg;
    }


    mlir::Value lower_call(const py::handle& expr)
    {
        auto func = loadvar(expr.attr("func"));
        auto args = expr.attr("args").cast<py::list>();
        auto kws = expr.attr("kws").cast<py::list>();
        auto vararg = expr.attr("vararg");

        mlir::SmallVector<mlir::Value, 8> args_list;
        mlir::SmallVector<std::pair<std::string, mlir::Value>, 8> kwargs_list;
        for (auto a : args)
        {
            args_list.push_back(loadvar(a));
        }
        for (auto a : kws)
        {
            auto item = a.cast<py::tuple>();
            auto name = item[0];
            auto val_name = item[1];
            kwargs_list.push_back({name.cast<std::string>(), loadvar(val_name)});
        }

        return builder.create<plier::PyCallOp>(get_current_loc(), func,
                                               args_list, kwargs_list);
    }

    mlir::Value lower_binop(const py::handle& expr)
    {
        auto op = expr.attr("fn");
        auto lhs_name = expr.attr("lhs");
        auto rhs_name = expr.attr("rhs");
        auto lhs = loadvar(lhs_name);
        auto rhs = loadvar(rhs_name);
        return resolve_op(lhs, rhs, op);
    }

    mlir::Value resolve_op(mlir::Value lhs, mlir::Value rhs, const py::handle& op)
    {
        for (auto elem : llvm::zip(insts.ops_names, insts.ops_handles))
        {
            if (op.is(std::get<1>(elem)))
            {
                auto op_name = std::get<0>(elem).op;
                return builder.create<plier::BinOp>(get_current_loc(), lhs, rhs, op_name);
            }
        }

        report_error(llvm::Twine("resolve_op not handled: \"") + py::str(op).cast<std::string>() + "\"");
    }

    void storevar(mlir::Value val, const py::handle& inst)
    {
        vars_map[inst.attr("name").cast<std::string>()] = val;
        val.setType(get_type(inst));
    }

    mlir::Value loadvar(const py::handle& inst)
    {
        auto it = vars_map.find(inst.attr("name").cast<std::string>());
        assert(vars_map.end() != it);
        return it->second;
    }

    void delvar(const py::handle& inst)
    {
        auto var = loadvar(inst);
        builder.create<plier::DelOp>(get_current_loc(), var);
    }

    void retvar(const py::handle& inst)
    {
        auto var = loadvar(inst);
        auto func_type = func.getType();
        auto ret_type = func_type.getResult(0);
        auto var_type = var.getType();
        if (ret_type != var_type)
        {
            var = builder.create<plier::CastOp>(get_current_loc(), ret_type, var);
        }
        builder.create<mlir::ReturnOp>(get_current_loc(), var);
    }

    void branch(const py::handle& cond, const py::handle& tr, const py::handle& fl)
    {
        auto c = loadvar(cond);
        auto tr_block = blocks_map.find(tr.cast<int>())->second;
        auto fl_block = blocks_map.find(fl.cast<int>())->second;
        auto cond_val = builder.create<plier::CastOp>(get_current_loc(), mlir::IntegerType::get(1, &ctx), c);
        builder.create<mlir::CondBranchOp>(get_current_loc(), cond_val, tr_block, fl_block);
    }

    void jump(const py::handle& target)
    {
        auto block = blocks_map.find(target.cast<int>())->second;
        builder.create<mlir::BranchOp>(get_current_loc(), mlir::None, block);
    }

    mlir::Attribute get_const_val(const py::handle& val)
    {
        if (py::isinstance<py::int_>(val))
        {
            return builder.getI64IntegerAttr(val.cast<int64_t>());
        }
        report_error(llvm::Twine("get_const_val unhandled type \"") + py::str(val.get_type()).cast<std::string>() + "\"");
    }

    mlir::FunctionType get_func_type(const py::handle& fnargs, const py::handle& restype)
    {
        auto ret = get_obj_type(restype());
        llvm::SmallVector<mlir::Type, 8> args;
        for (auto arg : fnargs())
        {
            args.push_back(get_obj_type(arg));
        }
        return mlir::FunctionType::get(args, {ret}, &ctx);
    }

    mlir::Location get_current_loc()
    {
        return builder.getUnknownLoc(); // TODO
    }

    void fixup_phis()
    {
        auto build_arg_list = [&](mlir::Block* block, auto& outgoing_phi_nodes, auto& list)
        {
            for (auto& o : outgoing_phi_nodes)
            {
                if (o.dest_block == block)
                {
                    auto arg_index = o.arg_index;
                    if (list.size() <= arg_index)
                    {
                        list.resize(arg_index + 1);
                    }
                    auto it = vars_map.find(o.var_name);
                    assert(vars_map.end() != it);
                    auto arg_type = block->getArgument(arg_index).getType();
                    auto val = builder.create<plier::CastOp>(builder.getUnknownLoc(), arg_type, it->second);
                    list[arg_index] = val;
                }
            }
        };
        for (auto& bb : func)
        {
            auto it = block_infos.find(&bb);
            if (block_infos.end() != it)
            {
                auto& info = it->second;
                auto term = bb.getTerminator();
                if (nullptr == term)
                {
                    report_error("broken ir: block without terminator");
                }
                builder.setInsertionPointToEnd(&bb);

                if (auto op = mlir::dyn_cast<mlir::BranchOp>(term))
                {
                    auto dest = op.getDest();
                    mlir::SmallVector<mlir::Value, 8> args;
                    build_arg_list(dest, info.outgoing_phi_nodes, args);
                    op.erase();
                    builder.create<mlir::BranchOp>(builder.getUnknownLoc(), dest, args);
                }
                else if (auto op = mlir::dyn_cast<mlir::CondBranchOp>(term))
                {
                    auto true_dest = op.trueDest();
                    auto false_dest = op.falseDest();
                    auto cond = op.getCondition();
                    mlir::SmallVector<mlir::Value, 8> true_args;
                    mlir::SmallVector<mlir::Value, 8> false_args;
                    build_arg_list(true_dest, info.outgoing_phi_nodes, true_args);
                    build_arg_list(false_dest, info.outgoing_phi_nodes, false_args);
                    op.erase();
                    builder.create<mlir::CondBranchOp>(builder.getUnknownLoc(), cond, true_dest, true_args, false_dest, false_args);
                }
                else
                {
                    report_error(llvm::Twine("Unhandled terminator: ") + term->getName().getStringRef());
                }
            }
        }
    }

};

CompilerContext::Settings get_settings(const py::handle& settings)
{
    CompilerContext::Settings ret;
    ret.verify = settings["verify"].cast<bool>();
    ret.pass_statistics = settings["pass_statistics"].cast<bool>();
    ret.pass_timings = settings["pass_timings"].cast<bool>();
    ret.ir_printing = settings["ir_printing"].cast<bool>();
    return ret;
}

py::bytes gen_ll_module(mlir::ModuleOp mod)
{
    std::string err;
    llvm::raw_string_ostream err_stream(err);
    auto diag_handler = [&](mlir::Diagnostic& diag)
    {
        if (diag.getSeverity() == mlir::DiagnosticSeverity::Error)
        {
            err_stream << diag;
        }
    };
    llvm::LLVMContext ll_ctx;
    std::unique_ptr<llvm::Module> ll_mod;
    scoped_diag_handler(*mod.getContext(), diag_handler, [&]()
    {
        ll_mod = mlir::translateModuleToLLVMIR(mod, ll_ctx);
        if (nullptr == ll_mod)
        {
            err_stream.flush();
            report_error(llvm::Twine("Cannot generate LLVM module\n") + err);
        }
    });
    assert(nullptr != ll_mod);
//    ll_mod->dump();
    return serialize_mod(*ll_mod);
}

void create_pipeline(PipelineRegistry& registry)
{
    register_base_pipeline(registry);
    register_lower_to_llvm_pipeline(registry);
    register_plier_to_std_pipeline(registry);
    register_plier_to_linalg_pipeline(registry);
}
}

py::bytes lower_function(const py::object& compilation_context, const py::object& func_ir)
{
    mlir::registerDialect<mlir::StandardOpsDialect>();
    mlir::registerDialect<plier::PlierDialect>();
    mlir::MLIRContext context;
    auto mod = plier_lowerer(context).lower(compilation_context, func_ir);
    PipelineRegistry registry;
    create_pipeline(registry);
    auto settings = get_settings(compilation_context["compiler_settings"]);
    CompilerContext compiler(context, settings, registry);
    compiler.run(mod);
    return gen_ll_module(mod);
}
