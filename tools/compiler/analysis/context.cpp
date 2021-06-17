#include <iostream>
#include "llvm/IR/Function.h"
#include "llvm/IR/Constant.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "zero/analysis/context.h"
#include "zero/symbol/symbol.h"

weasel::AnalysContext::AnalysContext(std::string moduleName)
{
    _context = new llvm::LLVMContext();
    _module = new llvm::Module(moduleName, *getContext());
    _builder = new llvm::IRBuilder<>(*getContext());
    _fpm = new llvm::legacy::FunctionPassManager(_module);

    // Do simple "peephole" optimizations and bit-twiddling optzns.
    _fpm->add(llvm::createInstructionCombiningPass());
    // Reassociate expressions.
    _fpm->add(llvm::createReassociatePass());
    // Eliminate Common SubExpressions.
    _fpm->add(llvm::createGVNPass());
    // Simplify the control flow graph (deleting unreachable blocks, etc).
    _fpm->add(llvm::createCFGSimplificationPass());
    // Initialize Function Pass
    _fpm->doInitialization();
}

std::string weasel::AnalysContext::getDefaultLabel()
{
    return std::to_string(_counter++);
}

llvm::Function *weasel::AnalysContext::codegen(weasel::Function *funAST)
{
    auto funName = funAST->getIdentifier();
    if (getModule()->getFunction(funName))
    {
        return nullptr;
    }

    auto isVararg = funAST->getFunctionType()->getIsVararg();
    auto funArgs = funAST->getArgs();
    auto retTy = funAST->getFunctionType()->getReturnType();
    auto args = std::vector<llvm::Type *>();
    auto argsLength = funArgs.size() - (isVararg ? 1 : 0);
    for (size_t i = 0; i < argsLength; i++)
    {
        args.push_back(funArgs[i]->getArgumentType());
    }

    auto *funTyLLVM = llvm::FunctionType::get(retTy, args, isVararg);
    auto *funLLVM = llvm::Function::Create(funTyLLVM, llvm::Function::LinkageTypes::ExternalLinkage, funName, *getModule());
    auto idx = 0;
    for (auto &item : funLLVM->args())
    {
        item.setName(funArgs[idx++]->getArgumentName());
    }

    // Add Function to symbol table
    {
        auto attr = std::make_shared<Attribute>(funName, AttributeScope::ScopeGlobal, AttributeKind::SymbolFunction, funLLVM);
        SymbolTable::getInstance().insert(funName, attr);
    }

    if (funAST->getIsDefine())
    {
        auto *entry = llvm::BasicBlock::Create(*getContext(), "", funLLVM);
        getBuilder()->SetInsertPoint(entry);

        // Add Parameter to symbol table
        {
            // Enter to parameter scope
            SymbolTable::getInstance().enterScope();

            for (auto &item : funLLVM->args())
            {
                auto refName = item.getName();
                auto paramName = std::string(refName.begin(), refName.end());
                auto *allocParam = getBuilder()->CreateAlloca(item.getType());

                getBuilder()->CreateStore(&item, allocParam);

                auto attr = std::make_shared<Attribute>(paramName, AttributeScope::ScopeParam, AttributeKind::SymbolParameter, allocParam);

                SymbolTable::getInstance().insert(paramName, attr);
            }
        }

        // Create Block
        if (funAST->getBody())
        {
            funAST->getBody()->codegen(this);

            if (funLLVM->getReturnType()->isVoidTy())
            {
                getBuilder()->CreateRetVoid();
            }
        }

        // Exit from parameter scope
        {
            auto exit = SymbolTable::getInstance().exitScope();
            if (!exit)
            {
                return nullptr;
            }
        }
    }

    return funLLVM;
}

llvm::Value *weasel::AnalysContext::codegen(StatementExpression *expr)
{
    std::cout << "Statements : " << expr->getBody().size() << "\n";

    // Enter to new statement
    {
        SymbolTable::getInstance().enterScope();
    }

    for (auto &item : expr->getBody())
    {
        item->codegen(this);
    }

    // Exit from statement
    {
        SymbolTable::getInstance().exitScope();
    }

    return nullptr;
}

llvm::Value *weasel::AnalysContext::codegen(CallExpression *expr)
{
    auto identifier = expr->getIdentifier();
    auto args = expr->getArguments();
    auto *fun = getModule()->getFunction(identifier);

    std::vector<llvm::Value *> argsV;
    for (size_t i = 0; i < args.size(); i++)
    {
        argsV.push_back(args[i]->codegen(this));
        if (!argsV.back())
        {
            return ErrorTable::addError(expr->getToken(), "Expected argument list index " + i);
        }
    }

    return getBuilder()->CreateCall(fun, argsV, fun->getReturnType()->isVoidTy() ? "" : identifier);
}

llvm::Value *weasel::AnalysContext::codegen(NumberLiteralExpression *expr)
{
    return getBuilder()->getInt32(expr->getValue());
}

llvm::Value *weasel::AnalysContext::codegen(StringLiteralExpression *expr)
{
    auto *str = getBuilder()->CreateGlobalString(expr->getValue());
    std::vector<llvm::Value *> idxList;
    idxList.push_back(getBuilder()->getInt8(0));
    idxList.push_back(getBuilder()->getInt8(0));

    return llvm::ConstantExpr::getGetElementPtr(str->getType()->getElementType(), str, idxList, true);
}

llvm::Value *weasel::AnalysContext::codegen(NilLiteralExpression *expr)
{
    return llvm::ConstantPointerNull::getNullValue(getBuilder()->getInt8PtrTy());
}

llvm::Value *weasel::AnalysContext::codegen(DeclarationExpression *expr)
{
    // Get Value Representation
    llvm::Value *value;
    llvm::Type *declTy;

    auto exprValue = expr->getValue();
    if (exprValue)
    {
        value = exprValue->codegen(this);
        declTy = expr->getType();

        if (value->getType()->getTypeID() == llvm::Type::TypeID::VoidTyID)
        {
            return ErrorTable::addError(exprValue->getToken(), "Cannot assign void to a variable");
        }

        if (value->getValueID() == llvm::Value::ConstantPointerNullVal && declTy)
        {
            value = llvm::ConstantPointerNull::getNullValue(declTy);
        }

        if (declTy)
        {
            auto compareTy = compareType(declTy, value->getType());
            if (compareTy == CompareType::Different)
            {
                return ErrorTable::addError(exprValue->getToken(), "Cannot assign, expression type is different");
            }

            if (compareTy == CompareType::Casting)
            {
                value = castIntegerType(value, declTy);
            }
        }
        else
        {
            declTy = value->getType();
        }
    }
    else
    {
        assert(expr->getType());

        declTy = expr->getType();
        if (declTy->isPointerTy())
        {
            value = llvm::ConstantPointerNull::get(llvm::PointerType::get(declTy->getPointerElementType(), declTy->getPointerAddressSpace()));
        }
        else
        {
            value = llvm::ConstantInt::get(declTy, 0);
        }
    }

    // Allocating Address for declaration
    auto varName = expr->getIdentifier();
    auto *alloc = getBuilder()->CreateAlloca(declTy, 0, varName);

    // Add Variable Declaration to symbol table
    {
        auto attr = std::make_shared<Attribute>(varName, AttributeScope::ScopeLocal, AttributeKind::SymbolVariable, alloc);

        SymbolTable::getInstance().insert(varName, attr);
    }

    return getBuilder()->CreateStore(value, alloc);
}

llvm::Value *weasel::AnalysContext::codegen(BinaryOperatorExpression *expr)
{
    auto token = expr->getOperator();
    auto *rhs = expr->getRHS()->codegen(this);
    auto *lhs = expr->getLHS()->codegen(this);
    auto compareTy = compareType(lhs->getType(), rhs->getType());

    if (compareTy == CompareType::Different)
    {
        return logErrorV(std::string("type LHS != type RHS"));
    }

    if (compareTy == CompareType::Casting)
    {
        castIntegerType(lhs, rhs);
    }

    switch (token->getTokenKind())
    {
    case TokenKind::TokenOperatorStar:
        return getBuilder()->CreateMul(lhs, rhs, lhs->getName());
    case TokenKind::TokenOperatorSlash:
        return getBuilder()->CreateSDiv(lhs, rhs, lhs->getName());
    // case TokenKind::TokenPuncPercent: return llvm::BinaryOperator::
    case TokenKind::TokenOperatorPlus:
        return getBuilder()->CreateAdd(lhs, rhs, lhs->getName());
    case TokenKind::TokenOperatorMinus:
        return getBuilder()->CreateSub(lhs, rhs, lhs->getName());
    case TokenKind::TokenOperatorEqual:
    {
        auto *loadLhs = llvm::dyn_cast<llvm::LoadInst>(lhs);
        if (!loadLhs)
        {
            return ErrorTable::addError(expr->getLHS()->getToken(), "LHS not valid");
        }

        auto *allocLhs = llvm::dyn_cast<llvm::AllocaInst>(loadLhs->getPointerOperand());
        if (!allocLhs)
        {
            return ErrorTable::addError(expr->getLHS()->getToken(), "LHS is not a valid address pointer");
        }

        getBuilder()->CreateStore(rhs, allocLhs);
        return getBuilder()->CreateLoad(allocLhs);
    }
    default:
        std::cout << "HELLO ERROR\n";
        return nullptr;
    }
}

llvm::Value *weasel::AnalysContext::codegen(ReturnExpression *expr)
{
    if (!expr->getValue())
    {
        return getBuilder()->CreateRetVoid();
    }

    auto *val = expr->getValue()->codegen(this);

    // Get Last Function from symbol table
    auto funAttr = SymbolTable::getInstance().getLastFunction();
    if (!funAttr)
    {
        return ErrorTable::addError(expr->getToken(), "Return Statement cannot find last function from symbol table");
    }
    auto *fun = llvm::dyn_cast<llvm::Function>(funAttr->getValue());
    auto *returnTy = fun->getReturnType();
    auto compareTy = compareType(returnTy, val->getType());

    if (compareTy == CompareType::Different)
    {
        return ErrorTable::addError(expr->getToken(), "Return Type with value type is different");
    }

    if (compareTy == CompareType::Casting)
    {
        val = castIntegerType(val, returnTy);
    }

    return getBuilder()->CreateRet(val);
}

llvm::Value *weasel::AnalysContext::codegen(VariableExpression *expr)
{
    // Get Allocator from Symbol Table
    auto varName = expr->getIdentifier();
    auto alloc = SymbolTable::getInstance().get(varName);
    if (!alloc)
    {
        return ErrorTable::addError(expr->getToken(), "Variable " + varName + " Not declared");
    }

    if (expr->isAddressOf())
    {
        return alloc->getValue();
    }

    return getBuilder()->CreateLoad(alloc->getValue(), varName);
}
