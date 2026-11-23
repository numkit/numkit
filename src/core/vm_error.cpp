// src/core/vm_error.cpp
// VM exception dispatch, source-location enrichment, and call-stack capture.
#include "vm_internal.hpp"
#include <keel/array.h>
#include <keel/core.h>
#include <keel/format.h>
#include <keel/string.h>
#include <numkit/core/bytecode.hpp>
#include <numkit/core/debugger.hpp>
#include <numkit/core/engine.hpp>
#include <numkit/core/vm.hpp>
#include <numkit/value/error.hpp>

#include <exception>

namespace numkit {

keel::Array<CallStackFrame> VM::captureCallStack() const
{
    keel::Array<CallStackFrame> out;

    auto appendFrames = [&](const keel::Array<CallFrame> &stack) {
        for (keel::usize i = stack.size(); i-- > 0;) {
            const CallFrame &f = stack[i];
            if (!f.chunk)
                continue;
            const BytecodeChunk &ch = *f.chunk;
            keel::i32 line = 0;
            if (f.ip && !ch.code.empty()) {
                const Instruction *base = ch.code.data();
                const Instruction *end = base + ch.code.size();
                const Instruction *effIp = f.ip;
                const bool isInnermost = (i + 1 == stack.size());
                if (!isInnermost && effIp > base)
                    --effIp;
                if (effIp >= base && effIp < end) {
                    const keel::usize idx = keel::as<keel::usize>(effIp - base);
                    if (idx < ch.sourceMap.size())
                        line = ch.sourceMap[idx].line;
                }
            }
            keel::String file;
            if (ch.originKnown) {
                file = ch.originFile.clone();
            } else if (const keel::String *sf = engine_.currentScriptFile()) {
                file = sf->clone();
            }
            out.push_back(CallStackFrame{keel::move(file), ch.name.clone(), line});
        }
    };

    appendFrames(frames_);
    for (keel::usize psi = activePausedStates_.size(); psi-- > 0;) {
        if (const PausedState *ps = activePausedStates_[psi])
            appendFrames(ps->frames);
    }
    return out;
}

bool VM::dispatchTryCatch(const char *msg, const char *identifier)
{
    if (tryStack_.empty())
        return false;

    // Capture the call stack BEFORE unwinding frames to the catch handler.
    // If an inner re-entrant call already captured the deeper stack, preserve it.
    keel::Array<CallStackFrame> stack = engine_.takeLastErrorStack();
    if (stack.empty())
        stack = captureCallStack();

    TryHandler th = tryStack_.back();
    tryStack_.pop_back();

    // Unwind call frames to the handler's frame (exception may propagate across calls)
    while (frames_.size() > th.frameIndex + 1) {
        CallFrame &f = frames_.back();
        if (f.chunk) {
            exportChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *f.chunk, f.R, f.nregs,
                                             f.isScriptFrame);
        }
        // Drop the unwound frames' provenance pushes: the lowest unwound
        // frame's originDepth is the cut point — entries below it belong to
        // surviving frames or non-VM scopes (the run() builtin).
        engine_.truncateScriptOrigins(f.originDepth);
        // Pop debug frame
        if (auto *ctl = debugCtl())
            ctl->popFrame();
        // Remove try handlers from this frame
        while (!tryStack_.empty() && tryStack_.back().frameIndex >= frames_.size() - 1)
            tryStack_.pop_back();
        // Trim for-loop stack
        forStack_.resize(keel::min(forStack_.size(), f.forStackBase));
        // Cleanup registers
        for (keel::u32 i = 0; i < f.nregs; ++i)
            if (!f.R[i].isDoubleScalar() && !f.R[i].isEmpty())
                f.R[i] = Value();
        regStackTop_ = f.regBase;
        frames_.pop_back();
    }

    // Restore for-loop stack to the state at TRY_BEGIN
    if (forStack_.size() > th.forStackSize)
        forStack_.resize(th.forStackSize);

    // Set up catch context in the handler's frame
    CallFrame &frame = frames_.back();
    R_ = frame.R;
    if (frame.chunk) {
        importChunkGlobalsAndPersistents(engine_, *engine_.globalsEnv_, *frame.chunk, frame.R,
                                         frame.nregs, frame.isScriptFrame, /*seedMissing=*/false);
    }

    engine_.recordLastError(msg, identifier, stack.as_ref());
    frame.R[th.exReg] = makeExceptionStruct(msg, identifier, stack.as_ref(), engine_.mr_);
    frame.ip = th.catchIp;
    return true;
}

// Derive error context description from the opcode + operands at throw time.
// This is the VM equivalent of TreeWalker's describeNode() — zero storage overhead,
// only runs on the error path.
keel::String VM::describeInstruction(const Instruction &instr,
                                     const BytecodeChunk &chunk)
{
    switch (instr.op) {
    // Function calls
    case OpCode::CALL:
    case OpCode::CALL_FLATTEN:
    case OpCode::CALL_MULTI:
    case OpCode::CALL_FLATTEN_MULTI: {
        keel::i32 funcIdx = instr.d;
        if (funcIdx >= 0 && funcIdx < keel::as<keel::i32>(chunk.strings.size()))
            return keel::format("in call to '{}'", chunk.strings[funcIdx]);
        return keel::String::from("in function call");
    }
    case OpCode::CALL_BUILTIN: {
        const char *nm = fastBuiltinName(static_cast<FastBuiltinId>(instr.d));
        return nm ? keel::format("in call to '{}'", nm)
                  : keel::String::from("in builtin call");
    }
    case OpCode::CALL_INDIRECT:
    case OpCode::CALL_INDIRECT_FLATTEN:
    case OpCode::CALL_INDIRECT_MULTI:
        return keel::String::from("in function call");
    case OpCode::CALL_METHOD:
    case OpCode::CALL_METHOD_MULTI: {
        keel::i32 nameIdx = instr.d;
        if (nameIdx >= 0 && nameIdx < keel::as<keel::i32>(chunk.strings.size()))
            return keel::format("in method call '.{}'", chunk.strings[nameIdx]);
        return keel::String::from("in method call");
    }

    // Cell indexing
    case OpCode::CELL_GET:
    case OpCode::CELL_GET_OR_CREATE:
    case OpCode::CELL_SET:
    case OpCode::CELL_GET_2D:
    case OpCode::CELL_SET_2D:
    case OpCode::CELL_GET_MULTI:
    case OpCode::CELL_GET_ND:
    case OpCode::CELL_SET_ND:
        return keel::String::from("in cell indexing");

    // Field access
    case OpCode::FIELD_GET:
    case OpCode::FIELD_GET_OR_CREATE:
    case OpCode::FIELD_SET: {
        keel::i32 nameIdx = instr.d;
        if (nameIdx >= 0 && nameIdx < keel::as<keel::i32>(chunk.strings.size()))
            return keel::format("in field access '.{}'", chunk.strings[nameIdx]);
        return keel::String::from("in field access");
    }
    case OpCode::FIELD_GET_DYN:
    case OpCode::FIELD_GET_OR_CREATE_DYN:
    case OpCode::FIELD_SET_DYN:
        return keel::String::from("in dynamic field access");
    case OpCode::STRUCT_ELEM_FIELD_SET: {
        keel::i32 nameIdx = instr.d;
        if (nameIdx >= 0 && nameIdx < keel::as<keel::i32>(chunk.strings.size()))
            return keel::format("in struct-array element write '.{}'", chunk.strings[nameIdx]);
        return keel::String::from("in struct-array element write");
    }
    case OpCode::STRUCT_ELEM_GET_OR_CREATE:
    case OpCode::STRUCT_ELEM_SET:
        return keel::String::from("in struct-array element access");

    // Binary operators (base and scalar-specialized variants share text)
    case OpCode::ADD:  case OpCode::ADD_SS:  return keel::String::from("in operator '+'");
    case OpCode::SUB:  case OpCode::SUB_SS:  return keel::String::from("in operator '-'");
    case OpCode::MUL:  case OpCode::MUL_SS:  return keel::String::from("in operator '*'");
    case OpCode::RDIV: case OpCode::RDIV_SS: return keel::String::from("in operator '/'");
    case OpCode::LDIV: return keel::String::from("in operator '\\'");
    case OpCode::POW:  case OpCode::POW_SS:  return keel::String::from("in operator '^'");
    case OpCode::EMUL: return keel::String::from("in operator '.*'");
    case OpCode::ERDIV:return keel::String::from("in operator './'");
    case OpCode::ELDIV:return keel::String::from("in operator '.\\'");
    case OpCode::EPOW: return keel::String::from("in operator '.^'");

    // Unary operators
    case OpCode::NEG:        case OpCode::NEG_S: return keel::String::from("in unary operator '-'");
    case OpCode::NOT:        return keel::String::from("in unary operator '~'");
    case OpCode::CTRANSPOSE:
    case OpCode::TRANSPOSE:  return keel::String::from("in transpose operator");

    // Colon expressions
    case OpCode::COLON:
    case OpCode::COLON3:
        return keel::String::from("in colon expression");

    // Matrix/cell construction
    case OpCode::HORZCAT:
    case OpCode::HORZCAT_APPEND:
    case OpCode::VERTCAT:
    case OpCode::VERTCAT_APPEND:
        return keel::String::from("in matrix construction");
    case OpCode::CELL_LITERAL:
        return keel::String::from("in cell construction");
    case OpCode::CLOSURE_MAKE:
        return keel::String::from("in anonymous function creation");

    // Indexing
    case OpCode::INDEX_GET:
    case OpCode::INDEX_GET_2D:
    case OpCode::INDEX_GET_ND:
    case OpCode::INDEX_SET:
    case OpCode::INDEX_SET_2D:
    case OpCode::INDEX_SET_ND:
    case OpCode::INDEX_DELETE:
    case OpCode::INDEX_DELETE_2D:
    case OpCode::INDEX_DELETE_ND:
        return keel::String::from("in array indexing");

    default:
        return keel::String{};
    }
}

void VM::enrichError(Error &mle, const Instruction *ip, const BytecodeChunk &chunk)
{
    if (engine_.lastErrorStack().empty())
        engine_.setLastErrorStack(captureCallStack());
    keel::StringRef id = mle.identifier().empty()
                             ? keel::StringRef("numkit:error")
                             : mle.identifier().as_ref();
    engine_.recordLastError(mle.what(), id, engine_.lastErrorStack().as_ref());
    keel::usize instrIdx = keel::as<keel::usize>(ip - chunk.code.data());
    if (instrIdx < chunk.sourceMap.size() && chunk.sourceMap[instrIdx].line > 0) {
        mle.attachIfMissing(chunk.sourceMap[instrIdx].line,
                            chunk.sourceMap[instrIdx].col,
                            chunk.name,
                            describeInstruction(*ip, chunk));
    }
}

[[noreturn]] void VM::enrichAndThrow(const std::exception &ex,
                                     const Instruction *ip,
                                     const BytecodeChunk &chunk)
{
    if (engine_.lastErrorStack().empty())
        engine_.setLastErrorStack(captureCallStack());
    engine_.recordLastError(ex.what(), "numkit:error", engine_.lastErrorStack().as_ref());
    keel::usize instrIdx = keel::as<keel::usize>(ip - chunk.code.data());
    if (instrIdx < chunk.sourceMap.size() && chunk.sourceMap[instrIdx].line > 0) {
        keel::String context = describeInstruction(*ip, chunk);
        throw Error(ex.what(),
                    chunk.sourceMap[instrIdx].line,
                    chunk.sourceMap[instrIdx].col,
                    chunk.name,
                    context);
    }
    // No source location available — wrap in Error anyway for consistency
    throw Error(ex.what());
}

} // namespace numkit
