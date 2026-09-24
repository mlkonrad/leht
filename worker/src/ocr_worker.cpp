// SPDX-License-Identifier: AGPL-3.0-or-later
//
// leht-worker --ocr=LANGS: reads words off page images for the viewer.
//
// A separate process from the document worker, for two reasons. Tesseract
// and its language data are a few hundred megabytes that no other document
// needs, so they are loaded only while someone is running OCR. And it sees
// pixels only -- the document worker renders the page -- so the one large
// C++ library that reads attacker-influenced images here does it inside its
// own sandbox, with nothing else in the process worth taking.
#include "ocr_worker.hpp"

#include "leht/error.hpp"
#include "leht/ipc/protocol.hpp"
#include "leht/ocr/ocr.hpp"

#include <cstdio>
#include <optional>
#include <system_error>

namespace leht::worker {

using ipc::decode_as;
using ipc::Frame;
using ipc::MsgType;

int run_ocr_worker(ipc::Channel& channel, const std::string& languages, bool sandbox,
                   const SandboxOptions& options, bool load_late) {
    const std::string datadir = ocr::default_datadir();
    std::optional<ocr::Recognizer> recognizer;
    // Every language loads now, while files can still be opened. Loading
    // after the sandbox is up (load_late, which only a test asks for) is a
    // file open under seccomp, and the process is killed for it.
    if (!load_late) {
        recognizer.emplace(languages, datadir);
    }
    if (sandbox) {
        apply_sandbox(options);
    }
    if (load_late) {
        recognizer.emplace(languages, datadir);
    }

    for (;;) {
        std::optional<Frame> frame;
        try {
            frame = channel.recv();
        } catch (const std::system_error&) {
            return 0;
        }
        if (!frame || frame->type == MsgType::Shutdown) {
            return 0;  // EOF or asked to: the viewer is done with OCR
        }
        try {
            switch (frame->type) {
            case MsgType::Hello:
                channel.send(frame->id, ipc::HelloAck{});
                break;
            case MsgType::Recognize: {
                const ipc::Recognize m = decode_as<ipc::Recognize>(*frame);
                try {
                    channel.send(frame->id, ipc::Words{recognizer->recognize(m.bitmap, m.zoom)});
                } catch (const Error& e) {
                    channel.send(frame->id, ipc::Failed{e.what()});
                }
                break;
            }
            default:
                channel.send(frame->id, ipc::Failed{"the OCR worker only reads pages"});
                break;
            }
        } catch (const ipc::ProtocolError& e) {
            std::fprintf(stderr, "leht-worker --ocr: bad request: %s\n", e.what());
            return 2;
        } catch (const std::system_error&) {
            return 0;  // a send failed: the viewer has gone
        }
    }
}

}  // namespace leht::worker
