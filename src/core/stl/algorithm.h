#pragma once

// Convenience include — all standard algorithms available via std:: prefix.
#include <algorithm>

// kimix::sort (pdqsort) is this project's preferred sorting entry point over
// std::sort; it is declared in stl/pdqsort.h (see the comment there for the
// benchmark behind it) and re-exposed here so "algorithm-like" includes get it.
#include "pdqsort.h" // same directory (src/core/stl/)
