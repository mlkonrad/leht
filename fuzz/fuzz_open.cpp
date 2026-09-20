// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fuzz target for the document parser.
//
// Parsing an untrusted PDF is leht's real attack surface: a PDF arrives by
// email or download and gets opened without a thought. MuPDF is C, so a parser
// bug is a memory-safety bug, and the only defence is to have already found it.
//
// LLVMFuzzerTestOneInput is libFuzzer's and AFL++'s standard entry point. Build
// with clang for coverage-guided fuzzing. Because clang is not always present,
// this file also carries a standalone driver that replays the corpus with
// deterministic mutations -- far weaker than coverage guidance, but it runs
// under GCC with ASan/UBSan today and works as a CI regression harness.

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/renderer.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

namespace {

/// One process-wide Context. Reusing it is not just faster: it also means a
/// malformed input that corrupted shared MuPDF state would poison every later
/// iteration, which is a bug worth finding rather than isolating away.
leht::Context& shared_context() {
    static leht::Context ctx;
    return ctx;
}

/// Fuzzed input can describe absurd page geometry. Rendering a 90,000 px page
/// is neither a bug nor useful work, so skip those rather than time out.
constexpr int kMaxRenderEdge = 4000;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    if (size == 0 || size > (8U << 20)) {
        return 0;  // empty and absurdly large inputs are not interesting
    }

    try {
        leht::Context& ctx = shared_context();
        leht::Document doc = leht::Document::open_memory(ctx, data, size);

        const int pages = doc.page_count();
        if (pages <= 0) {
            return 0;
        }
        (void)doc.metadata("info:Title");
        if (doc.needs_password()) {
            return 0;  // cannot get further without the password
        }

        // Rendering exercises far more of the parser than opening does.
        leht::Renderer renderer{ctx, doc};
        const leht::PageSize size_at_1 = renderer.page_size(0, 1.0F);
        if (size_at_1.width > 0 && size_at_1.height > 0 &&
            size_at_1.width < kMaxRenderEdge &&
            size_at_1.height < kMaxRenderEdge) {
            (void)renderer.render(0, 1.0F);
        }
    } catch (const leht::Error&) {
        // Expected: malformed input must be rejected, not crash.
    } catch (const std::bad_alloc&) {
        // Also acceptable: a fuzzed length field can request an absurd buffer.
    }
    return 0;
}

#ifndef LEHT_LIBFUZZER

// --------------------------------------------------------------------------
// Standalone driver: corpus replay plus deterministic mutation.
// --------------------------------------------------------------------------

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <random>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

std::vector<std::uint8_t> read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

/// Deterministic mutations, so a failure is reproducible from its seed.
void mutate(std::vector<std::uint8_t>& bytes, std::mt19937& rng) {
    if (bytes.empty()) {
        return;
    }
    switch (rng() % 4) {
        case 0: {  // flip a bit
            const std::size_t at = rng() % bytes.size();
            bytes[at] = static_cast<std::uint8_t>(bytes[at] ^ (1U << (rng() % 8)));
            break;
        }
        case 1: {  // truncate -- catches missing length checks
            bytes.resize(1 + (rng() % bytes.size()));
            break;
        }
        case 2: {  // overwrite a byte with an extreme value
            const std::size_t at = rng() % bytes.size();
            static constexpr std::uint8_t kExtremes[] = {0x00, 0xFF, 0x7F, 0x80};
            bytes[at] = kExtremes[rng() % 4];
            break;
        }
        default: {  // splice a chunk over another
            const std::size_t len = 1 + (rng() % std::min<std::size_t>(64, bytes.size()));
            const std::size_t from = rng() % (bytes.size() - len + 1);
            const std::size_t to = rng() % (bytes.size() - len + 1);
            std::memmove(bytes.data() + to, bytes.data() + from, len);
            break;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <corpus-dir> [iterations] [seed]\n"
                     "\n"
                     "Replays every file in the corpus, then runs mutated\n"
                     "variants. Build with LEHT_SANITIZE=ON or this proves\n"
                     "very little.\n",
                     argv[0]);
        return 2;
    }

    const fs::path corpus = argv[1];
    const int iterations = argc > 2 ? std::atoi(argv[2]) : 2000;
    const unsigned seed =
        argc > 3 ? static_cast<unsigned>(std::atoi(argv[3])) : 1234U;

    std::vector<std::vector<std::uint8_t>> seeds;
    if (fs::is_directory(corpus)) {
        for (const auto& entry : fs::directory_iterator(corpus)) {
            if (entry.is_regular_file()) {
                auto bytes = read_file(entry.path());
                if (!bytes.empty()) {
                    seeds.push_back(std::move(bytes));
                }
            }
        }
    } else if (fs::is_regular_file(corpus)) {
        seeds.push_back(read_file(corpus));
    }

    if (seeds.empty()) {
        std::fprintf(stderr, "no usable seed inputs in %s\n", corpus.c_str());
        return 2;
    }

    std::printf("fuzzing %zu seed input%s, %d iterations, seed %u\n",
                seeds.size(), seeds.size() == 1 ? "" : "s", iterations, seed);

    // Every seed unmutated first: the corpus must pass cleanly.
    for (const auto& bytes : seeds) {
        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());
    }

    // Save every input before testing it. A crash takes the process down with
    // no chance to report, so the artifact has to already be on disk.
    const fs::path artifact =
        fs::temp_directory_path() / "leht-fuzz-crash.bin";

    std::mt19937 rng{seed};
    for (int i = 0; i < iterations; ++i) {
        std::vector<std::uint8_t> bytes = seeds[rng() % seeds.size()];
        const int rounds = 1 + static_cast<int>(rng() % 8);
        for (int r = 0; r < rounds; ++r) {
            mutate(bytes, rng);
        }

        {
            std::ofstream out(artifact, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }
        std::printf("  iteration %d: %zu bytes\n", i, bytes.size());
        std::fflush(stdout);

        LLVMFuzzerTestOneInput(bytes.data(), bytes.size());

    }

    // Survived: remove the artifact so a stale file cannot mislead later.
    std::error_code ec;
    fs::remove(artifact, ec);
    std::printf("done: no crash, no sanitizer report\n");
    return 0;
}

#endif  // LEHT_LIBFUZZER
