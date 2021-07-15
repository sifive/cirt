//===- circt-clang.cpp : convert C code into MLIR SCF dialect
//--------------------------===//
// (c) Copyright 2020 Xilinx Inc. All Rights Reserved.

#include "clang/AST/ASTConsumer.h"
#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/Stmt.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"

#include <cassert>
#include <execinfo.h>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

using namespace clang;

typedef std::vector<int> int_vec;
typedef std::vector<std::string> str_vec;

// -------------------------------------------
// This tool emits the MLIR code from C code by traversing the Clang AST.
// -------------------------------------------

// Default mode configuration
// Disable full flow check - enabling debug mode will lower the MLIR through
// SCF, standard, handshake till HPX Ops.
bool debug = false;
// Output file name
std::string output = "output.mlir";

// Output file stream
std::ofstream mlirOut;
// Number of variables in the MLIR code including intermediate variables
int varCnt = 0;
// Number of intermediate variables
int unnameVarCnt = 0;
// Unary variables to be updated after statement
int_vec unaryUpdateList;

// Print error messages
auto error = [](auto str) {
  std::cout << "\033[0;31mError: " << str << "\033[0m" << std::endl;
  assert(0);
};

// Print highlight messages, like warning
auto message = [](auto str) {
  std::cout << "\033[0;33mWarning: " << str << "\033[0m" << std::endl;
};

// Translation unit
class MLIRGeneratorVisitor : public RecursiveASTVisitor<MLIRGeneratorVisitor> {
public:
  explicit MLIRGeneratorVisitor(ASTContext *Context) : Context(Context) {}

  virtual bool VisitFunctionDecl(FunctionDecl *func) {
    // Currently ignore the function prototype
    if (func->hasBody())
      funcGen(func);
    return true;
  }

private:
  struct variable {
    int ID;
    // Original variable name in string
    std::string name;
    // Variable Declaration
    ValueDecl *VarDecl;
    QualType type;
  };
  typedef std::vector<variable *> var_vec;

  ASTContext *Context;
  // Variable list
  var_vec vars;

  bool isIndex(QualType type) { return type == QualType(); }
  QualType getIndexType(void) { return QualType(); }

  // Extract the element type for an array
  QualType getArrayElementType(const ConstantArrayType *Type) {
    auto arrayType = dyn_cast<clang::ConstantArrayType>(Type);
    auto elementType = arrayType;
    // Extract all the dimensions
    do {
      elementType = arrayType;
      arrayType = dyn_cast<clang::ConstantArrayType>(
          arrayType->getArrayElementTypeNoTypeQual());
    } while (arrayType);
    return elementType->getArrayElementTypeNoTypeQual()
        ->getLocallyUnqualifiedSingleStepDesugaredType();
  }

  // Extract a list of indices for an array access
  void getArrayIdxExpr(ArraySubscriptExpr *arrayExpr, int_vec *idxList,
                       std::string *exprBuff) {
    auto expr = arrayExpr;
    do {
      auto idxExpr = expr->getIdx();
      assert(idxExpr);
      auto idx = castToIndex(exprGen(idxExpr, -1, exprBuff), exprBuff);
      idxList->push_back(idx);
      expr = dyn_cast<ArraySubscriptExpr>(
          dyn_cast<ImplicitCastExpr>(expr->getBase())->getSubExpr());
    } while (expr);
  }

  // Extract the variable ID of the array label
  int getArrayLabel(ArraySubscriptExpr *expr) {
    auto subExpr = dyn_cast<ImplicitCastExpr>(expr->getBase())->getSubExpr();
    while (auto arrayExpr = dyn_cast<ArraySubscriptExpr>(subExpr))
      subExpr = dyn_cast<ImplicitCastExpr>(arrayExpr->getBase())->getSubExpr();
    assert(subExpr);
    auto declRefExpr = dyn_cast<DeclRefExpr>(subExpr);
    assert(declRefExpr && "Unsupported array label format");
    auto label = dyn_cast<VarDecl>(declRefExpr->getDecl());
    assert(label && "Unsupported array label format");
    return getVarID(label);
  }

  // Print the variable type to equivalent MLIR representation in string
  std::string typePrint(QualType Type) {
    // Customised type - index
    if (isIndex(Type))
      return "index";
    else if (Type->isBooleanType())
      return "i1";
    else if (Type->isIntegerType())
      return "i" + std::to_string(Context->getTypeSize(Type));
    else if (Type->isFloatingType())
      return "f64";
    else if (Type->isPointerType())
      return typePrint(Type->getPointeeType());
    else if (Type->isConstantArrayType()) {
      std::string arrayDimension = "memref<";
      auto arrayType = dyn_cast<clang::ConstantArrayType>(Type);
      auto elementType = getArrayElementType(arrayType);
      // Extract all the dimensions
      do {
        arrayDimension +=
            std::to_string(arrayType->getSize().getSExtValue()) + "x";
        arrayType = dyn_cast<clang::ConstantArrayType>(
            arrayType->getArrayElementTypeNoTypeQual());
      } while (arrayType);
      arrayDimension += typePrint(elementType) + ">";
      return arrayDimension;
    } else if (!Type->isVoidType())
      error("Undefined type found in typePrint: " +
            Type->getPointeeType().getAsString());
    return "";
  }
  std::string typePrint(int i) { return typePrint(getVarType(i)); }

  // Return the name of the variable as string
  std::string getVarName(int i) {
    if (i < 0 || i >= varCnt)
      error("getVarName: Invalid var ID: " + std::to_string(i));
    return vars[i]->name;
  }

  // Return the ID of the variable
  int getVarID(ValueDecl *decl) {
    assert(decl && "Unknown variable referenced");
    for (auto &var : vars) {
      if (var->VarDecl && var->VarDecl == decl)
        return var->ID;
    }
    error("Undefined variable: " + decl->getNameAsString());
    return -1;
  }

  // Check whether a variable has been declared at the check point
  bool hasBeenDeclared(ValueDecl *decl) {
    for (auto &var : vars) {
      if (var->VarDecl == decl)
        return true;
    }
    return false;
  }

  // Return the type of the variable
  QualType getVarType(int i) {
    if (i < 0 || i >= varCnt)
      error("getVarType: Invalid var ID: " + std::to_string(i));
    return vars[i]->type;
  }

  // Set the name of a variable to the given string
  void setVarName(int i, std::string name) {
    if (i < 0 || i >= varCnt)
      error("Invalid var ID: " + std::to_string(i));
    vars[i]->name = name;
  }

  // Increment variable count for a named variable
  std::string incrName(std::string name) {
    int strLoc = name.rfind(".");
    return (strLoc != -1)
               ? name.substr(0, strLoc + 1) +
                     std::to_string(1 + std::stoi(name.substr(strLoc + 1)))
               : name + ".0";
  }
  // Create a copy of the variable for the updated value
  std::string incrVarName(int i) {
    assert(i >= 0 && i < varCnt);
    auto name = vars[i]->name;
    if (vars[i]->VarDecl == nullptr)
      message("May have an unnessary operation on " + name);
    auto newName = incrName(name);
    setVarName(i, newName);
    return newName;
  }
  std::string incrVarName(ValueDecl *decl) {
    return incrVarName(getVarID(decl));
  }

  // Add a Variable to the variable list
  int addVar(ValueDecl *decl, QualType type) {
    variable *var = new variable;
    var->ID = varCnt;
    var->VarDecl = decl;
    if (decl == nullptr) {
      var->name = "%" + std::to_string(unnameVarCnt);
      unnameVarCnt++;
    } else
      var->name = "%" + decl->getNameAsString();
    var->type = type;
    vars.push_back(var);
    varCnt++;
    return var->ID;
  }
  int addVar(ValueDecl *decl) {
    assert(decl != nullptr);
    return addVar(decl, decl->getType());
  }

  // Generate a constant value
  int constIntGen(int value, QualType ty, std::string *buff) {
    assert((isIndex(ty) || ty->isIntegerType()) &&
           "Generating a const int as a non-int type");
    auto tmp = addVar(nullptr, ty);
    *buff += getVarName(tmp) + " = constant " + std::to_string(value) + " : " +
             typePrint(ty) + "\n";
    return tmp;
  }

  // Print the binary operator in MLIR
  std::string binaryOpPrint(BinaryOperator *BinaryOp, QualType typeIn) {
    // typeIn == nullptr is Index
    std::string isInt = (isIndex(typeIn) || typeIn->isIntegerType())
                            ? "i"
                            : ((typeIn->isFloatingType()) ? "f" : "x");
    assert(isInt != "x");
    std::string sign = (isIndex(typeIn) || typeIn->isSignedIntegerType())
                           ? "s"
                           : (typeIn->isUnsignedIntegerType() ? "u" : "x");
    std::string signLong =
        (isIndex(typeIn) || typeIn->isSignedIntegerType())
            ? "_signed"
            : (typeIn->isUnsignedIntegerType() ? "_unsigned" : "x");

    switch (
        BinaryOp
            ->getOpcode()) { // jc:
                             // https://github.com/mull-project/mull/blob/3c1ad3b8ca428b816a3e3e5bd36568afd4090260/lib/JunkDetection/CXX/CXXJunkDetector.cpp
    case clang::BinaryOperator::Opcode::BO_Add:
      return "add" + isInt;
      break;
    case clang::BinaryOperator::Opcode::BO_AddAssign:
      return "add" + isInt;
      break;
    case clang::BinaryOperator::Opcode::BO_Rem:
      return (typeIn->isIntegerType()) ? "remi" + signLong : "remf";
      break;
    case clang::BinaryOperator::Opcode::BO_Sub:
      return "sub" + isInt;
      break;
    case clang::BinaryOperator::Opcode::BO_SubAssign:
      return "sub" + isInt;
      break;
    case clang::BinaryOperator::Opcode::BO_Mul:
      return "mul" + isInt;
      break;
    case clang::BinaryOperator::Opcode::BO_MulAssign:
      return "mul" + isInt;
      break;
    case clang::BinaryOperator::Opcode::BO_Div:
      return (typeIn->isIntegerType()) ? "divi" + signLong : "divf";
      break;
    case clang::BinaryOperator::Opcode::BO_DivAssign:
      return (typeIn->isIntegerType()) ? "divi" + signLong : "divf";
      break;
    case clang::BinaryOperator::Opcode::BO_Or:
      assert(typeIn->isIntegerType());
      return "or";
      break;
    case clang::BinaryOperator::Opcode::BO_LOr:
      assert(typeIn->isIntegerType());
      return "or";
      break;
    case clang::BinaryOperator::Opcode::BO_And:
      assert(typeIn->isIntegerType());
      return "and";
      break;
    case clang::BinaryOperator::Opcode::BO_LAnd:
      assert(typeIn->isIntegerType());
      return "and";
      break;
    case clang::BinaryOperator::Opcode::BO_LT:
      return "cmp" + isInt + " \"" + sign + "lt\",";
      break;
    case clang::BinaryOperator::Opcode::BO_LE:
      return "cmp" + isInt + " \"" + sign + "le\",";
      break;
    case clang::BinaryOperator::Opcode::BO_GT:
      return "cmp" + isInt + " \"" + sign + "gt\",";
      break;
    case clang::BinaryOperator::Opcode::BO_GE:
      return "cmp" + isInt + " \"" + sign + "ge\",";
      break;
    case clang::BinaryOperator::Opcode::BO_EQ:
      return "cmp" + isInt + " \"eq\",";
      break;
    case clang::BinaryOperator::Opcode::BO_NE:
      return "cmp" + isInt + " \"ne\",";
      break;
    case clang::BinaryOperator::Opcode::BO_Xor:
      return "xor";
      break;
    case clang::BinaryOperator::Opcode::BO_XorAssign:
      return "xor";
      break;
    case clang::BinaryOperator::Opcode::BO_Shr:
      return (typeIn->isSignedIntegerType()) ? "shift_right_signed"
                                             : "shift_right_unsigned";
      break;
    default:
      error("Undefined binaryOperator in binaryOpPrint: " +
            std::string(BinaryOp->getOpcodeStr()));
    }
    return "";
  }

  // Generate the binary expression from Clang AST
  int binaryOperatorGen(clang::Stmt *Stmt, int resultVar,
                        std::string *exprBuff) {
    auto BinaryOp = dyn_cast<BinaryOperator>(Stmt);
    auto result = -1;
    auto operand = Stmt->child_begin();

    if (BinaryOp->getOpcodeStr() == "=") {
      if (operand->getStmtClass() == Stmt::StmtClass::DeclRefExprClass) {
        // Assign to a register
        auto a = dyn_cast<DeclRefExpr>(*operand)->getDecl();
        result =
            exprGen(*(++operand), getVarID(dyn_cast<VarDecl>(a)), exprBuff);
      } else if (operand->getStmtClass() ==
                 Stmt::StmtClass::ArraySubscriptExprClass) {
        auto arrayElement = dyn_cast<ArraySubscriptExpr>(*operand);
        result = exprGen(*(++operand), -1, exprBuff);
        assert(arrayElement);
        assert(result != -1);
        storeGen(result, arrayElement, exprBuff);
      } else
        error("Unknown assignment in binaryOperatorGen");
    } else { // An intermediate binary expression
      int var0, var1;
      var0 = exprGen(*operand, -1, exprBuff);
      var1 = exprGen(*(++operand), -1, exprBuff);
      auto typeIn = getVarType(var0);
      if (typeIn != getVarType(var1))
        typeIn = typeCast(&var0, &var1, exprBuff);
      auto op = binaryOpPrint(BinaryOp, typeIn);
      if (resultVar != -1) {
        auto buff = " = " + op + " " + getVarName(var0) + ", " +
                    getVarName(var1) + " : " + typePrint(typeIn) + "\n";
        *exprBuff += incrVarName(resultVar) + buff;
        result = resultVar;
      } else {
        result = addVar(nullptr, BinaryOp->getType());
        *exprBuff += getVarName(result) + " = " + op + " " + getVarName(var0) +
                     ", " + getVarName(var1) + " : " + typePrint(typeIn) + "\n";
      }
      assert(!BinaryOp->isComparisonOp() ||
             getVarType(result)->isBooleanType());
    }
    return result;
  }

  // Return the precision of the type, which can be used to decide which
  // variable needs to be cast
  int getTypePrecision(QualType Type) {
    if (isIndex(Type))
      return 0; // least precision
    else if (Type->isIntegerType())
      return 1;
    else if (Type->isFloatingType())
      return 2;
    else
      error("Undefined type found in getTypePrecision: " +
            Type->getPointeeType().getAsString());
    return -1;
  }

  // Cast a variable from a type to another
  QualType typeCast(int *var0, int *var1, std::string *exprBuff) {
    auto v0 = *var0;
    auto v1 = *var1;
    auto ty0 = getVarType(v0);
    auto ty1 = getVarType(v1);
    auto p0 = getTypePrecision(ty0);
    auto p1 = getTypePrecision(ty1);

    if (p0 < p1) {
      *var0 = castTo(v0, p0 * 10 + p1, ty1, exprBuff);
      return ty1;
    } else {
      *var1 = castTo(v1, p1 * 10 + p0, ty0, exprBuff);
      return ty0;
    }
  }

  // Cast type to the high-precision type based on the given mode
  int castTo(int var, int mode, QualType ty, std::string *exprBuff) {
    int res = -1;
    switch (mode) {
    case 1: // index2integer
      res = addVar(nullptr, ty);
      *exprBuff += getVarName(res) + " = index_cast " + getVarName(var) +
                   " : index to i" + std::to_string(Context->getTypeSize(ty)) +
                   "\n";
      return res;
      break;
    case 10: // integer2index
      res = addVar(nullptr, ty);
      *exprBuff +=
          getVarName(res) + " = index_cast " + getVarName(var) + " : i" +
          std::to_string(Context->getTypeSize(getVarType(var))).c_str() +
          " to index\n";
      return res;
      break;
    case 2: // index2float
      error("Undefined type conversion in castTo: " + std::to_string(2));
      break;
    case 12: // int2float
      error("Undefined type conversion in castTo: " + std::to_string(12));
      break;
    case 31: // bool2int
      res = addVar(nullptr, ty);
      *exprBuff += getVarName(res) + " = sexti " + getVarName(var) +
                   " : i1 to i" + std::to_string(Context->getTypeSize(ty)) +
                   "\n";
      break;
    case 13: // int2bool
      res = addVar(nullptr, ty);
      *exprBuff +=
          getVarName(res) + " = trunci " + getVarName(var) + " : i" +
          +std::to_string(Context->getTypeSize(getVarType(var))).c_str() +
          " to i1\n";
      break;
    case 41: // short2int
      res = addVar(nullptr, ty);
      *exprBuff +=
          getVarName(res) + " = sexti " + getVarName(var) + " : i" +
          +std::to_string(Context->getTypeSize(getVarType(var))).c_str() +
          " to i" + std::to_string(Context->getTypeSize(ty)) + "\n";
      break;
    case 14: // int2short
      res = addVar(nullptr, ty);
      *exprBuff +=
          getVarName(res) + " = trunci " + getVarName(var) + " : i" +
          +std::to_string(Context->getTypeSize(getVarType(var))).c_str() +
          " to i" + std::to_string(Context->getTypeSize(ty)) + "\n";
      break;
    default:
      error("Undefined type conversion in castTo: " + std::to_string(mode));
    }
    return res;
  }

  // Generate storeOp in MLIR
  std::string addStore(int src, int dst, int_vec *index) {
    assert(index->size() > 0 && "No index for store op");

    std::string buffer;
    auto arrayType = getVarType(dst);
    std::string buff, args;
    for (auto &i : *index)
      args += getVarName(castToIndex(i, &buff)) + ", ";
    args.pop_back();
    args.pop_back();
    args += "] : " + typePrint(arrayType) + "\n";
    buff += "store " + getVarName(src) + ", " + getVarName(dst) + "[" + args;
    return buff;
  }

  // Generate loadOp in MLIR
  int addLoad(int array, int resultVar, std::string *buff, int_vec *index) {
    assert(index->size() > 0 && "No index for load op");
    auto type = getVarType(array);
    auto arrayType = dyn_cast<clang::ConstantArrayType>(type);
    assert(arrayType);
    auto elementType = getArrayElementType(arrayType);
    int res;
    std::string resName;
    if (resultVar == -1) {
      res = addVar(nullptr, elementType);
      resName = getVarName(res);
    } else {
      res = resultVar;
      resName = incrVarName(res);
    }
    std::string args = resName + " = load " + getVarName(array) + "[";
    for (auto &i : *index)
      args += getVarName(castToIndex(i, buff)) + ", ";
    args.pop_back();
    args.pop_back();
    args += "] : " + typePrint(type) + "\n";
    *buff += args;
    return res;
  }

  // Generate store op
  void storeGen(int resultVar, ArraySubscriptExpr *expr,
                std::string *exprBuff) {
    int_vec idxList;
    getArrayIdxExpr(expr, &idxList, exprBuff);
    auto array = getArrayLabel(expr);
    *exprBuff += addStore(resultVar, array, &idxList);
  }

  // Generate load op
  int loadGen(ArraySubscriptExpr *expr, int resultVar, std::string *exprBuff) {
    int_vec idxList;
    getArrayIdxExpr(expr, &idxList, exprBuff);
    auto array = getArrayLabel(expr);
    int res = addLoad(array, resultVar, exprBuff, &idxList);
    return res;
  }

  // Generate unary expression
  int unaryExprGen(clang::Stmt *Stmt, int resultVar, std::string *exprBuff) {
    UnaryOperator *unaryOp = dyn_cast<UnaryOperator>(Stmt);
    auto res = -1;
    auto operand = unaryOp->getSubExpr();
    auto var = exprGen(operand, -1, exprBuff);
    auto typeIn = getVarType(var);
    std::string buff;
    std::string isInt = (typeIn->isIntegerType())
                            ? "i"
                            : ((typeIn->isFloatingType()) ? "f" : "x");

    switch (unaryOp->getOpcode()) {
    case clang::UnaryOperator::Opcode::UO_PreInc:
      assert(typeIn->isIntegerType() &&
             "unary op \"++\" not supported for floating types");
      buff = " = addi " + getVarName(var) + ", " +
             getVarName(constIntGen(1, typeIn, exprBuff)) + " : " +
             typePrint(typeIn) + "\n";
      *exprBuff += incrVarName(var) + buff;
      if (resultVar == -1)
        res = var;
      else {
        res = addVar(nullptr, typeIn);
        // Directly assignment is not legal in mlir. Instead use +0 here
        *exprBuff += getVarName(res) + " = addi " + getVarName(var) + ", " +
                     getVarName(constIntGen(0, typeIn, exprBuff)) + " : " +
                     typePrint(typeIn) + "\n";
      }
      break;
    case clang::UnaryOperator::Opcode::UO_PostInc:
      assert(typeIn->isIntegerType() &&
             "unary op \"++\" not supported for floating types");
      buff = getVarName(var);
      *exprBuff += incrName(getVarName(var)) + " = addi " + buff + ", " +
                   getVarName(constIntGen(1, typeIn, exprBuff)) + " : " +
                   typePrint(typeIn) + "\n";
      assert(std::find(unaryUpdateList.begin(), unaryUpdateList.end(), var) ==
                 unaryUpdateList.end() &&
             "Multiple unary ops for one variable in a single statement is not "
             "supported");
      unaryUpdateList.push_back(var);
      res = var;
      break;
    case clang::UnaryOperator::Opcode::UO_PreDec:
      assert(typeIn->isIntegerType() &&
             "unary op \"--\" not supported for floating types");
      buff = " = subi " + getVarName(var) + ", " +
             getVarName(constIntGen(1, typeIn, exprBuff)) + " : " +
             typePrint(typeIn) + "\n";
      *exprBuff += incrVarName(var) + buff;
      if (resultVar == -1)
        res = var;
      else {
        res = addVar(nullptr, typeIn);
        // Directly assignment is not legal in mlir. Instead use +0 here
        *exprBuff += getVarName(res) + " = addi " + getVarName(var) + ", " +
                     getVarName(constIntGen(0, typeIn, exprBuff)) + " : " +
                     typePrint(typeIn) + "\n";
      }
      error("Unsupported UnaryOperator in unaryExprGen: " +
            std::string(unaryOp->getOpcodeStr(unaryOp->getOpcode())));
      break;
    case clang::UnaryOperator::Opcode::UO_PostDec:
      assert(typeIn->isIntegerType() &&
             "unary op \"--\" not supported for floating types");
      buff = getVarName(var);
      *exprBuff += incrName(getVarName(var)) + " = subi " + buff + ", " +
                   getVarName(constIntGen(1, typeIn, exprBuff)) + " : " +
                   typePrint(typeIn) + "\n";
      assert(std::find(unaryUpdateList.begin(), unaryUpdateList.end(), var) ==
                 unaryUpdateList.end() &&
             "Multiple unary ops for one variable in a single statement is not "
             "supported");
      unaryUpdateList.push_back(var);
      res = var;
      break;
    case clang::UnaryOperator::Opcode::UO_Not:
      error("Unsupported UnaryOperator in unaryExprGen: " +
            std::string(unaryOp->getOpcodeStr(unaryOp->getOpcode())));
      break;
    case clang::UnaryOperator::Opcode::UO_LNot:
      if (resultVar == -1) {
        res = addVar(nullptr, typeIn);
        *exprBuff += getVarName(res) + " = xor " +
                     getVarName(constIntGen(0, typeIn, exprBuff)) + ", " +
                     getVarName(var) + " : " + typePrint(typeIn) + "\n";
      } else {
        res = resultVar;
        buff = getVarName(var);
        *exprBuff += incrVarName(res) + " = xor " +
                     getVarName(constIntGen(0, typeIn, exprBuff)) + ", " +
                     buff + " : " + typePrint(typeIn) + "\n";
      }
      break;
    case clang::UnaryOperator::Opcode::UO_Minus:
      if (resultVar == -1) {
        res = addVar(nullptr, typeIn);
        *exprBuff += getVarName(res) + " = sub" + isInt + " " +
                     getVarName(constIntGen(0, typeIn, exprBuff)) + ", " +
                     getVarName(var) + " : " + typePrint(typeIn) + "\n";
      } else {
        res = resultVar;
        buff = getVarName(var);
        *exprBuff += incrVarName(res) + " = sub" + isInt + " " +
                     getVarName(constIntGen(0, typeIn, exprBuff)) + ", " +
                     buff + " : " + typePrint(typeIn) + "\n";
      }
      break;
    default:
      error("Undefined UnaryOperator in unaryExprGen: " +
            std::string(unaryOp->getOpcodeStr(unaryOp->getOpcode())));
    }
    return res;
  }

  // Cast an integer to index
  int castToIndex(int i, std::string *exprBuff) {
    if (!isIndex(getVarType(i))) {
      auto res = addVar(nullptr, getIndexType());
      *exprBuff += getVarName(res) + " = index_cast " + getVarName(i) + " : " +
                   typePrint(i) + " to index\n";
      return res;
    } else
      return i;
  }

  // Generate integer/floating literal expression
  int literalExprGen(Expr *expr, int result, std::string *exprBuff) {
    auto intLiteral = dyn_cast<IntegerLiteral>(expr);
    auto floatLiteral = dyn_cast<FloatingLiteral>(expr);

    llvm::SmallString<16> value;
    if (intLiteral)
      value = intLiteral->getValue().toString(10, true);
    else if (floatLiteral)
      floatLiteral->getValue().toString(value);
    else
      error("Unsupported literal type");

    if (result == -1) {
      auto rhsType = expr->getType();
      auto varID = addVar(nullptr, rhsType);
      *exprBuff += getVarName(varID) + " = constant " + std::string(value) +
                   " : " + typePrint(rhsType) + "\n";
      return varID;
    } else {
      *exprBuff += incrVarName(result) + " = constant " + std::string(value) +
                   " : " + typePrint(result) + "\n";
      return result;
    }
  }

  int findCastMode(QualType dst, QualType src) {
    auto dstType = typePrint(dst);
    auto srcType = typePrint(src);
    if (srcType[0] == 'i' && srcType != "index" &&
        dstType == "index") // integer2index
      return 10;
    else if (srcType == "index" && dstType != "index" &&
             dstType[0] == 'i') // index2integer
      return 1;
    else if (srcType == "i1" && dstType == "i32") // bool2int
      return 31;
    else if (srcType == "i32" && dstType == "i1") // int2bool
      return 13;
    else if (srcType == "i16" && dstType == "i32") // short2int
      return 41;
    else if (srcType == "i32" && dstType == "i16") // int2short
      return 14;
    else
      error("Unsupported cast : " + srcType + " -> " + dstType);
    return -1;
  }

  // Generate Implicit Cast Expression
  int implicitCastExprGen(ImplicitCastExpr *expr, int resultVar,
                          std::string *exprBuff) {
    auto result = exprGen(expr->getSubExpr(), resultVar, exprBuff);
    auto dstType = expr->getType();
    auto srcType = getVarType(result);
    if (typePrint(dstType) == typePrint(srcType))
      return result;
    else if (dstType->isPointerType())
      // Skip cast if there is an array operand (normally used by call op)
      return result;
    else {
      auto mode = findCastMode(dstType, srcType);
      return castTo(result, mode, dstType, exprBuff);
    }
  }

  // Generate conditional operator
  // TODO: use SCF.if?
  int conditionalOperatorGen(ConditionalOperator *expr, int resultVar,
                             std::string *exprBuff) {
    auto condExpr = exprGen(expr->getCond(), -1, exprBuff);
    auto trueExpr = exprGen(expr->getTrueExpr(), -1, exprBuff);
    auto falseExpr = exprGen(expr->getFalseExpr(), -1, exprBuff);
    auto ty = getVarType(trueExpr);
    assert(getVarType(falseExpr) == ty);
    assert(getVarType(condExpr)->isBooleanType());
    if (resultVar == -1) {
      auto result = addVar(nullptr, ty);
      *exprBuff += getVarName(result) + " = select " + getVarName(condExpr) +
                   ", " + getVarName(trueExpr) + ", " + getVarName(falseExpr) +
                   " : " + typePrint(ty) + "\n";
      return result;
    } else {
      *exprBuff += incrVarName(resultVar) + " = select " +
                   getVarName(condExpr) + ", " + getVarName(trueExpr) + ", " +
                   getVarName(falseExpr) + " : " + typePrint(ty) + "\n";
      return resultVar;
    }
  }

  // TODO: use SCF.if?
  int callExprGen(CallExpr *expr, int resultVar, std::string *exprBuff) {
    std::string varNames;
    for (auto arg : expr->arguments()) {
      // Skip casting the array operands
      auto a = exprGen((Stmt *)arg, -1, exprBuff);
      varNames += getVarName(a) + ", ";
    }
    varNames.pop_back();
    varNames.pop_back();

    auto func = expr->getDirectCallee();
    std::string types = "(";
    if (!func->param_empty()) {
      for (auto &parameter : func->parameters()) {
        auto type = parameter->getOriginalType();
        types += typePrint(type) + ", ";
      }
      types.pop_back();
      types.pop_back();
    }
    types += ") -> ";
    std::string returnType;
    if (!func->isNoReturn()) {
      returnType = typePrint(func->getDeclaredReturnType());
      types += "(" + returnType + ")";
    } else
      types += "()";

    if (returnType != "") {
      if (resultVar == -1) {
        auto result = addVar(nullptr, func->getDeclaredReturnType());
        *exprBuff += getVarName(result) + " = call @" +
                     func->getNameInfo().getName().getAsString() + "(" +
                     varNames + ") : " + types + "\n";
        return result;
      } else {
        *exprBuff += incrVarName(resultVar) + " = call @" +
                     func->getNameInfo().getName().getAsString() + "(" +
                     varNames + ") : " + types + "\n";
        return resultVar;
      }
    } else
      *exprBuff += "call @" + func->getNameInfo().getName().getAsString() +
                   "(" + varNames + ") : " + types + "\n";
    return resultVar;
  }

  // Generate a generic expression
  int exprGen(clang::Stmt *Stmt, int resultVar, std::string *exprBuff) {
    if (debug)
      std::cout << "Found stmt: " << Stmt->getStmtClassName() << "\n";
    auto Type = Stmt->getStmtClass();
    auto result = -1;
    switch (Type) {
    case Stmt::StmtClass::ImplicitCastExprClass:
      result = implicitCastExprGen(dyn_cast<ImplicitCastExpr>(Stmt), resultVar,
                                   exprBuff);
      break;
    case Stmt::StmtClass::ArraySubscriptExprClass:
      result = loadGen(dyn_cast<ArraySubscriptExpr>(Stmt), resultVar, exprBuff);
      break;
    case Stmt::StmtClass::BinaryOperatorClass:
      result = binaryOperatorGen(Stmt, resultVar, exprBuff);
      break;
    case Stmt::StmtClass::ParenExprClass:
      result =
          exprGen(dyn_cast<ParenExpr>(Stmt)->getSubExpr(), resultVar, exprBuff);
      break;
    case Stmt::StmtClass::UnaryOperatorClass:
      result = unaryExprGen(Stmt, resultVar, exprBuff);
      break;
    case Stmt::StmtClass::DeclRefExprClass:
      if (resultVar == -1)
        result =
            getVarID(dyn_cast<VarDecl>(dyn_cast<DeclRefExpr>(Stmt)->getDecl()));
      else {
        auto var =
            getVarID(dyn_cast<VarDecl>(dyn_cast<DeclRefExpr>(Stmt)->getDecl()));
        auto ty = getVarType(var);
        std::string isInt = (isIndex(ty) || ty->isIntegerType())
                                ? "i"
                                : ((ty->isFloatingType()) ? "f" : "x");
        assert(isInt != "x");
        auto name = getVarName(var);
        *exprBuff += incrVarName(resultVar) + " = add" + isInt + " " +
                     getVarName(constIntGen(0, ty, exprBuff)) + ", " + name +
                     " : " + typePrint(ty) + "\n";
        result = resultVar;
      }
      break;
    case Stmt::StmtClass::IntegerLiteralClass:
      result = literalExprGen(dyn_cast<Expr>(Stmt), resultVar, exprBuff);
      break;
    case Stmt::StmtClass::FloatingLiteralClass:
      result = literalExprGen(dyn_cast<Expr>(Stmt), resultVar, exprBuff);
      break;
    case Stmt::StmtClass::CStyleCastExprClass:
      result = exprGen(dyn_cast<CStyleCastExpr>(Stmt)->getSubExpr(), resultVar,
                       exprBuff);
      break;
    case Stmt::StmtClass::ConditionalOperatorClass:
      result = conditionalOperatorGen(dyn_cast<ConditionalOperator>(Stmt),
                                      resultVar, exprBuff);
      break;
    case Stmt::StmtClass::CallExprClass:
      result = callExprGen(dyn_cast<CallExpr>(Stmt), resultVar, exprBuff);
      break;
    default:
      error("Undefined expression in exprGen: " +
            std::string(Stmt->getStmtClassName()));
    }
    if (result == -1)
      error("Translation failed for " + std::string(Stmt->getStmtClassName()));
    return result;
  }

  // Extract the variable list for the yield op
  // Stmt - body of the statement
  // yields - list of variables to yeild
  void getYieldVars(clang::Stmt *Stmt, int_vec *yields) {
    if (Stmt) {
      if (auto forStmt = dyn_cast<ForStmt>(Stmt)) {
        getYieldVars(forStmt->getInit(), yields);
        getYieldVars(forStmt->getCond(), yields);
        getYieldVars(forStmt->getInc(), yields);
        getYieldVars(forStmt->getBody(), yields);
      } else if (auto ifStmt = dyn_cast<IfStmt>(Stmt)) {
        getYieldVars(ifStmt->getCond(), yields);
        getYieldVars(ifStmt->getThen(), yields);
        getYieldVars(ifStmt->getElse(), yields);
      } else {
        for (auto st : Stmt->children()) {
          if (st->getStmtClass() == Stmt::StmtClass::DeclRefExprClass) {
            // If a pre-defined variable is referred
            auto var = dyn_cast<VarDecl>(dyn_cast<DeclRefExpr>(st)->getDecl());
            if (!isa<ImplicitCastExpr>(Stmt) && hasBeenDeclared(var)) {
              // If it is a store, then add to the yield list
              auto varID = getVarID(var);
              if (std::find(yields->begin(), yields->end(), varID) ==
                  yields->end())
                yields->push_back(varID);
            }
          } else
            getYieldVars(st, yields);
        }
      }
    }
  }

  // Generate if statements
  void ifGen(IfStmt *Stmt, std::string *exprBuff) {
    Expr *cond = Stmt->getCond();
    std::string condition = getVarName(exprGen(
        &*cond, -1, exprBuff)); // JC: conditional variable not supported now
    // yield
    int_vec yieldsTrue, yieldsElse, yields;
    clang::Stmt *scfTrue = Stmt->getThen();
    if (scfTrue)
      getYieldVars(scfTrue, &yieldsTrue);
    clang::Stmt *scfFalse = Stmt->getElse();
    if (scfFalse)
      getYieldVars(scfFalse, &yieldsElse);

    bool toYield = (yieldsTrue.size() + yieldsElse.size() > 0);
    str_vec yieldsVars;
    std::string buffer;
    std::string yieldTypes;
    if (toYield) {
      // merge two yield lists
      yields = yieldsTrue;
      for (auto y : yieldsElse) {
        if (std::find(yields.begin(), yields.end(), y) == yields.end())
          yields.push_back(y);
      }
      // store var names in the parent region
      for (auto y : yields)
        yieldsVars.push_back(getVarName(y));
      // skip result vars to print if header
      buffer += " = scf.if " + condition + " -> (";
      for (auto y : yields)
        yieldTypes += typePrint(y) + ", ";
      yieldTypes.pop_back();
      yieldTypes.pop_back();
      buffer += yieldTypes + ") {\n";
    } else
      buffer += "scf.if " + condition + " {\n";
    if (scfTrue)
      stmtGen(scfTrue, &buffer);
    if (toYield) { // true branch
      buffer += "scf.yield ";
      for (auto y : yields)
        buffer += getVarName(y) + ", ";
      buffer.pop_back();
      buffer.pop_back();
      buffer += " : " + yieldTypes + "\n";
      for (unsigned long i = 0; i < yields.size(); i++)
        setVarName(yields[i], yieldsVars[i]);
    }
    buffer += "} else {\n";
    if (scfFalse)
      stmtGen(scfFalse, &buffer);
    if (toYield) { // false branch
      buffer += "scf.yield ";
      for (auto y : yields)
        buffer += getVarName(y) + ", ";
      buffer.pop_back();
      buffer.pop_back();
      buffer += " : " + yieldTypes + "\n";
      for (unsigned long i = 0; i < yields.size(); i++)
        setVarName(yields[i], yieldsVars[i]);
    }
    buffer += "}\n";
    std::string resultVars;
    if (toYield) { // now print the result variables
      for (auto y : yields)
        resultVars += incrVarName(y) + ", ";
      resultVars.pop_back();
      resultVars.pop_back();
    }
    *exprBuff += resultVars + buffer;
  }

  // Return whether the loop has jump
  bool hasJump(ForStmt *Stmt) {
    clang::Stmt *loopBody = Stmt->getBody();
    return hasGotoOrContinueOrBreakOrReturn(loopBody);
  };
  bool hasJump(WhileStmt *Stmt) {
    clang::Stmt *loopBody = Stmt->getBody();
    return hasGotoOrContinueOrBreakOrReturn(loopBody);
  };

  // Return whether the loop has jump
  bool hasGotoOrContinueOrBreakOrReturn(Stmt *Stmt) {
    if (Stmt)
      return 0;
    AsmStmt::StmtClass Type = Stmt->getStmtClass();
    ForStmt *forStmt;
    IfStmt *ifStmt;
    int s = 0;
    switch (
        Type) { // JC: class ref at
                // https://github.com/silent-silence/CodeRefactor/blob/56462375195d26d0620cc454cb56f72abab760bb/AST/ASTContext.h
    case Stmt::StmtClass::GotoStmtClass:
      return true;
    case Stmt::StmtClass::IndirectGotoStmtClass:
      return true;
    case Stmt::StmtClass::ContinueStmtClass:
      return true;
    case Stmt::StmtClass::BreakStmtClass:
      return true;
    case Stmt::StmtClass::ReturnStmtClass:
      return true;
    case Stmt::StmtClass::ForStmtClass:
      forStmt = dyn_cast<ForStmt>(Stmt);
      s += hasGotoOrContinueOrBreakOrReturn(forStmt->getInit());
      s += hasGotoOrContinueOrBreakOrReturn(forStmt->getCond());
      s += hasGotoOrContinueOrBreakOrReturn(forStmt->getInc());
      s += hasGotoOrContinueOrBreakOrReturn(forStmt->getBody());
      break;
    case Stmt::StmtClass::IfStmtClass:
      ifStmt = dyn_cast<IfStmt>(Stmt);
      s += hasGotoOrContinueOrBreakOrReturn(ifStmt->getCond());
      s += hasGotoOrContinueOrBreakOrReturn(ifStmt->getThen());
      s += hasGotoOrContinueOrBreakOrReturn(ifStmt->getElse());
      break;
    default:
      for (auto &st : Stmt->children())
        s += hasGotoOrContinueOrBreakOrReturn(st);
    }
    return (s > 0);
  }

  // Check whether a loop can be transformed into a SCF.for. If it can, generate
  // SCF.For op
  bool trySCFForGen(ForStmt *Stmt, std::string *exprBuff) {

    // Init check - check if the loop iterator is declared within the loop
    auto init = Stmt->getInit();
    auto initDecl = dyn_cast<DeclStmt>(init);
    if (!init || !initDecl->isSingleDecl())
      return false;
    auto indDecl = dyn_cast<VarDecl>(initDecl->getSingleDecl());
    assert(indDecl && indDecl->hasInit() && "Unsupported init for scf.for op");
    // indvar - loop iterator
    VarDecl *indVar = dyn_cast<VarDecl>(indDecl);

    // Step check - has step & step has to be a positive constant
    bool stepHold = false;
    auto inc = Stmt->getInc();
    UnaryOperator *unaryOp;
    CompoundAssignOperator *compAssign;
    int step;
    if (inc) {
      unaryOp = dyn_cast<UnaryOperator>(inc);
      compAssign = dyn_cast<CompoundAssignOperator>(inc);
      // step has to be in the forms like i++ or i+=c
      if (unaryOp) {
        auto varDecl = dyn_cast<DeclRefExpr>(unaryOp->getSubExpr());
        if (varDecl) {
          if (dyn_cast<VarDecl>(varDecl->getDecl()) == indVar) {
            if (unaryOp->isIncrementOp()) {
              stepHold = true;
              step = 1;
            }
          }
        }
      } else if (compAssign) {
        auto varDecl = dyn_cast<DeclRefExpr>(compAssign->getLHS());
        if (varDecl) {
          if (dyn_cast<VarDecl>(varDecl->getDecl()) == indVar) {
            // Floating point iterator not supported yet
            auto isStepConstant =
                dyn_cast<IntegerLiteral>(compAssign->getRHS());
            if (isStepConstant) {
              if (isStepConstant->getValue().isStrictlyPositive()) {
                stepHold = true;
                step = isStepConstant->getValue().getZExtValue();
              }
            }
          }
        }
      }
    }
    if (!stepHold)
      return false;

    // Condition check - has condition & inequality check
    bool condHold = false;
    bool equality = false;
    auto isIterLHS = 0;
    auto cond = Stmt->getCond();
    BinaryOperator *binaryCond;
    clang::Stmt *hbExpr;
    if (cond) {
      binaryCond = dyn_cast<BinaryOperator>(cond);
      auto opcode = binaryCond->getOpcode();
      // loop iterator should be seen in the loop condition
      isIterLHS = (opcode == clang::BinaryOperator::Opcode::BO_LT ||
                   opcode == clang::BinaryOperator::Opcode::BO_LE)
                      ? 1
                      : (opcode == clang::BinaryOperator::Opcode::BO_GT ||
                         opcode == clang::BinaryOperator::Opcode::BO_GE)
                            ? 2
                            : 0;
      if (binaryCond && isIterLHS) {
        equality = (opcode == clang::BinaryOperator::Opcode::BO_GT ||
                    opcode == clang::BinaryOperator::Opcode::BO_LT)
                       ? false
                       : true;
        clang::Stmt *iter;
        if (isIterLHS > 1) {
          iter = binaryCond->getRHS();
          hbExpr = binaryCond->getLHS();
        } else {
          iter = binaryCond->getLHS();
          hbExpr = binaryCond->getRHS();
        }
        if (iter->getStmtClass() == Stmt::StmtClass::ImplicitCastExprClass &&
            dyn_cast<DeclRefExpr>(
                dyn_cast<ImplicitCastExpr>(iter)->getSubExpr())
                    ->getDecl() == indVar)
          condHold = true;
      }
    }
    if (!condHold)
      return false;

    // // Code generation
    // // Temporary store the variables in the current region
    // // TODO: This inefficient, to be optimized
    // str_vec varTemp;
    // for (auto &var : vars)
    //   varTemp.push_back(var->name);

    // SCF header generation - try to extract the four constraints
    // indvar - loop iterator
    // lb - low bound of the loop
    // hb - high bound of the loop
    // step - step of the loop
    std::string buffer;
    auto lb = exprGen(&*(indDecl->getInit()), -1, &buffer);
    auto indID = (initDecl) ? addVar(indVar, getIndexType())
                            : castToIndex(getVarID(indVar), exprBuff);
    auto hb = exprGen(&*hbExpr, -1, &buffer);
    auto indType = getVarType(indID);
    auto stepVar = constIntGen(step, indType, &buffer);

    // Generate yield variable list, i.e. get the set of modified variables
    int_vec yields;
    auto loopBody = Stmt->getBody();
    getYieldVars(loopBody, &yields);
    std::string argsBuff;
    if (yields.size() > 0) {
      for (auto &y : yields) {
        auto name = getVarName(y);
        argsBuff += incrVarName(y) + " = " + name + ", ";
      }
      argsBuff.pop_back();
      argsBuff.pop_back();
    }

    // Generate loop body
    // TODO: loop iterator updated within the loop body?
    if (debug)
      std::cout << "Starting loop body generation..." << std::endl;
    std::string body;
    stmtGen(loopBody, &body);
    if (debug)
      std::cout << "Ended loop body generation..." << std::endl;

    // Generate yield op
    auto toYield = (yields.size() > 0);
    std::string header, yieldTypes;
    str_vec yieldsVars;
    if (toYield) {
      for (auto &y : yields) {
        yieldsVars.push_back(getVarName(y));
        yieldTypes += typePrint(y) + ", ";
      }
      yieldTypes.pop_back();
      yieldTypes.pop_back();
      body += "scf.yield ";
      for (auto y : yields)
        body += getVarName(y) + ", ";
      body.pop_back();
      body.pop_back();
      body += " : " + yieldTypes + "\n";
      for (unsigned long i = 0; i < yields.size(); i++)
        setVarName(yields[i], yieldsVars[i]);
      for (auto y : yields)
        header += incrVarName(y) + ", ";
      header.pop_back();
      header.pop_back();
      header += " = ";
    }
    auto lbIdx = castToIndex(lb, &buffer);
    auto hbIdx = castToIndex(hb, &buffer);
    auto stepIdx = castToIndex(stepVar, &buffer);
    *exprBuff += buffer;
    header += "scf.for " + getVarName(indID) + " = " + getVarName(lbIdx) +
              " to " + getVarName(hbIdx) + " step " + getVarName(stepIdx) +
              "\n";
    header += "iter_args(" + argsBuff + ") -> (" + yieldTypes + ") {\n";
    *exprBuff += header + body + "}\n";
    return true;
  }

  // Generate generic for loops without jumping as scf.while loops.
  // init is outside of the while loop, condition is the same, and body followed
  // by inc is the body of a while loop
  void generalForGen(ForStmt *Stmt, std::string *exprBuff) {
    auto init = Stmt->getInit();
    if (init)
      stmtGen(init, exprBuff);
    auto cond = Stmt->getCond();
    assert(cond);
    auto inc = Stmt->getInc();

    // Generate yield variable list, i.e. get the set of modified variables
    int_vec yields;
    auto loopBody = Stmt->getBody();
    getYieldVars(loopBody, &yields);
    getYieldVars(inc, &yields);
    std::string argsBuff, conditionArgs, yieldTypes;
    if (yields.size() > 0) {
      for (auto &y : yields) {
        auto name = getVarName(y);
        auto newName = incrVarName(y);
        yieldTypes += typePrint(y) + ", ";
        argsBuff += newName + " = " + name + ", ";
        conditionArgs += newName + ", ";
      }
      argsBuff.pop_back();
      argsBuff.pop_back();
      conditionArgs.pop_back();
      conditionArgs.pop_back();
      yieldTypes.pop_back();
      yieldTypes.pop_back();
    }
    auto header = "scf.while (" + argsBuff + ") : (" + yieldTypes + ") -> (" +
                  yieldTypes + ") {\n";
    auto condition = exprGen(cond, -1, &header);
    std::string bodyArgs;
    if (yields.size() > 0) {
      for (auto &y : yields)
        bodyArgs += incrVarName(y) + " : " + typePrint(y) + ", ";
      bodyArgs.pop_back();
      bodyArgs.pop_back();
    }
    header += "scf.condition(" + getVarName(condition) + ") " + conditionArgs +
              " : " + yieldTypes + "\n} do {\n^bb0(" + bodyArgs + "): \n";

    std::string body;
    stmtGen(loopBody, &body);
    stmtGen(inc, &body);

    // Generate yield op
    auto toYield = (yields.size() > 0);
    std::string results;
    str_vec yieldsVars;
    if (toYield) {
      for (auto y : yields)
        yieldsVars.push_back(getVarName(y));

      body += "scf.yield ";
      for (auto y : yields)
        body += getVarName(y) + ", ";
      body.pop_back();
      body.pop_back();
      body += " : " + yieldTypes + "\n";
      for (unsigned long i = 0; i < yields.size(); i++)
        setVarName(yields[i], yieldsVars[i]);
      for (auto y : yields)
        results += incrVarName(y) + ", ";
      results.pop_back();
      results.pop_back();
      results += " = ";
    }
    *exprBuff += results + header + body + "}\n";
  }

  // Generate for loops
  void forGen(ForStmt *Stmt, std::string *exprBuff) {
    if (!trySCFForGen(Stmt, exprBuff)) {
      if (!hasJump(Stmt)) {
        // Generate irregular for loops into while loops
        generalForGen(Stmt, exprBuff);
      } else
        error("Jumps in loops unsupported for now.");
    }
  }

  // Generate while loops
  void whileGen(WhileStmt *Stmt, std::string *exprBuff) {
    if (hasJump(Stmt))
      error("Jumps in loops unsupported for now.");

    auto cond = Stmt->getCond();
    assert(cond);
    // Generate yield variable list, i.e. get the set of modified variables
    int_vec yields;
    auto loopBody = Stmt->getBody();
    getYieldVars(loopBody, &yields);
    std::string argsBuff, conditionArgs, yieldTypes;
    if (yields.size() > 0) {
      for (auto &y : yields) {
        auto name = getVarName(y);
        auto newName = incrVarName(y);
        yieldTypes += typePrint(y) + ", ";
        argsBuff += newName + " = " + name + ", ";
        conditionArgs += newName + ", ";
      }
      argsBuff.pop_back();
      argsBuff.pop_back();
      conditionArgs.pop_back();
      conditionArgs.pop_back();
      yieldTypes.pop_back();
      yieldTypes.pop_back();
    }
    auto header = "scf.while (" + argsBuff + ") : (" + yieldTypes + ") -> (" +
                  yieldTypes + ") {\n";
    auto condition = exprGen(cond, -1, &header);
    std::string bodyArgs;
    if (yields.size() > 0) {
      for (auto &y : yields)
        bodyArgs += incrVarName(y) + " : " + typePrint(y) + ", ";
      bodyArgs.pop_back();
      bodyArgs.pop_back();
    }
    header += "scf.condition(" + getVarName(condition) + ") " + conditionArgs +
              " : " + yieldTypes + "\n} do {\n^bb0(" + bodyArgs + "): \n";

    std::string body;
    stmtGen(loopBody, &body);

    // Generate yield op
    auto toYield = (yields.size() > 0);
    std::string results;
    str_vec yieldsVars;
    if (toYield) {
      for (auto y : yields)
        yieldsVars.push_back(getVarName(y));

      body += "scf.yield ";
      for (auto y : yields)
        body += getVarName(y) + ", ";
      body.pop_back();
      body.pop_back();
      body += " : " + yieldTypes + "\n";
      for (unsigned long i = 0; i < yields.size(); i++)
        setVarName(yields[i], yieldsVars[i]);
      for (auto y : yields)
        results += incrVarName(y) + ", ";
      results.pop_back();
      results.pop_back();
      results += " = ";
    }
    *exprBuff += results + header + body + "}\n";
  }

  // Generate variable declarations
  void declStmtGen(DeclStmt *Stmt, std::string *exprBuff) {
    for (auto decl : Stmt->decls()) {
      if (VarDecl *temp = dyn_cast<VarDecl>(decl)) {
        auto var = addVar(&*temp);
        if (temp->getType()->isArrayType()) {
          assert(!temp->hasInit() && "Initialised arrays not supported");
          *exprBuff +=
              incrVarName(var) + " = alloc() : " + typePrint(var) + "\n";
        } else if (temp->hasInit())
          exprGen(&*(temp->getInit()), var, exprBuff);
        else
          // TODO: declaring unintialised varaibles are initialised to 0 in
          // MLIR (will be processed by an additional MLIR pass)
          *exprBuff +=
              incrVarName(var) + " = constant 0 : " + typePrint(var) + "\n";

      } else
        error("Undefined declaration found in stmtGen.");
    }
  }

  // Generate compound assignment
  void compoundAssignStmtGen(CompoundAssignOperator *compAssign,
                             std::string *stmtBuff) {
    auto rhs = exprGen(&*(compAssign->getRHS()), -1, stmtBuff);
    auto rhsType = getVarType(rhs);
    auto lhs = compAssign->getLHS();
    auto lhsAsArray = dyn_cast<ArraySubscriptExpr>(lhs);

    if (lhsAsArray) { // Is an array
      int_vec idxList;
      getArrayIdxExpr(lhsAsArray, &idxList, stmtBuff);
      auto array = getArrayLabel(lhsAsArray);
      auto lhsVar = addLoad(array, -1, stmtBuff, &idxList);
      auto lhsVarType = getVarType(lhsVar);
      assert(lhsVarType == rhsType &&
             "casting for compound assignment not supported");
      auto result = addVar(nullptr, lhsVarType);
      *stmtBuff += getVarName(result) + " = " +
                   binaryOpPrint(compAssign, rhsType) + " " +
                   getVarName(lhsVar) + ", " + getVarName(rhs) + " : " +
                   typePrint(rhsType) + "\n";
      *stmtBuff += addStore(result, array, &idxList);
    } else {
      auto lhsVar = dyn_cast<DeclRefExpr>(lhs);
      assert(lhsVar &&
             "Unsupproted destination format for compound assignment");
      auto result = getVarID(lhsVar->getDecl());
      assert(getVarType(result) == rhsType &&
             "casting for compound assignment not supported");
      auto buffer = " = " + binaryOpPrint(compAssign, rhsType) + " " +
                    getVarName(result) + ", " + getVarName(rhs) + " : " +
                    typePrint(rhsType) + "\n";
      *stmtBuff += incrVarName(result) + buffer;
    }
  }

  // Generate general statements
  void stmtGen(Stmt *Stmt, std::string *stmtBuff) {
    if (debug)
      std::cout << "Found stmt: " << Stmt->getStmtClassName() << "\n";
    std::string buffer = "";
    AsmStmt::StmtClass Type = Stmt->getStmtClass();
    int var;
    auto children = Stmt->children();
    assert(!unaryUpdateList.size());
    switch (Type) {
    case Stmt::StmtClass::BinaryOperatorClass:
      binaryOperatorGen(Stmt, -1, stmtBuff);
      break;
    case Stmt::StmtClass::DeclStmtClass:
      declStmtGen(dyn_cast<DeclStmt>(Stmt), stmtBuff);
      break;
    case Stmt::StmtClass::CompoundStmtClass:
      for (auto &st : children)
        stmtGen(st, stmtBuff);
      break;
    case Stmt::StmtClass::IfStmtClass:
      ifGen(dyn_cast<IfStmt>(Stmt), stmtBuff);
      break;
    case Stmt::StmtClass::ReturnStmtClass:
      assert(std::distance(cbegin(children), cend(children)) == 1);
      var = exprGen(*(Stmt->child_begin()), -1, stmtBuff);
      *stmtBuff += std::string("return " + getVarName(var) + " : " +
                               typePrint(var) + "\n");
      break;
    case Stmt::StmtClass::UnaryOperatorClass:
      unaryExprGen(Stmt, -1, stmtBuff);
      break;
    case Stmt::StmtClass::ForStmtClass:
      forGen(dyn_cast<ForStmt>(Stmt), stmtBuff);
      break;
    case Stmt::StmtClass::WhileStmtClass:
      whileGen(dyn_cast<WhileStmt>(Stmt), stmtBuff);
      break;
    case Stmt::StmtClass::CompoundAssignOperatorClass:
      compoundAssignStmtGen(dyn_cast<CompoundAssignOperator>(Stmt), stmtBuff);
      break;
    case Stmt::StmtClass::CallExprClass:
      callExprGen(dyn_cast<CallExpr>(Stmt), -1, stmtBuff);
      break;
    default:
      error("Undefined statement class in stmtGen: " +
            std::string(Stmt->getStmtClassName()));
    }
    if (unaryUpdateList.size() > 0) {
      for (auto &i : unaryUpdateList)
        incrVarName(i);
      unaryUpdateList.clear();
    }
  }

  // Generate functions
  void funcGen(FunctionDecl *func) {
    auto funcName = func->getNameInfo().getName().getAsString();
    if (debug)
      std::cout << "Found func: " << funcName << "\n";
    std::string funcBody;
    mlirOut << "func @" << funcName << "(";
    // Print function arguments
    if (!func->param_empty()) {
      std::string buffer;
      for (auto &parameter : func->parameters()) {
        auto type = parameter->getOriginalType();
        auto argName = getVarName(addVar(parameter, type));
        if (type->isConstantArrayType())
          // Fixed size array, e.g. int[10]
          buffer += argName + " : " + typePrint(type) + ", ";
        else if (type->isPointerType())
          // Pointer argument, e.g. int *
          error("Pointer argument not supported yet for function " +
                func->getNameInfo().getName().getAsString());
        else if (!type->isPointerType() && !type->isArrayType())
          // Normal variables, e.g. int, float
          buffer += getVarName(addVar(parameter, type)) + " : " +
                    typePrint(type) + ", ";
        else {
          std::cout << typePrint(type) << "\n";
          std::cout << buffer << "\n";
          error("Undefined argument type for function " +
                func->getNameInfo().getName().getAsString());
        }
      }
      mlirOut << buffer.substr(0, buffer.length() - 2) << ")";
    } else
      mlirOut << ")";
    // Print function return type
    bool appendReturn;
    if (!func->isNoReturn()) {
      auto returnType = typePrint((func->getDeclaredReturnType()));
      appendReturn = (returnType == "") ? true : false;
      mlirOut << " -> (" << returnType << ")";
    } else
      appendReturn = true;
    mlirOut << "{\n";
    stmtGen(func->getBody(), &funcBody);
    mlirOut << funcBody;
    // Add a return op if the return type is void
    if (appendReturn)
      mlirOut << "return\n";
    mlirOut << "}\n";
  }
};

// Consumer
class MLIRGeneratorConsumer : public clang::ASTConsumer {
public:
  explicit MLIRGeneratorConsumer(ASTContext *Context) : Visitor(Context) {}

  virtual void HandleTranslationUnit(ASTContext &Context) override {
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());
  }

private:
  MLIRGeneratorVisitor Visitor;
};

// Action
class MLIRGeneratorAction : public clang::ASTFrontendAction {
public:
  virtual std::unique_ptr<clang::ASTConsumer>
  CreateASTConsumer(clang::CompilerInstance &Compiler,
                    llvm::StringRef InFile) override {
    return std::unique_ptr<clang::ASTConsumer>(
        new MLIRGeneratorConsumer(&Compiler.getASTContext()));
  }
};

// Print help infomation before exit
void exit(void) {
  std::cout << "Usage: circt-clang [options] <source>" << std::endl;
  std::cout << "--debug\t\t\t\t- Enable debug mode to print debug information"
            << std::endl;
  std::cout << "-o <filename>\t\t\t- Specify the file name of the output MLIR"
            << std::endl;
}

// Tool configurations
// * --help display help infomation
// * --debug print additional debugging info
bool option(std::string op) {
  if (op == "--help")
    return false;
  else if (op == "--debug") {
    debug = true;
    return true;
  } else {
    std::cout << "\033[0;31mUndefined option " << op << "\033[0m" << std::endl;
    return false;
  }
}

int main(int argc, char **argv) {
  if (argc < 2) {
    exit();
    return -1;
  }

  // Detect configurations and input file
  std::string fileName;
  int i;
  for (i = 1; i < argc; i++) {
    if (argv[i][0] == '-' && argv[i][1] == '-') {
      if (!option(argv[i])) {
        exit();
        return -1;
      }
    } else if (std::string(argv[i]) == "-o" && i < argc - 1) {
      ++i;
      output = argv[i];
    } else if (fileName.empty())
      fileName = argv[i];
    else {
      std::cout << "\033[0;31mError: Unrecognised command.\033[0m\n";
      exit();
      return -1;
    }
  }
  std::ifstream fin(fileName);
  if (!fin.is_open()) {
    std::cout << "\033[0;31mError: Cannot find file " << fileName << "\033[0m"
              << std::endl;
    return -1;
  }

  // Process
  std::stringstream code;
  code << fin.rdbuf();
  if (debug) {
    std::cout << "C input: " << std::endl;
    std::cout << code.str() << std::endl;
  }
  mlirOut.open(output);
  if (debug)
    std::cout << "Debug log: " << std::endl;
  clang::tooling::runToolOnCode(std::make_unique<MLIRGeneratorAction>(),
                                code.str());
  mlirOut.close();
  if (debug)
    std::cout << "Translation finished." << std::endl;
  return 0;
}
