// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Helpers for the editing tests: corpus paths, temp files, and the external
// checkers (qpdf, poppler) that give an independent opinion on what we wrote.
#pragma once

#include "test_harness.hpp"

#include <sys/wait.h>

#include <filesystem>
#include <string>

namespace leht::test {

inline std::string corpus(const char* name) {
    return std::string(LEHT_CORPUS_DIR) + "/" + name;
}

/// A path in the temp directory, removed on construction and destruction.
class TempPath {
public:
    explicit TempPath(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / ("leht_test_" + name)) {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    ~TempPath() {
        std::error_code ec;
        std::filesystem::remove(path_, ec);
    }
    TempPath(const TempPath&) = delete;
    TempPath& operator=(const TempPath&) = delete;
    [[nodiscard]] std::string str() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

/// True when `tool` is on PATH. External checkers are test-time conveniences.
inline bool have_tool(const char* tool) {
    const std::string cmd = std::string("command -v ") + tool + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

/// qpdf --check: an independent opinion on whether a file we wrote is a valid
/// PDF, precisely because qpdf is not the library that wrote it. Returns true
/// (and says so) when qpdf is absent; a skipped check is not a pass, so the
/// message is loud.
inline bool qpdf_check(const std::string& path) {
    if (!have_tool("qpdf")) {
        std::fprintf(stderr, "  SKIP qpdf --check (qpdf not installed)\n");
        return true;
    }
    // Exit 0 is clean, 3 is "warnings only" -- still a readable file. Only
    // real errors (2) fail.
    const std::string cmd = "qpdf --check '" + path + "' >/dev/null 2>&1";
    const int rc = std::system(cmd.c_str());
    return rc != -1 && WIFEXITED(rc) && (WEXITSTATUS(rc) == 0 || WEXITSTATUS(rc) == 3);
}

}  // namespace leht::test
