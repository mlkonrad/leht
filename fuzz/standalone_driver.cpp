// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Standalone fuzz driver: corpus replay plus deterministic mutation.
//
// Linked into each fuzz target when building without clang. It calls the
// target's LLVMFuzzerTestOneInput, so the same target works unchanged under
// libFuzzer, AFL++ or this driver. Far weaker than coverage guidance, but it
// runs under GCC today -- and it is what found the MuPDF 1.28.2 repair bug.

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size);


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
