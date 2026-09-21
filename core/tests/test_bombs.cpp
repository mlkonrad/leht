// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Decompression-bomb regression tests.
//
// tests/corpus/bombs/image_16k.pdf is 888 bytes and declares a 16000x16000 RGB
// image -- 768 MB decoded. compress() used to decode it at full resolution and
// peak at 773 MB to produce a thumbnail. It now asks MuPDF to decode at the
// size it is about to scale to, and peaks around 53 MB.
//
// This is its own executable on purpose: ru_maxrss is a process-lifetime peak,
// so measuring it inside a shared test binary would be meaningless.

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/error.hpp"
#include "leht/ops/compress.hpp"
#include "test_harness.hpp"

#include <sys/resource.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

std::string bomb() {
    return std::string(LEHT_CORPUS_DIR) + "/bombs/image_16k.pdf";
}

/// Peak resident set size of this process so far, in MB.
long peak_rss_mb() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_maxrss / 1024;  // ru_maxrss is in KB on Linux
}

// AddressSanitizer maps shadow memory and pads every allocation, so absolute
// RSS under ASan says nothing about Leht. The memory bound only applies to an
// uninstrumented build; under ASan the test still checks the bomb completes.
#if defined(__SANITIZE_ADDRESS__)
constexpr bool kMeasureMemory = false;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kMeasureMemory = false;
#else
constexpr bool kMeasureMemory = true;
#endif
#else
constexpr bool kMeasureMemory = true;
#endif

/// The old code peaked at 773 MB here. 256 MB is loose enough not to flake on
/// allocator variation, and far below the old figure, so a regression to
/// full-resolution decoding cannot slip under it.
constexpr long kPeakBudgetMb = 256;

void bomb_compresses_within_memory_budget() {
    if (!fs::exists(bomb())) {
        std::printf("      SKIP: %s missing, run tests/corpus/generate.sh\n",
                    bomb().c_str());
        return;
    }
    leht::Context ctx;
    const std::string out =
        (fs::temp_directory_path() / "leht_bomb_out.pdf").string();

    for (const leht::ops::CompressPreset preset :
         {leht::ops::CompressPreset::Screen, leht::ops::CompressPreset::Ebook,
          leht::ops::CompressPreset::Print}) {
        leht::ops::CompressOptions options;
        options.preset = preset;
        const auto result = leht::ops::compress(ctx, bomb(), out, options);
        CHECK(result.output_bytes > 0);
    }

    const long peak = peak_rss_mb();
    std::printf("      peak RSS %ld MB (budget %ld MB%s)\n", peak, kPeakBudgetMb,
                kMeasureMemory ? "" : ", not enforced under ASan");
    if (kMeasureMemory) {
        CHECK(peak < kPeakBudgetMb);
    }

    std::error_code ec;
    fs::remove(out, ec);
}

/// The bomb's output must still be a valid, openable document.
void bomb_output_is_valid() {
    if (!fs::exists(bomb())) {
        return;
    }
    leht::Context ctx;
    const std::string out =
        (fs::temp_directory_path() / "leht_bomb_valid.pdf").string();
    leht::ops::compress(ctx, bomb(), out);

    leht::Document doc = leht::Document::open(ctx, out);
    CHECK(doc.page_count() == 1);

    std::error_code ec;
    fs::remove(out, ec);
}

}  // namespace

int main() {
    RUN(bomb_compresses_within_memory_budget);
    RUN(bomb_output_is_valid);
    return 0;
}
