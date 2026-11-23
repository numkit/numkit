// include/tree_walker.hpp
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/ref.h>
#include <keel/small_array.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/core/debugger.hpp>
#include <numkit/value/error.hpp>
#include <numkit/core/types.hpp>


namespace numkit {

class Engine;

class TreeWalker
{
public:
    explicit TreeWalker(Engine &engine);

    Value execute(const ASTNode *ast, Environment *env);

    // True when executing inside a user function (not top-level script)
    keel::i32 callDepth() const { return currentRecursionDepth_; }

    // Active call stack (parallel to C++ recursion of callUserFunction).
    // Maintained via FrameGuard RAII so it stays in sync across exception
    // unwind. Front = outermost; back = innermost. Used by Engine's
    // callerEnv() / inputName() so builtins can introspect the stack
    // regardless of which backend is active.
    struct ActiveFrame
    {
        Environment *env = nullptr;
        // Arg names from caller's CALL site (bare identifier or empty).
        // Read by inputname(k).
        keel::Array<keel::String> callerArgNames;
    };
    const keel::Array<ActiveFrame> &activeFrames() const { return activeFrames_; }
    Environment *callerEnvAtDepth(keel::i32 n, Environment *fallback) const;
    keel::ArrayRef<keel::String> currentFrameCallerArgNames() const;
    keel::i64 currentFunctionNargin() const;
    keel::i64 currentFunctionNargout() const;
    keel::Array<CallStackFrame> captureCallStack() const;

    // ── Public callback API ───────────────────────────────────
    // Used by Engine::callFunctionHandle so that builtins (e.g.
    // cellfun, fzero, integral) can invoke a function handle from C++
    // regardless of the active backend (TW or VM). Both forms also
    // accept anonymous handles — they're stored as synthetic user
    // functions and looked up via lookupUserFunction.
    Value callHandlePublic(const Value &handle,
                            keel::ArrayRef<Value> args,
                            Environment *env);
    keel::Array<Value> callHandleMultiPublic(const Value &handle,
                                              keel::ArrayRef<Value> args,
                                              Environment *env,
                                              keel::usize nout);

    // classdef support (runs method/constructor bodies on the TreeWalker
    // regardless of the active backend; the object dispatch that calls
    // these may originate in the VM). `runClassMethod` passes args verbatim
    // (the caller already prepends `self`). `runClassCtor` pre-seeds the
    // constructor's output variable with the default-constructed instance
    // (MATLAB's implicit `obj` initialisation), then returns it.
    keel::Array<Value> runClassMethod(const UserFunction &uf, keel::ArrayRef<Value> args,
                                      keel::usize nout);
    Value runClassCtor(const UserFunction &ctor, const Value &seed,
                       keel::ArrayRef<Value> args);
    // Evaluate a standalone expression AST (used for classdef property
    // default values at registration time).
    Value evalExpressionPublic(const ASTNode *expr, Environment *env);

    // Function call — public entry points for TreeWalker user-function dispatch.
    // Optional `callNode` extracts caller's arg names for inputname(k);
    // nullptr falls back to no names.
    Value callUserFunction(const UserFunction &func, keel::ArrayRef<Value> args,
                           Environment *env,
                           const ASTNode *callNode = nullptr);
    keel::Array<Value> callUserFunctionMulti(const UserFunction &func,
                                              keel::ArrayRef<Value> args,
                                              Environment *env,
                                              keel::usize nout,
                                              const ASTNode *callNode = nullptr);

private:
    Engine &engine_;

    FlowSignal flowSignal_ = FlowSignal::NONE;
    keel::i32 currentRecursionDepth_ = 0;

    struct IndexContext
    {
        const Value *array = nullptr;
        keel::i32 dimension = 0;
        keel::i32 ndims = 1;
    };
    keel::Array<IndexContext> indexContextStack_;

    // Parallel call stack of (localEnv, argNames) — pushed on entry to
    // callUserFunction(), popped on exit (RAII). Lifetime safe because
    // each localEnv lives on the C++ stack frame of the same call.
    keel::Array<ActiveFrame> activeFrames_;

    // Unified call-stack trace (script + function frames, outermost→innermost)
    // for MException.stack and CLI tracebacks. Maintained via TraceGuard RAII.
    keel::Array<CallStackFrame> callTrace_;

    class TraceGuard
    {
    public:
        TraceGuard(keel::Array<CallStackFrame> &trace,
                   keel::StringRef file,
                   keel::StringRef name,
                   keel::i32 line)
            : trace_(trace)
        {
            trace_.push_back(CallStackFrame{keel::String(file), keel::String(name), line});
        }
        ~TraceGuard() { trace_.pop_back(); }
        TraceGuard(const TraceGuard &) = delete;
        TraceGuard &operator=(const TraceGuard &) = delete;

    private:
        keel::Array<CallStackFrame> &trace_;
    };

    // Active heap-allocated lexical Environments (`Arc<Environment>`), pushed
    // when entering a function frame that may host nested functions so
    // `UserFunction::rawClosureEnv` resolves without self-referencing cycles.
    keel::SmallArray<keel::Arc<Environment>, 16> activeLexicalEnvs_;

    class LexicalEnvGuard
    {
    public:
        LexicalEnvGuard(keel::SmallArray<keel::Arc<Environment>, 16> &stack,
                        keel::Arc<Environment> env)
            : stack_(stack), pushed_(static_cast<bool>(env))
        {
            if (pushed_)
                stack_.push_back(keel::move(env));
        }
        ~LexicalEnvGuard()
        {
            if (pushed_)
                stack_.pop_back();
        }
        LexicalEnvGuard(const LexicalEnvGuard &) = delete;
        LexicalEnvGuard &operator=(const LexicalEnvGuard &) = delete;

    private:
        keel::SmallArray<keel::Arc<Environment>, 16> &stack_;
        bool pushed_;
    };

    keel::Arc<Environment> resolveClosureEnv(const UserFunction &uf) const;

    class FrameGuard
    {
    public:
        FrameGuard(keel::Array<ActiveFrame> &stack, Environment *env,
                   keel::Array<keel::String> argNames = {})
            : stack_(stack)
        {
            ActiveFrame f;
            f.env = env;
            f.callerArgNames = keel::move(argNames);
            stack_.push_back(keel::move(f));
        }
        ~FrameGuard() { stack_.pop_back(); }
        FrameGuard(const FrameGuard &) = delete;
        FrameGuard &operator=(const FrameGuard &) = delete;

    private:
        keel::Array<ActiveFrame> &stack_;
    };

    class IndexContextGuard
    {
    public:
        IndexContextGuard(keel::Array<IndexContext> &stack, IndexContext ctx)
            : stack_(stack)
        {
            stack_.push_back(ctx);
        }
        ~IndexContextGuard() { stack_.pop_back(); }
        IndexContextGuard(const IndexContextGuard &) = delete;
        IndexContextGuard &operator=(const IndexContextGuard &) = delete;

    private:
        keel::Array<IndexContext> &stack_;
    };

    class RecursionGuard
    {
    public:
        RecursionGuard(keel::i32 &depth, keel::i32 maxDepth)
            : depth_(depth)
        {
            if (++depth_ > maxDepth) {
                --depth_;
                auto msg = keel::format("Maximum recursion depth ({}) exceeded", maxDepth);
                throw numkit::Error(msg.c_str(), 0, 0, "", "", "numkit:recursionLimit");
            }
        }
        ~RecursionGuard() { --depth_; }
        RecursionGuard(const RecursionGuard &) = delete;
        RecursionGuard &operator=(const RecursionGuard &) = delete;

    private:
        keel::i32 &depth_;
    };

    // Display helpers
    void output(keel::StringRef s);
    void displayValue(keel::StringRef name, const Value &val);

    // Core dispatch
    // execNode is the single-value entry point: it collapses any comma-separated list
    // the expression produced (CSL of 1 -> the element; 0/>1 -> error), so every
    // operand / condition / index / assignment-RHS gets a single value and a CSL can
    // never leak. Splice contexts (call args, [..], {..}, multi-assign, varargout) call
    // execNodeExpand instead, which preserves a CSL so they can flatten it.
    Value execNode(const ASTNode *node, Environment *env);
    Value execNodeExpand(const ASTNode *node, Environment *env);
    Value execNodeInner(const ASTNode *node, Environment *env);
    Value execBlock(const ASTNode *node, Environment *env);

    // Element-wise fusion: if `node` matches a registered idiom, evaluate its
    // (side-effect-free) operands and run the fused kernel into `out`; returns
    // true on success. false = no match / kernel declined → caller uses the
    // normal path (re-evaluating the pure operands is safe).
    bool tryFusion(const ASTNode *node, Environment *env, Value &out);

    // Statements
    Value execIdentifier(const ASTNode *node, Environment *env, keel::usize nargout = 1);
    Value execAssign(const ASTNode *node, Environment *env);
    Value execMultiAssign(const ASTNode *node, Environment *env);
    Value execBinaryOp(const ASTNode *node, Environment *env);
    Value execUnaryOp(const ASTNode *node, Environment *env);
    Value execCall(const ASTNode *node, Environment *env, keel::usize nargout = 1);
    // Superclass-qualified call `lhs@Base(args)` (classdef super-call).
    // `superRef` is the SUPERCLASS_REF node, `callNode` the enclosing CALL
    // (its children[1..] are the arguments). Disambiguates super-constructor
    // (`obj@Base(args)`, lhs is a variable) from super-method
    // (`method@Base(obj,...)`, lhs is a method-name identifier).
    keel::Array<Value> execSuperCall(const ASTNode *superRef, const ASTNode *callNode,
                                     Environment *env, keel::usize nargout);
    Value execCellIndex(const ASTNode *node, Environment *env);
    // The SELECTED contents of a single-subscript cell brace-index node `c{sub}`
    // as a comma-separated list: c{:} -> all elements (column-major); c{vec} ->
    // the indexed elements; c{i} -> exactly one. Throws if the base is not a cell.
    // Shared by every CSL-consuming site (array literal, call args, cell literal).
    keel::Array<Value> cellBraceContents(const ASTNode *node, Environment *env);
    Value execFieldAccess(const ASTNode *node, Environment *env);
    Value execMatrixLiteral(const ASTNode *node, Environment *env);
    Value execCellLiteral(const ASTNode *node, Environment *env);
    Value execColonExpr(const ASTNode *node, Environment *env);
    Value execIf(const ASTNode *node, Environment *env);
    Value execFor(const ASTNode *node, Environment *env);
    Value execWhile(const ASTNode *node, Environment *env);
    Value execSwitch(const ASTNode *node, Environment *env);
    Value execFunctionDef(const ASTNode *node, Environment *env);
    Value execExprStmt(const ASTNode *node, Environment *env);
    Value execCommandCall(const ASTNode *node, Environment *env);
    Value execAnonFunc(const ASTNode *node, Environment *env);
    Value execTryCatch(const ASTNode *node, Environment *env);
    Value execDeleteAssign(const ASTNode *node, Environment *env);
    Value execGlobalPersistent(const ASTNode *node, Environment *env);

    // Assignment helpers
    // Dispatch an already-evaluated value to any lvalue target node
    // (identifier / a(i) / s.f / s.(e) / c{i}). Shared by single-target
    // ASSIGN and multi-output MULTI_ASSIGN.
    void assignLValue(const ASTNode *lhs, const Value &rhs, Environment *env);
    void execIndexedAssign(const ASTNode *lhs, const Value &rhs, Environment *env);
    void execFieldAssign(const ASTNode *lhs, const Value &rhs, Environment *env);
    void execCellAssign(const ASTNode *lhs, const Value &rhs, Environment *env);
    // Resolve any addressable lvalue *object* to a mutable storage slot,
    // creating/coercing containers along the way. Handles identifiers,
    // field chains (incl. struct-array element fields), dynamic fields
    // and cell-content slots, composing to arbitrary depth
    // (`a.b{2}.c(3) = ...`). Paren-indexing of a plain array is NOT a
    // stable slot — it is applied as the final write by execIndexedAssign
    // on the slot this returns.
    Value &resolveObjectSlot(const ASTNode *node, Environment *env);
    Value &resolveCellSlot(const ASTNode *node, Environment *env);
    Value &resolveFieldLValue(const ASTNode *node, Environment *env);

    // Object model (object_model.md): property access via class hooks.
    Value objectPropGet(const Value &obj, keel::StringRef name, Environment *env);
    void objectPropSet(Value &objSlot, keel::StringRef name, const Value &rhs,
                       Environment *env);

    // Indexing helpers
    bool tryResolveScalarIndex(const ASTNode *indexExpr,
                               const Value &array,
                               keel::i32 dim,
                               keel::i32 ndims,
                               Environment *env,
                               keel::usize &outIdx);
    keel::Array<keel::usize> resolveIndex(
        const ASTNode *indexExpr, const Value &array, keel::i32 dim, keel::i32 ndims, Environment *env);
    Value execIndexAccess(const Value &var, const ASTNode *callNode, Environment *env);
    keel::Array<Value> execCallMulti(const ASTNode *node, Environment *env, keel::usize nout);
    Value callFuncHandle(const Value &handle, keel::ArrayRef<Value> args, Environment *env,
                         const ASTNode *callNode = nullptr);
    keel::Array<Value> callFuncHandleMulti(const Value &handle,
                                            keel::ArrayRef<Value> args,
                                            Environment *env,
                                            keel::usize nout,
                                            const ASTNode *callNode = nullptr);

    // Utilities
    bool isKnownFunction(keel::StringRef name) const;

    // ── Debugger ──────────────────────────────────────────
    keel::String topLevelName_ = keel::String::from("<script>");
    DebugController *debugCtl();
};

} // namespace numkit
