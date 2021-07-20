#include "taichi/codegen/codegen_llvm.h"

#include "taichi/ir/statements.h"
#include "taichi/struct/struct_llvm.h"
#include "taichi/util/file_sequence_writer.h"

TLANG_NAMESPACE_BEGIN

// TODO: sort function definitions to match declaration order in header

// OffloadedTask

OffloadedTask::OffloadedTask(CodeGenLLVM *codegen) : codegen(codegen) {
  func = nullptr;
}

void OffloadedTask::begin(const std::string &name) {
  this->name = name;
}

void OffloadedTask::end() {
  codegen->offloaded_tasks.push_back(*this);
}

void OffloadedTask::operator()(Context *context) {
  TI_ASSERT(func);
  func(context);
}

void OffloadedTask::compile() {
  TI_ASSERT(!func);
  auto kernel_symbol = codegen->tlctx->lookup_function_pointer(name);
  TI_ASSERT_INFO(kernel_symbol, "Function not found");

  func = (task_fp_type)kernel_symbol;
}

// TODO(k-ye): Hide FunctionCreationGuard inside cpp file
FunctionCreationGuard::FunctionCreationGuard(
    CodeGenLLVM *mb,
    std::vector<llvm::Type *> arguments)
    : mb(mb) {
  // Create the loop body function
  auto body_function_type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(*mb->llvm_context), arguments, false);

  body = llvm::Function::Create(body_function_type,
                                llvm::Function::InternalLinkage,
                                "function_body", mb->module.get());
  old_func = mb->func;
  // emit into loop body function
  mb->func = body;

  allocas = llvm::BasicBlock::Create(*mb->llvm_context, "allocs", body);
  old_entry = mb->entry_block;
  mb->entry_block = allocas;

  entry = llvm::BasicBlock::Create(*mb->llvm_context, "entry", mb->func);

  ip = mb->builder->saveIP();
  mb->builder->SetInsertPoint(entry);

  auto body_bb =
      llvm::BasicBlock::Create(*mb->llvm_context, "function_body", mb->func);
  mb->builder->CreateBr(body_bb);
  mb->builder->SetInsertPoint(body_bb);
}

FunctionCreationGuard::~FunctionCreationGuard() {
  mb->builder->CreateRetVoid();
  mb->func = old_func;
  mb->builder->restoreIP(ip);

  {
    llvm::IRBuilderBase::InsertPointGuard gurad(*mb->builder);
    mb->builder->SetInsertPoint(allocas);
    mb->builder->CreateBr(entry);
    mb->entry_block = old_entry;
  }
}

namespace {

class CodeGenStmtGuard {
 public:
  using Getter = std::function<llvm::BasicBlock *(void)>;
  using Setter = std::function<void(llvm::BasicBlock *)>;

  explicit CodeGenStmtGuard(Getter getter, Setter setter)
      : saved_stmt_(getter()), setter_(std::move(setter)) {
  }

  ~CodeGenStmtGuard() {
    setter_(saved_stmt_);
  }

  CodeGenStmtGuard(CodeGenStmtGuard &&) = default;
  CodeGenStmtGuard &operator=(CodeGenStmtGuard &&) = default;

 private:
  llvm::BasicBlock *saved_stmt_;
  Setter setter_;
};

CodeGenStmtGuard make_loop_reentry_guard(CodeGenLLVM *cg) {
  return CodeGenStmtGuard([cg]() { return cg->current_loop_reentry; },
                          [cg](llvm::BasicBlock *saved_stmt) {
                            cg->current_loop_reentry = saved_stmt;
                          });
}

CodeGenStmtGuard make_while_after_loop_guard(CodeGenLLVM *cg) {
  return CodeGenStmtGuard([cg]() { return cg->current_while_after_loop; },
                          [cg](llvm::BasicBlock *saved_stmt) {
                            cg->current_while_after_loop = saved_stmt;
                          });
}

}  // namespace

// CodeGenLLVM

uint64 CodeGenLLVM::task_counter = 0;

void CodeGenLLVM::visit(Block *stmt_list) {
  for (auto &stmt : stmt_list->statements) {
    stmt->accept(this);
  }
}

void CodeGenLLVM::visit(AllocaStmt *stmt) {
  TI_ASSERT(stmt->width() == 1);
  llvm_val[stmt] =
      create_entry_block_alloca(stmt->ret_type, stmt->ret_type.is_pointer());
  // initialize as zero if element is not a pointer
  if (!stmt->ret_type.is_pointer())
    builder->CreateStore(tlctx->get_constant(stmt->ret_type, 0),
                         llvm_val[stmt]);
}

void CodeGenLLVM::visit(RandStmt *stmt) {
  llvm_val[stmt] = create_call(
      fmt::format("rand_{}", data_type_name(stmt->ret_type)), {get_context()});
}

void CodeGenLLVM::emit_extra_unary(UnaryOpStmt *stmt) {
  auto input = llvm_val[stmt->operand];
  auto input_taichi_type = stmt->operand->ret_type;
  auto op = stmt->op_type;
  auto input_type = input->getType();

#define UNARY_STD(x)                                                    \
  else if (op == UnaryOpType::x) {                                      \
    if (input_taichi_type->is_primitive(PrimitiveTypeID::f32)) {        \
      llvm_val[stmt] =                                                  \
          builder->CreateCall(get_runtime_function(#x "_f32"), input);  \
    } else if (input_taichi_type->is_primitive(PrimitiveTypeID::f64)) { \
      llvm_val[stmt] =                                                  \
          builder->CreateCall(get_runtime_function(#x "_f64"), input);  \
    } else if (input_taichi_type->is_primitive(PrimitiveTypeID::i32)) { \
      llvm_val[stmt] =                                                  \
          builder->CreateCall(get_runtime_function(#x "_i32"), input);  \
    } else {                                                            \
      TI_NOT_IMPLEMENTED                                                \
    }                                                                   \
  }
  if (false) {
  }
  UNARY_STD(abs)
  UNARY_STD(exp)
  UNARY_STD(log)
  UNARY_STD(tan)
  UNARY_STD(tanh)
  UNARY_STD(sgn)
  UNARY_STD(logic_not)
  UNARY_STD(acos)
  UNARY_STD(asin)
  UNARY_STD(cos)
  UNARY_STD(sin)
  else if (op == UnaryOpType::sqrt) {
    llvm_val[stmt] =
        builder->CreateIntrinsic(llvm::Intrinsic::sqrt, {input_type}, {input});
  }
  else {
    TI_P(unary_op_type_name(op));
    TI_NOT_IMPLEMENTED
  }
#undef UNARY_STD
}

std::unique_ptr<RuntimeObject> CodeGenLLVM::emit_struct_meta_object(
    SNode *snode) {
  std::unique_ptr<RuntimeObject> meta;
  if (snode->type == SNodeType::dense) {
    meta = std::make_unique<RuntimeObject>("DenseMeta", this, builder.get());
    emit_struct_meta_base("Dense", meta->ptr, snode);
    meta->call("set_morton_dim", tlctx->get_constant((int)snode->_morton));
  } else if (snode->type == SNodeType::pointer) {
    meta = std::make_unique<RuntimeObject>("PointerMeta", this, builder.get());
    emit_struct_meta_base("Pointer", meta->ptr, snode);
  } else if (snode->type == SNodeType::root) {
    meta = std::make_unique<RuntimeObject>("RootMeta", this, builder.get());
    emit_struct_meta_base("Root", meta->ptr, snode);
  } else if (snode->type == SNodeType::dynamic) {
    meta = std::make_unique<RuntimeObject>("DynamicMeta", this, builder.get());
    emit_struct_meta_base("Dynamic", meta->ptr, snode);
    meta->call("set_chunk_size", tlctx->get_constant(snode->chunk_size));
  } else if (snode->type == SNodeType::bitmasked) {
    meta =
        std::make_unique<RuntimeObject>("BitmaskedMeta", this, builder.get());
    emit_struct_meta_base("Bitmasked", meta->ptr, snode);
  } else {
    TI_P(snode_type_name(snode->type));
    TI_NOT_IMPLEMENTED;
  }
  return meta;
}

void CodeGenLLVM::emit_struct_meta_base(const std::string &name,
                                        llvm::Value *node_meta,
                                        SNode *snode) {
  RuntimeObject common("StructMeta", this, builder.get(), node_meta);
  std::size_t element_size;
  if (snode->type == SNodeType::dense) {
    auto body_type =
        StructCompilerLLVM::get_llvm_body_type(module.get(), snode);
    auto element_ty = body_type->getArrayElementType();
    element_size = tlctx->get_type_size(element_ty);
  } else if (snode->type == SNodeType::pointer) {
    auto element_ty = StructCompilerLLVM::get_llvm_node_type(
        module.get(), snode->ch[0].get());
    element_size = tlctx->get_type_size(element_ty);
  } else {
    auto element_ty =
        StructCompilerLLVM::get_llvm_element_type(module.get(), snode);
    element_size = tlctx->get_type_size(element_ty);
  }
  common.set("snode_id", tlctx->get_constant(snode->id));
  common.set("element_size", tlctx->get_constant((uint64)element_size));
  common.set("max_num_elements",
             tlctx->get_constant(snode->max_num_elements()));
  common.set("context", get_context());

  /*
  uint8 *(*lookup_element)(uint8 *, int i);
  uint8 *(*from_parent_element)(uint8 *);
  bool (*is_active)(uint8 *, int i);
  int (*get_num_elements)(uint8 *);
  void (*refine_coordinates)(PhysicalCoordinates *inp_coord,
                             PhysicalCoordinates *refined_coord,
                             int index);
                             */

  std::vector<std::string> functions = {"lookup_element", "is_active",
                                        "get_num_elements"};

  for (auto const &f : functions)
    common.set(f, get_runtime_function(fmt::format("{}_{}", name, f)));

  // "from_parent_element", "refine_coordinates" are different for different
  // snodes, even if they have the same type.
  if (snode->parent)
    common.set("from_parent_element",
               get_runtime_function(snode->get_ch_from_parent_func_name()));

  if (snode->type != SNodeType::place)
    common.set("refine_coordinates",
               get_runtime_function(snode->refine_coordinates_func_name()));
}

CodeGenLLVM::CodeGenLLVM(Kernel *kernel,
                         IRNode *ir,
                         std::unique_ptr<llvm::Module> &&module)
    // TODO: simplify LLVMModuleBuilder ctor input
    : LLVMModuleBuilder(module == nullptr
                            ? kernel->program->get_llvm_context(kernel->arch)
                                  ->clone_struct_module()
                            : std::move(module),
                        kernel->program->get_llvm_context(kernel->arch)),
      kernel(kernel),
      ir(ir),
      prog(kernel->program) {
  if (ir == nullptr)
    this->ir = kernel->ir.get();
  initialize_context();

  context_ty = get_runtime_type("Context");
  physical_coordinate_ty = get_runtime_type(kLLVMPhysicalCoordinatesName);

  kernel_name = kernel->name + "_kernel";
}

llvm::Value *CodeGenLLVM::cast_int(llvm::Value *input_val,
                                   Type *from,
                                   Type *to) {
  if (from == to)
    return input_val;
  auto from_size = 0;
  if (from->is<CustomIntType>()) {
    from_size = data_type_size(from->cast<CustomIntType>()->get_compute_type());
  } else {
    from_size = data_type_size(from);
  }
  if (from_size < data_type_size(to)) {
    if (is_signed(from)) {
      return builder->CreateSExt(input_val, tlctx->get_data_type(to));
    } else {
      return builder->CreateZExt(input_val, tlctx->get_data_type(to));
    }
  } else {
    return builder->CreateTrunc(input_val, tlctx->get_data_type(to));
  }
}

void CodeGenLLVM::visit(UnaryOpStmt *stmt) {
  auto input = llvm_val[stmt->operand];
  auto input_type = input->getType();
  auto op = stmt->op_type;

#define UNARY_INTRINSIC(x)                                                   \
  else if (op == UnaryOpType::x) {                                           \
    llvm_val[stmt] =                                                         \
        builder->CreateIntrinsic(llvm::Intrinsic::x, {input_type}, {input}); \
  }
  if (stmt->op_type == UnaryOpType::cast_value) {
    llvm::CastInst::CastOps cast_op;
    auto from = stmt->operand->ret_type;
    auto to = stmt->cast_type;
    if (from == to) {
      llvm_val[stmt] = llvm_val[stmt->operand];
    } else if (is_real(from) != is_real(to)) {
      if (is_real(from) && is_integral(to)) {
        cast_op = llvm::Instruction::CastOps::FPToSI;
      } else if (is_integral(from) && is_real(to)) {
        cast_op = llvm::Instruction::CastOps::SIToFP;
      } else {
        TI_P(data_type_name(from));
        TI_P(data_type_name(to));
        TI_NOT_IMPLEMENTED;
      }
      llvm_val[stmt] =
          builder->CreateCast(cast_op, llvm_val[stmt->operand],
                              tlctx->get_data_type(stmt->cast_type));
    } else if (is_real(from) && is_real(to)) {
      if (data_type_size(from) < data_type_size(to)) {
        llvm_val[stmt] = builder->CreateFPExt(
            llvm_val[stmt->operand], tlctx->get_data_type(stmt->cast_type));
      } else {
        llvm_val[stmt] = builder->CreateFPTrunc(
            llvm_val[stmt->operand], tlctx->get_data_type(stmt->cast_type));
      }
    } else if (!is_real(from) && !is_real(to)) {
      // TODO: implement casting into custom integer type
      TI_ASSERT(!to->is<CustomIntType>());
      llvm_val[stmt] = cast_int(llvm_val[stmt->operand], from, to);
    }
  } else if (stmt->op_type == UnaryOpType::cast_bits) {
    TI_ASSERT(data_type_size(stmt->ret_type) ==
              data_type_size(stmt->cast_type));
    if (stmt->operand->ret_type.is_pointer()) {
      TI_ASSERT(is_integral(stmt->cast_type));
      llvm_val[stmt] = builder->CreatePtrToInt(
          llvm_val[stmt->operand], tlctx->get_data_type(stmt->cast_type));
    } else {
      llvm_val[stmt] = builder->CreateBitCast(
          llvm_val[stmt->operand], tlctx->get_data_type(stmt->cast_type));
    }
  } else if (op == UnaryOpType::rsqrt) {
    llvm::Function *sqrt_fn = llvm::Intrinsic::getDeclaration(
        module.get(), llvm::Intrinsic::sqrt, input->getType());
    auto intermediate = builder->CreateCall(sqrt_fn, input, "sqrt");
    llvm_val[stmt] = builder->CreateFDiv(
        tlctx->get_constant(stmt->ret_type, 1.0), intermediate);
  } else if (op == UnaryOpType::bit_not) {
    llvm_val[stmt] = builder->CreateNot(input);
  } else if (op == UnaryOpType::neg) {
    if (is_real(stmt->operand->ret_type)) {
      llvm_val[stmt] = builder->CreateFNeg(input, "neg");
    } else {
      llvm_val[stmt] = builder->CreateNeg(input, "neg");
    }
  }
  UNARY_INTRINSIC(floor)
  UNARY_INTRINSIC(ceil)
  else emit_extra_unary(stmt);
#undef UNARY_INTRINSIC
}

void CodeGenLLVM::visit(BinaryOpStmt *stmt) {
  auto op = stmt->op_type;
  auto ret_type = stmt->ret_type;
  if (op == BinaryOpType::add) {
    if (is_real(stmt->ret_type)) {
      llvm_val[stmt] =
          builder->CreateFAdd(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    } else {
      llvm_val[stmt] =
          builder->CreateAdd(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    }
  } else if (op == BinaryOpType::sub) {
    if (is_real(stmt->ret_type)) {
      llvm_val[stmt] =
          builder->CreateFSub(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    } else {
      llvm_val[stmt] =
          builder->CreateSub(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    }
  } else if (op == BinaryOpType::mul) {
    if (is_real(stmt->ret_type)) {
      llvm_val[stmt] =
          builder->CreateFMul(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    } else {
      llvm_val[stmt] =
          builder->CreateMul(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    }
  } else if (op == BinaryOpType::floordiv) {
    if (is_integral(ret_type))
      llvm_val[stmt] =
          create_call(fmt::format("floordiv_{}", data_type_name(ret_type)),
                      {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
    else {
      auto div = builder->CreateFDiv(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
      llvm_val[stmt] = builder->CreateIntrinsic(
          llvm::Intrinsic::floor, {tlctx->get_data_type(ret_type)}, {div});
    }
  } else if (op == BinaryOpType::div) {
    if (is_real(stmt->ret_type)) {
      llvm_val[stmt] =
          builder->CreateFDiv(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    } else {
      llvm_val[stmt] =
          builder->CreateSDiv(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    }
  } else if (op == BinaryOpType::mod) {
    llvm_val[stmt] =
        builder->CreateSRem(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
  } else if (op == BinaryOpType::bit_and) {
    llvm_val[stmt] =
        builder->CreateAnd(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
  } else if (op == BinaryOpType::bit_or) {
    llvm_val[stmt] =
        builder->CreateOr(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
  } else if (op == BinaryOpType::bit_xor) {
    llvm_val[stmt] =
        builder->CreateXor(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
  } else if (op == BinaryOpType::bit_shl) {
    llvm_val[stmt] =
        builder->CreateShl(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
  } else if (op == BinaryOpType::bit_sar) {
    if (is_signed(stmt->lhs->element_type())) {
      llvm_val[stmt] =
          builder->CreateAShr(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    } else {
      llvm_val[stmt] =
          builder->CreateLShr(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    }
  } else if (op == BinaryOpType::max) {
    if (is_real(ret_type)) {
      llvm_val[stmt] =
          builder->CreateMaxNum(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    } else if (ret_type->is_primitive(PrimitiveTypeID::i32)) {
      llvm_val[stmt] =
          create_call("max_i32", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
    } else {
      TI_P(data_type_name(ret_type));
      TI_NOT_IMPLEMENTED
    }
  } else if (op == BinaryOpType::atan2) {
    if (arch_is_cpu(current_arch())) {
      if (ret_type->is_primitive(PrimitiveTypeID::f32)) {
        llvm_val[stmt] = create_call(
            "atan2_f32", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else if (ret_type->is_primitive(PrimitiveTypeID::f64)) {
        llvm_val[stmt] = create_call(
            "atan2_f64", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else {
        TI_P(data_type_name(ret_type));
        TI_NOT_IMPLEMENTED
      }
    } else if (current_arch() == Arch::cuda) {
      if (ret_type->is_primitive(PrimitiveTypeID::f32)) {
        llvm_val[stmt] = create_call(
            "__nv_atan2f", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else if (ret_type->is_primitive(PrimitiveTypeID::f64)) {
        llvm_val[stmt] = create_call(
            "__nv_atan2", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else {
        TI_P(data_type_name(ret_type));
        TI_NOT_IMPLEMENTED
      }
    } else {
      TI_NOT_IMPLEMENTED
    }
  } else if (op == BinaryOpType::pow) {
    if (arch_is_cpu(current_arch())) {
      if (ret_type->is_primitive(PrimitiveTypeID::f32)) {
        llvm_val[stmt] =
            create_call("pow_f32", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else if (ret_type->is_primitive(PrimitiveTypeID::f64)) {
        llvm_val[stmt] =
            create_call("pow_f64", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else if (ret_type->is_primitive(PrimitiveTypeID::i32)) {
        llvm_val[stmt] =
            create_call("pow_i32", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else if (ret_type->is_primitive(PrimitiveTypeID::i64)) {
        llvm_val[stmt] =
            create_call("pow_i64", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else {
        TI_P(data_type_name(ret_type));
        TI_NOT_IMPLEMENTED
      }
    } else if (current_arch() == Arch::cuda) {
      if (ret_type->is_primitive(PrimitiveTypeID::f32)) {
        llvm_val[stmt] = create_call(
            "__nv_powf", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else if (ret_type->is_primitive(PrimitiveTypeID::f64)) {
        llvm_val[stmt] =
            create_call("__nv_pow", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else if (ret_type->is_primitive(PrimitiveTypeID::i32)) {
        llvm_val[stmt] =
            create_call("pow_i32", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else if (ret_type->is_primitive(PrimitiveTypeID::i64)) {
        llvm_val[stmt] =
            create_call("pow_i64", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
      } else {
        TI_P(data_type_name(ret_type));
        TI_NOT_IMPLEMENTED
      }
    } else {
      TI_NOT_IMPLEMENTED
    }
  } else if (op == BinaryOpType::min) {
    if (is_real(ret_type)) {
      llvm_val[stmt] =
          builder->CreateMinNum(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
    } else if (ret_type->is_primitive(PrimitiveTypeID::i32)) {
      llvm_val[stmt] =
          create_call("min_i32", {llvm_val[stmt->lhs], llvm_val[stmt->rhs]});
    } else {
      TI_P(data_type_name(ret_type));
      TI_NOT_IMPLEMENTED
    }
  } else if (is_comparison(op)) {
    llvm::Value *cmp = nullptr;
    auto input_type = stmt->lhs->ret_type;
    if (op == BinaryOpType::cmp_eq) {
      if (is_real(input_type)) {
        cmp = builder->CreateFCmpOEQ(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
      } else {
        cmp = builder->CreateICmpEQ(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
      }
    } else if (op == BinaryOpType::cmp_le) {
      if (is_real(input_type)) {
        cmp = builder->CreateFCmpOLE(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
      } else {
        if (is_signed(input_type)) {
          cmp =
              builder->CreateICmpSLE(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
        } else {
          cmp =
              builder->CreateICmpULE(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
        }
      }
    } else if (op == BinaryOpType::cmp_ge) {
      if (is_real(input_type)) {
        cmp = builder->CreateFCmpOGE(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
      } else {
        if (is_signed(input_type)) {
          cmp =
              builder->CreateICmpSGE(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
        } else {
          cmp =
              builder->CreateICmpUGE(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
        }
      }
    } else if (op == BinaryOpType::cmp_lt) {
      if (is_real(input_type)) {
        cmp = builder->CreateFCmpOLT(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
      } else {
        if (is_signed(input_type)) {
          cmp =
              builder->CreateICmpSLT(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
        } else {
          cmp =
              builder->CreateICmpULT(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
        }
      }
    } else if (op == BinaryOpType::cmp_gt) {
      if (is_real(input_type)) {
        cmp = builder->CreateFCmpOGT(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
      } else {
        if (is_signed(input_type)) {
          cmp =
              builder->CreateICmpSGT(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
        } else {
          cmp =
              builder->CreateICmpUGT(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
        }
      }
    } else if (op == BinaryOpType::cmp_ne) {
      if (is_real(input_type)) {
        cmp = builder->CreateFCmpONE(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
      } else {
        cmp = builder->CreateICmpNE(llvm_val[stmt->lhs], llvm_val[stmt->rhs]);
      }
    } else {
      TI_NOT_IMPLEMENTED
    }
    llvm_val[stmt] = builder->CreateSExt(cmp, llvm_type(PrimitiveType::i32));
  } else {
    TI_P(binary_op_type_name(op));
    TI_NOT_IMPLEMENTED
  }
}

llvm::Type *CodeGenLLVM::llvm_type(DataType dt) {
  if (dt->is_primitive(PrimitiveTypeID::i8) ||
      dt->is_primitive(PrimitiveTypeID::u8)) {
    return llvm::Type::getInt8Ty(*llvm_context);
  } else if (dt->is_primitive(PrimitiveTypeID::i16) ||
             dt->is_primitive(PrimitiveTypeID::u16)) {
    return llvm::Type::getInt16Ty(*llvm_context);
  } else if (dt->is_primitive(PrimitiveTypeID::i32) ||
             dt->is_primitive(PrimitiveTypeID::u32)) {
    return llvm::Type::getInt32Ty(*llvm_context);
  } else if (dt->is_primitive(PrimitiveTypeID::i64) ||
             dt->is_primitive(PrimitiveTypeID::u64)) {
    return llvm::Type::getInt64Ty(*llvm_context);
  } else if (dt->is_primitive(PrimitiveTypeID::u1)) {
    return llvm::Type::getInt1Ty(*llvm_context);
  } else if (dt->is_primitive(PrimitiveTypeID::f32)) {
    return llvm::Type::getFloatTy(*llvm_context);
  } else if (dt->is_primitive(PrimitiveTypeID::f64)) {
    return llvm::Type::getDoubleTy(*llvm_context);
  } else {
    TI_NOT_IMPLEMENTED;
  }
  return nullptr;
}

llvm::Type *CodeGenLLVM::llvm_ptr_type(DataType dt) {
  return llvm::PointerType::get(llvm_type(dt), 0);
}

void CodeGenLLVM::visit(TernaryOpStmt *stmt) {
  TI_ASSERT(stmt->op_type == TernaryOpType::select);
  llvm_val[stmt] = builder->CreateSelect(
      builder->CreateTrunc(llvm_val[stmt->op1], llvm_type(PrimitiveType::u1)),
      llvm_val[stmt->op2], llvm_val[stmt->op3]);
}

void CodeGenLLVM::visit(IfStmt *if_stmt) {
  // TODO: take care of vectorized cases
  llvm::BasicBlock *true_block =
      llvm::BasicBlock::Create(*llvm_context, "true_block", func);
  llvm::BasicBlock *false_block =
      llvm::BasicBlock::Create(*llvm_context, "false_block", func);
  llvm::BasicBlock *after_if =
      llvm::BasicBlock::Create(*llvm_context, "after_if", func);
  builder->CreateCondBr(
      builder->CreateICmpNE(llvm_val[if_stmt->cond], tlctx->get_constant(0)),
      true_block, false_block);
  builder->SetInsertPoint(true_block);
  if (if_stmt->true_statements) {
    if_stmt->true_statements->accept(this);
  }
  builder->CreateBr(after_if);
  builder->SetInsertPoint(false_block);
  if (if_stmt->false_statements) {
    if_stmt->false_statements->accept(this);
  }
  builder->CreateBr(after_if);
  builder->SetInsertPoint(after_if);
}

llvm::Value *CodeGenLLVM::create_print(std::string tag,
                                       DataType dt,
                                       llvm::Value *value) {
  if (!arch_is_cpu(kernel->arch)) {
    TI_WARN("print not supported on arch {}", arch_name(kernel->arch));
    return nullptr;
  }
  std::vector<llvm::Value *> args;
  std::string format = data_type_format(dt);
  auto runtime_printf = call("LLVMRuntime_get_host_printf", get_runtime());
  args.push_back(builder->CreateGlobalStringPtr(
      ("[llvm codegen debug] " + tag + " = " + format + "\n").c_str(),
      "format_string"));
  if (dt->is_primitive(PrimitiveTypeID::f32))
    value =
        builder->CreateFPExt(value, tlctx->get_data_type(PrimitiveType::f64));
  args.push_back(value);
  return builder->CreateCall(runtime_printf, args);
}

llvm::Value *CodeGenLLVM::create_print(std::string tag, llvm::Value *value) {
  if (value->getType() == llvm::Type::getFloatTy(*llvm_context))
    return create_print(
        tag,
        TypeFactory::get_instance().get_primitive_type(PrimitiveTypeID::f32),
        value);
  else if (value->getType() == llvm::Type::getInt32Ty(*llvm_context))
    return create_print(
        tag,
        TypeFactory::get_instance().get_primitive_type(PrimitiveTypeID::i32),
        value);
  else
    TI_NOT_IMPLEMENTED
}

void CodeGenLLVM::visit(PrintStmt *stmt) {
  TI_ASSERT(stmt->width() == 1);
  std::vector<llvm::Value *> args;
  std::string formats;
  for (auto const &content : stmt->contents) {
    if (std::holds_alternative<Stmt *>(content)) {
      auto arg_stmt = std::get<Stmt *>(content);
      auto value = llvm_val[arg_stmt];
      if (arg_stmt->ret_type->is_primitive(PrimitiveTypeID::f32))
        value = builder->CreateFPExt(value,
                                     tlctx->get_data_type(PrimitiveType::f64));
      args.push_back(value);
      formats += data_type_format(arg_stmt->ret_type);
    } else {
      auto arg_str = std::get<std::string>(content);
      auto value = builder->CreateGlobalStringPtr(arg_str, "content_string");
      args.push_back(value);
      formats += "%s";
    }
  }
  auto runtime_printf = call("LLVMRuntime_get_host_printf", get_runtime());
  args.insert(args.begin(),
              builder->CreateGlobalStringPtr(formats.c_str(), "format_string"));

  llvm_val[stmt] = builder->CreateCall(runtime_printf, args);
}

void CodeGenLLVM::visit(ConstStmt *stmt) {
  TI_ASSERT(stmt->width() == 1);
  auto val = stmt->val[0];
  if (val.dt->is_primitive(PrimitiveTypeID::f32)) {
    llvm_val[stmt] =
        llvm::ConstantFP::get(*llvm_context, llvm::APFloat(val.val_float32()));
  } else if (val.dt->is_primitive(PrimitiveTypeID::f64)) {
    llvm_val[stmt] =
        llvm::ConstantFP::get(*llvm_context, llvm::APFloat(val.val_float64()));
  } else if (val.dt->is_primitive(PrimitiveTypeID::i32)) {
    llvm_val[stmt] = llvm::ConstantInt::get(
        *llvm_context, llvm::APInt(32, (uint64)val.val_int32(), true));
  } else if (val.dt->is_primitive(PrimitiveTypeID::u32)) {
    llvm_val[stmt] = llvm::ConstantInt::get(
        *llvm_context, llvm::APInt(32, (uint64)val.val_uint32(), false));
  } else if (val.dt->is_primitive(PrimitiveTypeID::i64)) {
    llvm_val[stmt] = llvm::ConstantInt::get(
        *llvm_context, llvm::APInt(64, (uint64)val.val_int64(), true));
  } else if (val.dt->is_primitive(PrimitiveTypeID::u64)) {
    llvm_val[stmt] = llvm::ConstantInt::get(
        *llvm_context, llvm::APInt(64, val.val_uint64(), false));
  } else {
    TI_P(data_type_name(val.dt));
    TI_NOT_IMPLEMENTED;
  }
}

void CodeGenLLVM::visit(WhileControlStmt *stmt) {
  using namespace llvm;

  BasicBlock *after_break =
      BasicBlock::Create(*llvm_context, "after_break", func);
  TI_ASSERT(current_while_after_loop);
  auto cond =
      builder->CreateICmpEQ(llvm_val[stmt->cond], tlctx->get_constant(0));
  builder->CreateCondBr(cond, current_while_after_loop, after_break);
  builder->SetInsertPoint(after_break);
}

void CodeGenLLVM::visit(ContinueStmt *stmt) {
  using namespace llvm;
  if (stmt->as_return()) {
    builder->CreateRetVoid();
  } else {
    TI_ASSERT(current_loop_reentry != nullptr);
    builder->CreateBr(current_loop_reentry);
  }
  // Stmts after continue are useless, so we switch the insertion point to
  // /dev/null. In LLVM IR, the "after_continue" label shows "No predecessors!".
  BasicBlock *after_continue =
      BasicBlock::Create(*llvm_context, "after_continue", func);
  builder->SetInsertPoint(after_continue);
}

void CodeGenLLVM::visit(WhileStmt *stmt) {
  using namespace llvm;
  BasicBlock *body = BasicBlock::Create(*llvm_context, "while_loop_body", func);
  builder->CreateBr(body);
  builder->SetInsertPoint(body);
  auto lrg = make_loop_reentry_guard(this);
  current_loop_reentry = body;

  BasicBlock *after_loop =
      BasicBlock::Create(*llvm_context, "after_while", func);
  auto walg = make_while_after_loop_guard(this);
  current_while_after_loop = after_loop;

  stmt->body->accept(this);

  builder->CreateBr(body);  // jump to head

  builder->SetInsertPoint(after_loop);
}

llvm::Value *CodeGenLLVM::cast_pointer(llvm::Value *val,
                                       std::string dest_ty_name,
                                       int addr_space) {
  return builder->CreateBitCast(
      val, llvm::PointerType::get(get_runtime_type(dest_ty_name), addr_space));
}

void CodeGenLLVM::emit_list_gen(OffloadedStmt *listgen) {
  auto snode_child = listgen->snode;
  auto snode_parent = listgen->snode->parent;
  auto meta_child = cast_pointer(emit_struct_meta(snode_child), "StructMeta");
  auto meta_parent = cast_pointer(emit_struct_meta(snode_parent), "StructMeta");
  if (snode_parent->type == SNodeType::root) {
    // Since there's only one container to expand, we need a special kernel for
    // more parallelism.
    call("element_listgen_root", get_runtime(), meta_parent, meta_child);
  } else {
    call("element_listgen_nonroot", get_runtime(), meta_parent, meta_child);
  }
}

void CodeGenLLVM::emit_gc(OffloadedStmt *stmt) {
  auto snode = stmt->snode->id;
  call("node_gc", get_runtime(), tlctx->get_constant(snode));
}

llvm::Value *CodeGenLLVM::create_call(llvm::Value *func,
                                      std::vector<llvm::Value *> args) {
  check_func_call_signature(func, args);
  return builder->CreateCall(func, args);
}
llvm::Value *CodeGenLLVM::create_call(std::string func_name,
                                      std::vector<llvm::Value *> args) {
  auto func = get_runtime_function(func_name);
  return create_call(func, args);
}

void CodeGenLLVM::create_increment(llvm::Value *ptr, llvm::Value *value) {
  builder->CreateStore(builder->CreateAdd(builder->CreateLoad(ptr), value),
                       ptr);
}

void CodeGenLLVM::create_naive_range_for(RangeForStmt *for_stmt) {
  using namespace llvm;
  BasicBlock *body = BasicBlock::Create(*llvm_context, "for_loop_body", func);
  BasicBlock *loop_inc =
      BasicBlock::Create(*llvm_context, "for_loop_inc", func);
  BasicBlock *after_loop = BasicBlock::Create(*llvm_context, "after_for", func);
  BasicBlock *loop_test =
      BasicBlock::Create(*llvm_context, "for_loop_test", func);

  auto loop_var = create_entry_block_alloca(PrimitiveType::i32);
  loop_vars_llvm[for_stmt].push_back(loop_var);

  if (!for_stmt->reversed) {
    builder->CreateStore(llvm_val[for_stmt->begin], loop_var);
  } else {
    builder->CreateStore(
        builder->CreateSub(llvm_val[for_stmt->end], tlctx->get_constant(1)),
        loop_var);
  }
  builder->CreateBr(loop_test);

  {
    // test block
    builder->SetInsertPoint(loop_test);
    llvm::Value *cond;
    if (!for_stmt->reversed) {
      cond = builder->CreateICmp(llvm::CmpInst::Predicate::ICMP_SLT,
                                 builder->CreateLoad(loop_var),
                                 llvm_val[for_stmt->end]);
    } else {
      cond = builder->CreateICmp(llvm::CmpInst::Predicate::ICMP_SGE,
                                 builder->CreateLoad(loop_var),
                                 llvm_val[for_stmt->begin]);
    }
    builder->CreateCondBr(cond, body, after_loop);
  }

  {
    {
      auto lrg = make_loop_reentry_guard(this);
      // The continue stmt should jump to the loop-increment block!
      current_loop_reentry = loop_inc;
      // body cfg
      builder->SetInsertPoint(body);

      for_stmt->body->accept(this);
    }

    builder->CreateBr(loop_inc);
    builder->SetInsertPoint(loop_inc);

    if (!for_stmt->reversed) {
      create_increment(loop_var, tlctx->get_constant(1));
    } else {
      create_increment(loop_var, tlctx->get_constant(-1));
    }
    builder->CreateBr(loop_test);
  }

  // next cfg
  builder->SetInsertPoint(after_loop);
}

void CodeGenLLVM::visit(RangeForStmt *for_stmt) {
  create_naive_range_for(for_stmt);
}

void CodeGenLLVM::visit(ArgLoadStmt *stmt) {
  auto raw_arg = call(builder.get(), "Context_get_args", get_context(),
                      tlctx->get_constant(stmt->arg_id));

  llvm::Type *dest_ty = nullptr;
  if (stmt->is_ptr) {
    dest_ty =
        llvm::PointerType::get(tlctx->get_data_type(PrimitiveType::i32), 0);
    llvm_val[stmt] = builder->CreateIntToPtr(raw_arg, dest_ty);
  } else {
    TI_ASSERT(!stmt->ret_type->is<PointerType>());
    if (auto cit = stmt->ret_type->cast<CustomIntType>()) {
      if (cit->get_is_signed())
        dest_ty = tlctx->get_data_type(PrimitiveType::i32);
      else
        dest_ty = tlctx->get_data_type(PrimitiveType::u32);
    } else {
      dest_ty = tlctx->get_data_type(stmt->ret_type);
    }
    auto dest_bits = dest_ty->getPrimitiveSizeInBits();
    auto truncated = builder->CreateTrunc(
        raw_arg, llvm::Type::getIntNTy(*llvm_context, dest_bits));
    llvm_val[stmt] = builder->CreateBitCast(truncated, dest_ty);
  }
}

void CodeGenLLVM::visit(ReturnStmt *stmt) {
  if (stmt->ret_type.is_pointer()) {
    TI_NOT_IMPLEMENTED
  } else {
    auto intermediate_bits = 0;
    if (auto cit = stmt->value->ret_type->cast<CustomIntType>()) {
      intermediate_bits = data_type_bits(cit->get_compute_type());
    } else {
      intermediate_bits =
          tlctx->get_data_type(stmt->value->ret_type)->getPrimitiveSizeInBits();
    }
    llvm::Type *intermediate_type =
        llvm::Type::getIntNTy(*llvm_context, intermediate_bits);
    llvm::Type *dest_ty = tlctx->get_data_type<int64>();
    auto extended = builder->CreateZExt(
        builder->CreateBitCast(llvm_val[stmt->value], intermediate_type),
        dest_ty);
    builder->CreateCall(get_runtime_function("LLVMRuntime_store_result"),
                        {get_runtime(), extended});
  }
}

void CodeGenLLVM::visit(LocalLoadStmt *stmt) {
  TI_ASSERT(stmt->width() == 1);
  llvm_val[stmt] = builder->CreateLoad(llvm_val[stmt->src[0].var]);
}

void CodeGenLLVM::visit(LocalStoreStmt *stmt) {
  auto mask = stmt->parent->mask();
  if (mask && stmt->width() != 1) {
    TI_NOT_IMPLEMENTED
  } else {
    builder->CreateStore(llvm_val[stmt->val], llvm_val[stmt->dest]);
  }
}

void CodeGenLLVM::visit(AssertStmt *stmt) {
  TI_ASSERT((int)stmt->args.size() <= taichi_error_message_max_num_arguments);
  auto argument_buffer_size = llvm::ArrayType::get(
      llvm::Type::getInt64Ty(*llvm_context), stmt->args.size());

  // TODO: maybe let all asserts in a single offload share a single buffer?
  auto arguments = create_entry_block_alloca(argument_buffer_size);

  std::vector<llvm::Value *> args;
  args.emplace_back(get_runtime());
  args.emplace_back(llvm_val[stmt->cond]);
  args.emplace_back(builder->CreateGlobalStringPtr(stmt->text));

  for (int i = 0; i < stmt->args.size(); i++) {
    auto arg = stmt->args[i];
    TI_ASSERT(llvm_val[arg]);

    // First convert the argument to an integral type with the same number of
    // bits:
    auto cast_type = llvm::Type::getIntNTy(
        *llvm_context, 8 * (std::size_t)data_type_size(arg->ret_type));
    auto cast_int = builder->CreateBitCast(llvm_val[arg], cast_type);

    // Then zero-extend the conversion result into int64:
    auto cast_int64 =
        builder->CreateZExt(cast_int, llvm::Type::getInt64Ty(*llvm_context));

    // Finally store the int64 value to the argument buffer:
    builder->CreateStore(
        cast_int64, builder->CreateGEP(arguments, {tlctx->get_constant(0),
                                                   tlctx->get_constant(i)}));
  }

  args.emplace_back(tlctx->get_constant((int)stmt->args.size()));
  args.emplace_back(builder->CreateGEP(
      arguments, {tlctx->get_constant(0), tlctx->get_constant(0)}));

  llvm_val[stmt] = create_call("taichi_assert_format", args);
}

void CodeGenLLVM::visit(SNodeOpStmt *stmt) {
  auto snode = stmt->snode;
  if (stmt->op_type == SNodeOpType::append) {
    TI_ASSERT(snode->type == SNodeType::dynamic);
    TI_ASSERT(stmt->ret_type->is_primitive(PrimitiveTypeID::i32));
    llvm_val[stmt] =
        call(snode, llvm_val[stmt->ptr], "append", {llvm_val[stmt->val]});
  } else if (stmt->op_type == SNodeOpType::length) {
    TI_ASSERT(snode->type == SNodeType::dynamic);
    llvm_val[stmt] = call(snode, llvm_val[stmt->ptr], "get_num_elements", {});
  } else if (stmt->op_type == SNodeOpType::is_active) {
    llvm_val[stmt] =
        call(snode, llvm_val[stmt->ptr], "is_active", {llvm_val[stmt->val]});
  } else if (stmt->op_type == SNodeOpType::activate) {
    llvm_val[stmt] =
        call(snode, llvm_val[stmt->ptr], "activate", {llvm_val[stmt->val]});
  } else if (stmt->op_type == SNodeOpType::deactivate) {
    if (snode->type == SNodeType::pointer || snode->type == SNodeType::hash ||
        snode->type == SNodeType::bitmasked) {
      llvm_val[stmt] =
          call(snode, llvm_val[stmt->ptr], "deactivate", {llvm_val[stmt->val]});
    } else if (snode->type == SNodeType::dynamic) {
      llvm_val[stmt] = call(snode, llvm_val[stmt->ptr], "deactivate", {});
    }
  } else {
    TI_NOT_IMPLEMENTED
  }
}

void CodeGenLLVM::visit(AtomicOpStmt *stmt) {
  // auto mask = stmt->parent->mask();
  // TODO: deal with mask when vectorized
  // TODO(type): support all AtomicOpTypes on custom types
  TI_ASSERT(stmt->width() == 1);
  for (int l = 0; l < stmt->width(); l++) {
    llvm::Value *old_value;
    if (stmt->op_type == AtomicOpType::add) {
      auto dst_type =
          stmt->dest->ret_type->as<PointerType>()->get_pointee_type();
      if (dst_type->is<PrimitiveType>() && is_integral(stmt->val->ret_type)) {
        old_value = builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::BinOp::Add, llvm_val[stmt->dest],
            llvm_val[stmt->val], llvm::AtomicOrdering::SequentiallyConsistent);
      } else if (!dst_type->is<CustomFloatType>() &&
                 is_real(stmt->val->ret_type)) {
        old_value = builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::BinOp::FAdd, llvm_val[stmt->dest],
            llvm_val[stmt->val], llvm::AtomicOrdering::SequentiallyConsistent);
      } else if (auto cit = dst_type->cast<CustomIntType>()) {
        old_value = atomic_add_custom_int(stmt, cit);
      } else if (auto cft = dst_type->cast<CustomFloatType>()) {
        old_value = atomic_add_custom_float(stmt, cft);
      } else {
        TI_NOT_IMPLEMENTED
      }
    } else if (stmt->op_type == AtomicOpType::min) {
      if (is_integral(stmt->val->ret_type)) {
        old_value = builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::BinOp::Min, llvm_val[stmt->dest],
            llvm_val[stmt->val], llvm::AtomicOrdering::SequentiallyConsistent);
      } else if (stmt->val->ret_type->is_primitive(PrimitiveTypeID::f32)) {
        old_value =
            builder->CreateCall(get_runtime_function("atomic_min_f32"),
                                {llvm_val[stmt->dest], llvm_val[stmt->val]});
      } else if (stmt->val->ret_type->is_primitive(PrimitiveTypeID::f64)) {
        old_value =
            builder->CreateCall(get_runtime_function("atomic_min_f64"),
                                {llvm_val[stmt->dest], llvm_val[stmt->val]});
      } else {
        TI_NOT_IMPLEMENTED
      }
    } else if (stmt->op_type == AtomicOpType::max) {
      if (is_integral(stmt->val->ret_type)) {
        old_value = builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::BinOp::Max, llvm_val[stmt->dest],
            llvm_val[stmt->val], llvm::AtomicOrdering::SequentiallyConsistent);
      } else if (stmt->val->ret_type->is_primitive(PrimitiveTypeID::f32)) {
        old_value =
            builder->CreateCall(get_runtime_function("atomic_max_f32"),
                                {llvm_val[stmt->dest], llvm_val[stmt->val]});
      } else if (stmt->val->ret_type->is_primitive(PrimitiveTypeID::f64)) {
        old_value =
            builder->CreateCall(get_runtime_function("atomic_max_f64"),
                                {llvm_val[stmt->dest], llvm_val[stmt->val]});
      } else {
        TI_NOT_IMPLEMENTED
      }
    } else if (stmt->op_type == AtomicOpType::bit_and) {
      if (is_integral(stmt->val->ret_type)) {
        old_value = builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::BinOp::And, llvm_val[stmt->dest],
            llvm_val[stmt->val], llvm::AtomicOrdering::SequentiallyConsistent);
      } else {
        TI_NOT_IMPLEMENTED
      }
    } else if (stmt->op_type == AtomicOpType::bit_or) {
      if (is_integral(stmt->val->ret_type)) {
        old_value = builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::BinOp::Or, llvm_val[stmt->dest],
            llvm_val[stmt->val], llvm::AtomicOrdering::SequentiallyConsistent);
      } else {
        TI_NOT_IMPLEMENTED
      }
    } else if (stmt->op_type == AtomicOpType::bit_xor) {
      if (is_integral(stmt->val->ret_type)) {
        old_value = builder->CreateAtomicRMW(
            llvm::AtomicRMWInst::BinOp::Xor, llvm_val[stmt->dest],
            llvm_val[stmt->val], llvm::AtomicOrdering::SequentiallyConsistent);
      } else {
        TI_NOT_IMPLEMENTED
      }
    } else {
      TI_NOT_IMPLEMENTED
    }
    llvm_val[stmt] = old_value;
  }
}

void CodeGenLLVM::visit(GlobalPtrStmt *stmt) {
  TI_ERROR("Global Ptrs should have been lowered.");
}

void CodeGenLLVM::visit(GlobalStoreStmt *stmt) {
  TI_ASSERT(!stmt->parent->mask() || stmt->width() == 1);
  TI_ASSERT(llvm_val[stmt->val]);
  TI_ASSERT(llvm_val[stmt->dest]);
  auto ptr_type = stmt->dest->ret_type->as<PointerType>();
  if (ptr_type->is_bit_pointer()) {
    auto pointee_type = ptr_type->get_pointee_type();
    if (!pointee_type->is<CustomIntType>()) {
      if (stmt->dest->as<GetChStmt>()->input_snode->type ==
          SNodeType::bit_struct) {
        TI_ERROR(
            "Bit struct stores with type {} should have been "
            "handled by BitStructStoreStmt.",
            pointee_type->to_string());
      } else {
        TI_ERROR("Bit array only supports custom int type.");
      }
    }
    llvm::Value *store_value = nullptr;
    auto *cit = pointee_type->as<CustomIntType>();
    store_value = llvm_val[stmt->val];
    store_custom_int(llvm_val[stmt->dest], cit, store_value, /*atomic=*/true);
  } else {
    builder->CreateStore(llvm_val[stmt->val], llvm_val[stmt->dest]);
  }
}

void CodeGenLLVM::visit(GlobalLoadStmt *stmt) {
  int width = stmt->width();
  TI_ASSERT(width == 1);
  auto ptr_type = stmt->src->ret_type->as<PointerType>();
  if (ptr_type->is_bit_pointer()) {
    auto val_type = ptr_type->get_pointee_type();
    if (val_type->is<CustomIntType>()) {
      llvm_val[stmt] = load_as_custom_int(llvm_val[stmt->src], val_type);
    } else if (auto cft = val_type->cast<CustomFloatType>()) {
      TI_ASSERT(stmt->src->is<GetChStmt>());
      llvm_val[stmt] = load_custom_float(stmt->src);
    } else {
      TI_NOT_IMPLEMENTED
    }
  } else {
    llvm_val[stmt] = builder->CreateLoad(tlctx->get_data_type(stmt->ret_type),
                                         llvm_val[stmt->src]);
  }
}

void CodeGenLLVM::visit(ElementShuffleStmt *stmt){
    TI_NOT_IMPLEMENTED
    /*
    auto init = stmt->elements.serialize(
        [](const VectorElement &elem) {
          return fmt::format("{}[{}]", elem.stmt->raw_name(), elem.index);
        },
        "{");
    if (stmt->pointer) {
      emit("{} * const {} [{}] {};", data_type_name(stmt->ret_type),
           stmt->raw_name(), stmt->width(), init);
    } else {
      emit("const {} {} ({});", stmt->ret_data_type_name(), stmt->raw_name(),
           init);
    }
    */
}

std::string CodeGenLLVM::get_runtime_snode_name(SNode *snode) {
  if (snode->type == SNodeType::root) {
    return "Root";
  } else if (snode->type == SNodeType::dense) {
    return "Dense";
  } else if (snode->type == SNodeType::dynamic) {
    return "Dynamic";
  } else if (snode->type == SNodeType::pointer) {
    return "Pointer";
  } else if (snode->type == SNodeType::hash) {
    return "Hash";
  } else if (snode->type == SNodeType::bitmasked) {
    return "Bitmasked";
  } else if (snode->type == SNodeType::bit_struct) {
    return "BitStruct";
  } else if (snode->type == SNodeType::bit_array) {
    return "BitArray";
  } else {
    TI_P(snode_type_name(snode->type));
    TI_NOT_IMPLEMENTED
  }
}

llvm::Value *CodeGenLLVM::call(SNode *snode,
                               llvm::Value *node_ptr,
                               const std::string &method,
                               const std::vector<llvm::Value *> &arguments) {
  auto prefix = get_runtime_snode_name(snode);
  auto s = emit_struct_meta(snode);
  auto s_ptr =
      builder->CreateBitCast(s, llvm::Type::getInt8PtrTy(*llvm_context));

  node_ptr =
      builder->CreateBitCast(node_ptr, llvm::Type::getInt8PtrTy(*llvm_context));

  std::vector<llvm::Value *> func_arguments{s_ptr, node_ptr};

  func_arguments.insert(func_arguments.end(), arguments.begin(),
                        arguments.end());

  return call(builder.get(), prefix + "_" + method, func_arguments);
}

void CodeGenLLVM::visit(GetRootStmt *stmt) {
  if (stmt->root() == nullptr)
    llvm_val[stmt] = builder->CreateBitCast(
        get_root(SNodeTree::kFirstID),
        llvm::PointerType::get(
            StructCompilerLLVM::get_llvm_node_type(
                module.get(), prog->get_snode_root(SNodeTree::kFirstID)),
            0));
  else
    llvm_val[stmt] = builder->CreateBitCast(
        get_root(stmt->root()->get_snode_tree_id()),
        llvm::PointerType::get(
            StructCompilerLLVM::get_llvm_node_type(module.get(), stmt->root()),
            0));
}

void CodeGenLLVM::visit(BitExtractStmt *stmt) {
  int mask = (1u << (stmt->bit_end - stmt->bit_begin)) - 1;
  llvm_val[stmt] = builder->CreateAnd(
      builder->CreateLShr(llvm_val[stmt->input], stmt->bit_begin),
      tlctx->get_constant(mask));
}

void CodeGenLLVM::visit(LinearizeStmt *stmt) {
  llvm::Value *val = tlctx->get_constant(0);
  for (int i = 0; i < (int)stmt->inputs.size(); i++) {
    val = builder->CreateAdd(
        builder->CreateMul(val, tlctx->get_constant(stmt->strides[i])),
        llvm_val[stmt->inputs[i]]);
  }
  llvm_val[stmt] = val;
}

void CodeGenLLVM::visit(IntegerOffsetStmt *stmt){TI_NOT_IMPLEMENTED}

llvm::Value *CodeGenLLVM::create_bit_ptr_struct(llvm::Value *byte_ptr_base,
                                                llvm::Value *bit_offset) {
  // 1. get the bit pointer LLVM struct
  // struct bit_pointer {
  //    i8* byte_ptr;
  //    i32 offset;
  // };
  auto struct_type = llvm::StructType::get(
      *llvm_context, {llvm::Type::getInt8PtrTy(*llvm_context),
                      llvm::Type::getInt32Ty(*llvm_context)});
  // 2. allocate the bit pointer struct
  auto bit_ptr_struct = create_entry_block_alloca(struct_type);
  // 3. store `byte_ptr_base` into `bit_ptr_struct` (if provided)
  if (byte_ptr_base) {
    auto byte_ptr = builder->CreateBitCast(
        byte_ptr_base, llvm::PointerType::getInt8PtrTy(*llvm_context));
    builder->CreateStore(
        byte_ptr, builder->CreateGEP(bit_ptr_struct, {tlctx->get_constant(0),
                                                      tlctx->get_constant(0)}));
  }
  // 4. store `offset` in `bit_ptr_struct` (if provided)
  if (bit_offset) {
    builder->CreateStore(
        bit_offset,
        builder->CreateGEP(bit_ptr_struct,
                           {tlctx->get_constant(0), tlctx->get_constant(1)}));
  }
  return bit_ptr_struct;
}

llvm::Value *CodeGenLLVM::offset_bit_ptr(llvm::Value *input_bit_ptr,
                                         int bit_offset_delta) {
  auto byte_ptr_base = builder->CreateLoad(builder->CreateGEP(
      input_bit_ptr, {tlctx->get_constant(0), tlctx->get_constant(0)}));
  auto input_offset = builder->CreateLoad(builder->CreateGEP(
      input_bit_ptr, {tlctx->get_constant(0), tlctx->get_constant(1)}));
  auto new_bit_offset =
      builder->CreateAdd(input_offset, tlctx->get_constant(bit_offset_delta));
  return create_bit_ptr_struct(byte_ptr_base, new_bit_offset);
}

void CodeGenLLVM::visit(SNodeLookupStmt *stmt) {
  llvm::Value *parent = nullptr;
  parent = llvm_val[stmt->input_snode];
  TI_ASSERT(parent);
  auto snode = stmt->snode;
  if (snode->type == SNodeType::root) {
    llvm_val[stmt] = builder->CreateGEP(parent, llvm_val[stmt->input_index]);
  } else if (snode->type == SNodeType::dense ||
             snode->type == SNodeType::pointer ||
             snode->type == SNodeType::dynamic ||
             snode->type == SNodeType::bitmasked) {
    if (stmt->activate) {
      call(snode, llvm_val[stmt->input_snode], "activate",
           {llvm_val[stmt->input_index]});
    }
    llvm_val[stmt] = call(snode, llvm_val[stmt->input_snode], "lookup_element",
                          {llvm_val[stmt->input_index]});
  } else if (snode->type == SNodeType::bit_struct) {
    llvm_val[stmt] = parent;
  } else if (snode->type == SNodeType::bit_array) {
    auto element_num_bits =
        snode->dt->as<BitArrayType>()->get_element_num_bits();
    auto offset = tlctx->get_constant(element_num_bits);
    offset = builder->CreateMul(offset, llvm_val[stmt->input_index]);
    llvm_val[stmt] = create_bit_ptr_struct(llvm_val[stmt->input_snode], offset);
  } else {
    TI_INFO(snode_type_name(snode->type));
    TI_NOT_IMPLEMENTED
  }
}

void CodeGenLLVM::visit(GetChStmt *stmt) {
  if (stmt->input_snode->type == SNodeType::bit_array) {
    llvm_val[stmt] = llvm_val[stmt->input_ptr];
  } else if (stmt->ret_type->as<PointerType>()->is_bit_pointer()) {
    auto bit_struct = stmt->input_snode->dt->cast<BitStructType>();
    auto bit_offset = bit_struct->get_member_bit_offset(
        stmt->input_snode->child_id(stmt->output_snode));
    auto offset = tlctx->get_constant(bit_offset);
    llvm_val[stmt] = create_bit_ptr_struct(llvm_val[stmt->input_ptr], offset);
  } else {
    auto ch = create_call(stmt->output_snode->get_ch_from_parent_func_name(),
                          {builder->CreateBitCast(
                              llvm_val[stmt->input_ptr],
                              llvm::PointerType::getInt8PtrTy(*llvm_context))});
    llvm_val[stmt] = builder->CreateBitCast(
        ch, llvm::PointerType::get(StructCompilerLLVM::get_llvm_node_type(
                                       module.get(), stmt->output_snode),
                                   0));
  }
}

void CodeGenLLVM::visit(ExternalPtrStmt *stmt) {
  TI_ASSERT(stmt->width() == 1);

  auto argload = stmt->base_ptrs[0]->as<ArgLoadStmt>();
  auto arg_id = argload->arg_id;
  int num_indices = stmt->indices.size();
  std::vector<llvm::Value *> sizes(num_indices);

  for (int i = 0; i < num_indices; i++) {
    auto raw_arg = builder->CreateCall(
        get_runtime_function("Context_get_extra_args"),
        {get_context(), tlctx->get_constant(arg_id), tlctx->get_constant(i)});
    sizes[i] = raw_arg;
  }

  auto dt = stmt->ret_type.ptr_removed();
  auto base = builder->CreateBitCast(
      llvm_val[stmt->base_ptrs[0]],
      llvm::PointerType::get(tlctx->get_data_type(dt), 0));

  auto linear_index = tlctx->get_constant(0);
  for (int i = 0; i < num_indices; i++) {
    linear_index = builder->CreateMul(linear_index, sizes[i]);
    linear_index = builder->CreateAdd(linear_index, llvm_val[stmt->indices[i]]);
  }

  llvm_val[stmt] = builder->CreateGEP(base, linear_index);
}

void CodeGenLLVM::visit(ExternalTensorShapeAlongAxisStmt *stmt) {
  const auto arg_id = stmt->arg_id;
  const auto axis = stmt->axis;
  llvm_val[stmt] = builder->CreateCall(
      get_runtime_function("Context_get_extra_args"),
      {get_context(), tlctx->get_constant(arg_id), tlctx->get_constant(axis)});
}

std::string CodeGenLLVM::init_offloaded_task_function(OffloadedStmt *stmt,
                                                      std::string suffix) {
  current_loop_reentry = nullptr;
  current_while_after_loop = nullptr;

  task_function_type =
      llvm::FunctionType::get(llvm::Type::getVoidTy(*llvm_context),
                              {llvm::PointerType::get(context_ty, 0)}, false);

  auto task_kernel_name = fmt::format("{}_{}_{}{}", kernel_name, task_counter,
                                      stmt->task_name(), suffix);
  task_counter += 1;
  func = llvm::Function::Create(task_function_type,
                                llvm::Function::ExternalLinkage,
                                task_kernel_name, module.get());

  current_task = std::make_unique<OffloadedTask>(this);
  current_task->begin(task_kernel_name);

  for (auto &arg : func->args()) {
    kernel_args.push_back(&arg);
  }
  kernel_args[0]->setName("context");

  if (kernel_argument_by_val())
    func->addParamAttr(0, llvm::Attribute::ByVal);

  // entry_block has all the allocas
  this->entry_block = llvm::BasicBlock::Create(*llvm_context, "entry", func);

  // The real function body
  func_body_bb = llvm::BasicBlock::Create(*llvm_context, "body", func);
  builder->SetInsertPoint(func_body_bb);
  return task_kernel_name;
}

void CodeGenLLVM::finalize_offloaded_task_function() {
  builder->CreateRetVoid();

  // entry_block should jump to the body after all allocas are inserted
  builder->SetInsertPoint(entry_block);
  builder->CreateBr(func_body_bb);

  if (prog->config.print_kernel_llvm_ir) {
    static FileSequenceWriter writer("taichi_kernel_generic_llvm_ir_{:04d}.ll",
                                     "unoptimized LLVM IR (generic)");
    writer.write(module.get());
  }
  TI_ASSERT(!llvm::verifyFunction(*func, &llvm::errs()));
  // TI_INFO("Kernel function verified.");
}

std::tuple<llvm::Value *, llvm::Value *> CodeGenLLVM::get_range_for_bounds(
    OffloadedStmt *stmt) {
  llvm::Value *begin, *end;
  if (stmt->const_begin) {
    begin = tlctx->get_constant(stmt->begin_value);
  } else {
    auto begin_stmt = Stmt::make<GlobalTemporaryStmt>(
        stmt->begin_offset,
        TypeFactory::create_vector_or_scalar_type(1, PrimitiveType::i32));
    begin_stmt->accept(this);
    begin = builder->CreateLoad(llvm_val[begin_stmt.get()]);
  }
  if (stmt->const_end) {
    end = tlctx->get_constant(stmt->end_value);
  } else {
    auto end_stmt = Stmt::make<GlobalTemporaryStmt>(
        stmt->end_offset,
        TypeFactory::create_vector_or_scalar_type(1, PrimitiveType::i32));
    end_stmt->accept(this);
    end = builder->CreateLoad(llvm_val[end_stmt.get()]);
  }
  return std::tuple(begin, end);
}

void CodeGenLLVM::create_offload_struct_for(OffloadedStmt *stmt, bool spmd) {
  using namespace llvm;
  // TODO: instead of constructing tons of LLVM IR, writing the logic in
  // runtime.cpp may be a cleaner solution. See
  // CodeGenLLVMCPU::create_offload_range_for as an example.

  llvm::Function *body = nullptr;
  auto leaf_block = stmt->snode;

  // When looping over bit_arrays, we always vectorize and generate struct for
  // on their parent node (usually "dense") instead of itself for higher
  // performance. Also, note that the loop must be bit_vectorized for
  // bit_arrays, and their parent must be "dense".
  if (leaf_block->type == SNodeType::bit_array) {
    if (leaf_block->parent->type == SNodeType::dense) {
      leaf_block = leaf_block->parent;
    } else {
      TI_ERROR(
          "Struct-for looping through bit array but its parent is not dense")
    }
  }

  {
    // Create the loop body function
    auto guard = get_function_creation_guard({
        llvm::PointerType::get(get_runtime_type("Context"), 0),
        get_tls_buffer_type(),
        llvm::PointerType::get(get_runtime_type("Element"), 0),
        tlctx->get_data_type<int>(),
        tlctx->get_data_type<int>(),
    });

    body = guard.body;

    /* Function structure:
     *
     * function_body (entry):
     *   loop_index = lower_bound;
     *   tls_prologue()
     *   bls_prologue()
     *   goto loop_test
     *
     * loop_test:
     *   if (loop_index < upper_bound)
     *     goto loop_body
     *   else
     *     goto func_exit
     *
     * loop_body:
     *   initialize_coordinates()
     *   if (bitmasked voxel is active)
     *     goto struct_for_body
     *   else
     *     goto loop_body_tail
     *
     * struct_for_body:
     *   ... (Run codegen on the StructForStmt::body Taichi Block)
     *   goto loop_body_tail
     *
     * loop_body_tail:
     *   loop_index += block_dim
     *   goto loop_test
     *
     * func_exit:
     *   bls_epilogue()
     *   tls_epilogue()
     *   return
     */

    auto loop_index =
        create_entry_block_alloca(llvm::Type::getInt32Ty(*llvm_context));

    RuntimeObject element("Element", this, builder.get(), get_arg(2));

    // Loop ranges
    auto lower_bound = get_arg(3);
    auto upper_bound = get_arg(4);

    parent_coordinates = element.get_ptr("pcoord");
    block_corner_coordinates =
        create_entry_block_alloca(physical_coordinate_ty);

    auto refine =
        get_runtime_function(leaf_block->refine_coordinates_func_name());
    // A block corner is the global coordinate/index of the lower-left corner
    // cell within that block, and is the same for all the cells within that
    // block.
    create_call(refine, {parent_coordinates, block_corner_coordinates,
                         tlctx->get_constant(0)});

    if (stmt->tls_prologue) {
      stmt->tls_prologue->accept(this);
    }

    if (stmt->bls_prologue) {
      call("block_barrier");  // "__syncthreads()"
      stmt->bls_prologue->accept(this);
      call("block_barrier");  // "__syncthreads()"
    }

    llvm::Value *thread_idx = nullptr, *block_dim = nullptr;

    if (spmd) {
      thread_idx =
          builder->CreateIntrinsic(Intrinsic::nvvm_read_ptx_sreg_tid_x, {}, {});
      block_dim = builder->CreateIntrinsic(Intrinsic::nvvm_read_ptx_sreg_ntid_x,
                                           {}, {});
      builder->CreateStore(builder->CreateAdd(thread_idx, lower_bound),
                           loop_index);
    } else {
      builder->CreateStore(lower_bound, loop_index);
    }

    auto loop_test_bb = BasicBlock::Create(*llvm_context, "loop_test", func);
    auto loop_body_bb = BasicBlock::Create(*llvm_context, "loop_body", func);
    auto body_tail_bb =
        BasicBlock::Create(*llvm_context, "loop_body_tail", func);
    auto func_exit = BasicBlock::Create(*llvm_context, "func_exit", func);
    auto struct_for_body_bb =
        BasicBlock::Create(*llvm_context, "struct_for_body_body", func);

    builder->CreateBr(loop_test_bb);

    {
      // loop_test:
      //   if (loop_index < upper_bound)
      //     goto loop_body;
      //   else
      //     goto func_exit

      builder->SetInsertPoint(loop_test_bb);
      auto cond =
          builder->CreateICmp(llvm::CmpInst::Predicate::ICMP_SLT,
                              builder->CreateLoad(loop_index), upper_bound);
      builder->CreateCondBr(cond, loop_body_bb, func_exit);
    }

    // ***********************
    // Begin loop_body_bb:
    builder->SetInsertPoint(loop_body_bb);

    // initialize the coordinates
    auto new_coordinates = create_entry_block_alloca(physical_coordinate_ty);

    create_call(refine, {parent_coordinates, new_coordinates,
                         builder->CreateLoad(loop_index)});

    // One more refine step is needed for bit_arrays to make final coordinates
    // non-consecutive, since each thread will process multiple
    // coordinates via vectorization
    if (stmt->snode->type == SNodeType::bit_array && stmt->snode->parent) {
      if (stmt->snode->parent->type == SNodeType::dense) {
        refine =
            get_runtime_function(stmt->snode->refine_coordinates_func_name());

        create_call(refine,
                    {new_coordinates, new_coordinates, tlctx->get_constant(0)});
      } else {
        TI_ERROR(
            "Struct-for looping through bit array but its parent is not dense");
      }
    }

    current_coordinates = new_coordinates;

    // exec_cond: safe-guard the execution of loop body:
    //  - if non-POT field dim exists, make sure we don't go out of bounds
    //  - if leaf block is bitmasked, make sure we only loop over active
    //    voxels
    auto exec_cond = tlctx->get_constant(true);
    auto snode = stmt->snode;
    if (snode->type == SNodeType::bit_array && snode->parent) {
      if (snode->parent->type == SNodeType::dense) {
        snode = snode->parent;
      } else {
        TI_ERROR(
            "Struct-for looping through bit array but its parent is not dense");
      }
    }

    auto coord_object = RuntimeObject(kLLVMPhysicalCoordinatesName, this,
                                      builder.get(), new_coordinates);
    if (!prog->config.packed) {
      for (int i = 0; i < snode->num_active_indices; i++) {
        auto j = snode->physical_index_position[i];
        if (!bit::is_power_of_two(snode->extractors[j].num_elements_from_root)) {
          auto coord = coord_object.get("val", tlctx->get_constant(j));
          exec_cond = builder->CreateAnd(
              exec_cond,
              builder->CreateICmp(
                  llvm::CmpInst::ICMP_SLT, coord,
                  tlctx->get_constant(snode->extractors[j].num_elements_from_root)));
        }
      }
    }

    if (snode->type == SNodeType::bitmasked ||
        snode->type == SNodeType::pointer) {
      // test whether the current voxel is active or not
      auto is_active = call(snode, element.get("element"), "is_active",
                            {builder->CreateLoad(loop_index)});
      is_active =
          builder->CreateTrunc(is_active, llvm::Type::getInt1Ty(*llvm_context));
      exec_cond = builder->CreateAnd(exec_cond, is_active);
    }

    builder->CreateCondBr(exec_cond, struct_for_body_bb, body_tail_bb);

    {
      builder->SetInsertPoint(struct_for_body_bb);

      // The real loop body of the StructForStmt
      stmt->body->accept(this);

      builder->CreateBr(body_tail_bb);
    }

    {
      // body tail: increment loop_index and jump to loop_test
      builder->SetInsertPoint(body_tail_bb);

      if (spmd) {
        create_increment(loop_index, block_dim);
      } else {
        create_increment(loop_index, tlctx->get_constant(1));
      }
      builder->CreateBr(loop_test_bb);

      builder->SetInsertPoint(func_exit);
    }

    if (stmt->bls_epilogue) {
      call("block_barrier");  // "__syncthreads()"
      stmt->bls_epilogue->accept(this);
      call("block_barrier");  // "__syncthreads()"
    }

    if (stmt->tls_epilogue) {
      stmt->tls_epilogue->accept(this);
    }
  }

  int list_element_size = std::min(leaf_block->max_num_elements(),
                                   (int64)taichi_listgen_max_element_size);
  int num_splits = std::max(1, list_element_size / stmt->block_dim);

  auto struct_for_func = get_runtime_function("parallel_struct_for");

  if (arch_is_gpu(current_arch())) {
    // Note that on CUDA local array allocation must have a compile-time
    // constant size. Therefore, instead of passing in the tls_buffer_size
    // argument, we directly clone the "parallel_struct_for" function and
    // replace the "alignas(8) char tls_buffer[1]" statement with "alignas(8)
    // char tls_buffer[tls_buffer_size]" at compile time.

    auto value_map = llvm::ValueToValueMapTy();
    auto patched_struct_for_func =
        llvm::CloneFunction(struct_for_func, value_map);

    int replaced_alloca_types = 0;

    // Find the "1" in "char tls_buffer[1]" and replace it with
    // "tls_buffer_size"
    for (auto &bb : *patched_struct_for_func) {
      for (llvm::Instruction &inst : bb) {
        auto alloca = llvm::dyn_cast<AllocaInst>(&inst);
        if (!alloca || alloca->getAlignment() != 8)
          continue;
        auto alloca_type = alloca->getAllocatedType();
        auto char_type = llvm::Type::getInt8Ty(*llvm_context);
        // Allocated type should be array [1 x i8]
        if (alloca_type->isArrayTy() &&
            alloca_type->getArrayNumElements() == 1 &&
            alloca_type->getArrayElementType() == char_type) {
          auto new_type = llvm::ArrayType::get(char_type, stmt->tls_size);
          alloca->setAllocatedType(new_type);
          replaced_alloca_types += 1;
        }
      }
    }

    // There should be **exactly** one replacement.
    TI_ASSERT(replaced_alloca_types == 1);

    struct_for_func = patched_struct_for_func;
  }
  // Loop over nodes in the element list, in parallel
  create_call(
      struct_for_func,
      {get_context(), tlctx->get_constant(leaf_block->id),
       tlctx->get_constant(list_element_size), tlctx->get_constant(num_splits),
       body, tlctx->get_constant(stmt->tls_size),
       tlctx->get_constant(stmt->num_cpu_threads)});
  // TODO: why do we need num_cpu_threads on GPUs?

  current_coordinates = nullptr;
  parent_coordinates = nullptr;
  block_corner_coordinates = nullptr;
}

void CodeGenLLVM::visit(LoopIndexStmt *stmt) {
  if (stmt->loop->is<OffloadedStmt>() &&
      stmt->loop->as<OffloadedStmt>()->task_type ==
          OffloadedStmt::TaskType::struct_for) {
    llvm_val[stmt] = builder->CreateLoad(builder->CreateGEP(
        current_coordinates, {tlctx->get_constant(0), tlctx->get_constant(0),
                              tlctx->get_constant(stmt->index)}));
  } else {
    llvm_val[stmt] =
        builder->CreateLoad(loop_vars_llvm[stmt->loop][stmt->index]);
  }
}

void CodeGenLLVM::visit(LoopLinearIndexStmt *stmt) {
  if (stmt->loop->is<OffloadedStmt>() &&
      stmt->loop->as<OffloadedStmt>()->task_type ==
          OffloadedStmt::TaskType::struct_for) {
    llvm_val[stmt] = create_call("thread_idx");
  } else {
    TI_NOT_IMPLEMENTED;
  }
}

void CodeGenLLVM::visit(BlockCornerIndexStmt *stmt) {
  if (stmt->loop->is<OffloadedStmt>() &&
      stmt->loop->as<OffloadedStmt>()->task_type ==
          OffloadedStmt::TaskType::struct_for) {
    TI_ASSERT(block_corner_coordinates);
    llvm_val[stmt] = builder->CreateLoad(
        builder->CreateGEP(block_corner_coordinates,
                           {tlctx->get_constant(0), tlctx->get_constant(0),
                            tlctx->get_constant(stmt->index)}));
  } else {
    TI_NOT_IMPLEMENTED;
  }
}

void CodeGenLLVM::visit(BlockDimStmt *stmt) {
  TI_NOT_IMPLEMENTED  // No need for this statement for now. Untested so mark
                      // it as a loud failure.
      llvm_val[stmt] = create_call("block_dim", {});
}

void CodeGenLLVM::visit(GlobalTemporaryStmt *stmt) {
  auto runtime = get_runtime();
  auto buffer = call("get_temporary_pointer", runtime,
                     tlctx->get_constant((int64)stmt->offset));

  TI_ASSERT(stmt->width() == 1);
  auto ptr_type = llvm::PointerType::get(
      tlctx->get_data_type(stmt->ret_type.ptr_removed()), 0);
  llvm_val[stmt] = builder->CreatePointerCast(buffer, ptr_type);
}

void CodeGenLLVM::visit(ThreadLocalPtrStmt *stmt) {
  auto base = get_tls_base_ptr();
  TI_ASSERT(stmt->width() == 1);
  auto ptr = builder->CreateGEP(base, tlctx->get_constant(stmt->offset));
  auto ptr_type = llvm::PointerType::get(
      tlctx->get_data_type(stmt->ret_type.ptr_removed()), 0);
  llvm_val[stmt] = builder->CreatePointerCast(ptr, ptr_type);
}

void CodeGenLLVM::visit(BlockLocalPtrStmt *stmt) {
  TI_ASSERT(bls_buffer);
  auto base = bls_buffer;
  TI_ASSERT(stmt->width() == 1);
  auto ptr = builder->CreateGEP(
      base, {tlctx->get_constant(0), llvm_val[stmt->offset]});
  auto ptr_type = llvm::PointerType::get(
      tlctx->get_data_type(stmt->ret_type.ptr_removed()), 0);
  llvm_val[stmt] = builder->CreatePointerCast(ptr, ptr_type);
}

void CodeGenLLVM::visit(ClearListStmt *stmt) {
  auto snode_child = stmt->snode;
  auto snode_parent = stmt->snode->parent;
  auto meta_child = cast_pointer(emit_struct_meta(snode_child), "StructMeta");
  auto meta_parent = cast_pointer(emit_struct_meta(snode_parent), "StructMeta");
  call("clear_list", get_runtime(), meta_parent, meta_child);
}

void CodeGenLLVM::visit(InternalFuncStmt *stmt) {
  create_call(stmt->func_name, {get_context()});
}

void CodeGenLLVM::visit(AdStackAllocaStmt *stmt) {
  TI_ASSERT(stmt->width() == 1);
  TI_ASSERT_INFO(stmt->max_size > 0,
                 "Adaptive autodiff stack's size should have been determined.");
  auto type = llvm::ArrayType::get(llvm::Type::getInt8Ty(*llvm_context),
                                   stmt->size_in_bytes());
  auto alloca = create_entry_block_alloca(type, sizeof(int64));
  llvm_val[stmt] = builder->CreateBitCast(
      alloca, llvm::PointerType::getInt8PtrTy(*llvm_context));
  call("stack_init", llvm_val[stmt]);
}

void CodeGenLLVM::visit(AdStackPopStmt *stmt) {
  call("stack_pop", llvm_val[stmt->stack]);
}

void CodeGenLLVM::visit(AdStackPushStmt *stmt) {
  auto stack = stmt->stack->as<AdStackAllocaStmt>();
  call("stack_push", llvm_val[stack], tlctx->get_constant(stack->max_size),
       tlctx->get_constant(stack->element_size_in_bytes()));
  auto primal_ptr = call("stack_top_primal", llvm_val[stack],
                         tlctx->get_constant(stack->element_size_in_bytes()));
  primal_ptr = builder->CreateBitCast(
      primal_ptr,
      llvm::PointerType::get(tlctx->get_data_type(stmt->ret_type), 0));
  builder->CreateStore(llvm_val[stmt->v], primal_ptr);
}

void CodeGenLLVM::visit(AdStackLoadTopStmt *stmt) {
  auto stack = stmt->stack->as<AdStackAllocaStmt>();
  auto primal_ptr = call("stack_top_primal", llvm_val[stack],
                         tlctx->get_constant(stack->element_size_in_bytes()));
  primal_ptr = builder->CreateBitCast(
      primal_ptr,
      llvm::PointerType::get(tlctx->get_data_type(stmt->ret_type), 0));
  llvm_val[stmt] = builder->CreateLoad(primal_ptr);
}

void CodeGenLLVM::visit(AdStackLoadTopAdjStmt *stmt) {
  auto stack = stmt->stack->as<AdStackAllocaStmt>();
  auto adjoint = call("stack_top_adjoint", llvm_val[stack],
                      tlctx->get_constant(stack->element_size_in_bytes()));
  adjoint = builder->CreateBitCast(
      adjoint, llvm::PointerType::get(tlctx->get_data_type(stmt->ret_type), 0));
  llvm_val[stmt] = builder->CreateLoad(adjoint);
}

void CodeGenLLVM::visit(AdStackAccAdjointStmt *stmt) {
  auto stack = stmt->stack->as<AdStackAllocaStmt>();
  auto adjoint_ptr = call("stack_top_adjoint", llvm_val[stack],
                          tlctx->get_constant(stack->element_size_in_bytes()));
  adjoint_ptr = builder->CreateBitCast(
      adjoint_ptr,
      llvm::PointerType::get(tlctx->get_data_type(stack->ret_type), 0));
  auto old_val = builder->CreateLoad(adjoint_ptr);
  TI_ASSERT(is_real(stmt->v->ret_type));
  auto new_val = builder->CreateFAdd(old_val, llvm_val[stmt->v]);
  builder->CreateStore(new_val, adjoint_ptr);
}

void CodeGenLLVM::visit(RangeAssumptionStmt *stmt) {
  llvm_val[stmt] = llvm_val[stmt->input];
}

void CodeGenLLVM::visit(LoopUniqueStmt *stmt) {
  llvm_val[stmt] = llvm_val[stmt->input];
}

void CodeGenLLVM::eliminate_unused_functions() {
  TaichiLLVMContext::eliminate_unused_functions(
      module.get(), [&](std::string func_name) {
        for (auto &task : offloaded_tasks) {
          if (task.name == func_name)
            return true;
        }
        return false;
      });
}

FunctionType CodeGenLLVM::compile_module_to_executable() {
  TI_AUTO_PROF
  eliminate_unused_functions();

  tlctx->add_module(std::move(module));

  for (auto &task : offloaded_tasks) {
    task.compile();
  }
  auto offloaded_tasks_local = offloaded_tasks;
  auto kernel_name_ = kernel_name;
  return [=](Context &context) {
    TI_TRACE("Launching kernel {}", kernel_name_);
    for (auto task : offloaded_tasks_local) {
      task(&context);
    }
  };
}

FunctionCreationGuard CodeGenLLVM::get_function_creation_guard(
    std::vector<llvm::Type *> argument_types) {
  return FunctionCreationGuard(this, argument_types);
}

void CodeGenLLVM::initialize_context() {
  if (kernel->arch == Arch::cuda) {
    tlctx = prog->llvm_context_device.get();
  } else {
    tlctx = prog->llvm_context_host.get();
  }
  llvm_context = tlctx->get_this_thread_context();
  builder = std::make_unique<llvm::IRBuilder<>>(*llvm_context);
}

llvm::Value *CodeGenLLVM::get_arg(int i) {
  std::vector<llvm::Value *> args;
  for (auto &arg : func->args()) {
    args.push_back(&arg);
  }
  return args[i];
}

llvm::Value *CodeGenLLVM::get_context() {
  return get_arg(0);
}

llvm::Value *CodeGenLLVM::get_tls_base_ptr() {
  return get_arg(1);
}

llvm::Type *CodeGenLLVM::get_tls_buffer_type() {
  return llvm::Type::getInt8PtrTy(*llvm_context);
}

std::vector<llvm::Type *> CodeGenLLVM::get_xlogue_argument_types() {
  return {llvm::PointerType::get(get_runtime_type("Context"), 0),
          get_tls_buffer_type()};
}

llvm::Type *CodeGenLLVM::get_xlogue_function_type() {
  return llvm::FunctionType::get(llvm::Type::getVoidTy(*llvm_context),
                                 get_xlogue_argument_types(), false);
}

llvm::Value *CodeGenLLVM::get_root(int snode_tree_id) {
  return create_call("LLVMRuntime_get_roots",
                     {get_runtime(), tlctx->get_constant(snode_tree_id)});
}

llvm::Value *CodeGenLLVM::get_runtime() {
  auto runtime_ptr = create_call("Context_get_runtime", {get_context()});
  return builder->CreateBitCast(
      runtime_ptr, llvm::PointerType::get(get_runtime_type("LLVMRuntime"), 0));
}

llvm::Value *CodeGenLLVM::emit_struct_meta(SNode *snode) {
  auto obj = emit_struct_meta_object(snode);
  TI_ASSERT(obj != nullptr);
  return obj->ptr;
}

void CodeGenLLVM::emit_to_module() {
  TI_AUTO_PROF
  ir->accept(this);
}

FunctionType CodeGenLLVM::gen() {
  emit_to_module();
  return compile_module_to_executable();
}

llvm::Value *CodeGenLLVM::create_xlogue(std::unique_ptr<Block> &block) {
  llvm::Value *xlogue;

  auto xlogue_type = get_xlogue_function_type();
  auto xlogue_ptr_type = llvm::PointerType::get(xlogue_type, 0);

  if (block) {
    auto guard = get_function_creation_guard(get_xlogue_argument_types());
    block->accept(this);
    xlogue = guard.body;
  } else {
    xlogue = llvm::ConstantPointerNull::get(xlogue_ptr_type);
  }

  return xlogue;
}

TLANG_NAMESPACE_END
