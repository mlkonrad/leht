// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstdio>
#include <cstdlib>
#include <exception>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,     \
                         #cond);                                             \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)

#define RUN(fn)                                                              \
    do {                                                                     \
        try {                                                                \
            fn();                                                            \
            std::printf("  ok  %s\n", #fn);                                  \
        } catch (const std::exception& e) {                                  \
            std::fprintf(stderr, "FAIL %s threw: %s\n", #fn, e.what());      \
            std::exit(1);                                                    \
        }                                                                    \
    } while (0)
