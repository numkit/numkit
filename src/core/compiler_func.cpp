// src/core/compiler_func.cpp
#include <keel/core.h>
#include <keel/format.h>
#include <keel/utf8.h>
#include <numkit/core/compiler.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/stack_guard.hpp>
#include <numkit/core/vm.hpp>
#include "compiler_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

namespace numkit {

keel::u16 Compiler::compileFunctionDef(const ASTNode *node)
{
    // Nested functions inside an enclosing function body are pre-compiled
    // into scoped "outer>inner" chunks by compileFunction's prologue and must
    // never leak into the global / script-level function table.
    if (inFunctionBody_)
        return 0;

    // Compile function body into a separate BytecodeChunk. Which
    // bucket it lands in depends on whether we're inside a script
    // scope — script-local functions live in their own store so
    // `clear all` can't touch them (MATLAB parity).
    BytecodeChunk funcChunk = compileFunction(node);

    UserFunction uf;
    uf.name = node->strValue.clone();
    uf.params = node->paramNames.clone();
    uf.returns = node->returnNames.clone();
    uf.body = keel::Arc<const ASTNode>(cloneNode(node->children[0].get()));
    uf.closureEnv = nullptr;

    if (scriptDepth_ > 0) {
        scriptLocalCompiledFuncs_.put(node->strValue.clone(), keel::move(funcChunk));
        engine_.adoptUserFunction(node->strValue, keel::move(uf), /*scriptScope=*/true);
    } else {
        compiledFuncs_.put(node->strValue.clone(), keel::move(funcChunk));
        engine_.adoptUserFunction(node->strValue, keel::move(uf), /*scriptScope=*/false);
    }
    return 0;
}

void Compiler::registerFunction(const ASTNode *funcDef)
{
    // compileFunction (invoked by compileFunctionDef) already saves and
    // restores chunk_/varRegisters_/… so this is safe on an otherwise-idle
    // Compiler.
    compileFunctionDef(funcDef);
}

void Compiler::registerFunctionAs(keel::StringRef qualifiedName,
                                   const ASTNode *funcDef)
{
    AnonOriginSentry sentry(*this, /*known=*/false, keel::String{}, keel::String{}, keel::String{});
    registerFunctionAsImpl_(qualifiedName, funcDef);
}

void Compiler::registerFunctionAs(keel::StringRef qualifiedName,
                                   const ASTNode *funcDef, keel::StringRef fsName,
                                   keel::StringRef scriptDir,
                                   keel::StringRef scriptFile)
{
    AnonOriginSentry sentry(*this, /*known=*/true, keel::String::from(fsName),
                            keel::String::from(scriptDir), keel::String::from(scriptFile));
    registerFunctionAsImpl_(qualifiedName, funcDef);
}

Compiler::AnonOriginSentry::AnonOriginSentry(Compiler &c, bool known, keel::String fs,
                                             keel::String dir, keel::String file)
    : c_(c)
    , known_(c.anonOriginKnown_)
    , fs_(keel::move(c.anonOriginFs_))
    , dir_(keel::move(c.anonOriginDir_))
    , file_(keel::move(c.anonOriginFile_))
{
    c_.anonOriginKnown_ = known;
    c_.anonOriginFs_ = keel::move(fs);
    c_.anonOriginDir_ = keel::move(dir);
    c_.anonOriginFile_ = keel::move(file);
}

Compiler::AnonOriginSentry::~AnonOriginSentry()
{
    c_.anonOriginKnown_ = known_;
    c_.anonOriginFs_ = keel::move(fs_);
    c_.anonOriginDir_ = keel::move(dir_);
    c_.anonOriginFile_ = keel::move(file_);
}

void Compiler::registerFunctionAsImpl_(keel::StringRef qualifiedName,
                                        const ASTNode *funcDef)
{
    BytecodeChunk funcChunk;
    if (funcDef->strValue == qualifiedName) {
        funcChunk = compileFunction(funcDef);
    } else {
        ASTNode fn(NodeType::FUNCTION_DEF);
        fn.strValue = keel::String::from(qualifiedName);
        fn.paramNames = funcDef->paramNames.clone();
        fn.returnNames = funcDef->returnNames.clone();
        fn.line = funcDef->line;
        fn.col = funcDef->col;
        fn.endLine = funcDef->endLine;
        if (!funcDef->children.empty())
            fn.children.push_back(cloneNode(funcDef->children[0].get()));
        funcChunk = compileFunction(&fn);
    }

    UserFunction uf;
    uf.name = keel::String::from(qualifiedName);
    uf.params = funcDef->paramNames.clone();
    uf.returns = funcDef->returnNames.clone();
    uf.body = keel::Arc<const ASTNode>(cloneNode(funcDef->children[0].get()));
    uf.closureEnv = nullptr;

    if (scriptDepth_ > 0) {
        scriptLocalCompiledFuncs_.put(keel::String::from(qualifiedName), keel::move(funcChunk));
        engine_.adoptUserFunction(qualifiedName, keel::move(uf), /*scriptScope=*/true);
    } else {
        compiledFuncs_.put(keel::String::from(qualifiedName), keel::move(funcChunk));
        engine_.adoptUserFunction(qualifiedName, keel::move(uf), /*scriptScope=*/false);
    }
}

void Compiler::setChunkOrigin(keel::StringRef qualifiedName,
                              keel::StringRef fsName, keel::StringRef scriptDir,
                              keel::StringRef scriptFile)
{
    auto stampMap = [&](keel::HashMap<keel::String, BytecodeChunk> &tbl) {
        const keel::String prefix = keel::format("{}>", qualifiedName);
        for (auto [k, ch] : tbl) {
            if (k == qualifiedName
                || (k.size() > prefix.size() && k.starts_with(prefix))) {
                ch.originKnown = true;
                ch.originFs.assign(fsName);
                ch.originDir.assign(scriptDir);
                ch.originFile.assign(scriptFile);
            }
        }
    };
    stampMap(compiledFuncs_);
    stampMap(scriptLocalCompiledFuncs_);
}

void Compiler::eraseCompiledFunc(keel::StringRef name)
{
    compiledFuncs_.remove(name);
    const keel::String prefix = keel::format("{}>", name);
    keel::Array<keel::String> toRemove;
    for (auto [k, ch] : compiledFuncs_) {
        if (k.size() > prefix.size() && k.starts_with(prefix))
            toRemove.push_back(k.clone());
    }
    for (const auto &k : toRemove)
        compiledFuncs_.remove(k);
}

void Compiler::eraseCompiledFuncsForClass(keel::StringRef className)
{
    // Method chunks are keyed by their fully-qualified name "ClassName>method"
    // (set in ensureClassMethodCompiled). Erase every such key so a redefined
    // class recompiles fresh bodies rather than serving the stale cache — the
    // root of "I rewrote the method but behaviour didn't change". Inherited
    // methods keep their own "Base>method" keys and are untouched.
    const keel::String prefix = keel::format("{}>", className);
    keel::Array<keel::String> toRemove;
    for (auto [k, ch] : compiledFuncs_) {
        if (k.size() > prefix.size() && k.starts_with(prefix))
            toRemove.push_back(k.clone());
    }
    for (const auto &k : toRemove)
        compiledFuncs_.remove(k);
}

const BytecodeChunk *Compiler::ensureClassMethodCompiled(const UserFunction &uf)
{
    if (const BytecodeChunk *existing = findCompiled(uf.name))
        return existing;
    ASTNode fn(NodeType::FUNCTION_DEF);
    fn.strValue = uf.name.clone();
    fn.paramNames = uf.params.clone();
    fn.returnNames = uf.returns.clone();
    if (uf.body)
        fn.children.push_back(cloneNode(uf.body.get()));
    BytecodeChunk chunk;
    {
        AnonOriginSentry sentry(*this, uf.originKnown, uf.sourceFs.clone(),
                                 uf.sourceDir.clone(), uf.sourceFile.clone());
        chunk = compileFunction(&fn);
    }
    chunk.name = uf.name.clone();
    // The method body runs under the classdef file's provenance —
    // mfilename() inside reports the class file (registerClassDef
    // stamped it on the UserFunction; inherited methods carry their
    // declaring class's file).
    chunk.originKnown = uf.originKnown;
    chunk.originFs = uf.sourceFs.clone();
    chunk.originDir = uf.sourceDir.clone();
    chunk.originFile = uf.sourceFile.clone();
    auto ins = compiledFuncs_.try_insert(uf.name.clone(), keel::move(chunk));
    return ins.value;
}

const BytecodeChunk *Compiler::findCompiled(keel::StringRef name) const
{
    if (const BytecodeChunk *c = scriptLocalCompiledFuncs_.get(name))
        return c;
    return compiledFuncs_.get(name);
}

void Compiler::beginScriptScope()
{
    savedScriptLocalCompiledFuncs_.push_back(keel::move(scriptLocalCompiledFuncs_));
    scriptLocalCompiledFuncs_.clear();
    ++scriptDepth_;
}

void Compiler::endScriptScope()
{
    if (scriptDepth_ == 0)
        return;
    --scriptDepth_;
    if (savedScriptLocalCompiledFuncs_.empty()) {
        scriptLocalCompiledFuncs_.clear();
        return;
    }
    scriptLocalCompiledFuncs_ = keel::move(savedScriptLocalCompiledFuncs_.back());
    savedScriptLocalCompiledFuncs_.pop_back();
}

void Compiler::promoteScriptLocalsToWorkspace()
{
    for (auto [k, ch] : scriptLocalCompiledFuncs_)
        compiledFuncs_.put(k.clone(), keel::move(ch));
    scriptLocalCompiledFuncs_.clear();
}

namespace {

void collectScopeGlobalPersistentDecls(const ASTNode *node,
                                       keel::Array<keel::String> &globals,
                                       keel::Array<keel::String> &persistents)
{
    if (!node || node->type == NodeType::FUNCTION_DEF || node->type == NodeType::ANON_FUNC)
        return;
    if (node->type == NodeType::GLOBAL_STMT) {
        for (const auto &nm : node->paramNames) {
            bool found = false;
            for (const auto &g : globals)
                if (g == nm) { found = true; break; }
            if (!found)
                globals.push_back(nm.clone());
        }
        return;
    }
    if (node->type == NodeType::PERSISTENT_STMT) {
        for (const auto &nm : node->paramNames) {
            bool found = false;
            for (const auto &p : persistents)
                if (p == nm) { found = true; break; }
            if (!found)
                persistents.push_back(nm.clone());
        }
        return;
    }
    for (const auto &ch : node->children)
        collectScopeGlobalPersistentDecls(ch.get(), globals, persistents);
    for (const auto &[cond, body] : node->branches) {
        collectScopeGlobalPersistentDecls(cond.get(), globals, persistents);
        collectScopeGlobalPersistentDecls(body.get(), globals, persistents);
    }
    if (node->elseBranch)
        collectScopeGlobalPersistentDecls(node->elseBranch.get(), globals, persistents);
}

void collectDirectNestedDefs(const ASTNode *node, keel::Array<const ASTNode *> &out)
{
    if (!node || node->type == NodeType::ANON_FUNC)
        return;
    if (node->type == NodeType::FUNCTION_DEF) {
        out.push_back(node);
        return;
    }
    for (const auto &ch : node->children)
        collectDirectNestedDefs(ch.get(), out);
    for (const auto &[cond, body] : node->branches) {
        collectDirectNestedDefs(cond.get(), out);
        collectDirectNestedDefs(body.get(), out);
    }
    if (node->elseBranch)
        collectDirectNestedDefs(node->elseBranch.get(), out);
}

bool isPseudoScopeVar(keel::StringRef name)
{
    return name == "nargin" || name == "nargout" || name == "varargin" || name == "varargout";
}

} // namespace

BytecodeChunk Compiler::compileFunction(const ASTNode *funcDef,
                                        keel::Arc<const keel::String> sourceCode)
{
    struct FunctionCompileGuard
    {
        Compiler &c;
        BytecodeChunk savedChunk;
        keel::HashMap<keel::String, keel::u16> savedVarRegs;
        keel::Array<LoopContext> savedLoopStack;
        keel::HashMap<keel::i32, keel::u16> savedConstCache;
        keel::HashMap<keel::i32, keel::u16> savedPinnedConsts;
        RegBitset savedScalarRegs;
        RegBitset savedDefinedRegs;
        bool savedMayClear;
        bool savedMayInjectVars;
        keel::HashMap<keel::String, keel::u16> savedUpvalueRegs;
        keel::HashMap<keel::String, keel::u16> savedAllScopeUpvalBoxRegs;
        keel::HashMap<keel::String, NestedFuncInfo> savedNestedFuncInfo;
        const EnclosingLexicalContext *savedPendingLexicalCtx;
        keel::i32 savedNextReg;
        keel::i32 savedPeakReg;
        keel::i32 savedMaxVarReg;
        bool savedTopLevel;
        bool savedInFunctionBody;
        bool savedInFusionFallback;
        keel::u16 savedNargoutContext;
        keel::i32 savedActiveTryDepth;
        keel::u16 savedIdxArr;
        keel::u16 savedIdxDim;
        keel::u16 savedIdxNdims;
        SourceLoc savedLoc;

        explicit FunctionCompileGuard(Compiler &comp)
            : c(comp)
            , savedChunk(keel::move(comp.chunk_))
            , savedVarRegs(keel::move(comp.varRegisters_))
            , savedLoopStack(keel::move(comp.loopStack_))
            , savedConstCache(keel::move(comp.constRegCache_))
            , savedPinnedConsts(keel::move(comp.pinnedConstRegs_))
            , savedScalarRegs(keel::move(comp.scalarRegs_))
            , savedDefinedRegs(keel::move(comp.definedRegs_))
            , savedMayClear(comp.mayClear_)
            , savedMayInjectVars(comp.mayInjectVars_)
            , savedUpvalueRegs(keel::move(comp.upvalueRegs_))
            , savedAllScopeUpvalBoxRegs(keel::move(comp.allScopeUpvalueBoxRegs_))
            , savedNestedFuncInfo(keel::move(comp.nestedFuncInfo_))
            , savedPendingLexicalCtx(comp.pendingLexicalCtx_)
            , savedNextReg(comp.nextReg_)
            , savedPeakReg(comp.peakReg_)
            , savedMaxVarReg(comp.maxVarReg_)
            , savedTopLevel(comp.isTopLevel_)
            , savedInFunctionBody(comp.inFunctionBody_)
            , savedInFusionFallback(comp.inFusionFallback_)
            , savedNargoutContext(comp.nargoutContext_)
            , savedActiveTryDepth(comp.activeTryDepth_)
            , savedIdxArr(comp.indexContextArr_)
            , savedIdxDim(comp.indexContextDim_)
            , savedIdxNdims(comp.indexContextNdims_)
            , savedLoc(comp.currentLoc_)
        {}

        ~FunctionCompileGuard()
        {
            c.chunk_ = keel::move(savedChunk);
            c.varRegisters_ = keel::move(savedVarRegs);
            c.loopStack_ = keel::move(savedLoopStack);
            c.constRegCache_ = keel::move(savedConstCache);
            c.pinnedConstRegs_ = keel::move(savedPinnedConsts);
            c.scalarRegs_ = keel::move(savedScalarRegs);
            c.definedRegs_ = keel::move(savedDefinedRegs);
            c.mayClear_ = savedMayClear;
            c.mayInjectVars_ = savedMayInjectVars;
            c.upvalueRegs_ = keel::move(savedUpvalueRegs);
            c.allScopeUpvalueBoxRegs_ = keel::move(savedAllScopeUpvalBoxRegs);
            c.nestedFuncInfo_ = keel::move(savedNestedFuncInfo);
            c.pendingLexicalCtx_ = savedPendingLexicalCtx;
            c.nextReg_ = savedNextReg;
            c.peakReg_ = savedPeakReg;
            c.maxVarReg_ = savedMaxVarReg;
            c.isTopLevel_ = savedTopLevel;
            c.inFunctionBody_ = savedInFunctionBody;
            c.inFusionFallback_ = savedInFusionFallback;
            c.nargoutContext_ = savedNargoutContext;
            c.activeTryDepth_ = savedActiveTryDepth;
            c.indexContextArr_ = savedIdxArr;
            c.indexContextDim_ = savedIdxDim;
            c.indexContextNdims_ = savedIdxNdims;
            c.currentLoc_ = savedLoc;
        }
    } stateGuard(*this);

    const EnclosingLexicalContext *lexicalCtx = stateGuard.savedPendingLexicalCtx;
    pendingLexicalCtx_ = nullptr;

    isTopLevel_ = false;
    inFunctionBody_ = true;
    inFusionFallback_ = false;
    nargoutContext_ = 1;
    activeTryDepth_ = 0;
    indexContextArr_ = kInvalidReg;
    indexContextDim_ = 0;
    indexContextNdims_ = 1;

    chunk_ = BytecodeChunk{};
    chunk_.name = funcDef->strValue.clone();
    chunk_.sourceCode = sourceCode ? keel::move(sourceCode) : stateGuard.savedChunk.sourceCode;
    chunk_.paramNames = funcDef->paramNames.clone();
    chunk_.returnNames = funcDef->returnNames.clone();
    if (funcDef->paramNames.size() >= keel::as<keel::usize>(kInvalidReg)
        || funcDef->returnNames.size() >= keel::as<keel::usize>(kInvalidReg))
        throw std::runtime_error(
            keel::format("Compiler: function '{}' declares more than 65534 parameters or returns",
                         funcDef->strValue)
                .c_str());
    chunk_.numParams = keel::as<keel::u16>(funcDef->paramNames.size());
    chunk_.numReturns = keel::as<keel::u16>(funcDef->returnNames.size());
    varRegisters_.clear();
    loopStack_.clear();
    constRegCache_.clear();
    pinnedConstRegs_.clear();
    scalarRegs_.reset();
    definedRegs_.reset();
    mayClear_ = false;
    mayInjectVars_ = false;
    upvalueRegs_.clear();
    allScopeUpvalueBoxRegs_.clear();
    nestedFuncInfo_.clear();
    nextReg_ = 0;
    peakReg_ = 0;
    maxVarReg_ = 0;

    const ASTNode *bodyRoot = funcDef->children.empty() ? nullptr : funcDef->children[0].get();

    // Pre-scan global and persistent declarations in this function's own body
    // before setting up lexical upvalues and preallocating locals.
    if (bodyRoot)
        collectScopeGlobalPersistentDecls(bodyRoot, chunk_.globalNames, chunk_.persistentNames);

    // Allocate registers for parameters (they come pre-loaded by VM)
    for (auto &param : funcDef->paramNames) {
        varRegWrite(param);
    }

    // Allocate registers for return variables (default to empty; no initialization needed).
    for (auto &ret : funcDef->returnNames) {
        varRegWrite(ret); // will be populated by the body
    }

    for (auto &gn : chunk_.globalNames)
        varRegWrite(gn);
    for (auto &pn : chunk_.persistentNames)
        varRegWrite(pn);

    // Always allocate nargin/nargout so they're visible in debugger (pre-
    // loaded by the VM on entry).
    keel::u16 narginReg = varRegLookup("nargin");
    keel::u16 nargoutReg = varRegLookup("nargout");

    // Collect direct nested function definitions and set up lexical upvalues.
    keel::Array<const ASTNode *> directNestedDefs;
    if (bodyRoot)
        collectDirectNestedDefs(bodyRoot, directNestedDefs);

    keel::HashSet<keel::String> ownDeclaredShadow;
    for (const auto &p : funcDef->paramNames) ownDeclaredShadow.insert(p.clone());
    for (const auto &r : funcDef->returnNames) ownDeclaredShadow.insert(r.clone());
    for (const auto &g : chunk_.globalNames) ownDeclaredShadow.insert(g.clone());
    for (const auto &p : chunk_.persistentNames) ownDeclaredShadow.insert(p.clone());

    keel::Array<keel::String> activeUpvalues;
    if (lexicalCtx) {
        for (const auto &u : lexicalCtx->inheritedUpvalueNames) {
            keel::u16 boxReg = allocPinnedReg();
            chunk_.upvalueParamRegs.push_back(boxReg);
            allScopeUpvalueBoxRegs_.put(u.clone(), boxReg);
            activeUpvalues.push_back(u.clone());
            if (!ownDeclaredShadow.contains(u) && !isPseudoScopeVar(u)) {
                varRegLookup(u, /*preloadReserved=*/false);
                upvalueRegs_.put(u.clone(), boxReg);
                chunk_.upvalueBoxRegs.put(u.clone(), boxReg);
            }
        }
        for (auto [sibName, sibInfo] : lexicalCtx->siblingNestedFuncs)
            nestedFuncInfo_.put(sibName.clone(), sibInfo.clone());
    }

    keel::Array<keel::String> assignedVec;
    if (bodyRoot)
        collectAssignedNames(bodyRoot, assignedVec);
    keel::HashSet<keel::String> assignedSet;
    for (const auto &a : assignedVec)
        assignedSet.insert(a.clone());
    keel::HashSet<keel::String> identifiers;
    if (bodyRoot)
        collectAllIdentifiers(bodyRoot, identifiers);

    if (!directNestedDefs.empty()) {
        keel::HashSet<keel::String> directNestedNames;
        for (const ASTNode *nd : directNestedDefs)
            directNestedNames.insert(nd->strValue.clone());

        auto collectDescendantRefs = [&](auto &self, const ASTNode *nfn,
                                         keel::HashSet<keel::String> &allRefs,
                                         keel::HashSet<keel::String> &asgnRefs) -> void {
            if (!nfn || nfn->children.empty())
                return;
            const ASTNode *nb = nfn->children[0].get();
            keel::HashSet<keel::String> shadow;
            for (const auto &p : nfn->paramNames) shadow.insert(p.clone());
            for (const auto &r : nfn->returnNames) shadow.insert(r.clone());
            keel::Array<keel::String> ng, np;
            collectScopeGlobalPersistentDecls(nb, ng, np);
            for (const auto &g : ng) shadow.insert(g.clone());
            for (const auto &p : np) shadow.insert(p.clone());

            keel::HashSet<keel::String> nIds;
            collectAllIdentifiers(nb, nIds);
            keel::Array<keel::String> nAsgnVec;
            collectAssignedNames(nb, nAsgnVec);

            for (const auto &id : nIds) {
                if (!shadow.contains(id) && !isPseudoScopeVar(id))
                    allRefs.insert(id.clone());
            }
            for (const auto &asgn : nAsgnVec) {
                if (!shadow.contains(asgn) && !isPseudoScopeVar(asgn)) {
                    allRefs.insert(asgn.clone());
                    asgnRefs.insert(asgn.clone());
                }
            }

            keel::Array<const ASTNode *> subDefs;
            collectDirectNestedDefs(nb, subDefs);
            for (const ASTNode *sd : subDefs)
                shadow.insert(sd->strValue.clone());
            for (const ASTNode *sd : subDefs) {
                keel::HashSet<keel::String> subAll, subAsgn;
                self(self, sd, subAll, subAsgn);
                for (const auto &id : subAll)
                    if (!shadow.contains(id) && !isPseudoScopeVar(id))
                        allRefs.insert(id.clone());
                for (const auto &asgn : subAsgn)
                    if (!shadow.contains(asgn) && !isPseudoScopeVar(asgn))
                        asgnRefs.insert(asgn.clone());
            }
        };

        keel::HashSet<keel::String> descAllRefs, descAsgnRefs;
        for (const ASTNode *nd : directNestedDefs)
            collectDescendantRefs(collectDescendantRefs, nd, descAllRefs, descAsgnRefs);

        keel::HashSet<keel::String> ownDeclaredOrAssigned = ownDeclaredShadow.clone();
        for (const auto &a : assignedVec)
            ownDeclaredOrAssigned.insert(a.clone());

        // Deterministic order for newly boxed variables of this scope
        keel::Array<keel::String> candidateVars;
        auto addCandidate = [&](keel::StringRef nm) {
            if (nm.empty() || isPseudoScopeVar(nm) || directNestedNames.contains(nm)
                || allScopeUpvalueBoxRegs_.contains(nm))
                return;
            for (const auto &cv : candidateVars)
                if (cv == nm)
                    return;
            candidateVars.push_back(keel::String::from(nm));
        };
        for (const auto &p : funcDef->paramNames) addCandidate(p);
        for (const auto &r : funcDef->returnNames) addCandidate(r);
        for (const auto &g : chunk_.globalNames) addCandidate(g);
        for (const auto &p : chunk_.persistentNames) addCandidate(p);
        for (const auto &a : assignedVec) addCandidate(a);
        for (const auto &id : identifiers) {
            if (descAsgnRefs.contains(id))
                addCandidate(id);
        }

        keel::HashSet<keel::String> preloadedOnEntry;
        for (const auto &p : funcDef->paramNames) preloadedOnEntry.insert(p.clone());
        for (const auto &r : funcDef->returnNames) preloadedOnEntry.insert(r.clone());
        for (const auto &g : chunk_.globalNames) preloadedOnEntry.insert(g.clone());
        for (const auto &p : chunk_.persistentNames) preloadedOnEntry.insert(p.clone());

        for (const auto &nm : candidateVars) {
            const bool shared = (ownDeclaredOrAssigned.contains(nm) && descAllRefs.contains(nm))
                                || (identifiers.contains(nm) && descAsgnRefs.contains(nm));
            if (!shared)
                continue;
            keel::u16 mirrorReg = varRegLookup(nm, /*preloadReserved=*/false);
            keel::u16 boxReg = allocPinnedReg();
            keel::u16 initSrc = preloadedOnEntry.contains(nm) ? mirrorReg : kInvalidReg;
            emitAB(OpCode::UPVAL_NEW, boxReg, initSrc);
            upvalueRegs_.put(nm.clone(), boxReg);
            allScopeUpvalueBoxRegs_.put(nm.clone(), boxReg);
            chunk_.upvalueBoxRegs.put(nm.clone(), boxReg);
            activeUpvalues.push_back(nm.clone());
            chunk_.assignedVars.insert(nm.clone());
        }

        // Register all direct nested functions before compiling any of them so
        // mutual recursion between siblings and self-recursion resolve cleanly.
        for (const ASTNode *nd : directNestedDefs) {
            keel::String compiledName = keel::format("{}>{}", chunk_.name, nd->strValue);
            keel::String dispSrc = compiledName.clone();
            for (keel::usize ci = 0; ci < dispSrc.size(); ++ci)
                if (dispSrc[ci] == '>')
                    dispSrc[ci] = '/';
            nestedFuncInfo_.put(nd->strValue.clone(), NestedFuncInfo{
                keel::move(compiledName), keel::move(dispSrc), activeUpvalues.clone()});
        }

        EnclosingLexicalContext childLexCtx{activeUpvalues.clone(), nestedFuncInfo_.clone()};
        for (const ASTNode *nd : directNestedDefs) {
            const NestedFuncInfo &info = *nestedFuncInfo_.get(nd->strValue);
            ASTNode nestedNode(NodeType::FUNCTION_DEF);
            nestedNode.strValue = info.compiledName.clone();
            nestedNode.paramNames = nd->paramNames.clone();
            nestedNode.returnNames = nd->returnNames.clone();
            nestedNode.line = nd->line;
            nestedNode.col = nd->col;
            nestedNode.endLine = nd->endLine;
            if (!nd->children.empty())
                nestedNode.children.push_back(cloneNode(nd->children[0].get()));

            pendingLexicalCtx_ = &childLexCtx;
            BytecodeChunk nestedChunk = compileFunction(&nestedNode, chunk_.sourceCode);
            pendingLexicalCtx_ = nullptr;
            nestedChunk.parentFuncName = chunk_.parentFuncName.empty()
                                             ? chunk_.name.clone()
                                             : chunk_.parentFuncName.clone();

            UserFunction nuf;
            nuf.name = info.compiledName.clone();
            nuf.params = nd->paramNames.clone();
            nuf.returns = nd->returnNames.clone();
            if (!nd->children.empty())
                nuf.body = keel::Arc<const ASTNode>(cloneNode(nd->children[0].get()));
            nuf.closureEnv = nullptr;
            nuf.numUpvalueCaptures = info.upvalueNames.size();

            if (scriptDepth_ > 0) {
                scriptLocalCompiledFuncs_.put(info.compiledName.clone(), keel::move(nestedChunk));
                engine_.adoptUserFunction(info.compiledName, keel::move(nuf), /*scriptScope=*/true);
            } else {
                compiledFuncs_.put(info.compiledName.clone(), keel::move(nestedChunk));
                engine_.adoptUserFunction(info.compiledName, keel::move(nuf), /*scriptScope=*/false);
            }
        }
    }

    for (const auto &name : identifiers) {
        if (varRegisters_.contains(name) || assignedSet.contains(name) || upvalueRegs_.contains(name))
            continue;
        if (engine_.isReservedName(name)) {
            varRegLookup(name, /*preloadReserved=*/true);
        }
    }

    if (bodyRoot) {
        preallocateAssignedVars(bodyRoot);
        preallocatePinnedConstants(bodyRoot);
    }
    if (!mayClear_) {
        definedRegs_.set(narginReg);
        definedRegs_.set(nargoutReg);
    }

    // Compile body
    if (bodyRoot)
        compileNode(bodyRoot);

    // Set source location to the 'end' keyword so breakpoints on it fire
    if (funcDef->endLine > 0)
        currentLoc_ = {keel::as<keel::u16>(funcDef->endLine), 1};

    // Emit return: collect return values
    const auto &rnames = funcDef->returnNames;
    const bool hasVarargout = !rnames.empty() && rnames.back() == "varargout";
    if (hasVarargout) {
        emitVarargoutReturn();
    } else if (rnames.size() == 1) {
        keel::u16 retReg = varRegLookup(funcDef->returnNames[0]);
        emitUpvalLoadRawIfShared(funcDef->returnNames[0], retReg);
        emitA(OpCode::RET, retReg);
    } else if (funcDef->returnNames.empty()) {
        emitNone(OpCode::RET_EMPTY);
    } else {
        keel::usize nret = funcDef->returnNames.size();
        keel::Array<keel::u16> retRegs;
        for (auto &name : funcDef->returnNames) {
            keel::u16 r = varRegLookup(name);
            emitUpvalLoadRawIfShared(name, r);
            retRegs.push_back(r);
        }
        bool consecutive = true;
        for (keel::usize i = 1; i < nret; ++i)
            if (retRegs[i] != retRegs[0] + i) { consecutive = false; break; }
        if (consecutive) {
            emitAB(OpCode::RET_MULTI, retRegs[0], keel::as<keel::u16>(nret));
        } else {
            keel::u16 base = nextReg_;
            for (keel::usize i = 0; i < nret; ++i) {
                keel::u16 slot = tempReg();
                if (retRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, retRegs[i]);
            }
            emitAB(OpCode::RET_MULTI, base, keel::as<keel::u16>(nret));
        }
    }

    chunk_.numRegisters = keel::as<keel::u32>(peakReg_);

    for (auto [name, reg] : varRegisters_)
        chunk_.varMap.push_back(VarRegEntry{name.clone(), reg});

    chunk_.narginReg = narginReg;
    chunk_.nargoutReg = nargoutReg;
    if (const keel::u16 *vaReg = varRegisters_.get("varargin"))
        chunk_.vararginReg = *vaReg;
    if (!chunk_.returnNames.empty()) {
        if (const keel::u16 *frReg = varRegisters_.get(chunk_.returnNames[0]))
            chunk_.firstReturnReg = *frReg;
    }
    chunk_.mayUseInputname = mayInjectVars_;
    if (!chunk_.mayUseInputname) {
        for (const auto &s : chunk_.strings) {
            if (s == "inputname" || s == "eval" || s == "evalin" || s == "feval") {
                chunk_.mayUseInputname = true;
                break;
            }
        }
    }

    return keel::move(chunk_);
}

void Compiler::emitVarargoutReturn()
{
    const auto &rns = chunk_.returnNames;
    const keel::usize numFixed = rns.size() - 1;
    keel::u16 vaReg = varRegLookup("varargout");
    emitUpvalLoadRawIfShared("varargout", vaReg);
    keel::u16 fixedBase = 0;
    if (numFixed > 0) {
        fixedBase = keel::as<keel::u16>(nextReg_);
        for (keel::usize i = 0; i < numFixed; ++i) {
            keel::u16 slot = tempReg();
            keel::u16 r = varRegLookup(rns[i]);
            emitUpvalLoadRawIfShared(rns[i], r);
            if (r != slot)
                emitAB(OpCode::MOVE, slot, r);
        }
    }
    emit(Instruction::make_abcde(OpCode::RET_VARARGOUT, fixedBase,
                                 keel::as<keel::u16>(numFixed), vaReg, 0, 0));
}

keel::u16 Compiler::compileReturn(const ASTNode * /*node*/)
{
    if (!chunk_.returnNames.empty() && chunk_.returnNames.back() == "varargout") {
        emitVarargoutReturn();
        return 0;
    }
    if (chunk_.returnNames.size() <= 1) {
        if (!chunk_.returnNames.empty()) {
            keel::u16 retReg = varRegLookup(chunk_.returnNames[0]);
            emitUpvalLoadRawIfShared(chunk_.returnNames[0], retReg);
            emitA(OpCode::RET, retReg);
        } else {
            emitNone(OpCode::RET_EMPTY);
        }
    } else {
        keel::usize nret = chunk_.returnNames.size();
        keel::Array<keel::u16> retRegs;
        for (auto &name : chunk_.returnNames) {
            keel::u16 r = varRegLookup(name);
            emitUpvalLoadRawIfShared(name, r);
            retRegs.push_back(r);
        }
        bool consecutive = true;
        for (keel::usize i = 1; i < nret; ++i)
            if (retRegs[i] != retRegs[0] + i) { consecutive = false; break; }
        if (consecutive) {
            emitAB(OpCode::RET_MULTI, retRegs[0], keel::as<keel::u16>(nret));
        } else {
            keel::u16 base = nextReg_;
            for (keel::usize i = 0; i < nret; ++i) {
                keel::u16 slot = tempReg();
                if (retRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, retRegs[i]);
            }
            emitAB(OpCode::RET_MULTI, base, keel::as<keel::u16>(nret));
        }
    }
    return 0;
}

keel::u16 Compiler::compileDeleteAssign(const ASTNode *node)
{
    auto *lhs = node->children[0].get();
    if (lhs->type != NodeType::CALL || lhs->children.empty())
        throw std::runtime_error("Compiler: invalid delete assignment syntax");

    auto *target = lhs->children[0].get();
    if (target->type != NodeType::IDENTIFIER)
        throw std::runtime_error("Compiler: invalid delete target");

    keel::u16 arr = varRegWrite(target->strValue);
    emitUpvalLoadRawIfShared(target->strValue, arr);
    keel::usize nargs = lhs->children.size() - 1;

    {
        IndexContextGuard guard(*this, arr, keel::as<keel::u16>(nargs));

        if (nargs == 1) {
            keel::u16 idx = compileNode(lhs->children[1].get());
            emitAB(OpCode::INDEX_DELETE, arr, idx);
        } else if (nargs == 2) {
            keel::u16 row = compileNode(lhs->children[1].get());
            guard.setDim(1);
            keel::u16 col = compileNode(lhs->children[2].get());
            emitABC(OpCode::INDEX_DELETE_2D, arr, row, col);
        } else {
            keel::Array<keel::u16> idxRegs;
            for (keel::usize i = 0; i < nargs; ++i) {
                idxRegs.push_back(compileNode(lhs->children[1 + i].get()));
                if (i + 1 < nargs)
                    guard.setDim(keel::as<keel::u16>(i + 1));
            }
            keel::u16 base = nextReg_;
            for (keel::usize i = 0; i < nargs; ++i) {
                keel::u16 slot = tempReg();
                if (idxRegs[i] != slot)
                    emitAB(OpCode::MOVE, slot, idxRegs[i]);
            }
            emitABC(OpCode::INDEX_DELETE_ND, arr, base, keel::as<keel::u16>(nargs));
        }
    }

    emitUpvalStoreIfShared(target->strValue, arr);
    return arr;
}

// ============================================================
// Builtin ID resolution (compile-time)
// ============================================================

FastBuiltinId Compiler::resolveBuiltinId(keel::StringRef name, keel::usize nargs)
{
    if (nargs == 1) {
        if (name == "abs")   return FastBuiltinId::Abs;
        if (name == "floor") return FastBuiltinId::Floor;
        if (name == "ceil")  return FastBuiltinId::Ceil;
        if (name == "round") return FastBuiltinId::Round;
        if (name == "fix")   return FastBuiltinId::Fix;
        if (name == "sqrt")  return FastBuiltinId::Sqrt;
        if (name == "exp")   return FastBuiltinId::Exp;
        if (name == "log")   return FastBuiltinId::Log;
        if (name == "log2")  return FastBuiltinId::Log2;
        if (name == "log10") return FastBuiltinId::Log10;
        if (name == "sin")   return FastBuiltinId::Sin;
        if (name == "cos")   return FastBuiltinId::Cos;
        if (name == "tan")   return FastBuiltinId::Tan;
        if (name == "sign")  return FastBuiltinId::Sign;
        if (name == "isnan") return FastBuiltinId::IsNan;
        if (name == "isinf") return FastBuiltinId::IsInf;
    }
    if (nargs == 2) {
        if (name == "mod")   return FastBuiltinId::Mod;
        if (name == "rem")   return FastBuiltinId::Rem;
        if (name == "max")   return FastBuiltinId::Max;
        if (name == "min")   return FastBuiltinId::Min;
        if (name == "pow")   return FastBuiltinId::Pow;
        if (name == "atan2") return FastBuiltinId::Atan2;
    }
    return FastBuiltinId::None;
}

keel::String Compiler::disassemble(const BytecodeChunk &chunk)
{
    keel::String os = keel::format("=== {} (regs={} params={} returns={}) ===\n",
                                   chunk.name,
                                   (keel::i32) chunk.numRegisters,
                                   (keel::i32) chunk.numParams,
                                   (keel::i32) chunk.numReturns);

    auto opName = [](OpCode op) -> const char * {
        switch (op) {
        case OpCode::LOAD_CONST:
            return "LOAD_CONST";
        case OpCode::LOAD_EMPTY:
            return "LOAD_EMPTY";
        case OpCode::LOAD_STRING:
            return "LOAD_STRING";
        case OpCode::MOVE:
            return "MOVE";
        case OpCode::COLLAPSE:
            return "COLLAPSE";
        case OpCode::ADD:
            return "ADD";
        case OpCode::SUB:
            return "SUB";
        case OpCode::MUL:
            return "MUL";
        case OpCode::RDIV:
            return "RDIV";
        case OpCode::LDIV:
            return "LDIV";
        case OpCode::POW:
            return "POW";
        case OpCode::NEG:
            return "NEG";
        case OpCode::UPLUS:
            return "UPLUS";
        case OpCode::NOT:
            return "NOT";
        case OpCode::EMUL:
            return "EMUL";
        case OpCode::ERDIV:
            return "ERDIV";
        case OpCode::ELDIV:
            return "ELDIV";
        case OpCode::EPOW:
            return "EPOW";
        case OpCode::EQ:
            return "EQ";
        case OpCode::NE:
            return "NE";
        case OpCode::LT:
            return "LT";
        case OpCode::GT:
            return "GT";
        case OpCode::LE:
            return "LE";
        case OpCode::GE:
            return "GE";
        case OpCode::AND:
            return "AND";
        case OpCode::OR:
            return "OR";
        case OpCode::JMP:
            return "JMP";
        case OpCode::JMP_TRUE:
            return "JMP_TRUE";
        case OpCode::JMP_FALSE:
            return "JMP_FALSE";
        case OpCode::FOR_INIT:
            return "FOR_INIT";
        case OpCode::FOR_NEXT:
            return "FOR_NEXT";
        case OpCode::FOR_INIT_RANGE:
            return "FOR_INIT_RANGE";
        case OpCode::COLON:
            return "COLON";
        case OpCode::COLON3:
            return "COLON3";
        case OpCode::COLON_ALL:
            return "COLON_ALL";
        case OpCode::HORZCAT:
            return "HORZCAT";
        case OpCode::HORZCAT_APPEND:
            return "HORZCAT_APPEND";
        case OpCode::HORZCAT_APPEND_FLATTEN:
            return "HORZCAT_APPEND_FLATTEN";
        case OpCode::VERTCAT:
            return "VERTCAT";
        case OpCode::INDEX_GET:
            return "INDEX_GET";
        case OpCode::INDEX_GET_2D:
            return "INDEX_GET_2D";
        case OpCode::INDEX_SET:
            return "INDEX_SET";
        case OpCode::INDEX_SET_2D:
            return "INDEX_SET_2D";
        case OpCode::INDEX_GET_ND:
            return "INDEX_GET_ND";
        case OpCode::INDEX_SET_ND:
            return "INDEX_SET_ND";
        case OpCode::INDEX_DELETE:
            return "INDEX_DELETE";
        case OpCode::INDEX_DELETE_2D:
            return "INDEX_DELETE_2D";
        case OpCode::INDEX_DELETE_ND:
            return "INDEX_DELETE_ND";
        case OpCode::LOAD_END:
            return "LOAD_END";
        case OpCode::CALL:
            return "CALL";
        case OpCode::CALL_FLATTEN:
            return "CALL_FLATTEN";
        case OpCode::CALL_FLATTEN_MULTI:
            return "CALL_FLATTEN_MULTI";
        case OpCode::CALL_BUILTIN:
            return "CALL_BUILTIN";
        case OpCode::CTRANSPOSE:
            return "CTRANSPOSE";
        case OpCode::TRANSPOSE:
            return "TRANSPOSE";
        case OpCode::DISPLAY:
            return "DISPLAY";
        case OpCode::RET:
            return "RET";
        case OpCode::RET_EMPTY:
            return "RET_EMPTY";
        case OpCode::HALT:
            return "HALT";
        case OpCode::NOP:
            return "NOP";
        case OpCode::ASSERT_DEF:
            return "ASSERT_DEF";
        case OpCode::ADD_SS:
            return "ADD_SS";
        case OpCode::SUB_SS:
            return "SUB_SS";
        case OpCode::MUL_SS:
            return "MUL_SS";
        case OpCode::RDIV_SS:
            return "RDIV_SS";
        case OpCode::POW_SS:
            return "POW_SS";
        case OpCode::NEG_S:
            return "NEG_S";
        case OpCode::CLEAR_VAR:
            return "CLEAR_VAR";
        case OpCode::CLEAR_DYN:
            return "CLEAR_DYN";
        case OpCode::EXIST_VAR:
            return "EXIST_VAR";
        case OpCode::WHO:
            return "WHO";
        case OpCode::WHOS:
            return "WHOS";
        case OpCode::FIELD_GET:
            return "FIELD_GET";
        case OpCode::FIELD_GET_OR_CREATE:
            return "FIELD_GET_OR_CREATE";
        case OpCode::FIELD_SET:
            return "FIELD_SET";
        case OpCode::FIELD_GET_DYN:
            return "FIELD_GET_DYN";
        case OpCode::FIELD_GET_OR_CREATE_DYN:
            return "FIELD_GET_OR_CREATE_DYN";
        case OpCode::FIELD_SET_DYN:
            return "FIELD_SET_DYN";
        case OpCode::STRUCT_ELEM_FIELD_SET:
            return "STRUCT_ELEM_FIELD_SET";
        case OpCode::STRUCT_ELEM_GET_OR_CREATE:
            return "STRUCT_ELEM_GET_OR_CREATE";
        case OpCode::STRUCT_ELEM_SET:
            return "STRUCT_ELEM_SET";
        case OpCode::LVAL_UNSHARE_FIELD:
            return "LVAL_UNSHARE_FIELD";
        case OpCode::LVAL_UNSHARE_CELL:
            return "LVAL_UNSHARE_CELL";
        case OpCode::HORZCAT_APPEND_CSL:
            return "HORZCAT_APPEND_CSL";
        case OpCode::CELL_LITERAL:
            return "CELL_LITERAL";
        case OpCode::CELL_GET:
            return "CELL_GET";
        case OpCode::CELL_GET_OR_CREATE:
            return "CELL_GET_OR_CREATE";
        case OpCode::CELL_GET_2D:
            return "CELL_GET_2D";
        case OpCode::CELL_SET:
            return "CELL_SET";
        case OpCode::CELL_SET_2D:
            return "CELL_SET_2D";
        case OpCode::CELL_GET_MULTI:
            return "CELL_GET_MULTI";
        case OpCode::CELL_GET_MULTI_2D:
            return "CELL_GET_MULTI_2D";
        case OpCode::CELL_GET_ND:
            return "CELL_GET_ND";
        case OpCode::CELL_SET_ND:
            return "CELL_SET_ND";
        case OpCode::CELL_APPEND_FLATTEN:
            return "CELL_APPEND_FLATTEN";
        case OpCode::TRY_BEGIN:
            return "TRY_BEGIN";
        case OpCode::TRY_END:
            return "TRY_END";
        case OpCode::THROW:
            return "THROW";
        case OpCode::CALL_INDIRECT:
            return "CALL_INDIRECT";
        case OpCode::CALL_METHOD:
            return "CALL_METHOD";
        case OpCode::CALL_METHOD_MULTI:
            return "CALL_METHOD_MULTI";
        case OpCode::CLOSURE_MAKE:
            return "CLOSURE_MAKE";
        case OpCode::UPVAL_NEW:
            return "UPVAL_NEW";
        case OpCode::UPVAL_LOAD:
            return "UPVAL_LOAD";
        case OpCode::UPVAL_STORE:
            return "UPVAL_STORE";
        case OpCode::CALL_MULTI:
            return "CALL_MULTI";
        case OpCode::AND_SC:
            return "AND_SC";
        case OpCode::OR_SC:
            return "OR_SC";
        case OpCode::LOAD_NARGIN:
            return "LOAD_NARGIN";
        case OpCode::LOAD_NARGOUT:
            return "LOAD_NARGOUT";
        case OpCode::MATRIX_BUILD:
            return "MATRIX_BUILD";
        case OpCode::BREAK:
            return "BREAK";
        case OpCode::CONTINUE:
            return "CONTINUE";
        case OpCode::GLOBAL_DECL:
            return "GLOBAL_DECL";
        case OpCode::PERSISTENT_DECL:
            return "PERSISTENT_DECL";
        case OpCode::TO_LOGICAL_SCALAR:
            return "TO_LOGICAL_SCALAR";
        default:
            return "???";
        }
    };

    for (keel::usize i = 0; i < chunk.code.size(); ++i) {
        const auto &ins = chunk.code[i];

        // Show source location if available
        if (i < chunk.sourceMap.size() && chunk.sourceMap[i].line > 0) {
            os.append(keel::format("  L{}:{}\t", chunk.sourceMap[i].line, chunk.sourceMap[i].col));
        } else {
            os.append("  \t");
        }

        os.append(keel::format("[{}] {} a={} b={} c={} d={} e={}",
                               i,
                               opName(ins.op),
                               (keel::i32) ins.a,
                               (keel::i32) ins.b,
                               (keel::i32) ins.c,
                               ins.d,
                               (keel::i32) ins.e));

        if (ins.op == OpCode::LOAD_CONST
            && ins.d >= 0
            && keel::as<keel::usize>(ins.d) < chunk.constants.size()) {
            auto &cv = chunk.constants[ins.d];
            if (cv.isDoubleScalar())
                os.append(keel::format("  ; val={}", cv.scalarVal()));
            else
                os.append(keel::format("  ; {}", cv.debugString()));
        }
        os.push_back('\n');
    }

    return os;
}

} // namespace numkit
