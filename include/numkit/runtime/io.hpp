/// @file io.hpp
/// @ingroup group_iofun
// src/runtime/include/numkit/runtime/io.hpp
//
// Language-runtime I/O functions coupled to the Engine execution context
// (outputText sink, fid table, OpenFile).
#pragma once

#include <keel/core.h>
#include <keel/ref.h>
#include <numkit/value/value.hpp>

namespace numkit {

/// @addtogroup group_iofun
/// @{

class Engine;
}

namespace numkit::runtime {

/// @brief Formats and writes text to engine stdout or open file descriptor.
/// @param engine Reference to executing Engine instance.
/// @param args Arguments [fid, fmt, ...] or [fmt, ...].
/// @return Number of characters written.
keel::usize fprintf(::numkit::Engine &engine, keel::ArrayRef<Value> args);

/// @brief Displays value to engine standard output (`disp(v)`).
/// @param engine Reference to executing Engine instance.
/// @param args Arguments span containing value to display.
void disp(::numkit::Engine &engine, keel::ArrayRef<Value> args);

/// @brief Reads formatted data from file descriptor (`fscanf(fid, fmt, size)`).
/// @param engine Reference to executing Engine instance.
/// @param args Input arguments span [fid, fmt, size].
/// @param nargout Number of requested outputs.
/// @param outs Output spans to fill with results.
void fscanf(::numkit::Engine &engine, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs);

/// @brief Reads tabular data from open file descriptor or string (`textscan(fid, fmt)`).
/// @param engine Reference to executing Engine instance.
/// @param args Input arguments span [fid/str, fmt, ...].
/// @param nargout Number of requested outputs.
/// @param outs Output spans to fill with results.
void textscan(::numkit::Engine &engine, keel::ArrayRef<Value> args, keel::usize nargout, keel::MutArrayRef<Value> outs);


/// @}
} // namespace numkit::runtime
