// SPDX-License-Identifier: AGPL-3.0-or-later
//
// QR codes for Smart-ID's device links, through libqrencode. Here rather
// than in the viewer so that the CLI's terminal code and the viewer's image
// are the same matrix.
#include "leht/crypto/crypto.hpp"

#include "leht/error.hpp"

#include <qrencode.h>

namespace leht::crypto {

QrCode qr_modules(const std::string& text) {
    // 8-bit mode, case kept: SK's links are case-sensitive, bit for bit.
    QRcode* code = QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_L, QR_MODE_8, 1);
    if (code == nullptr) {
        throw Error(0, "cannot make a QR code of " + std::to_string(text.size()) + " bytes");
    }
    QrCode out;
    out.size = code->width;
    const auto n = static_cast<std::size_t>(code->width) * static_cast<std::size_t>(code->width);
    out.dark.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        out.dark[i] = (code->data[i] & 1) != 0;
    }
    QRcode_free(code);
    return out;
}

}  // namespace leht::crypto
