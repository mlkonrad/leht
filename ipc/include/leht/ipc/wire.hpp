// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace leht::ipc {

/// Raised when bytes from the peer do not form a valid message.
///
/// The UI side reads what a sandboxed -- and so possibly compromised -- worker
/// wrote. Every malformed input must end here, as an ordinary exception, and
/// never as an out-of-bounds read or an unbounded allocation.
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Appends little-endian primitives to a byte buffer.
class Writer {
public:
    void u8(std::uint8_t v);
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void i32(std::int32_t v);
    void f32(float v);
    /// u32 length, then the bytes. No terminator.
    void str(const std::string& s);
    /// u32 length, then the bytes.
    void bytes(std::span<const std::uint8_t> b);

    [[nodiscard]] std::vector<std::uint8_t>& buffer() noexcept { return buf_; }

private:
    std::vector<std::uint8_t> buf_;
};

/// Reads little-endian primitives from a byte span, bounds-checking every read.
///
/// Every accessor throws ProtocolError rather than reading past the end, and
/// count() refuses element counts the remaining bytes could not possibly hold,
/// so a lying length can never drive a huge reserve().
class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> data) noexcept : data_(data) {}

    std::uint8_t u8();
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    std::int32_t i32();
    /// Rejects NaN and infinities: no field in the protocol legitimately holds one.
    float f32();
    bool boolean();
    std::string str(std::size_t max_len);
    std::vector<std::uint8_t> bytes(std::size_t max_len);

    /// Reads a u32 element count and checks that `min_elem_bytes` per element
    /// still fits in what is left.
    std::size_t count(std::size_t min_elem_bytes);

    /// Throws unless every byte has been consumed. Trailing garbage is a
    /// protocol error, not something to ignore.
    void finish() const;

    [[nodiscard]] std::size_t remaining() const noexcept { return data_.size() - pos_; }

private:
    std::span<const std::uint8_t> take(std::size_t n);

    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
};

}  // namespace leht::ipc
