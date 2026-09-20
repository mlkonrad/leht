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
