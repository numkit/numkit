// include/bytecode.hpp
#pragma once

#include <keel/array.h>
#include <keel/core.h>
#include <keel/hash_map.h>
#include <keel/hash_set.h>
#include <keel/smart_ptr.h>
#include <keel/string.h>
#include <numkit/value/value.hpp>
#include <cstdint>

namespace numkit {

enum class OpCode : keel::u8 {
    // ── Data movement ────────────────────────────────────────
    LOAD_CONST,   // dst, constIdx          R[dst] = constants[constIdx]
    LOAD_EMPTY,   // dst                    R[dst] = empty
    LOAD_STRING,  // dst, strIdx            R[dst] = strings[strIdx]
    MOVE,         // dst, src               R[dst] = R[src]
    // Collapse a comma-separated list reaching a single-value context: R[dst] =
    // collapseCsl(R[src]) (CSL of 1 -> the elem; 0/>1 -> error; non-CSL passes through).
    // Inserted by the compiler dataflow pass everywhere a CSL-tainted register feeds a
    // single-value sink. See dev-docs/memory/csl_first_class.md.
    COLLAPSE,     // dst, src               R[dst] = collapseCsl(R[src])
    LOAD_END,     // dst, arrReg, dim       R[dst] = size(R[arrReg], dim)
    COLON_ALL,    // dst                    R[dst] = <colon-all marker>
    LOAD_NARGIN,  // [reserved] compiler injects nargin via varMap instead
    LOAD_NARGOUT, // [reserved] compiler injects nargout via varMap instead

    // ── Arithmetic ───────────────────────────────────────────
    ADD,   // dst, a, b              R[dst] = R[a] + R[b]
    SUB,   // dst, a, b              R[dst] = R[a] - R[b]
    MUL,   // dst, a, b              R[dst] = R[a] * R[b]
    RDIV,  // dst, a, b              R[dst] = R[a] / R[b]
    LDIV,  // dst, a, b              R[dst] = R[a] \ R[b]
    POW,   // dst, a, b              R[dst] = R[a] ^ R[b]
    NEG,   // dst, src               R[dst] = -R[src]
    UPLUS, // dst, src               R[dst] = +R[src]

    // ── Element-wise arithmetic ──────────────────────────────
    EMUL,  // dst, a, b              R[dst] = R[a] .* R[b]
    ERDIV, // dst, a, b              R[dst] = R[a] ./ R[b]
    ELDIV, // dst, a, b              R[dst] = R[a] .\ R[b]
    EPOW,  // dst, a, b              R[dst] = R[a] .^ R[b]

    // ── Comparison ───────────────────────────────────────────
    EQ, // dst, a, b              R[dst] = R[a] == R[b]
    NE, // dst, a, b              R[dst] = R[a] ~= R[b]
    LT, // dst, a, b              R[dst] = R[a] < R[b]
    GT, // dst, a, b              R[dst] = R[a] > R[b]
    LE, // dst, a, b              R[dst] = R[a] <= R[b]
    GE, // dst, a, b              R[dst] = R[a] >= R[b]

    // ── Logical ──────────────────────────────────────────────
    AND,               // dst, a, b              R[dst] = R[a] & R[b]
    OR,                // dst, a, b              R[dst] = R[a] | R[b]
    NOT,               // dst, src               R[dst] = ~R[src]
    TO_LOGICAL_SCALAR, // dst, src               R[dst] = logicalScalar(R[src]) (for && / ||)
    AND_SC,            // [reserved] && short-circuit — compiler uses TO_LOGICAL_SCALAR + JMP_FALSE
    OR_SC,             // [reserved] || short-circuit — compiler uses TO_LOGICAL_SCALAR + JMP_TRUE

    // ── Control flow ─────────────────────────────────────────
    JMP,       // offset(int16)          unconditional jump
    JMP_TRUE,  // reg, offset(int16)     jump if R[reg] != 0
    JMP_FALSE, // reg, offset(int16)     jump if R[reg] == 0

    // ── For-loop ─────────────────────────────────────────────
    FOR_INIT,       // var, range, endOffset  setup iterator from R[range]
    FOR_INIT_RANGE, // var, start, stop, endOffset, step  fused `for v = a:b` /
                    //                                    `for v = a:s:b` — skips
                    //                                    materialising the colon
                    //                                    range value (avoids the
                    //                                    8N-byte allocation for
                    //                                    `for i = 1:N` loops).
                    //                                    e=0xFF → implicit step=1.
    FOR_NEXT,       // var, backOffset        advance iterator, jump back or fall through

    // ── Function calls ───────────────────────────────────────
    CALL,          // dst, argBase, nargs, funcIdx, e=nargout  R[dst] = func(R[base..base+nargs-1])
    // First-class call with possible CSL args: a=dst, b=argBase, c=nargs, d=funcIdx,
    // e=nargout. Flattens any CSL arg register into a runtime arg vector, then runs the
    // full CALL target dispatch (user fn / ctor / method / m-file / callback / external).
    // Emitted only when an arg could be a CSL, so the hot CALL is untouched.
    CALL_FLATTEN,  // dst, argBase, nargs, funcIdx, e=nargout
    // Multi-output sibling of CALL_FLATTEN: a=outBase, b=argBase, c=nargs, d=funcIdx,
    // e=nout. Flattens any CSL arg, then runs the CALL_MULTI dispatch. ([a,b]=f(x,c{idx}))
    CALL_FLATTEN_MULTI,
    CALL_MULTI,    // dstBase, funcIdx, argBase, nargs, e=nout
    CALL_BUILTIN,  // dst, builtinId, base, nargs     inline builtin (mod, sin, etc.)
    CALL_INDIRECT, // dst, fhReg, base, nargs         R[dst] = R[fhReg](R[base..base+nargs-1])
    // [a,b,…] = R[fhReg](R[base..]): multi-output indirect (handle-variable)
    // call. a=outBase, b=fhReg, c=argBase, d=nargs, e=nout. Mirrors
    // CALL_INDIRECT's handle resolution + CALL_MULTI's nout frame-push so a
    // stored function handle can return several outputs (e.g. fmincon nonlcon
    // `[c, ceq] = nonlcon(x)`).
    CALL_INDIRECT_MULTI,
    // f(c{:}) with a VARIABLE callee (handle or indexable value):
    // a=outBase, b=fhReg, c=argBase, d=nargs, e=nout. Same as
    // CALL_INDIRECT(_MULTI) but any CSL argument is flattened into the
    // runtime arg list first (handle-callee brace-splat:
    // bugs/closed/lang/handle-call-csl-splat).
    CALL_INDIRECT_FLATTEN,
    // Fused element-wise idiom (VM fusion). a=dst, b=operandBase, c=nOps,
    // d=ruleIdx (into engine.fusionRules()), e=skip. Gather R[base..base+nOps)
    // → rule.execute; on success R[dst]=result and skip the `e` following
    // fallback instructions; on decline fall through to them — the normally
    // compiled idiom, i.e. exact per-op semantics.
    FUSE_EWISE,

    // ── Array indexing ───────────────────────────────────────
    INDEX_GET,       // dst, arr, idx          R[dst] = R[arr](R[idx])         1D
    INDEX_GET_2D,    // dst, arr, row, col     R[dst] = R[arr](R[row], R[col]) 2D
    INDEX_GET_ND,    // dst, arr, base, ndims  R[dst] = R[arr](R[base]..R[base+ndims-1])
    INDEX_SET,       // arr, idx, val          R[arr](R[idx]) = R[val]         1D
    INDEX_SET_2D,    // arr, row, col, val     R[arr](R[row], R[col]) = R[val] 2D
    INDEX_SET_ND,    // arr, base, ndims, val  R[arr](R[base]..R[base+ndims-1]) = R[val]
    INDEX_DELETE,    // arr, idx               R[arr](R[idx]) = []             1D
    INDEX_DELETE_2D, // arr, row, col          R[arr](R[row], R[col]) = []     2D
    INDEX_DELETE_ND, // arr, base, ndims       R[arr](R[base]..R[base+ndims-1]) = []

    // ── Struct field access ──────────────────────────────────
    FIELD_GET,           // dst, obj, nameIdx      R[dst] = R[obj].fields[nameIdx]
    FIELD_GET_OR_CREATE, // dst, obj, nameIdx      like FIELD_GET but auto-creates struct/field
    FIELD_SET,           // obj, nameIdx, val      R[obj].fields[nameIdx] = R[val]
    FIELD_GET_DYN,       // dst, obj, nameReg      R[dst] = R[obj].(R[nameReg])
    FIELD_GET_OR_CREATE_DYN, // dst, obj, nameReg  like FIELD_GET_DYN but auto-creates
    FIELD_SET_DYN,       // obj, nameReg, val      R[obj].(R[nameReg]) = R[val]
    // Struct-array element field write: R[obj](R[idx]).field = R[val].
    // Auto-grows when idx exceeds current numel; creates a 1×0 struct
    // array if obj is unset / not a struct.
    STRUCT_ELEM_FIELD_SET, // a=obj, b=idxReg, c=valReg, d=nameIdx
    // obj.name(args): dotted call. a=dst, b=objReg, c=argBase, d=nameIdx,
    // e=nargs. If R[objReg] is an OBJECT whose class has method `name`,
    // dispatch it (self + args); otherwise fall back to FIELD_GET + the
    // CALL_INDIRECT machinery (struct-field func handle / index). Object
    // model — see object_model.md §3.
    CALL_METHOD,
    // [a,b] = obj.name(args): dotted multi-output method call. a=outBase,
    // b=objReg, c=argBase, d=nameIdx, e=(nargs<<4)|nout (each nibble ≤15 —
    // methods with >15 args/outputs are not supported via this path).
    // Dispatches the class method with nout result slots written to
    // R[outBase..outBase+nout). Object model — see object_model.md §3.
    CALL_METHOD_MULTI,
    // classdef superclass calls inside a method/ctor body (compiled into the
    // method chunk so the body runs on the VM). Both delegate to
    // Engine::superConstruct / superMethod.
    //   CALL_SUPER_CTOR:   `obj = obj@Base(args)` — a=dst, b=objReg (seed),
    //                      c=argBase, d=baseNameIdx, e=nargs.
    //   CALL_SUPER_METHOD: `[outs] = method@Base(obj, args)` — a=outBase,
    //                      b=argBase (args incl. obj at [0]), c=nargs,
    //                      d=idx of "Base>method", e=nout.
    CALL_SUPER_CTOR,
    CALL_SUPER_METHOD,
    // Struct-array element get/set as a whole scalar struct, used by the
    // general compound-lvalue store chain (`d(i).a.b = …`, `d(i,j,k).…`).
    // Subscripts live in R[base..base+nargs-1] (column-major, any rank).
    // GET auto-grows the array and returns a 1×1 struct (empty if the
    // slot was vacant); SET writes the scalar struct back into the
    // element. Coerce an unset/empty receiver to a struct array.
    STRUCT_ELEM_GET_OR_CREATE, // a=dst, b=obj, c=base, e=nargs
    STRUCT_ELEM_SET,           // a=obj, b=base, c=nargs, e=valReg
    // Compound-lvalue temporary unshare: when R[b] has heapRefCount()==1 and
    // R[a] is not an OBJECT, clears R[b]'s field/cell slot and records an
    // unwind-safe LvalDetach so the inner INDEX_SET/FIELD_SET/CELL_SET mutates
    // R[a] in place without COW cloning.
    LVAL_UNSHARE_FIELD,        // a=childReg, b=objReg, c=isDyn(0/1), d=nameIdx, e=nameReg
    LVAL_UNSHARE_CELL,         // a=childReg, b=cellReg, c=idxBase, e=nargs

    // ── Cell array access ────────────────────────────────────
    CELL_GET,      // dst, cell, idx         R[dst] = R[cell]{R[idx]}        1D
    // Like CELL_GET but for the compound-lvalue store chain: coerces an
    // unset/empty receiver to a cell and auto-grows to fit the subscripts
    // in R[base..base+nargs-1] (any rank), returning the content slot.
    CELL_GET_OR_CREATE, // a=dst, b=cell, c=base, e=nargs
    CELL_SET,      // cell, idx, val         R[cell]{R[idx]} = R[val]        1D
    CELL_GET_2D,   // dst, cell, row, col    R[dst] = R[cell]{R[row], R[col]}
    CELL_SET_2D,   // cell, row, col, val    R[cell]{R[row], R[col]} = R[val]
    CELL_GET_MULTI,// outBase, cell, idx, nout  R[outBase..+nout] = R[cell]{R[idx]}
    // 2-D cell CSL multi-assign [a,b]=c{r,cols}: a=outBase, b=cell, c=rowSubReg,
    // d=colSubReg, e=nout. resolveIndices per dim + sub2ind, column-major; assigns
    // up to nout of the selected contents to R[outBase..].
    CELL_GET_MULTI_2D,
    CELL_GET_ND,   // dst, cell, base, ndims   R[dst] = R[cell]{R[base]..R[base+ndims-1]}
    CELL_SET_ND,   // cell, base, ndims, val   R[cell]{R[base]..R[base+ndims-1]} = R[val]
    // First-class cell-literal builder: a=cellAcc (in/out, coerced to 1x0 if unset),
    // b=elem. If R[elem] is a comma-separated list, append ALL its items (column-major);
    // otherwise append R[elem] as ONE element. Driven by the runtime VALUE (isCsl), not
    // the compile-time subscript shape, so {c{idx}} with a variable idx flattens too.
    CELL_APPEND_FLATTEN, // cellAcc, elem

    // ── Transpose ────────────────────────────────────────────
    CTRANSPOSE, // dst, src               R[dst] = R[src]' (conjugate)
    TRANSPOSE,  // dst, src               R[dst] = R[src].' (non-conjugate)

    // ── Literals / construction ──────────────────────────────
    COLON,        // dst, start, stop       R[dst] = R[start]:R[stop]
    COLON3,       // dst, start, step, stop R[dst] = R[start]:R[step]:R[stop]
    HORZCAT,      // dst, base, count       R[dst] = [R[base], ..., R[base+count-1]]
    HORZCAT_APPEND,// dst, val              R[dst] = [R[dst], R[val]]   amortised O(1)
    // Comma-separated-list expansion: R[dst] = [R[dst], structArr(0).fname, ..., structArr(N-1).fname]
    HORZCAT_APPEND_CSL, // a=dst, b=structArrReg, d=nameIdx
                  //                        when dst is a row vector / empty and val is a real
                  //                        scalar; falls back to a 2-elem horzcat otherwise
    // First-class concat builder: a=dst (in/out), b=elem. If R[elem] is a CSL,
    // horzcat-append all its items; else append the one value. Driven by the runtime
    // value, so [a, c{idx}, b] flattens for any subscript. See csl_first_class.md.
    HORZCAT_APPEND_FLATTEN, // a=dst, b=elem
    VERTCAT,      // dst, base, count       R[dst] = [R[base]; ...; R[base+count-1]]
    // dst = [dst; val] — the vertical twin of HORZCAT_APPEND: scalar
    // fast path grows a {n,1} column in place (appendScalarCol);
    // anything else falls back to the generic two-element vertcat.
    // The accumulator operand of large column literals
    // (bugs/closed/core/register-exhaustion-no-fallback).
    VERTCAT_APPEND, // a=dst, b=val
    MATRIX_BUILD, // [reserved] compiler uses HORZCAT/VERTCAT instead
    CELL_LITERAL, // dst, base, count       {R[base]..R[base+count-1]}

    // ── Display ──────────────────────────────────────────────
    DISPLAY, // reg, nameIdx           print "name = value"

    // ── Return / flow signals ────────────────────────────────
    RET,       // reg                    return R[reg]
    RET_MULTI, // base, count            return R[base..base+count-1]
    RET_EMPTY, //                        return empty
    // varargout return: a=fixedBase, b=numFixed, c=varargoutReg. Returns the
    // numFixed leading fixed outputs (R[fixedBase..]) followed by the elements
    // of the varargout cell R[varargoutReg] — a DYNAMIC return count
    // (numFixed + numel(cell)), so a `function varargout = f(...)` can return
    // however many values the caller's nargout asks for.
    RET_VARARGOUT,
    BREAK,     // [reserved] compiler uses JMP + NOP(flag) instead
    CONTINUE,  // [reserved] compiler uses JMP + NOP(flag) instead

    // ── Error handling ───────────────────────────────────────
    TRY_BEGIN, // catchOffset, exReg     setup try, on catch: R[exReg] = exception
    TRY_END,   //                        cleanup try block
    THROW,     // reg                    error(R[reg])

    // ── Scope ────────────────────────────────────────────────
    GLOBAL_DECL,     // [reserved] compiler writes to chunk.globalNames instead
    PERSISTENT_DECL, // [reserved] compiler writes to chunk.globalNames instead
    CLOSURE_MAKE,    // dst, base, count       R[dst] = R[base] (FUNC_HANDLE) with captures R[base+1..base+count-1]
    UPVAL_NEW,       // a=boxReg, b=initReg    R[a] = makeUpvalueBox(b != kInvalidReg ? R[b] : Value())
    UPVAL_LOAD,      // a=dst, b=boxReg, c=mode, d=nameIdx  c=0: checked, c=1: raw, c=2: take
    UPVAL_STORE,     // a=boxReg, b=srcReg     asUpvalueBox(R[a])->value = R[b]
    CLEAR_VAR,       // reg                    R[reg] = empty (clear variable)
    CLEAR_DYN,       // nameReg                lookup name in varMap, clear register
    EXIST_VAR,       // dst, nameReg, filterReg  check name in varMap/functions → 0/1/5
    //                           filterReg=0: no filter; else R[filterReg] = 'var'/'builtin'
    WHO,  // base, count            list variables (count=0: all, else R[base..base+count-1])
    WHOS, // base, count            list variables with details (same)

    // ── Utility ──────────────────────────────────────────────
    NOP,        //                        no-op (patching, alignment)
    ASSERT_DEF, // reg, nameIdx           throw if R[reg] is unset (undefined variable)

    // ── Scalar-specialized (compiler guarantees double-scalar operands) ──
    ADD_SS,  // dst, a, b   R[dst] = R[a].scalar + R[b].scalar  (no type check)
    SUB_SS,  // dst, a, b
    MUL_SS,  // dst, a, b
    RDIV_SS, // dst, a, b
    POW_SS,  // dst, a, b   (includes integer-exponent fast paths)
    NEG_S,   // dst, src    R[dst] = -R[src].scalar

    // ── Fused multiply-add/sub (loop opt #2): R[a] = R[b] ± R[c]*R[e] ──
    // Collapses sum-of-products (MAC) chains — filters, polynomials, dot
    // products — into one opcode. Fast path is all-scalar (two-step double:
    // prod = c*e; a = b ± prod, so bit-identical to the unfused MUL+ADD/SUB).
    // Non-scalar operands fall back to the matching product then sum/diff.
    MULADD,  // a=dst, b=acc, c, e   R[a] = R[b] + R[c]*R[e]   (* matmul fallback)
    MULSUB,  // a=dst, b=acc, c, e   R[a] = R[b] - R[c]*R[e]
    EMULADD, // a=dst, b=acc, c, e   R[a] = R[b] + R[c].*R[e]
    EMULSUB, // a=dst, b=acc, c, e   R[a] = R[b] - R[c].*R[e]

    // ── End marker ───────────────────────────────────────────
    HALT, //                        stop execution
};

inline constexpr keel::u16 kInvalidReg = 0xFFFF;

// ============================================================
// Instruction: fixed 16-byte wide encoding
// ============================================================
//
//  Byte:  0       1       2..3    4..5    6..7    8..9    10..11  12..15
//        [opcode] [ pad ] [  a  ] [  b  ] [  c  ] [  e  ] [pad2 ] [  d (int32) ]
//
//  a, b, c, e — 16-bit register indices (0..65534; 0xFFFF = kInvalidReg) or immediates
//  d          — signed 32-bit jump offset or constant/string/func pool index
//
struct Instruction
{
    OpCode op = OpCode::HALT;
    keel::u8 pad = 0;
    keel::u16 a = 0; // dst / reg
    keel::u16 b = 0; // src1 / base
    keel::u16 c = 0; // src2 / nargs
    keel::u16 e = 0; // 5th operand (nout for CALL_MULTI, etc.)
    keel::u16 pad2 = 0;
    keel::i32 d = 0;  // offset / constIdx / funcIdx / nameIdx

    Instruction() = default;

    static Instruction make_abcde(OpCode op, keel::u16 a, keel::u16 b, keel::u16 c, keel::i32 d, keel::u16 e)
    {
        Instruction i;
        i.op = op;
        i.a = a;
        i.b = b;
        i.c = c;
        i.d = d;
        i.e = e;
        return i;
    }
    static Instruction make_abc(OpCode op, keel::u16 a, keel::u16 b, keel::u16 c)
    {
        Instruction i;
        i.op = op;
        i.a = a;
        i.b = b;
        i.c = c;
        return i;
    }
    static Instruction make_abd(OpCode op, keel::u16 a, keel::u16 b, keel::i32 d)
    {
        Instruction i;
        i.op = op;
        i.a = a;
        i.b = b;
        i.d = d;
        return i;
    }
    static Instruction make_ad(OpCode op, keel::u16 a, keel::i32 d)
    {
        Instruction i;
        i.op = op;
        i.a = a;
        i.d = d;
        return i;
    }
    static Instruction make_a(OpCode op, keel::u16 a)
    {
        Instruction i;
        i.op = op;
        i.a = a;
        return i;
    }
    static Instruction make_d(OpCode op, keel::i32 d)
    {
        Instruction i;
        i.op = op;
        i.d = d;
        return i;
    }
    static Instruction make_none(OpCode op)
    {
        Instruction i;
        i.op = op;
        return i;
    }
};

static_assert(sizeof(Instruction) == 16, "Instruction must be 16 bytes");

// ============================================================
// Source location for debugging / error reporting
// ============================================================
struct SourceLoc
{
    keel::u16 line = 0;
    keel::u16 col = 0;
};

struct VarRegEntry
{
    keel::String name;
    keel::u16 reg = 0;

    VarRegEntry clone(keel::Allocator *alloc = nullptr) const
    {
        return VarRegEntry{name.clone(alloc), reg};
    }
};

// ============================================================
// BytecodeChunk: compiled function or script
// ============================================================
struct BytecodeChunk
{
    keel::Array<Instruction> code;

    // Constant pools
    keel::Array<Value> constants;       // numeric/complex constants
    keel::Array<keel::String> strings;  // string constants + field/variable/function names

    // Metadata
    keel::String name;           // function name or "<script>"
    keel::String parentFuncName; // enclosing function name for nested functions
    keel::u32 numRegisters = 0;  // total registers needed (0..65535)
    keel::u16 numParams = 0;
    keel::u16 numReturns = 0;
    keel::Array<keel::String> paramNames;
    keel::Array<keel::String> returnNames;

    // Closure support: indices of captured variables from parent scope
    keel::Array<keel::u16> capturedRegisters;

    // Variable name → register mapping (for exporting to environment after execution)
    keel::Array<VarRegEntry> varMap;

    // Names of variables the compiler emits a write for (STORE, param intro,
    // for-loop var, catch var, global/persistent decl, etc). Reads alone do
    // not populate this set. Used by DebugWorkspace::names() to decide if a
    // built-in (pi, eps, …) should be visible — MATLAB only lists built-ins
    // in `whos` once they've been shadowed by an assignment.
    keel::HashSet<keel::String> assignedVars;

    // Global variable names this chunk routes through globalsEnv_: this chunk's
    // own `global X` declarations PLUS the base workspace's inherited globals
    // (seeded at compile so `gv = 5` without a re-declaration still writes the
    // global). Used by the import/export paths.
    keel::Array<keel::String> globalNames;

    // Just this chunk's OWN top-level `global X` declarations (a subset of
    // globalNames, excluding the inherited seed). Engine::runOneChunk uses it to
    // record base-workspace membership and seed a freshly declared global as [].
    // Excluding the inherited seed is what keeps `clear global g` from being
    // resurrected by a later chunk that merely inherited g.
    keel::Array<keel::String> ownGlobalDecls;

    // Function-scoped `persistent` variable names (isolated per function in
    // Engine::persistentStore_, completely separate from globalsEnv_).
    keel::Array<keel::String> persistentNames;

    // Hidden trailing upvalue-box parameter registers for nested functions.
    keel::Array<keel::u16> upvalueParamRegs;
    // Map of variable name -> register holding its shared UpvaluePayload box.
    keel::HashMap<keel::String, keel::u16> upvalueBoxRegs;

    // Per-CALL-site arg names — populated by the compiler for each
    // CALL / CALL_MULTI it emits. Key = index into `code` of the CALL
    // instruction. Value = list of arg names; entry i is the name of
    // the i-th arg if it was a bare identifier at the call site,
    // empty string otherwise. Used by `inputname(k)` to introspect
    // the caller's call site. Sparse on purpose — only populated for
    // calls that have at least one bare-identifier arg.
    keel::HashMap<keel::u32, keel::Array<keel::String>> callSiteArgNames;

    // Source mapping (parallel to code, same size — one entry per instruction)
    keel::Array<SourceLoc> sourceMap;

    // Original source text (shared across chunks compiled from the same source)
    keel::Arc<const keel::String> sourceCode;

    // Precomputed special register slots for O(1) pushCallFrame setup
    keel::u16 narginReg = kInvalidReg;
    keel::u16 nargoutReg = kInvalidReg;
    keel::u16 vararginReg = kInvalidReg;
    keel::u16 firstReturnReg = kInvalidReg;
    bool mayUseInputname = false;

    // m-file provenance — stamped by the engine's m-file resolver (file
    // functions), at compilation for anonymous functions and by
    // ensureClassMethodCompiled for classdef methods. Entering a chunk
    // with a KNOWN origin pushes a script-origin frame for the body, so
    // mfilename()/path resolution inside see that file/dir/fs. A known
    // but EMPTY origin masks the caller's file (command-window-born
    // functions). Not known (in-script defs, REPL evals) → no push.
    bool originKnown = false;
    keel::String originFs;
    keel::String originDir;
    keel::String originFile;

    BytecodeChunk clone(keel::Allocator *alloc = nullptr) const
    {
        BytecodeChunk cp;
        cp.code = code.clone(alloc);
        cp.constants = constants.clone(alloc);
        cp.strings = strings.clone(alloc);
        cp.name = name.clone(alloc);
        cp.parentFuncName = parentFuncName.clone(alloc);
        cp.numRegisters = numRegisters;
        cp.numParams = numParams;
        cp.numReturns = numReturns;
        cp.paramNames = paramNames.clone(alloc);
        cp.returnNames = returnNames.clone(alloc);
        cp.capturedRegisters = capturedRegisters.clone(alloc);
        cp.varMap = varMap.clone(alloc);
        cp.assignedVars = assignedVars.clone(alloc);
        cp.globalNames = globalNames.clone(alloc);
        cp.ownGlobalDecls = ownGlobalDecls.clone(alloc);
        cp.persistentNames = persistentNames.clone(alloc);
        cp.upvalueParamRegs = upvalueParamRegs.clone(alloc);
        cp.upvalueBoxRegs = upvalueBoxRegs.clone(alloc);
        cp.callSiteArgNames = callSiteArgNames.clone(alloc);
        cp.sourceMap = sourceMap.clone(alloc);
        cp.sourceCode = sourceCode;
        cp.narginReg = narginReg;
        cp.nargoutReg = nargoutReg;
        cp.vararginReg = vararginReg;
        cp.firstReturnReg = firstReturnReg;
        cp.mayUseInputname = mayUseInputname;
        cp.originKnown = originKnown;
        cp.originFs = originFs.clone(alloc);
        cp.originDir = originDir.clone(alloc);
        cp.originFile = originFile.clone(alloc);
        return cp;
    }
};

// ============================================================
// Inline scalar fast-path builtin IDs (OpCode::CALL_BUILTIN)
// Shared single source of truth between Compiler and VM.
// ============================================================
enum class FastBuiltinId : keel::i8
{
    None  = -1,
    // 1-arg scalar fast path
    Abs   = 0,
    Floor = 1,
    Ceil  = 2,
    Round = 3,
    Fix   = 4,
    Sqrt  = 5,
    Exp   = 6,
    Log   = 7,
    Log2  = 8,
    Log10 = 9,
    Sin   = 10,
    Cos   = 11,
    Tan   = 12,
    Sign  = 13,
    IsNan = 14,
    IsInf = 15,
    // 2-arg scalar fast path
    Mod   = 20,
    Rem   = 21,
    Max   = 22,
    Min   = 23,
    Pow   = 24,
    Atan2 = 25,
};

[[nodiscard]] constexpr bool isUnaryFastBuiltin(FastBuiltinId id) noexcept
{
    const keel::i8 v = static_cast<keel::i8>(id);
    return v >= static_cast<keel::i8>(FastBuiltinId::Abs)
        && v <= static_cast<keel::i8>(FastBuiltinId::IsInf);
}

[[nodiscard]] constexpr const char *fastBuiltinName(FastBuiltinId id) noexcept
{
    switch (id) {
    case FastBuiltinId::Abs:   return "abs";
    case FastBuiltinId::Floor: return "floor";
    case FastBuiltinId::Ceil:  return "ceil";
    case FastBuiltinId::Round: return "round";
    case FastBuiltinId::Fix:   return "fix";
    case FastBuiltinId::Sqrt:  return "sqrt";
    case FastBuiltinId::Exp:   return "exp";
    case FastBuiltinId::Log:   return "log";
    case FastBuiltinId::Log2:  return "log2";
    case FastBuiltinId::Log10: return "log10";
    case FastBuiltinId::Sin:   return "sin";
    case FastBuiltinId::Cos:   return "cos";
    case FastBuiltinId::Tan:   return "tan";
    case FastBuiltinId::Sign:  return "sign";
    case FastBuiltinId::IsNan: return "isnan";
    case FastBuiltinId::IsInf: return "isinf";
    case FastBuiltinId::Mod:   return "mod";
    case FastBuiltinId::Rem:   return "rem";
    case FastBuiltinId::Max:   return "max";
    case FastBuiltinId::Min:   return "min";
    case FastBuiltinId::Pow:   return "pow";
    case FastBuiltinId::Atan2: return "atan2";
    case FastBuiltinId::None:  return nullptr;
    }
    return nullptr;
}

} // namespace numkit
