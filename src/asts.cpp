#pragma once
#include "types.cpp"
#include "utils.cpp"
#include "values.cpp"

// Includes function arguments
static std::map<std::string, Value *> curr_named_variables;
static std::map<std::string, Type *> curr_named_var_types;
static std::map<std::string, Type *> curr_named_types;

class TopLevelAST {
public:
  virtual void gen_toplevel() = 0;
};
/// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
  virtual ~ExprAST() {}
  virtual Type *get_type() = 0;
  virtual Value *gen_value() = 0;
};

NumType *num_char_to_type(char type_char, bool has_dot) {
  switch (type_char) {
  case 'd':
    return new NumType(64, true, true);
  case 'f':
    return new NumType(32, true, true);
  case 'i':
    if (has_dot)
      error("'i' (int32) type can't have a '.'");
    return new NumType(32, false, true);
  case 'u':
    if (has_dot)
      error("'u' (uint32) type can't have a '.'");
    return new NumType(32, false, false);
  case 'l':
    if (has_dot)
      error("'l' (long, int64) type can't have a '.'");
    return new NumType(64, false, true);
  case 'b':
    if (has_dot)
      error("'b' (byte, uint8) type can't have a '.'");
    return new NumType(8, false, false);
  default:
    fprintf(stderr, "Error: Invalid number type id '%c'", type_char);
    exit(1);
  }
}
/// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
  char *val;
  unsigned int val_len;
  unsigned int base;
  NumType *type;

public:
  NumberExprAST(char *val, unsigned int val_len, char type_char, bool has_dot,
                unsigned int base)
      : val(val), val_len(val_len), base(base) {
    type = num_char_to_type(type_char, has_dot);
  }
  Type *get_type() { return type; }
  Value *gen_value() {
    LLVMValueRef num;
    if (type->is_floating)
      if (base != 10) {
        error("floating-point numbers with a base that isn't decimal aren't "
              "supported.");
      } else
        num = LLVMConstRealOfStringAndSize(type->llvm_type(), val, val_len);
    else
      num = LLVMConstIntOfStringAndSize(type->llvm_type(), val, val_len, base);
    return new ConstValue(type, num, nullptr);
  }
};
/// BoolExprAST - Expression class for boolean literals (true or false).
class BoolExprAST : public ExprAST {
  bool value;
  Type *type;

public:
  BoolExprAST(bool value) : value(value) {
    type = new NumType(1, false, false);
  }
  Type *get_type() { return type; }
  Value *gen_value() {
    return new ConstValue(type,
                          value ? LLVMConstAllOnes(type->llvm_type())
                                : LLVMConstNull(type->llvm_type()),
                          nullptr);
  }
};

class CastExprAST : public ExprAST {
  ExprAST *value;
  Type *from;
  Type *to;

public:
  CastExprAST(ExprAST *value, Type *to) : value(value), to(to) {
    from = value->get_type();
  }
  Value *gen_value() { return value->gen_value()->cast_to(to); }
  Type *get_type() { return to; }
};

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {
  char *name;
  unsigned int name_len;
  Type *type;

public:
  VariableExprAST(char *name, unsigned int name_len)
      : name(name), name_len(name_len) {
    type = curr_named_var_types[name];
    if (!type) {
      fprintf(stderr, "Error: Variable '%s' doesn't exist.", name);
      exit(1);
    }
  }
  Type *get_type() { return type; }
  Value *gen_value() {
    return curr_named_variables[std::string(name, name_len)];
  }
};

/// LetExprAST - Expression class for creating a variable, like "let a = 3".
class LetExprAST : public ExprAST, public TopLevelAST {

public:
  char *id;
  unsigned int id_len;
  Type *type;
  ExprAST *value;
  bool constant;
  bool global;
  LetExprAST(char *id, unsigned int id_len, Type *type, ExprAST *value,
             bool constant, bool global)
      : id(id), id_len(id_len), constant(constant), global(global) {
    if (type)
      curr_named_var_types[std::string(id, id_len)] = type;
    else if (value != nullptr)
      curr_named_var_types[std::string(id, id_len)] = type = value->get_type();
    else
      error("Untyped valueless variable");
    this->type = type;
    this->value = value;
  }
  Type *get_type() { return type; }
  void gen_toplevel() {
    LLVMValueRef ptr = LLVMAddGlobal(curr_module, type->llvm_type(), id);
    if (value) {
      Value *val = value->gen_value();
      if (ConstValue *expr = dynamic_cast<ConstValue *>(val))
        LLVMSetInitializer(ptr, val->gen_load());
      else
        error("Global variable needs a constant value inside it");
    }
    curr_named_variables[std::string(id, id_len)] =
        new BasicLoadValue(ptr, type);
  }
  Value *gen_value() {
    if (constant) {
      if (value)
        return curr_named_variables[std::string(id, id_len)] =
                   value->gen_value();
      else
        error("Constant variables need an initialization value");
    }
    LLVMValueRef ptr = LLVMBuildAlloca(curr_builder, type->llvm_type(), id);
    curr_named_variables[std::string(id, id_len)] =
        new BasicLoadValue(ptr, type);
    if (value) {
      LLVMValueRef llvm_val = value->gen_value()->cast_to(type)->gen_load();
      LLVMBuildStore(curr_builder, llvm_val, ptr);
    }
    return new BasicLoadValue(ptr, type);
  }
  LLVMValueRef u_gen_ptr() {
    if (constant)
      error("Can't point to a constant");
    LLVMValueRef ptr = LLVMBuildAlloca(curr_builder, type->llvm_type(), id);
    curr_named_variables[std::string(id, id_len)] =
        new BasicLoadValue(ptr, type);
    if (value) {
      LLVMValueRef llvm_val = value->gen_value()->gen_load();
      LLVMBuildStore(curr_builder, llvm_val, ptr);
    }
    return ptr;
  }
  LLVMValueRef gen_declare() {
    LLVMValueRef global = LLVMAddGlobal(curr_module, type->llvm_type(), id);
    curr_named_variables[std::string(id, id_len)] =
        new BasicLoadValue(global, type);
    return global;
  }
};

/// CharExprAST - Expression class for a single char ('a')
class CharExprAST : public ExprAST {
  char charr;

public:
  CharExprAST(char charr) : charr(charr) {}
  Type *get_type() { return new NumType(8, false, false); }
  Value *gen_value() {
    return new ConstValue(get_type(), LLVMConstInt(int_8_type, charr, false),
                          nullptr);
  }
};

/// StringExprAST - Expression class for multiple chars ("hello")
class StringExprAST : public ExprAST {
  char *chars;
  unsigned int length;
  NumType *c_type;
  TupleType *t_type;
  PointerType *p_type;

public:
  StringExprAST(char *chars, unsigned int length)
      : chars(chars), length(length) {
    if (chars[length - 1] != '\0')
      error("C-style strings should be fed into StringExprAST including the "
            "last null-byte");
    c_type = new NumType(8, false, true);
    t_type = new TupleType(c_type, length);
    p_type = new PointerType(c_type);
  }
  Type *get_type() { return p_type; }
  Value *gen_value() {
    LLVMValueRef str = LLVMConstString(chars, length, true);
    LLVMValueRef glob = LLVMAddGlobal(curr_module, t_type->llvm_type(), ".str");
    LLVMSetInitializer(glob, str);
    LLVMValueRef zeros[2] = {
        LLVMConstInt((new NumType(64, false, false))->llvm_type(), 0, false),
        LLVMConstInt((new NumType(64, false, false))->llvm_type(), 0, false)};
    // cast [ ... x i8 ]* to i8*
    LLVMValueRef cast = LLVMConstGEP2(t_type->llvm_type(), glob, zeros, 2);
    return new ConstValue(p_type, cast, nullptr);
  }
  // LLVMValueRef u_gen_load() {
  //   return gen_variable((char *)".str", false)->gen_load();
  // }
  // Value *gen_variable(char *name, bool constant) {
  //   LLVMValueRef str = LLVMConstString(chars, length, true);
  //   LLVMValueRef glob = LLVMAddGlobal(curr_module, type->llvm_type(), name);
  //   LLVMSetInitializer(glob, str);
  //   LLVMSetGlobalConstant(glob, constant);
  //   return new BasicLoadValue(glob, type);
  // }
};

LLVMValueRef gen_num_num_binop(int op, LLVMValueRef L, LLVMValueRef R,
                               NumType *lhs_nt, NumType *rhs_nt) {
  if (lhs_nt->bits > rhs_nt->bits)
    R = cast(R, rhs_nt, lhs_nt);
  else if (rhs_nt->bits > lhs_nt->bits)
    L = cast(L, lhs_nt, rhs_nt);
  bool floating = lhs_nt->is_floating && rhs_nt->is_floating;
  if (floating)
    switch (op) {
    case '+':
      return LLVMBuildFAdd(curr_builder, L, R, "");
    case '-':
      return LLVMBuildFSub(curr_builder, L, R, "");
    case '*':
      return LLVMBuildFMul(curr_builder, L, R, "");
    case '/':
      return LLVMBuildFDiv(curr_builder, L, R, "");
    case '%':
      return LLVMBuildFRem(curr_builder, L, R, "");
    case T_LAND:
    case '&':
      return LLVMBuildAnd(curr_builder, L, R, "");
    case T_LOR:
    case '|':
      return LLVMBuildOr(curr_builder, L, R, "");
    case '<':
      return LLVMBuildFCmp(curr_builder, LLVMRealULT, L, R, "");
    case '>':
      return LLVMBuildFCmp(curr_builder, LLVMRealUGT, L, R, "");
    case T_LEQ:
      return LLVMBuildFCmp(curr_builder, LLVMRealULE, L, R, "");
    case T_GEQ:
      return LLVMBuildFCmp(curr_builder, LLVMRealUGE, L, R, "");
    case T_EQEQ:
      return LLVMBuildFCmp(curr_builder, LLVMRealUEQ, L, R, "");
    case T_NEQ:
      return LLVMBuildFCmp(curr_builder, LLVMRealUNE, L, R, "");
    default:
      fprintf(stderr, "Error: invalid float_float binary operator '%c'", op);
      exit(1);
    }
  else if (!lhs_nt->is_floating && !rhs_nt->is_floating) {
    bool is_signed = lhs_nt->is_signed && rhs_nt->is_signed;
    switch (op) {
    case '+':
      return LLVMBuildAdd(curr_builder, L, R, "");
    case '-':
      return LLVMBuildSub(curr_builder, L, R, "");
    case '*':
      return LLVMBuildMul(curr_builder, L, R, "");
    case '/':
      return is_signed ? LLVMBuildSDiv(curr_builder, L, R, "")
                       : LLVMBuildUDiv(curr_builder, L, R, "");
    case '%':
      return is_signed ? LLVMBuildSRem(curr_builder, L, R, "")
                       : LLVMBuildURem(curr_builder, L, R, "");
    case T_LAND:
    case '&':
      return LLVMBuildAnd(curr_builder, L, R, "");
    case T_LOR:
    case '|':
      return LLVMBuildOr(curr_builder, L, R, "");
    case '<':
      return LLVMBuildICmp(curr_builder, is_signed ? LLVMIntSLT : LLVMIntULT, L,
                           R, "");
    case '>':
      return LLVMBuildICmp(curr_builder, is_signed ? LLVMIntSGT : LLVMIntUGT, L,
                           R, "");
    case T_LEQ:
      return LLVMBuildICmp(curr_builder, is_signed ? LLVMIntSLE : LLVMIntULE, L,
                           R, "");
    case T_GEQ:
      return LLVMBuildICmp(curr_builder, is_signed ? LLVMIntSGE : LLVMIntUGE, L,
                           R, "");
    case T_EQEQ:
      return LLVMBuildICmp(curr_builder, LLVMIntPredicate::LLVMIntEQ, L, R, "");
    case T_NEQ:
      return LLVMBuildICmp(curr_builder, LLVMIntPredicate::LLVMIntNE, L, R, "");
    default:
      fprintf(stderr, "Error: invalid int_int binary operator '%c'", op);
      exit(1);
    }
  } else {
    fprintf(stderr, "Error: invalid float_int binary operator '%c'", op);
    exit(1);
  }
}
LLVMValueRef gen_ptr_num_binop(int op, LLVMValueRef ptr, LLVMValueRef num,
                               PointerType *ptr_t, NumType *num_t) {
  switch (op) {
  case '-':
    // num = 0-num
    num = LLVMBuildSub(
        curr_builder,
        LLVMConstInt((new NumType(32, false, false))->llvm_type(), 0, false),
        num, "");
    /* falls through */
  case '+':
    return LLVMBuildGEP2(curr_builder, ptr_t->points_to->llvm_type(), ptr, &num,
                         1, "ptraddtmp");
  default:
    fprintf(stderr, "Error: invalid ptr_num binary operator '%c'", op);
    exit(1);
  }
}

/// BinaryExprAST - Expression class for a binary operator.
class BinaryExprAST : public ExprAST {
  int op;
  ExprAST *LHS, *RHS;
  Type *type;

public:
  BinaryExprAST(int op, ExprAST *LHS, ExprAST *RHS)
      : op(op), LHS(LHS), RHS(RHS) {
    Type *lhs_t = LHS->get_type();
    Type *rhs_t = RHS->get_type();
    TypeType lhs_tt = lhs_t->type_type();
    TypeType rhs_tt = rhs_t->type_type();

    if (op == '=')
      type = rhs_t;
    else if (lhs_tt == TypeType::Number &&
             rhs_tt == TypeType::Number) // int + int returns int, int < int
                                         // returns int1 (bool)
      type = (binop_precedence[op] == 10 /* comparison */)
                 ? new NumType(1, false, false)
                 : /* todo get max size and return that type */ lhs_t;
    else if (lhs_tt == TypeType::Pointer &&
             rhs_tt == TypeType::Number) // ptr + int returns offsetted ptr
      type = /* ptr */ lhs_t;
    else if (lhs_tt == TypeType::Number &&
             rhs_tt == TypeType::Pointer) // int + ptr returns offsetted ptr
      type = /* ptr */ rhs_t;
    else
      error("Unknown ptr_ptr op");
  }

  Type *get_type() { return type; }

  Value *gen_assign() {
    LLVMValueRef store_ptr = LHS->gen_value()->gen_ptr();
    Value *val = RHS->gen_value()->cast_to(LHS->get_type());
    LLVMBuildStore(curr_builder, val->gen_load(), store_ptr);
    return new BasicLoadValue(store_ptr, type);
  }
  Value *gen_value() {
    Type *lhs_t = LHS->get_type();
    PointerType *lhs_pt = dynamic_cast<PointerType *>(lhs_t);
    if (op == '=')
      return gen_assign();
    Type *rhs_t = RHS->get_type();
    NumType *lhs_nt = dynamic_cast<NumType *>(lhs_t);
    NumType *rhs_nt = dynamic_cast<NumType *>(rhs_t);
    PointerType *rhs_pt = dynamic_cast<PointerType *>(rhs_t);
    LLVMValueRef L = LHS->gen_value()->gen_load();
    LLVMValueRef R = RHS->gen_value()->gen_load();
    if (lhs_nt && rhs_nt)
      return new ConstValue(type, gen_num_num_binop(op, L, R, lhs_nt, rhs_nt),
                            nullptr);
    else if (lhs_nt && rhs_pt)
      return new ConstValue(type, gen_ptr_num_binop(op, R, L, rhs_pt, lhs_nt),
                            nullptr);
    else if (lhs_pt && rhs_nt)
      return new ConstValue(type, gen_ptr_num_binop(op, L, R, lhs_pt, rhs_nt),
                            nullptr);
    error("Unknown ptr_ptr op");
  }
};
/// UnaryExprAST - Expression class for a unary operator.
class UnaryExprAST : public ExprAST {
  char op;
  ExprAST *operand;
  Type *type;

public:
  UnaryExprAST(char op, ExprAST *operand) : op(op), operand(operand) {
    if (op == '*')
      if (PointerType *opt = dynamic_cast<PointerType *>(operand->get_type()))
        type = opt->get_points_to();
      else
        error("* can't be used on a non-pointer type");
    else if (op == '&')
      type = new PointerType(operand->get_type());
    else
      type = operand->get_type();
  }
  Type *get_type() { return type; }
  Value *gen_value() {
    auto zero =
        LLVMConstInt((new NumType(32, false, false))->llvm_type(), 0, false);
    Value *val = operand->gen_value();
    switch (op) {
    case '!':
      // shortcut for != 1
      return new ConstValue(
          type,
          LLVMBuildFCmp(curr_builder, LLVMRealONE, val->gen_load(),
                        LLVMConstReal(float_64_type, 1.0), ""),
          nullptr);
    case '-':
      // shortcut for 0-n
      return new ConstValue(type,
                            LLVMBuildFSub(curr_builder,
                                          LLVMConstReal(float_64_type, 0.0),
                                          val->gen_load(), ""),
                            nullptr);
    case '*':
      return new BasicLoadValue(val->gen_load(), type);
    case '&':
      return new ConstValue(type, val->gen_ptr(), nullptr);
    default:
      fprintf(stderr, "Error: invalid prefix unary operator '%c'", op);
      exit(1);
    }
  }
};

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
  FunctionType *func_t;
  ExprAST *called;
  ExprAST **args;
  unsigned int args_len;
  bool is_ptr;
  Type *type;

public:
  CallExprAST(ExprAST *called, ExprAST **args, unsigned int args_len)
      : called(called), args(args), args_len(args_len) {
    func_t = dynamic_cast<FunctionType *>(called->get_type());
    if (!func_t) {
      if (PointerType *ptr = dynamic_cast<PointerType *>(called->get_type()))
        func_t = dynamic_cast<FunctionType *>(ptr->get_points_to());
      else {
        fprintf(stderr, "Error: Function doesn't exist or is not a function");
        exit(1);
      }
    }
    if (func_t->vararg ? args_len < func_t->arg_count
                       : args_len != func_t->arg_count) {
      fprintf(stderr,
              "Error: Incorrect # arguments passed. (Expected %d, got %d)",
              func_t->arg_count, args_len);
      exit(1);
    }

    type = func_t->return_type;
  }

  Type *get_type() { return type; }
  Value *gen_value() {
    LLVMValueRef func = called->gen_value()->gen_load();
    if (!func)
      error("Unknown function referenced");
    LLVMValueRef *arg_vs = alloc_arr<LLVMValueRef>(args_len);
    for (unsigned i = 0; i < args_len; i++) {
      if (i < func_t->arg_count)
        arg_vs[i] =
            args[i]->gen_value()->cast_to(func_t->arguments[i])->gen_load();
      else
        arg_vs[i] = args[i]->gen_value()->gen_load();
    }
    return new ConstValue(func_t->return_type,
                          LLVMBuildCall2(curr_builder, func_t->llvm_type(),
                                         func, arg_vs, args_len, ""),
                          nullptr);
  }
};

/// IndexExprAST - Expression class for accessing indexes (a[0]).
class IndexExprAST : public ExprAST {
  ExprAST *value;
  ExprAST *index;
  Type *type;

public:
  IndexExprAST(ExprAST *value, ExprAST *index) : value(value), index(index) {
    Type *base_type = value->get_type();
    if (PointerType *p_type = dynamic_cast<PointerType *>(base_type))
      type = p_type->get_points_to();
    else if (TupleType *arr_type = dynamic_cast<TupleType *>(base_type))
      type = arr_type->get_elem_type();
    else {
      fprintf(stderr,
              "Invalid index, type not arrayish.\n"
              "Expected: array | pointer \nGot: %s",
              tt_to_str(base_type->type_type()));
      exit(1);
    }
  }

  Type *get_type() { return type; }

  Value *gen_value() {
    LLVMValueRef index_v = index->gen_value()->gen_load();
    return new BasicLoadValue(LLVMBuildGEP2(curr_builder, type->llvm_type(),
                                            value->gen_value()->gen_load(),
                                            &index_v, 1, "indextmp"),
                              type);
  }
};

/// PropAccessExprAST - Expression class for accessing properties (a.size).
class PropAccessExprAST : public ExprAST {
  ExprAST *source;
  StructType *source_type;
  unsigned int index;
  char *key;
  Type *type;

public:
  PropAccessExprAST(char *key, unsigned int name_len, ExprAST *source)
      : key(key), source(source) {
    source_type = dynamic_cast<StructType *>(
        dynamic_cast<PointerType *>(source->get_type())->get_points_to());
    index = source_type->get_index(key, name_len);
    type = source_type->get_elem_type(index);
  }

  Type *get_type() { return type; }

  Value *gen_value() {
    return new BasicLoadValue(
        LLVMBuildStructGEP2(curr_builder, source_type->llvm_type(),
                            source->gen_value()->gen_load(), index, key),
        type);
  }
};

struct CompleteExtensionName {
  char *str;
  unsigned int len;
};
CompleteExtensionName get_complete_extension_name(Type *base_type, char *name,
                                                  unsigned int name_len) {
  const char *called_type = base_type->stringify();
  CompleteExtensionName cen;
  cen.len = 4 + strlen(called_type) + name_len;
  cen.str = alloc_c(cen.len);
  strcpy(cen.str, "(");
  strcat(cen.str, called_type);
  strcat(cen.str, ")::");
  strcat(cen.str, name);
  return cen;
}
/// MethodCallExprAST - Expression class for calling methods (a.len()).
class MethodCallExprAST : public ExprAST {
  CallExprAST *underlying_call;

public:
  MethodCallExprAST(char *name, unsigned int name_len, ExprAST *source,
                    ExprAST **args, unsigned int args_len) {
    CompleteExtensionName cen =
        get_complete_extension_name(source->get_type(), name, name_len);
    VariableExprAST *called_function = new VariableExprAST(cen.str, cen.len);
    ExprAST **args_with_this = realloc_arr<ExprAST *>(args, args_len + 1);
    args_with_this[args_len] = source;
    underlying_call =
        new CallExprAST(called_function, args_with_this, args_len + 1);
  }

  Type *get_type() { return underlying_call->get_type(); }
  Value *gen_value() { return underlying_call->gen_value(); }
};

/// NewExprAST - Expression class for creating an instance of a struct (new
/// String { pointer = "hi", length = 2 } ).
class NewExprAST : public ExprAST {
  StructType *s_type;
  PointerType *p_type;
  unsigned int *indexes;
  char **keys;
  ExprAST **values;
  unsigned int key_count;

public:
  NewExprAST(StructType *s_type, char **keys, unsigned int *key_lens,
             ExprAST **values, unsigned int key_count)
      : s_type(s_type), values(values), key_count(key_count) {
    indexes = alloc_arr<unsigned int>(key_count);
    for (unsigned int i = 0; i < key_count; i++)
      indexes[i] = s_type->get_index(keys[i], key_lens[i]);
    p_type = new PointerType(s_type);
  }

  Type *get_type() { return p_type; }

  Value *gen_value() {
    LLVMValueRef ptr =
        LLVMBuildAlloca(curr_builder, s_type->llvm_type(), "newalloc");
    for (unsigned int i = 0; i < key_count; i++) {
      LLVMValueRef llvm_indexes[2] = {
          LLVMConstInt(LLVMInt32Type(), 0, false),
          LLVMConstInt(LLVMInt32Type(), indexes[i], false)};
      LLVMValueRef set_ptr = LLVMBuildStructGEP2(
          curr_builder, s_type->llvm_type(), ptr, indexes[i], "tmpgep");
      LLVMBuildStore(curr_builder, values[i]->gen_value()->gen_load(), set_ptr);
    }
    return new ConstValue(p_type, ptr, nullptr);
  }
};

class BlockExprAST : public ExprAST {
  ExprAST **exprs;
  unsigned int exprs_len;
  Type *type;

public:
  BlockExprAST(ExprAST **exprs, unsigned int exprs_len)
      : exprs(exprs), exprs_len(exprs_len) {
    if (exprs_len == 0)
      error("block can't be empty.");
    type = exprs[exprs_len - 1]->get_type();
  }
  Type *get_type() { return type; }
  Value *gen_value() {
    // generate code for all exprs and only return last expr
    for (unsigned int i = 0; i < exprs_len - 1; i++)
      exprs[i]->gen_value();
    return exprs[exprs_len - 1]->gen_value();
  }
};

/// NullExprAST - null
class NullExprAST : public ExprAST {
public:
  Type *type;
  NullExprAST(Type *type) : type(type) {}
  Type *get_type() { return type; }
  Value *gen_value() {
    return new ConstValue(type, LLVMConstNull(type->llvm_type()), nullptr);
  }
};

/// IfExprAST - Expression class for if/then/else.
class IfExprAST : public ExprAST {
public:
  ExprAST *cond, *then, *elze;
  Type *type;
  IfExprAST(ExprAST *cond, ExprAST *then,
            // elze because else cant be a variable name lol
            ExprAST *elze)
      : cond(cond), then(then) {
    Type *then_t = then->get_type();
    type = then_t;
    if (elze == nullptr)
      elze = new NullExprAST(type);
    Type *else_t = elze->get_type();
    if (then_t->neq(else_t)) {
      fprintf(stderr,
              "Error: while's then and else side don't have the same type, ");
      then_t->log_diff(else_t);
      fprintf(stderr, ".\n");
      exit(1);
    }
    this->elze = elze;
  }

  Type *get_type() { return type; }

  Value *gen_value() {
    LLVMValueRef cond_v = cond->gen_value()->gen_load();
    if (NumType *n = dynamic_cast<NumType *>(cond->get_type()))
      if (n->is_floating)
        cond_v = LLVMBuildFCmp(curr_builder, LLVMRealONE, cond_v,
                               LLVMConstReal(float_64_type, 0.0), "ifcond");
    LLVMValueRef func =
        LLVMGetBasicBlockParent(LLVMGetInsertBlock(curr_builder));
    LLVMBasicBlockRef then_bb =
        LLVMAppendBasicBlockInContext(curr_ctx, func, "ifthen");
    LLVMBasicBlockRef else_bb =
        LLVMCreateBasicBlockInContext(curr_ctx, "ifelse");
    LLVMBasicBlockRef merge_bb =
        LLVMCreateBasicBlockInContext(curr_ctx, "ifmerge");
    // if
    LLVMBuildCondBr(curr_builder, cond_v, then_bb, else_bb);
    // then
    LLVMPositionBuilderAtEnd(curr_builder, then_bb);
    Value *then_v = then->gen_value();
    LLVMBuildBr(curr_builder, merge_bb);
    // Codegen of 'then' can change the current block, update then_bb for the
    // PHI.
    then_bb = LLVMGetInsertBlock(curr_builder);
    // else
    LLVMAppendExistingBasicBlock(func, else_bb);
    LLVMPositionBuilderAtEnd(curr_builder, else_bb);
    Value *else_v = elze->gen_value();
    LLVMBuildBr(curr_builder, merge_bb);
    // Codegen of 'else' can change the current block, update else_bb for the
    // PHI.
    else_bb = LLVMGetInsertBlock(curr_builder);
    // merge
    LLVMAppendExistingBasicBlock(func, merge_bb);
    LLVMPositionBuilderAtEnd(curr_builder, merge_bb);
    return new PHIValue(then_bb, then_v, else_bb, else_v);
  }
};

/// WhileExprAST - Expression class for while loops.
class WhileExprAST : public IfExprAST {
public:
  using IfExprAST::IfExprAST; // inherit constructor from IfExprAST
  Value *gen_value() {
    LLVMValueRef cond_v = cond->gen_value()->gen_load();
    if (NumType *n = dynamic_cast<NumType *>(cond->get_type()))
      if (n->is_floating)
        cond_v = LLVMBuildFCmp(curr_builder, LLVMRealONE, cond_v,
                               LLVMConstReal(float_64_type, 0.0), "whilecond");
    LLVMValueRef func =
        LLVMGetBasicBlockParent(LLVMGetInsertBlock(curr_builder));
    LLVMBasicBlockRef then_bb =
        LLVMAppendBasicBlockInContext(curr_ctx, func, "whilethen");
    LLVMBasicBlockRef else_bb =
        LLVMCreateBasicBlockInContext(curr_ctx, "whileelse");
    LLVMBasicBlockRef merge_bb =
        LLVMCreateBasicBlockInContext(curr_ctx, "endwhile");
    // while
    LLVMBuildCondBr(curr_builder, cond_v, then_bb, else_bb);
    // then
    LLVMPositionBuilderAtEnd(curr_builder, then_bb);
    Value *then_v = then->gen_value();
    LLVMValueRef cond_v2 = cond->gen_value()->gen_load();
    if (NumType *n = dynamic_cast<NumType *>(cond->get_type()))
      if (n->is_floating)
        cond_v2 = LLVMBuildFCmp(curr_builder, LLVMRealONE, cond_v,
                                LLVMConstReal(float_64_type, 0.0), "whilecond");
    LLVMBuildCondBr(curr_builder, cond_v2, then_bb, merge_bb);
    // Codegen of 'then' can change the current block, update then_bb for the
    // PHI.
    then_bb = LLVMGetInsertBlock(curr_builder);
    // else
    LLVMAppendExistingBasicBlock(func, else_bb);
    LLVMPositionBuilderAtEnd(curr_builder, else_bb);
    Value *else_v = elze->gen_value();
    LLVMBuildBr(curr_builder, merge_bb);
    // Codegen of 'else' can change the current block, update else_bb for the
    // PHI.
    else_bb = LLVMGetInsertBlock(curr_builder);
    // merge
    LLVMAppendExistingBasicBlock(func, merge_bb);
    LLVMPositionBuilderAtEnd(curr_builder, merge_bb);
    return new PHIValue(then_bb, then_v, else_bb, else_v);
  }
};
/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the
/// number of arguments the function takes).
class PrototypeAST {
public:
  char **arg_names;
  unsigned int *arg_name_lengths;
  Type **arg_types;
  FunctionType *type;
  unsigned int arg_count;
  char *name;
  unsigned int name_len;
  PrototypeAST(char *name, unsigned int name_len, char **arg_names,
               unsigned int *arg_name_lengths, Type **arg_types,
               unsigned int arg_count, Type *return_type, bool vararg)
      : name(name), name_len(name_len), arg_names(arg_names),
        arg_name_lengths(arg_name_lengths), arg_types(arg_types),
        arg_count(arg_count) {
    for (unsigned i = 0; i != arg_count; ++i)
      curr_named_var_types[std::string(arg_names[i], arg_name_lengths[i])] =
          arg_types[i];
    curr_named_var_types[std::string(name, name_len)] = type =
        new FunctionType(return_type, arg_types, arg_count, vararg);
  }
  PrototypeAST(Type *this_type, char *name, unsigned int name_len,
               char **arg_names, unsigned int *arg_name_lengths,
               Type **arg_types, unsigned int arg_count, Type *return_type,
               bool vararg) {
    CompleteExtensionName cen =
        get_complete_extension_name(this_type, name, name_len);
    arg_count++;
    arg_names = realloc_arr<char *>(arg_names, arg_count);
    arg_names[arg_count - 1] = strdup("this");
    arg_name_lengths = realloc_arr<unsigned int>(arg_name_lengths, arg_count);
    arg_name_lengths[arg_count - 1] = 4;
    arg_types = realloc_arr<Type *>(arg_types, arg_count);
    arg_types[arg_count - 1] = this_type;
    new (this) PrototypeAST(cen.str, cen.len, arg_names, arg_name_lengths,
                            arg_types, arg_count, return_type, vararg);
  }
  FunctionType *get_type() { return type; }
  LLVMValueRef codegen() {
    LLVMValueRef func = LLVMAddFunction(curr_module, name, type->llvm_type());
    curr_named_variables[std::string(name, name_len)] =
        new ConstValue(type, func, func);
    // Set names for all arguments.
    LLVMValueRef *params = alloc_arr<LLVMValueRef>(arg_count);
    LLVMGetParams(func, params);
    for (unsigned i = 0; i != arg_count; ++i)
      LLVMSetValueName2(params[i], arg_names[i], arg_name_lengths[i]);

    LLVMSetValueName2(func, name, name_len);
    LLVMPositionBuilderAtEnd(curr_builder, LLVMGetFirstBasicBlock(func));
    return func;
  }
};
/// DeclareExprAST - Expression class for defining an declare.
class DeclareExprAST : public TopLevelAST {
  LetExprAST *let = nullptr;
  PrototypeAST *prot = nullptr;
  Type *type;

public:
  DeclareExprAST(LetExprAST *let) : let(let) {
    type = let->get_type();
    register_declare();
  }
  DeclareExprAST(PrototypeAST *prot) : prot(prot) {
    type = prot->get_type();
    register_declare();
  }
  void register_declare() {
    if (FunctionType *fun_t = dynamic_cast<FunctionType *>(type))
      curr_named_var_types[std::string(prot->name, prot->name_len)] = fun_t;
    else
      curr_named_var_types[std::string(let->id, let->id_len)] = type;
  }
  void gen_toplevel() {
    register_declare();
    if (let)
      let->gen_declare();
    else
      prot->codegen();
  }
};

/// FunctionAST - This class represents a function definition itself.
class FunctionAST : public TopLevelAST {
  PrototypeAST *proto;
  ExprAST *body;

public:
  FunctionAST(PrototypeAST *proto, ExprAST *body) {
    if (proto->type->return_type == nullptr)
      proto->type->return_type = body->get_type();
    this->proto = proto;
    this->body = body;
  }

  void gen_toplevel() {
    // First, check for an existing function from a previous 'declare'
    // declaration.
    LLVMValueRef func = LLVMGetNamedFunction(curr_module, proto->name);

    if (!func)
      func = proto->codegen();

    if (!func)
      error("funcless behavior");

    if (LLVMCountBasicBlocks(func) != 0)
      error("Function cannot be redefined.");

    auto block = LLVMAppendBasicBlockInContext(curr_ctx, func, "");
    LLVMPositionBuilderAtEnd(curr_builder, block);

    unsigned int args_len = LLVMCountParams(func);
    LLVMValueRef *params = alloc_arr<LLVMValueRef>(args_len);
    LLVMGetParams(func, params);
    size_t unused = 0;
    for (unsigned i = 0; i != args_len; ++i)
      curr_named_variables[LLVMGetValueName2(params[i], &unused)] =
          new ConstValue(proto->arg_types[i], params[i], nullptr);
    Value *ret_val = body->gen_value()->cast_to(proto->type->return_type);
    // Finish off the function.
    LLVMBuildRet(curr_builder, ret_val->gen_load());
    // doesnt exist in c api (i think)
    // // Validate the generated code, checking for consistency.
    // // verifyFunction(*TheFunction);
  }
};

class StructAST : public TopLevelAST {
  char *name;
  unsigned int name_len;
  char **names;
  unsigned int *name_lengths;
  Type **types;
  unsigned int count;

public:
  StructAST(char *name, unsigned int name_len, char **names,
            unsigned int *name_lengths, Type **types, unsigned int count)
      : name(name), name_len(name_len), names(names),
        name_lengths(name_lengths), types(types), count(count) {}
  void gen_toplevel() {
    curr_named_types[std::string(name, name_len)] =
        new StructType(name, name_len, names, name_lengths, types, count);
  }
};

class TypeDefAST : public TopLevelAST {
  char *name;
  unsigned int name_len;
  Type *type;

public:
  TypeDefAST(char *name, unsigned int name_len, Type *type)
      : name(name), name_len(name_len), type(type) {}
  void gen_toplevel() { curr_named_types[std::string(name, name_len)] = type; }
};