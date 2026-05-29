// doctest main entry point — owns the implementation.  Every other
// `test_*.cpp` file in this directory just includes "doctest.h" and
// writes TEST_CASE blocks; doctest's registration macros take care
// of plumbing them into this main.
//
// Rule 51 / 52: this binary is part of the pre-merge gate.  Adding a
// new test file under `src/test_<name>.cpp` requires only that the
// build script (`build.ps1`) discovers it; no edit to this file.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"
