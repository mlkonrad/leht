// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

// The PDF side of signing. MuPDF only: the cryptography lives in leht::crypto,
// and the two meet at ByteRange.

#include <array>
#include <cstdint>

namespace leht::ops {

/// A signature's /ByteRange: the two spans of the file it signs, which are
/// everything except the hole holding the signature itself (/Contents, as a
/// hex string with its angle brackets).
///
///     [ offset0 length0 offset1 length1 ]   offset0 == 0
///       hole = [offset0 + length0, offset1)  -- starts '<', ends '>'
struct ByteRange {
    std::array<std::int64_t, 4> v{};

    [[nodiscard]] std::int64_t hole_begin() const { return v[0] + v[1]; }
    [[nodiscard]] std::int64_t hole_end() const { return v[2]; }
    /// Where the signed bytes end: the end of the revision the signature covers.
    [[nodiscard]] std::int64_t end() const { return v[2] + v[3]; }
};

}  // namespace leht::ops
