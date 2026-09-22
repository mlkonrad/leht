// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Helpers for the editing tests: corpus paths, temp files, and the external
// checkers (qpdf, poppler) that give an independent opinion on what we wrote.
#pragma once

#include "test_harness.hpp"

#include <sys/wait.h>

#include <algorithm>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

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

inline std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void write_file(const std::string& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

/// Runs `cmd` and returns its stdout.
inline std::string capture(const std::string& cmd) {
    std::string out;
    std::FILE* p = ::popen(cmd.c_str(), "r");
    if (p == nullptr) {
        return out;
    }
    char buf[4096];
    std::size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), p)) > 0) {
        out.append(buf, n);
    }
    ::pclose(p);
    return out;
}

/// Writes a PDF by hand, object by object, with a correct xref -- so a test
/// can put exactly the structure it wants to probe into a file, including
/// things ghostscript will not produce (a structure tree, a /Thumb, an
/// incremental update). Bodies are the text between "N 0 obj" and "endobj".
class PdfWriter {
public:
    void set(int num, const std::string& body) { objects_[num] = body; }

    /// An uncompressed stream object body.
    static std::string stream(const std::string& dict_entries, const std::string& data) {
        return "<< " + dict_entries + " /Length " + std::to_string(data.size()) +
               " >>\nstream\n" + data + "\nendstream";
    }

    /// The whole file as one revision.
    std::string finish(int root, const std::string& extra_trailer = "") {
        std::string out = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
        const int size = objects_.rbegin()->first + 1;
        std::map<int, std::size_t> offsets;
        for (const auto& [num, body] : objects_) {
            offsets[num] = out.size();
            out += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
        }
        xref_offset_ = out.size();
        out += "xref\n0 " + std::to_string(size) + "\n0000000000 65535 f \n";
        for (int i = 1; i < size; ++i) {
            const auto it = offsets.find(i);
            char line[32];
            if (it == offsets.end()) {
                std::snprintf(line, sizeof(line), "0000000000 65535 f \n");
            } else {
                std::snprintf(line, sizeof(line), "%010zu 00000 n \n", it->second);
            }
            out += line;
        }
        out += "trailer\n<< /Size " + std::to_string(size) + " /Root " +
               std::to_string(root) + " 0 R " + extra_trailer + " >>\nstartxref\n" +
               std::to_string(xref_offset_) + "\n%%EOF\n";
        size_ = size;
        return out;
    }

    /// Appends an incremental update to `base` (as returned by finish()),
    /// replacing or adding the objects in `changes`. The old bodies stay in the
    /// file, which is exactly what makes earlier revisions a leak.
    std::string update(const std::string& base, const std::map<int, std::string>& changes,
                       int root, const std::string& extra_trailer = "") {
        std::string out = base;
        std::map<int, std::size_t> offsets;
        for (const auto& [num, body] : changes) {
            offsets[num] = out.size();
            out += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
            size_ = std::max(size_, num + 1);
        }
        const std::size_t xref = out.size();
        out += "xref\n";
        for (const auto& [num, off] : offsets) {
            char line[64];
            std::snprintf(line, sizeof(line), "%d 1\n%010zu 00000 n \n", num, off);
            out += line;
        }
        out += "trailer\n<< /Size " + std::to_string(size_) + " /Root " +
               std::to_string(root) + " 0 R /Prev " + std::to_string(xref_offset_) + " " +
               extra_trailer + " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
        xref_offset_ = xref;
        return out;
    }

private:
    std::map<int, std::string> objects_;
    std::size_t xref_offset_ = 0;
    int size_ = 0;
};

}  // namespace leht::test
