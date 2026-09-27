// SPDX-License-Identifier: AGPL-3.0-or-later
//
// A file shaped like a prepared signature, for testing leht::crypto without a
// PDF: bytes, an empty hole, bytes. Shared by the crypto tests.
#pragma once

#include "leht/crypto/crypto.hpp"
#include "leht/error.hpp"
#include "edit_harness.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>

namespace leht::test {

using crypto::Bytes;
using crypto::CmsReport;
using crypto::Identity;
using crypto::SignOptions;
using crypto::TrustStore;
using ops::ByteRange;

/// A file shaped like a prepared signature: bytes, an empty hole, bytes. Not
/// a PDF -- leht::crypto never looks inside one, and this proves it.
struct Prepared {
    TempPath path;
    ByteRange range;
    std::string before, after;

    explicit Prepared(const std::string& name, std::size_t capacity = 8192,
                      std::string head = "%PDF-1.7 pretend /Contents ",
                      std::string tail = " /ByteRange [...] %%EOF\n")
        : path(name), before(std::move(head)), after(std::move(tail)) {
        const std::string hole = "<" + std::string(capacity * 2, '0') + ">";
        write_file(path.str(), before + hole + after);
        const auto b = static_cast<std::int64_t>(before.size());
        const auto h = static_cast<std::int64_t>(hole.size());
        range.v = {0, b, b + h, static_cast<std::int64_t>(after.size())};
    }

    [[nodiscard]] int open_rw() const {
        const int fd = ::open(path.str().c_str(), O_RDWR | O_CLOEXEC);
        CHECK(fd >= 0);
        return fd;
    }

    /// The DER now in the hole (zero padding stripped by the parser).
    [[nodiscard]] Bytes der() const {
        const std::string all = read_file(path.str());
        const std::string hex = all.substr(before.size() + 1,
                                           static_cast<std::size_t>(range.hole_end() -
                                                                    range.hole_begin() - 2));
        Bytes out(hex.size() / 2);
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = static_cast<std::uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
        }
        return out;
    }

    /// The signed bytes, streamed as a verifier would.
    [[nodiscard]] crypto::ContentReader content() const {
        auto data = std::make_shared<std::string>();
        const std::string all = read_file(path.str());
        *data = all.substr(0, before.size()) +
                all.substr(static_cast<std::size_t>(range.hole_end()));
        auto pos = std::make_shared<std::size_t>(0);
        return [data, pos](std::uint8_t* buf, std::size_t n) {
            const std::size_t k = std::min(n, data->size() - *pos);
            std::memcpy(buf, data->data() + *pos, k);
            *pos += k;
            return k;
        };
    }

    CmsReport verify(const TrustStore& trust) const {
        return crypto::verify_cms(der(), content(), trust);
    }
};

inline crypto::SignResult sign(const Prepared& p, const Identity& id, const SignOptions& o = {}) {
    const int fd = p.open_rw();
    try {
        const auto r = crypto::sign_prepared(fd, p.range, id, o);
        ::close(fd);
        return r;
    } catch (...) {
        ::close(fd);
        throw;
    }
}

template <typename F>
inline bool throws(F&& f) {
    try {
        f();
    } catch (const leht::Error&) {
        return true;
    }
    return false;
}

}  // namespace leht::test
