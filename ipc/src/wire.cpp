// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/ipc/wire.hpp"

#include <bit>
#include <cmath>
#include <limits>

namespace leht::ipc {

namespace {

template <typename T>
void put_le(std::vector<std::uint8_t>& buf, T v) {
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        buf.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

template <typename T>
T get_le(std::span<const std::uint8_t> b) {
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        v = static_cast<T>(v | static_cast<T>(static_cast<T>(b[i]) << (8 * i)));
    }
    return v;
}

void put_length(std::vector<std::uint8_t>& buf, std::size_t n) {
    if (n > std::numeric_limits<std::uint32_t>::max()) {
        throw ProtocolError("field too large to encode");
    }
    put_le(buf, static_cast<std::uint32_t>(n));
}

}  // namespace

void Writer::u8(std::uint8_t v) { buf_.push_back(v); }
void Writer::u16(std::uint16_t v) { put_le(buf_, v); }
void Writer::u32(std::uint32_t v) { put_le(buf_, v); }
void Writer::u64(std::uint64_t v) { put_le(buf_, v); }
void Writer::i32(std::int32_t v) { put_le(buf_, std::bit_cast<std::uint32_t>(v)); }
void Writer::f32(float v) { put_le(buf_, std::bit_cast<std::uint32_t>(v)); }

void Writer::str(const std::string& s) {
    put_length(buf_, s.size());
    buf_.insert(buf_.end(), s.begin(), s.end());
}

void Writer::bytes(std::span<const std::uint8_t> b) {
    put_length(buf_, b.size());
    buf_.insert(buf_.end(), b.begin(), b.end());
}

std::span<const std::uint8_t> Reader::take(std::size_t n) {
    if (n > remaining()) {
        throw ProtocolError("message truncated");
    }
    auto out = data_.subspan(pos_, n);
    pos_ += n;
    return out;
}

std::uint8_t Reader::u8() { return take(1)[0]; }
std::uint16_t Reader::u16() { return get_le<std::uint16_t>(take(2)); }
std::uint32_t Reader::u32() { return get_le<std::uint32_t>(take(4)); }
std::uint64_t Reader::u64() { return get_le<std::uint64_t>(take(8)); }
std::int32_t Reader::i32() { return std::bit_cast<std::int32_t>(u32()); }

float Reader::f32() {
    const float v = std::bit_cast<float>(u32());
    if (!std::isfinite(v)) {
        throw ProtocolError("non-finite float");
    }
    return v;
}

bool Reader::boolean() {
    const std::uint8_t v = u8();
    if (v > 1) {
        throw ProtocolError("boolean out of range");
    }
    return v == 1;
}

std::string Reader::str(std::size_t max_len) {
    const std::size_t n = u32();
    if (n > max_len) {
        throw ProtocolError("string longer than allowed");
    }
    auto b = take(n);
    return {b.begin(), b.end()};
}

std::vector<std::uint8_t> Reader::bytes(std::size_t max_len) {
    const std::size_t n = u32();
    if (n > max_len) {
        throw ProtocolError("byte field longer than allowed");
    }
    auto b = take(n);
    return {b.begin(), b.end()};
}

std::size_t Reader::count(std::size_t min_elem_bytes) {
    const std::size_t n = u32();
    if (min_elem_bytes != 0 && n > remaining() / min_elem_bytes) {
        throw ProtocolError("element count exceeds message size");
    }
    return n;
}

void Reader::finish() const {
    if (remaining() != 0) {
        throw ProtocolError("trailing bytes after message");
    }
}

}  // namespace leht::ipc
