// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Fuzz target for the viewer side of the worker protocol.
//
// After M3 the viewer never parses a PDF, but it does parse whatever
// leht-worker writes back -- and the worker is the process that just opened a
// hostile file. If that file achieved code execution inside the sandbox, the
// frames reaching the viewer are attacker-written. This decoder is the new
// trust boundary, so it gets the same treatment the PDF parser does.
//
// The input is written raw into a socket, then read back through
// Channel::recv() and every frame is decoded as its declared type -- the exact
// path the viewer takes, header parsing included.

#include "leht/ipc/channel.hpp"
#include "leht/ipc/protocol.hpp"

#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <new>
#include <system_error>

namespace {

using namespace leht::ipc;

void decode_frame(const Frame& f) {
    switch (f.type) {
    case MsgType::Hello: (void)decode_as<Hello>(f); break;
    case MsgType::Open: (void)decode_as<Open>(f); break;
    case MsgType::Authenticate: (void)decode_as<Authenticate>(f); break;
    case MsgType::Render: (void)decode_as<Render>(f); break;
    case MsgType::Cancel: (void)decode_as<Cancel>(f); break;
    case MsgType::Search: (void)decode_as<Search>(f); break;
    case MsgType::Select: (void)decode_as<Select>(f); break;
    case MsgType::Shutdown: (void)decode_as<Shutdown>(f); break;
    case MsgType::CancelSearch: (void)decode_as<CancelSearch>(f); break;
    case MsgType::HelloAck: (void)decode_as<HelloAck>(f); break;
    case MsgType::NeedsPassword: (void)decode_as<NeedsPassword>(f); break;
    case MsgType::Opened: (void)decode_as<Opened>(f); break;
    case MsgType::Outline: (void)decode_as<Outline>(f); break;
    case MsgType::Rendered: (void)decode_as<Rendered>(f); break;
    case MsgType::RenderSkipped: (void)decode_as<RenderSkipped>(f); break;
    case MsgType::PageMatches: (void)decode_as<PageMatches>(f); break;
    case MsgType::SearchDone: (void)decode_as<SearchDone>(f); break;
    case MsgType::SelectionResult: (void)decode_as<SelectionResult>(f); break;
    case MsgType::Failed: (void)decode_as<Failed>(f); break;
    case MsgType::Edit: (void)decode_as<Edit>(f); break;
    case MsgType::Save: (void)decode_as<Save>(f); break;
    case MsgType::ListAnnots: (void)decode_as<ListAnnots>(f); break;
    case MsgType::ListFields: (void)decode_as<ListFields>(f); break;
    case MsgType::Edited: (void)decode_as<Edited>(f); break;
    case MsgType::Saved: (void)decode_as<Saved>(f); break;
    case MsgType::AnnotList: (void)decode_as<AnnotList>(f); break;
    case MsgType::FieldList: (void)decode_as<FieldList>(f); break;
    case MsgType::PrepareSignature: (void)decode_as<PrepareSignature>(f); break;
    case MsgType::ListSignatures: (void)decode_as<ListSignatures>(f); break;
    case MsgType::SignaturePrepared: (void)decode_as<SignaturePrepared>(f); break;
    case MsgType::SignatureList: (void)decode_as<SignatureList>(f); break;
    case MsgType::Recognize: (void)decode_as<Recognize>(f); break;
    case MsgType::Words: (void)decode_as<Words>(f); break;
    case MsgType::ListTextPages: (void)decode_as<ListTextPages>(f); break;
    case MsgType::TextPageList: (void)decode_as<TextPageList>(f); break;
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    // Stay under the default socket buffer so one write() never blocks.
    if (size == 0 || size > (64U << 10)) {
        return 0;
    }

    try {
        auto [rx_end, tx_end] = socket_pair();
        std::size_t off = 0;
        while (off < size) {
            const ssize_t n = ::write(tx_end.get(), data + off, size - off);
            if (n <= 0) {
                return 0;
            }
            off += static_cast<std::size_t>(n);
        }
        tx_end.reset();  // EOF after the input

        Channel ch(std::move(rx_end), /*accept_fds=*/false);
        while (auto frame = ch.recv()) {
            try {
                decode_frame(*frame);
            } catch (const ProtocolError&) {
                // One bad frame; the viewer would drop the worker here, but
                // keep going to exercise the frames after it too.
            }
        }
    } catch (const ProtocolError&) {
        // Expected: malformed framing must be rejected, not crash.
    } catch (const std::system_error&) {
    } catch (const std::bad_alloc&) {
    }
    return 0;
}
