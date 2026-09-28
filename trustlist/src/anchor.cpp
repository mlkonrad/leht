// SPDX-License-Identifier: AGPL-3.0-or-later
//
// The one thing Leht takes on faith about the EU trusted lists: which
// certificates may sign the List of Trusted Lists. Everything else is verified
// from here, by signature.
//
// Source: the Commission's notice in the Official Journal of the European Union,
// C/2026/1944 of 15 April 2026, "Information related to data on Member States'
// trusted lists as notified under Commission Implementing Decision (EU)
// 2015/1505 as amended by Commission Implementing Decision (EU) 2025/2164":
//
//     https://eur-lex.europa.eu/eli/C/2026/1944/oj
//
// It publishes the LOTL location and the SHA-256 digests of the six
// LOTL-signing certificates below. They were copied from that notice and each
// was matched, byte for byte, against the certificates the LOTL itself carries
// (sequence 395, 24 Sep 2026). To check them yourself, compare this file with
// the notice -- nothing else in Leht needs to be taken on trust.
//
// When the Commission changes the signing certificates it either publishes a
// "pivot" LOTL, which Leht follows by signature (see xml.cpp), or a new notice
// in the Official Journal, which it cannot: the LOTL then names an OJ URL other
// than the one below, and `leht trusted-list update` says so and keeps the list
// it has. The fix is this file, with the new notice's digests.
#include "leht/trustlist/model.hpp"

namespace leht::trustlist {

const Anchor& eu_anchor() {
    static const Anchor anchor{
        "https://ec.europa.eu/tools/lotl/eu-lotl.xml",
        "https://eur-lex.europa.eu/eli/C/2026/1944/oj",
        {
            "c0641c4f7d56c431b1c924742db7fce9c1eef7d7fd212113a2768486b3abcdc5",
            "e0a620fbb6747362bb933ac44169d676a553444716cf5f31605f12a22b8396b1",
            "df7e29360c34b2b8d6d5f40325c1d4d12c9922cecd33b7407674a74b2b3ca1e5",
            "b63d416744e7098bf9ec2caa596a93bc2468e37f8284ba65ecc061711bcbaa18",
            "236103f03a8031ae8f47f9059bf8de38564cdbfebedde4a597d50f8980aa653b",
            "d2064fdd70f6982dcc516b86d9d5c56aea939417c624b2e478c0b29de54f8474",
        },
    };
    return anchor;
}

}  // namespace leht::trustlist
