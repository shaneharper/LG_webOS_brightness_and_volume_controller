#pragma once

// The min() and max() macros defined by default in the Arduino environment can cause unexpected behaviour as they can evaluate argument expressions more than once.
#undef min
#undef max
template <typename T> inline T min(const T& a, const T& b) { return (a < b) ? a : b; }
template <typename T> inline T max(const T& a, const T& b) { return (a > b) ? a : b; }
