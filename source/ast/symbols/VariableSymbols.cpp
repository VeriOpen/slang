//------------------------------------------------------------------------------
// VariableSymbols.cpp
// Contains variable-related symbol definitions
//
// SPDX-FileCopyrightText: Michael Popoloski
// SPDX-License-Identifier: MIT
//------------------------------------------------------------------------------
#include "slang/ast/symbols/VariableSymbols.h"

#include "slang/ast/ASTContext.h"
#include "slang/ast/ASTSerializer.h"
#include "slang/ast/Compilation.h"
#include "slang/ast/Definition.h"
#include "slang/ast/Scope.h"
#include "slang/ast/TimingControl.h"
#include "slang/ast/expressions/MiscExpressions.h"
#include "slang/ast/symbols/BlockSymbols.h"
#include "slang/ast/symbols/InstanceSymbols.h"
#include "slang/ast/symbols/PortSymbols.h"
#include "slang/ast/symbols/SubroutineSymbols.h"
#include "slang/ast/types/NetType.h"
#include "slang/ast/types/Type.h"
#include "slang/diagnostics/DeclarationsDiags.h"
#include "slang/diagnostics/LookupDiags.h"
#include "slang/diagnostics/ParserDiags.h"
#include "slang/syntax/AllSyntax.h"
#include "slang/syntax/SyntaxFacts.h"

namespace slang::ast {

using namespace parsing;
using namespace syntax;

static VariableLifetime getDefaultLifetime(const Scope& scope) {
    const Symbol& sym = scope.asSymbol();
    switch (sym.kind) {
        case SymbolKind::StatementBlock:
            return sym.as<StatementBlockSymbol>().defaultLifetime;
        case SymbolKind::Subroutine:
            return sym.as<SubroutineSymbol>().defaultLifetime;
        case SymbolKind::MethodPrototype:
            return VariableLifetime::Automatic;
        default:
            return VariableLifetime::Static;
    }
}

void VariableSymbol::fromSyntax(Compilation& compilation, const DataDeclarationSyntax& syntax,
                                const Scope& scope, SmallVectorBase<const ValueSymbol*>& results) {
    bool isConst = false;
    bool inProceduralContext = scope.isProceduralContext();
    std::optional<VariableLifetime> lifetime;
    for (Token mod : syntax.modifiers) {
        switch (mod.kind) {
            case TokenKind::VarKeyword:
                break;
            case TokenKind::ConstKeyword:
                isConst = true;
                break;
            case TokenKind::StaticKeyword:
                // Static lifetimes are allowed in all contexts.
                lifetime = VariableLifetime::Static;
                break;
            case TokenKind::AutomaticKeyword:
                // Automatic lifetimes are only allowed in procedural contexts.
                lifetime = VariableLifetime::Automatic;
                if (!inProceduralContext) {
                    scope.addDiag(diag::AutomaticNotAllowed, mod.range());
                    lifetime = VariableLifetime::Static;
                }
                break;
            default:
                ASSUME_UNREACHABLE;
        }
    }

    // If no explicit lifetime is provided, find the default one for this scope.
    bool hasExplicitLifetime = lifetime.has_value();
    if (!hasExplicitLifetime)
        lifetime = getDefaultLifetime(scope);

    const bool isInIface =
        scope.asSymbol().kind == SymbolKind::InstanceBody &&
        scope.asSymbol().as<InstanceBodySymbol>().getDefinition().definitionKind ==
            DefinitionKind::Interface;

    for (auto declarator : syntax.declarators) {
        auto variable = compilation.emplace<VariableSymbol>(declarator->name.valueText(),
                                                            declarator->name.location(), *lifetime);
        variable->setDeclaredType(*syntax.type);
        variable->setFromDeclarator(*declarator);
        variable->setAttributes(scope, syntax.attributes);
        results.push_back(variable);

        if (isConst)
            variable->flags |= VariableFlags::Const;

        if (isInIface)
            variable->getDeclaredType()->addFlags(DeclaredTypeFlags::InterfaceVariable);

        // If this is a static variable in a procedural context and it has an initializer,
        // the spec requires that the static keyword must be explicitly provided.
        if (*lifetime == VariableLifetime::Static && !hasExplicitLifetime &&
            declarator->initializer && scope.isProceduralContext()) {
            scope.addDiag(diag::StaticInitializerMustBeExplicit, declarator->name.range());
        }

        // Constants require an initializer.
        if (isConst && !declarator->initializer)
            scope.addDiag(diag::ConstVarNoInitializer, declarator->name.range());
    }
}

VariableSymbol& VariableSymbol::fromSyntax(Compilation& compilation,
                                           const ForVariableDeclarationSyntax& syntax,
                                           const VariableSymbol* lastVar) {
    auto nameToken = syntax.declarator->name;
    auto var = compilation.emplace<VariableSymbol>(nameToken.valueText(), nameToken.location(),
                                                   VariableLifetime::Automatic);

    if (syntax.type)
        var->setDeclaredType(*syntax.type);
    else {
        ASSERT(lastVar);
        var->getDeclaredType()->setLink(*lastVar->getDeclaredType());
    }

    var->setFromDeclarator(*syntax.declarator);
    return *var;
}

VariableSymbol::VariableSymbol(std::string_view name, SourceLocation loc,
                               VariableLifetime lifetime) :
    VariableSymbol(SymbolKind::Variable, name, loc, lifetime) {
}

VariableSymbol::VariableSymbol(SymbolKind childKind, std::string_view name, SourceLocation loc,
                               VariableLifetime lifetime) :
    ValueSymbol(childKind, name, loc),
    lifetime(lifetime) {
    if (lifetime == VariableLifetime::Automatic)
        getDeclaredType()->addFlags(DeclaredTypeFlags::AutomaticInitializer);
}

void VariableSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.write("lifetime", toString(lifetime));

    if (flags) {
        std::string str;
        if (flags.has(VariableFlags::Const))
            str += "const,";
        if (flags.has(VariableFlags::CompilerGenerated))
            str += "compiler_generated,";
        if (flags.has(VariableFlags::ImmutableCoverageOption))
            str += "imm_cov_option,";
        if (!str.empty()) {
            str.pop_back();
            serializer.write("flags", str);
        }
    }
}

FormalArgumentSymbol::FormalArgumentSymbol(std::string_view name, SourceLocation loc,
                                           ArgumentDirection direction, VariableLifetime lifetime) :
    VariableSymbol(SymbolKind::FormalArgument, name, loc, lifetime),
    direction(direction) {
}

void FormalArgumentSymbol::fromSyntax(const Scope& scope, const PortDeclarationSyntax& syntax,
                                      SmallVectorBase<const FormalArgumentSymbol*>& results) {
    if (syntax.header->kind != SyntaxKind::VariablePortHeader) {
        scope.addDiag(diag::ExpectedFunctionPort, syntax.header->sourceRange());
        return;
    }

    auto& comp = scope.getCompilation();
    auto& header = syntax.header->as<VariablePortHeaderSyntax>();
    ArgumentDirection direction = SemanticFacts::getDirection(header.direction.kind);
    VariableLifetime lifetime = getDefaultLifetime(scope);

    bool isConst = false;
    if (header.constKeyword) {
        ASSERT(direction == ArgumentDirection::Ref);
        isConst = true;
    }

    for (auto declarator : syntax.declarators) {
        auto arg = comp.emplace<FormalArgumentSymbol>(declarator->name.valueText(),
                                                      declarator->name.location(), direction,
                                                      lifetime);
        arg->setDeclaredType(*header.dataType);
        arg->setFromDeclarator(*declarator);
        arg->setAttributes(scope, syntax.attributes);
        results.push_back(arg);

        if (isConst)
            arg->flags |= VariableFlags::Const;
    }
}

bool FormalArgumentSymbol::mergeVariable(const VariableSymbol& variable) {
    // If we've already merged one variable already, we can't do any more.
    if (mergedVar)
        return false;

    auto syntax = getSyntax();
    ASSERT(syntax && syntax->parent);
    if (syntax->parent->kind != SyntaxKind::PortDeclaration)
        return false;

    auto& portDecl = syntax->parent->as<PortDeclarationSyntax>();
    auto& header = portDecl.header->as<VariablePortHeaderSyntax>();

    // If the port has a type declared this is already a full definition and
    // we shouldn't merge with any other variables (the caller will error for us).
    if (header.varKeyword || header.dataType->kind != SyntaxKind::ImplicitType)
        return false;

    // Save this variable reference; our DeclaredType will look into it later
    // when our type is fully resolved to merge in the variable's type info.
    getDeclaredType()->addFlags(DeclaredTypeFlags::FormalArgMergeVar);
    mergedVar = &variable;
    return true;
}

void FormalArgumentSymbol::serializeTo(ASTSerializer& serializer) const {
    VariableSymbol::serializeTo(serializer);
    serializer.write("direction", toString(direction));
}

void FieldSymbol::serializeTo(ASTSerializer& serializer) const {
    VariableSymbol::serializeTo(serializer);
    serializer.write("bitOffset", bitOffset);
    serializer.write("fieldIndex", fieldIndex);
}

NetSymbol::NetSymbol(std::string_view name, SourceLocation loc, const NetType& netType) :
    ValueSymbol(SymbolKind::Net, name, loc, DeclaredTypeFlags::NetType), netType(netType) {

    auto dt = getDeclaredType();
    dt->setLink(netType.declaredType);
    if (netType.netKind == NetType::Interconnect)
        dt->addFlags(DeclaredTypeFlags::InterconnectNet);
}

void NetSymbol::fromSyntax(const Scope& scope, const NetDeclarationSyntax& syntax,
                           SmallVectorBase<const NetSymbol*>& results) {
    auto& comp = scope.getCompilation();
    const NetType& netType = comp.getNetType(syntax.netType.kind);

    ExpansionHint expansionHint = ExpansionHint::None;
    switch (syntax.expansionHint.kind) {
        case TokenKind::VectoredKeyword:
            expansionHint = ExpansionHint::Vectored;
            break;
        case TokenKind::ScalaredKeyword:
            expansionHint = ExpansionHint::Scalared;
            break;
        default:
            break;
    }

    for (auto declarator : syntax.declarators) {
        auto net = comp.emplace<NetSymbol>(declarator->name.valueText(),
                                           declarator->name.location(), netType);
        net->expansionHint = expansionHint;
        net->setDeclaredType(*syntax.type);
        net->setFromDeclarator(*declarator);
        net->setAttributes(scope, syntax.attributes);
        results.push_back(net);
    }
}

void NetSymbol::fromSyntax(const Scope& scope, const UserDefinedNetDeclarationSyntax& syntax,
                           const Symbol* netTypeSym, SmallVectorBase<const NetSymbol*>& results) {
    auto& comp = scope.getCompilation();
    if (netTypeSym && netTypeSym->kind != SymbolKind::NetType) {
        scope.addDiag(diag::VarDeclWithDelay, syntax.delay->sourceRange());
        netTypeSym = nullptr;
    }

    const NetType* netType;
    if (!netTypeSym)
        netType = &comp.getNetType(TokenKind::Unknown);
    else
        netType = &netTypeSym->as<NetType>();

    for (auto declarator : syntax.declarators) {
        auto net = comp.emplace<NetSymbol>(declarator->name.valueText(),
                                           declarator->name.location(), *netType);
        net->setFromDeclarator(*declarator);
        net->setAttributes(scope, syntax.attributes);
        results.push_back(net);
    }
}

NetSymbol& NetSymbol::createImplicit(Compilation& compilation, const IdentifierNameSyntax& syntax,
                                     const NetType& netType) {
    auto t = syntax.identifier;
    auto net = compilation.emplace<NetSymbol>(t.valueText(), t.location(), netType);
    net->setType(compilation.getLogicType());
    net->isImplicit = true;
    net->setSyntax(syntax);
    return *net;
}

const TimingControl* NetSymbol::getDelay() const {
    if (delay)
        return *delay;

    auto scope = getParentScope();
    auto syntax = getSyntax();
    if (!scope || !syntax || !syntax->parent) {
        delay = nullptr;
        return nullptr;
    }

    ASTContext context(*scope, LookupLocation::before(*this), ASTFlags::NonProcedural);

    auto& parent = *syntax->parent;
    if (parent.kind == SyntaxKind::NetDeclaration) {
        auto delaySyntax = parent.as<NetDeclarationSyntax>().delay;
        if (delaySyntax) {
            delay = &TimingControl::bind(*delaySyntax, context);
            return *delay;
        }
    }
    else if (parent.kind == SyntaxKind::DataDeclaration) {
        auto type = parent.as<DataDeclarationSyntax>().type;
        if (type->kind == SyntaxKind::NamedType) {
            auto& nt = type->as<NamedTypeSyntax>();
            if (nt.name->kind == SyntaxKind::ClassName) {
                auto params = nt.name->as<ClassNameSyntax>().parameters;
                delay = &DelayControl::fromParams(scope->getCompilation(), *params, context);
                return *delay;
            }
        }
    }

    delay = nullptr;
    return nullptr;
}

std::optional<ChargeStrength> NetSymbol::getChargeStrength() const {
    auto syntax = getSyntax();
    if (syntax && syntax->parent && syntax->parent->kind == SyntaxKind::NetDeclaration) {
        auto& netDecl = syntax->parent->as<NetDeclarationSyntax>();
        if (netDecl.strength && netDecl.strength->kind == SyntaxKind::ChargeStrength) {
            return SemanticFacts::getChargeStrength(
                netDecl.strength->as<ChargeStrengthSyntax>().strength.kind);
        }
    }
    return {};
}

std::pair<std::optional<DriveStrength>, std::optional<DriveStrength>> NetSymbol::getDriveStrength()
    const {
    auto syntax = getSyntax();
    if (syntax && syntax->parent && syntax->parent->kind == SyntaxKind::NetDeclaration) {
        auto& netDecl = syntax->parent->as<NetDeclarationSyntax>();
        if (netDecl.strength)
            return SemanticFacts::getDriveStrength(*netDecl.strength);
    }
    return {};
}

void NetSymbol::checkInitializer() const {
    // Disallow initializers inside packages. Enforcing this check requires knowing
    // about user-defined nettypes, which is why we can't just do it in the parser.
    auto init = getInitializer();
    auto parent = getParentScope();
    if (init && parent && parent->asSymbol().kind == SymbolKind::Package && !init->bad())
        parent->addDiag(diag::PackageNetInit, init->sourceRange);
}

void NetSymbol::serializeTo(ASTSerializer& serializer) const {
    serializer.write("netType", netType);

    if (isImplicit)
        serializer.write("isImplicit", isImplicit);

    switch (expansionHint) {
        case Vectored:
            serializer.write("expansionHint", "vectored"sv);
            break;
        case Scalared:
            serializer.write("expansionHint", "scalared"sv);
            break;
        default:
            break;
    }

    if (auto delayCtrl = getDelay())
        serializer.write("delay", *delayCtrl);

    if (auto chargeStrength = getChargeStrength())
        serializer.write("chargeStrength", toString(*chargeStrength));

    auto [ds0, ds1] = getDriveStrength();
    if (ds0)
        serializer.write("driveStrength0", toString(*ds0));
    if (ds1)
        serializer.write("driveStrength1", toString(*ds1));
}

IteratorSymbol::IteratorSymbol(const Scope& scope, std::string_view name, SourceLocation loc,
                               const Type& arrayType) :
    TempVarSymbol(SymbolKind::Iterator, name, loc, VariableLifetime::Automatic),
    arrayType(arrayType) {

    flags |= VariableFlags::Const;
    setParent(scope);

    const Type* elemType = arrayType.getArrayElementType();
    if (!elemType)
        elemType = &scope.getCompilation().getErrorType();

    setType(*elemType);
}

IteratorSymbol::IteratorSymbol(std::string_view name, SourceLocation loc, const Type& arrayType,
                               const Type& indexType) :
    TempVarSymbol(SymbolKind::Iterator, name, loc, VariableLifetime::Automatic),
    arrayType(arrayType) {

    flags |= VariableFlags::Const;
    setType(indexType);
}

PatternVarSymbol::PatternVarSymbol(std::string_view name, SourceLocation loc, const Type& type) :
    TempVarSymbol(SymbolKind::PatternVar, name, loc, VariableLifetime::Automatic) {

    flags |= VariableFlags::Const;
    setType(type);
}

ClockVarSymbol::ClockVarSymbol(std::string_view name, SourceLocation loc,
                               ArgumentDirection direction, ClockingSkew inputSkew,
                               ClockingSkew outputSkew) :
    VariableSymbol(SymbolKind::ClockVar, name, loc, VariableLifetime::Static),
    direction(direction), inputSkew(inputSkew), outputSkew(outputSkew) {
}

void ClockVarSymbol::fromSyntax(const Scope& scope, const ClockingItemSyntax& syntax,
                                SmallVectorBase<const ClockVarSymbol*>& results) {
    // Lookups should happen in the parent of the clocking block, since other
    // clocking block members cannot reference each other.
    auto& comp = scope.getCompilation();
    auto parent = scope.asSymbol().getParentScope();
    ASSERT(parent);

    LookupLocation ll = LookupLocation::before(scope.asSymbol());
    ASTContext context(*parent, ll, ASTFlags::NonProcedural);

    ArgumentDirection dir = ArgumentDirection::In;
    ClockingSkew inputSkew, outputSkew;
    if (syntax.direction->input.kind == TokenKind::InOutKeyword) {
        dir = ArgumentDirection::InOut;
    }
    else {
        if (syntax.direction->input) {
            if (syntax.direction->inputSkew)
                inputSkew = ClockingSkew::fromSyntax(*syntax.direction->inputSkew, context);
        }

        if (syntax.direction->output) {
            dir = syntax.direction->input ? ArgumentDirection::InOut : ArgumentDirection::Out;
            if (syntax.direction->outputSkew)
                outputSkew = ClockingSkew::fromSyntax(*syntax.direction->outputSkew, context);
        }
    }

    if (dir == ArgumentDirection::Out || dir == ArgumentDirection::InOut)
        context = context.resetFlags(ASTFlags::LValue);

    for (auto decl : syntax.decls) {
        auto name = decl->name;
        auto arg = comp.emplace<ClockVarSymbol>(name.valueText(), name.location(), dir, inputSkew,
                                                outputSkew);
        arg->setSyntax(*decl);
        arg->setAttributes(*parent, syntax.attributes);
        results.push_back(arg);

        // If there is an initializer expression we take our type from that.
        // Otherwise we need to lookup the signal in our parent scope and
        // take the type from that.
        if (decl->value) {
            auto& expr = Expression::bind(*decl->value->expr, context);
            arg->setType(*expr.type);
            arg->setInitializer(expr);

            if (dir != ArgumentDirection::In)
                expr.requireLValue(context, decl->value->equals.location(), AssignFlags::ClockVar);
        }
        else {
            auto sym = Lookup::unqualifiedAt(*parent, name.valueText(), ll, name.range());
            if (sym && sym->kind != SymbolKind::Net && sym->kind != SymbolKind::Variable) {
                auto& diag = context.addDiag(diag::InvalidClockingSignal, name.range());
                diag << name.valueText();
                diag.addNote(diag::NoteDeclarationHere, sym->location);
                sym = nullptr;
            }

            if (sym) {
                auto sourceType = sym->getDeclaredType();
                ASSERT(sourceType);
                arg->getDeclaredType()->setLink(*sourceType);

                auto& valExpr = ValueExpressionBase::fromSymbol(
                    context, *sym, false, {arg->location, arg->location + arg->name.length()});

                if (dir != ArgumentDirection::In)
                    context.addDriver(sym->as<ValueSymbol>(), valExpr, AssignFlags::ClockVar);
            }
            else {
                arg->getDeclaredType()->setType(comp.getErrorType());
            }
        }
    }
}

void ClockVarSymbol::serializeTo(ASTSerializer& serializer) const {
    VariableSymbol::serializeTo(serializer);

    serializer.write("direction", toString(direction));

    if (inputSkew.hasValue()) {
        serializer.writeProperty("inputSkew");
        serializer.startObject();
        inputSkew.serializeTo(serializer);
        serializer.endObject();
    }

    if (outputSkew.hasValue()) {
        serializer.writeProperty("outputSkew");
        serializer.startObject();
        outputSkew.serializeTo(serializer);
        serializer.endObject();
    }
}

LocalAssertionVarSymbol::LocalAssertionVarSymbol(std::string_view name, SourceLocation loc) :
    VariableSymbol(SymbolKind::LocalAssertionVar, name, loc, VariableLifetime::Automatic) {
    getDeclaredType()->addFlags(DeclaredTypeFlags::RequireSequenceType);
}

void LocalAssertionVarSymbol::fromSyntax(const Scope& scope,
                                         const LocalVariableDeclarationSyntax& syntax,
                                         SmallVectorBase<const LocalAssertionVarSymbol*>& results) {
    auto& comp = scope.getCompilation();
    for (auto declarator : syntax.declarators) {
        auto var = comp.emplace<LocalAssertionVarSymbol>(declarator->name.valueText(),
                                                         declarator->name.location());
        var->setDeclaredType(*syntax.type);
        var->setFromDeclarator(*declarator);
        var->setAttributes(scope, syntax.attributes);
        results.push_back(var);

        // Local variables don't get added to any scope as members but
        // we still need a parent pointer set so they can participate in lookups.
        var->setParent(scope);
    }
}

} // namespace slang::ast
