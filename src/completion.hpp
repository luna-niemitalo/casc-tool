#pragma once

#include <string>

// Shell-completion script generation for `--print-completion=<shell>` (see
// main.cpp). See completion.cpp's own top comment for why this reads
// registry::commandTable()'s real OptionSpec lists instead of hand-listing
// flags a second time.
namespace completion {

// Throws std::runtime_error (via cli::ArgError's parent) for an unsupported
// shell name.
std::string generate(const std::string& shell);

}  // namespace completion
