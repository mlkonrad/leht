// SPDX-License-Identifier: AGPL-3.0-or-later
//
// leht-worker --trusted-list: reads and verifies the EU trusted lists for the
// viewer (queue M5).
//
// The lists are XML from about thirty servers, parsed by libxml2 and xmlsec1:
// the kind of input the viewer never parses itself. The viewer fetches bytes
// and hands them over; this process runs trustlist::advance() on them, inside
// its own sandbox, and answers with what to fetch next or with the verified
// result in Leht's compact form. It never sees a document, and the document
// worker never sees XML.
#include "trustlist_worker.hpp"

#include "leht/crypto/crypto.hpp"
#include "leht/error.hpp"
#include "leht/ipc/protocol.hpp"
#include "leht/trustlist/xml.hpp"

#include <cstdio>
#include <ctime>
#include <optional>
#include <system_error>

namespace leht::worker {

using ipc::decode_as;
using ipc::Frame;
using ipc::MsgType;

int run_trustlist_worker(ipc::Channel& channel, bool sandbox, const SandboxOptions& options,
                         bool init_late) {
    // OpenSSL as in the document worker: no config file, and every algorithm
    // a list's signature can use fetched now -- a lazy fetch later is a file
    // open, and seccomp kills the process for it. Then libxml2 and xmlsec1.
    const auto prepare = [] {
        crypto::init(/*load_config=*/false);
        crypto::preload_algorithms();
        trustlist::init();
    };
    if (!init_late) {
        prepare();
    }
    if (sandbox) {
        apply_sandbox(options);
    }
    if (init_late) {
        prepare();
    }

    for (;;) {
        std::optional<Frame> frame;
        try {
            frame = channel.recv();
        } catch (const std::system_error&) {
            return 0;
        }
        if (!frame || frame->type == MsgType::Shutdown) {
            return 0;
        }
        try {
            switch (frame->type) {
            case MsgType::Hello:
                channel.send(frame->id, ipc::HelloAck{});
                break;
            case MsgType::TrustedListStep: {
                const ipc::TrustedListStep m = decode_as<ipc::TrustedListStep>(*frame);
                std::vector<trustlist::Fetched> fetched;
                fetched.reserve(m.fetched.size());
                for (const ipc::FetchedRow& f : m.fetched) {
                    fetched.push_back({f.url, f.body, f.error});
                }
                try {
                    const trustlist::Step step = trustlist::advance(
                        trustlist::eu_anchor(), m.lotl, fetched,
                        static_cast<std::int64_t>(std::time(nullptr)));
                    ipc::TrustedListProgress out;
                    out.need = step.need;
                    out.done = step.done;
                    if (step.done) {
                        if (step.result.lotl.verified) {
                            out.blob = trustlist::encode(step.result);
                        } else {
                            out.problem = step.result.lotl.problem;
                        }
                    }
                    channel.send(frame->id, out);
                } catch (const Error& e) {
                    channel.send(frame->id, ipc::Failed{e.what()});
                }
                break;
            }
            default:
                channel.send(frame->id, ipc::Failed{"the trusted-list worker only reads lists"});
                break;
            }
        } catch (const ipc::ProtocolError& e) {
            std::fprintf(stderr, "leht-worker --trusted-list: bad request: %s\n", e.what());
            return 2;
        } catch (const std::bad_alloc&) {
            channel.send(frame->id, ipc::Failed{"out of memory"});
        } catch (const std::system_error&) {
            return 0;  // a send failed: the viewer has gone
        }
    }
}

}  // namespace leht::worker
