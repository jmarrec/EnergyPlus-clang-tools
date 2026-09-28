#pragma once

#include "path_format_fixer/PathFormatFixer.hpp"

#include <string>
#include <vector>

namespace llvm {
class StringRef;
}

// Runs the path-format-fixer transformation on in-memory `Code` and returns
// the rewritten source text (or `Code` unchanged if nothing matched / the
// tool failed to run).
std::string runOnCode(llvm::StringRef Code,
                      const std::vector<std::string>& FormatFunctionNames = {"EnergyPlus::format", "std::format", "fmt::format"});

