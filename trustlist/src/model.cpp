// SPDX-License-Identifier: AGPL-3.0-or-later
#include "leht/trustlist/model.hpp"

#include "leht/ipc/wire.hpp"

#include <cstdlib>
#include <ctime>
#include <stdexcept>

namespace leht::trustlist {

namespace {

constexpr std::uint32_t kMagic = 0x4C54544C;  // "LTTL"
constexpr std::uint32_t kVersion = 1;

// Generous against today's lists (a few thousand services, ~150 countries'
// worth of text), tight against a hostile length.
constexpr std::size_t kMaxText = 64 << 10;
constexpr std::size_t kMaxCert = 64 << 10;
constexpr std::size_t kMaxServices = 100000;
constexpr std::size_t kMaxItems = 4096;
constexpr int kMaxDepth = 8;

void put_i64(ipc::Writer& w, std::int64_t v) { w.u64(static_cast<std::uint64_t>(v)); }
std::int64_t get_i64(ipc::Reader& r) { return static_cast<std::int64_t>(r.u64()); }

void put_criteria(ipc::Writer& w, const Criteria& c) {
    w.u8(static_cast<std::uint8_t>(c.assert));
    w.u8(c.unknown ? 1 : 0);
    w.u32(static_cast<std::uint32_t>(c.key_usage.size()));
    for (const auto& ku : c.key_usage) {
        w.u32(static_cast<std::uint32_t>(ku.size()));
        for (const auto& [bit, value] : ku) {
            w.str(bit);
            w.u8(value ? 1 : 0);
        }
    }
    w.u32(static_cast<std::uint32_t>(c.policy_sets.size()));
    for (const auto& set : c.policy_sets) {
        w.u32(static_cast<std::uint32_t>(set.size()));
        for (const std::string& oid : set) {
            w.str(oid);
        }
    }
    w.u32(static_cast<std::uint32_t>(c.nested.size()));
    for (const Criteria& n : c.nested) {
        put_criteria(w, n);
    }
}

Criteria get_criteria(ipc::Reader& r, int depth) {
    if (depth > kMaxDepth) {
        throw std::runtime_error("trusted list: criteria nested too deep");
    }
    Criteria c;
    const std::uint8_t a = r.u8();
    if (a > 2) {
        throw std::runtime_error("trusted list: unknown criteria assertion");
    }
    c.assert = static_cast<Criteria::Assert>(a);
    c.unknown = r.boolean();
    const auto check = [](std::size_t n) {
        if (n > kMaxItems) {
            throw std::runtime_error("trusted list: too many criteria");
        }
        return n;
    };
    c.key_usage.resize(check(r.count(4)));
    for (auto& ku : c.key_usage) {
        ku.resize(check(r.count(5)));
        for (auto& [bit, value] : ku) {
            bit = r.str(kMaxText);
            value = r.boolean();
        }
    }
    c.policy_sets.resize(check(r.count(4)));
    for (auto& set : c.policy_sets) {
        set.resize(check(r.count(4)));
        for (std::string& oid : set) {
            oid = r.str(kMaxText);
        }
    }
    const std::size_t nested = check(r.count(15));
    for (std::size_t i = 0; i < nested; ++i) {
        c.nested.push_back(get_criteria(r, depth + 1));
    }
    return c;
}

void put_status(ipc::Writer& w, const ListStatus& s) {
    w.str(s.territory);
    w.str(s.url);
    w.u8(s.verified ? 1 : 0);
    w.str(s.problem);
    put_i64(w, s.issued);
    put_i64(w, s.next_update);
    w.u32(s.sequence);
    w.u32(s.services);
}

ListStatus get_status(ipc::Reader& r) {
    ListStatus s;
    s.territory = r.str(kMaxText);
    s.url = r.str(kMaxText);
    s.verified = r.boolean();
    s.problem = r.str(kMaxText);
    s.issued = get_i64(r);
    s.next_update = get_i64(r);
    s.sequence = r.u32();
    s.services = r.u32();
    return s;
}

}  // namespace

const Phase* Service::at(std::int64_t when) const {
    for (const Phase& p : phases) {  // newest first
        if (p.since <= when) {
            return &p;
        }
    }
    return nullptr;
}

Bytes encode(const TrustedList& list) {
    ipc::Writer w;
    w.u32(kMagic);
    w.u32(kVersion);
    put_i64(w, list.built);
    put_status(w, list.lotl);
    w.u32(static_cast<std::uint32_t>(list.lists.size()));
    for (const ListStatus& s : list.lists) {
        put_status(w, s);
    }
    w.u32(static_cast<std::uint32_t>(list.services.size()));
    for (const Service& s : list.services) {
        w.u8(static_cast<std::uint8_t>(s.type));
        w.str(s.territory);
        w.str(s.provider);
        w.str(s.name);
        w.u32(static_cast<std::uint32_t>(s.certs.size()));
        for (const Bytes& c : s.certs) {
            w.bytes(c);
        }
        w.u32(static_cast<std::uint32_t>(s.phases.size()));
        for (const Phase& p : s.phases) {
            put_i64(w, p.since);
            w.u8(p.granted ? 1 : 0);
            w.u8(p.for_esig ? 1 : 0);
            w.u8(p.for_eseal ? 1 : 0);
            w.u8(p.for_web ? 1 : 0);
            w.u32(static_cast<std::uint32_t>(p.qualifications.size()));
            for (const Qualification& q : p.qualifications) {
                w.u32(q.qualifiers);
                put_criteria(w, q.criteria);
            }
        }
    }
    return std::move(w.buffer());
}

TrustedList decode(const Bytes& blob) {
    try {
        ipc::Reader r(blob);
        if (r.u32() != kMagic) {
            throw std::runtime_error("trusted list: not Leht's compact form");
        }
        if (r.u32() != kVersion) {
            throw std::runtime_error("trusted list: made by another version of Leht; update it");
        }
        TrustedList list;
        list.built = get_i64(r);
        list.lotl = get_status(r);
        const std::size_t lists = r.count(40);
        if (lists > kMaxItems) {
            throw std::runtime_error("trusted list: too many lists");
        }
        for (std::size_t i = 0; i < lists; ++i) {
            list.lists.push_back(get_status(r));
        }
        const std::size_t n = r.count(21);
        if (n > kMaxServices) {
            throw std::runtime_error("trusted list: too many services");
        }
        list.services.reserve(n);
        for (std::size_t i = 0; i < n; ++i) {
            Service s;
            const std::uint8_t type = r.u8();
            if (type > 1) {
                throw std::runtime_error("trusted list: unknown service type");
            }
            s.type = static_cast<Service::Type>(type);
            s.territory = r.str(kMaxText);
            s.provider = r.str(kMaxText);
            s.name = r.str(kMaxText);
            const std::size_t certs = r.count(4);
            if (certs > kMaxItems) {
                throw std::runtime_error("trusted list: too many certificates");
            }
            for (std::size_t k = 0; k < certs; ++k) {
                s.certs.push_back(r.bytes(kMaxCert));
            }
            const std::size_t phases = r.count(16);
            if (phases > kMaxItems) {
                throw std::runtime_error("trusted list: too much history");
            }
            for (std::size_t k = 0; k < phases; ++k) {
                Phase p;
                p.since = get_i64(r);
                p.granted = r.boolean();
                p.for_esig = r.boolean();
                p.for_eseal = r.boolean();
                p.for_web = r.boolean();
                const std::size_t quals = r.count(19);
                if (quals > kMaxItems) {
                    throw std::runtime_error("trusted list: too many qualifications");
                }
                for (std::size_t q = 0; q < quals; ++q) {
                    Qualification x;
                    x.qualifiers = r.u32();
                    x.criteria = get_criteria(r, 0);
                    p.qualifications.push_back(std::move(x));
                }
                s.phases.push_back(std::move(p));
            }
            list.services.push_back(std::move(s));
        }
        r.finish();
        return list;
    } catch (const ipc::ProtocolError& e) {
        throw std::runtime_error(std::string("trusted list: damaged (") + e.what() + ")");
    }
}

std::string default_cache_path() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    const std::string base = xdg != nullptr && *xdg != '\0' ? std::string(xdg)
                             : home != nullptr               ? std::string(home) + "/.cache"
                                                             : std::string(".cache");
    return base + "/leht/trusted-list/trusted-list.bin";
}

std::int64_t parse_time(const std::string& iso) {
    // xsd:dateTime as the lists write it: 2026-09-24T12:04:06Z, sometimes with
    // fractional seconds or an offset. Only UTC ("Z" or no zone) is accepted;
    // the lists are UTC by rule.
    std::tm tm{};
    int y = 0;
    int mo = 0;
    int d = 0;
    int h = 0;
    int mi = 0;
    int s = 0;
    if (std::sscanf(iso.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) != 6) {
        return 0;
    }
    if (y < 1970 || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || s > 60) {
        return 0;
    }
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return static_cast<std::int64_t>(timegm(&tm));
}

}  // namespace leht::trustlist
