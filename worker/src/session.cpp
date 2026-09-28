// SPDX-License-Identifier: AGPL-3.0-or-later
#include "session.hpp"

#include "leht/context.hpp"
#include "leht/document.hpp"
#include "leht/edit.hpp"
#include "leht/error.hpp"
#include "leht/ops/annotate.hpp"
#include "leht/ops/compress.hpp"
#include "leht/ops/crop.hpp"
#include "leht/ops/encrypt.hpp"
#include "leht/ops/info.hpp"
#include "leht/ops/ocr_layer.hpp"
#include "leht/ops/organize.hpp"
#include "leht/crypto/crypto.hpp"
#include "leht/ops/forms.hpp"
#include "leht/ops/merge.hpp"
#include "leht/ops/pages.hpp"
#include "leht/ops/sign.hpp"
#include "leht/ops/redact.hpp"
#include "leht/ops/watermark.hpp"
#include "leht/renderer.hpp"
#include "leht/text.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <exception>
#include <functional>
#include <new>
#include <system_error>
#include <thread>
#include <utility>

namespace leht::worker {

using namespace leht::ipc;

namespace {

/// The biggest bitmap worth rendering is one that still fits in a frame.
constexpr std::size_t kMaxBitmapBytes = kMaxPayload - 4096;

void flatten(const std::vector<OutlineItem>& items, int depth, std::vector<OutlineRow>& out) {
    for (const OutlineItem& item : items) {
        out.push_back(OutlineRow{depth, item.title, item.page, item.y});
        flatten(item.children, depth + 1, out);
    }
}

}  // namespace

Session::Session(Channel& channel, Context& ctx)
    : channel_(channel), ctx_(ctx), cancel_(std::make_unique<leht::Cancel>()) {}

Session::~Session() { close_document(); }

void Session::close_document() noexcept {
    // Reverse dependency order: everything below borrows the document.
    text_pages_.clear();
    renderer_.reset();
    doc_.reset();
}

void Session::reader_loop() {
    try {
        while (auto frame = channel_.recv()) {
            if (frame->type == MsgType::Cancel) {
                const std::uint64_t gen = decode_as<ipc::Cancel>(*frame).generation;
                latest_generation_.store(gen);
                // Abort the in-flight render if it is older. See run() for why
                // this ordering cannot miss one.
                if (in_flight_generation_.load() < gen) {
                    cancel_->request();
                }
                continue;
            }
            if (frame->type == MsgType::CancelSearch) {
                const std::uint64_t epoch = decode_as<CancelSearch>(*frame).epoch;
                std::uint64_t seen = search_cancel_epoch_.load();
                while (seen < epoch && !search_cancel_epoch_.compare_exchange_weak(seen, epoch)) {
                }
                continue;
            }
            const bool shutdown = frame->type == MsgType::Shutdown;
            {
                const std::scoped_lock lock(mutex_);
                queue_.push_back(std::move(*frame));
            }
            ready_.notify_one();
            if (shutdown) {
                return;
            }
        }
    } catch (const std::exception& e) {
        // The viewer is the trusted side; garbage from it means something is
        // badly wrong. Stop rather than guess.
        std::fprintf(stderr, "leht-worker: %s\n", e.what());
        reader_exit_ = 2;
    }
    {
        const std::scoped_lock lock(mutex_);
        closed_ = true;
    }
    ready_.notify_one();
}

int Session::run() {
    std::thread reader([this] { reader_loop(); });
    int exit_code = 0;

    for (;;) {
        Frame frame;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return !queue_.empty() || closed_; });
            if (queue_.empty()) {
                break;  // EOF: the viewer is gone
            }
            frame = std::move(queue_.front());
            queue_.pop_front();
        }
        if (frame.type == MsgType::Shutdown) {
            break;
        }
        try {
            dispatch(frame);
        } catch (const ProtocolError& e) {
            std::fprintf(stderr, "leht-worker: bad request: %s\n", e.what());
            exit_code = 2;
            break;
        } catch (const std::system_error&) {
            break;  // a send failed: the viewer has gone, so there is no one to serve
        }
    }

    channel_.shutdown();  // unblocks the reader if it is still in recv()
    reader.join();
    return exit_code != 0 ? exit_code : reader_exit_;
}

void Session::dispatch(Frame& frame) {
    const std::uint64_t id = frame.id;
    try {
        switch (frame.type) {
        case MsgType::Hello:
            channel_.send(id, HelloAck{});
            return;
        case MsgType::Open:
            on_open(id, frame);
            return;
        case MsgType::Authenticate:
            on_authenticate(id, decode_as<Authenticate>(frame));
            return;
        case MsgType::Render:
            on_render(id, decode_as<Render>(frame));
            return;
        case MsgType::Search:
            on_search(id, decode_as<Search>(frame));
            return;
        case MsgType::Select:
            on_select(id, decode_as<Select>(frame));
            return;
        case MsgType::Edit:
            on_edit(id, decode_as<Edit>(frame));
            return;
        case MsgType::Save:
            on_save(id, frame);
            return;
        case MsgType::ListAnnots:
            (void)decode_as<ListAnnots>(frame);
            on_list_annots(id);
            return;
        case MsgType::ListFields:
            (void)decode_as<ListFields>(frame);
            on_list_fields(id);
            return;
        case MsgType::ListTextPages:
            (void)decode_as<ListTextPages>(frame);
            on_list_text_pages(id);
            return;
        case MsgType::PrepareSignature:
            on_prepare_signature(id, frame);
            return;
        case MsgType::ListSignatures:
            on_list_signatures(id, decode_as<ListSignatures>(frame));
            return;
        case MsgType::ListRevocationQueries:
            on_list_revocation_queries(id, decode_as<ListRevocationQueries>(frame));
            return;
        case MsgType::AddValidationData:
            on_add_validation_data(id, frame);
            return;
        case MsgType::PrepareDocTimestamp:
            on_prepare_doc_timestamp(id, frame);
            return;

        case MsgType::Compress:
            on_compress(id, frame);
            return;
        case MsgType::ExtractPages:
            on_extract_pages(id, frame);
            return;
        case MsgType::MergeBegin:
            on_merge_begin(id, decode_as<MergeBegin>(frame));
            return;
        case MsgType::MergeAdd:
            on_merge_add(id, frame);
            return;
        case MsgType::MergeFinish:
            on_merge_finish(id, frame);
            return;
        case MsgType::Protect:
            on_protect(id, frame);
            return;
        case MsgType::GetInfo:
            (void)decode_as<GetInfo>(frame);
            on_get_info(id);
            return;
        default:
            throw ProtocolError("not a request type");
        }
    } catch (const ProtocolError&) {
        throw;
    } catch (const std::bad_alloc&) {
        // Hitting RLIMIT_AS is how a decompression or allocation bomb ends up
        // here: an ordinary failure of this request, not of the worker.
        channel_.send(id, Failed{"out of memory"});
    } catch (const std::exception& e) {
        channel_.send(id, Failed{e.what()});
    }
}

void Session::on_open(std::uint64_t id, Frame& frame) {
    const Open m = decode_as<Open>(frame);
    close_document();
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to Open"});
        return;
    }
    const std::string hint = m.name.empty() ? std::string("pdf") : m.name;
    doc_ = std::make_unique<Document>(Document::open_fd(ctx_, frame.fd.release(), hint));
    if (doc_->needs_password()) {
        channel_.send(id, NeedsPassword{false});
        return;
    }
    finish_open(id);
}

void Session::on_authenticate(std::uint64_t id, const Authenticate& m) {
    if (!doc_) {
        channel_.send(id, Failed{"no document is open"});
        return;
    }
    if (!doc_->needs_password()) {
        finish_open(id);  // already unlocked; answer as if it just opened
        return;
    }
    if (doc_->authenticate(m.password)) {
        finish_open(id);
    } else {
        channel_.send(id, NeedsPassword{true});
    }
}

std::vector<PageSize> Session::base_sizes() {
    std::vector<PageSize> sizes;
    const int pages = doc_->page_count();
    sizes.reserve(static_cast<std::size_t>(pages));
    for (int p = 0; p < pages; ++p) {
        try {
            sizes.push_back(renderer_->page_size(p, 1.0F));
        } catch (const Error&) {
            sizes.push_back(PageSize{});  // a broken page, not a broken document
        }
    }
    return sizes;
}

void Session::finish_open(std::uint64_t id) {
    renderer_ = std::make_unique<Renderer>(ctx_, *doc_);
    text_pages_.clear();
    channel_.send(id, Opened{base_sizes()});

    Outline outline;
    try {
        flatten(doc_->outline(), 0, outline.rows);
    } catch (const Error&) {
        outline.rows.clear();  // a damaged outline is optional; the pages are not
    }
    channel_.send(id, outline);
}

void Session::on_render(std::uint64_t id, const Render& m) {
    const RenderSkipped skipped{m.page, m.generation};
    if (!renderer_) {
        channel_.send(id, skipped);
        return;
    }

    // Publish what is in flight BEFORE checking staleness, and rearm the token
    // only then. The reader stores the new generation before reading ours, so
    // either we see its generation here and skip, or it sees ours and aborts
    // the render -- a cancel cannot fall between the two.
    in_flight_generation_.store(m.generation);
    cancel_->reset();
    struct Clear {
        std::atomic<std::uint64_t>& g;
        ~Clear() { g.store(kNone); }
    } clear{in_flight_generation_};

    if (m.generation < latest_generation_.load()) {
        channel_.send(id, skipped);
        return;
    }

    try {
        const PageSize size = renderer_->page_size(m.page, m.zoom, m.rotation);
        const auto bytes = static_cast<std::size_t>(size.width) *
                           static_cast<std::size_t>(size.height) * 3U;
        if (size.width <= 0 || size.height <= 0 || bytes > kMaxBitmapBytes) {
            channel_.send(id, skipped);
            return;
        }
        auto bmp = renderer_->render(m.page, m.zoom, m.rotation, cancel_.get());
        if (!bmp) {
            channel_.send(id, skipped);  // cancelled
            return;
        }
        Rendered out;
        out.page = m.page;
        out.zoom = m.zoom;
        out.rotation = m.rotation;
        out.generation = m.generation;
        out.bitmap = std::move(*bmp);
        channel_.send(id, out);
    } catch (const Error&) {
        channel_.send(id, skipped);  // a bad page renders blank
    }
}

TextPage& Session::text_page(int page) {
    auto it = text_pages_.find(page);
    if (it == text_pages_.end()) {
        // Zoom 1.0: coordinates in base page pixels, which the viewer scales.
        it = text_pages_.emplace(page, std::make_unique<TextPage>(ctx_, *doc_, page, 1.0F)).first;
    }
    return *it->second;
}

void Session::on_search(std::uint64_t id, const Search& m) {
    if (!doc_ || !renderer_) {
        channel_.send(id, Failed{"no document is open"});
        return;
    }
    const auto cancelled = [&] { return m.epoch < search_cancel_epoch_.load(); };
    std::uint32_t total = 0;
    if (!m.needle.empty()) {
        const int pages = doc_->page_count();
        for (int p = 0; p < pages && !cancelled(); ++p) {
            PageMatches matches;
            matches.page = p;
            try {
                for (const SearchHit& hit : text_page(p).search(m.needle)) {
                    matches.quads.insert(matches.quads.end(), hit.quads.begin(), hit.quads.end());
                }
            } catch (const Error&) {
                continue;  // an unreadable page contributes no matches
            }
            if (!matches.quads.empty()) {
                total += static_cast<std::uint32_t>(matches.quads.size());
                channel_.send(id, matches);
            }
        }
    }
    channel_.send(id, SearchDone{total});
}

void Session::on_select(std::uint64_t id, const Select& m) {
    if (!doc_ || !renderer_) {
        channel_.send(id, Failed{"no document is open"});
        return;
    }
    Selection sel = text_page(m.page).select(m.ax, m.ay, m.bx, m.by, m.mode);
    channel_.send(id, SelectionResult{m.page, std::move(sel.quads), std::move(sel.text)});
}

bool Session::require_document(std::uint64_t id) {
    if (!doc_ || !renderer_) {
        channel_.send(id, Failed{"no document is open"});
        return false;
    }
    return true;
}

void Session::on_edit(std::uint64_t id, const Edit& m) {
    if (!require_document(id)) {
        return;
    }
    Edited out;
    switch (m.kind) {
    case Edit::Kind::Redact: {
        const ops::RedactResult r = ops::redact(ctx_, *doc_, m.page, m.rects);
        out.pages = r.pages;
        break;
    }
    case Edit::Kind::RedactText: {
        ops::RedactResult r = ops::redact_text(ctx_, *doc_, m.text);
        out.pages = r.pages;
        out.remaining = std::move(r.remaining);
        break;
    }
    case Edit::Kind::AddAnnot:
        out.annot_id = ops::add_annotation(ctx_, *doc_, m.page, m.annot);
        out.pages = {m.page};
        break;
    case Edit::Kind::DeleteAnnot: {
        int page = -1;
        for (const ops::AnnotInfo& a : ops::list_annotations(ctx_, *doc_)) {
            if (a.id == m.annot_id) {
                page = a.page;
            }
        }
        if (page < 0 || !ops::delete_annotation(ctx_, *doc_, m.annot_id)) {
            channel_.send(id, Failed{"no annotation with that id"});
            return;
        }
        out.pages = {page};
        break;
    }
    case Edit::Kind::SetField:
        ops::set_field(ctx_, *doc_, m.name, m.text);
        out.all_pages = true;  // a field's widgets may be on several pages
        break;
    case Edit::Kind::Watermark:
        (void)ops::watermark(ctx_, *doc_, m.pages, m.watermark);
        out.pages = page_set(m.pages, doc_->page_count());
        break;
    case Edit::Kind::CropMargins:
        (void)ops::crop_margins(ctx_, *doc_, m.pages, m.margins);
        out.pages = page_set(m.pages, doc_->page_count());
        break;
    case Edit::Kind::MoveAnnot:
    case Edit::Kind::SetAnnotContents: {
        int page = -1;
        for (const ops::AnnotInfo& a : ops::list_annotations(ctx_, *doc_)) {
            if (a.id == m.annot_id) {
                page = a.page;
            }
        }
        const bool done =
            page >= 0 && (m.kind == Edit::Kind::MoveAnnot
                              ? !m.rects.empty() &&
                                    ops::move_annotation(ctx_, *doc_, m.annot_id, m.rects.front())
                              : ops::set_annotation_contents(ctx_, *doc_, m.annot_id, m.text));
        if (!done) {
            channel_.send(id, Failed{"no annotation with that id"});
            return;
        }
        out.pages = {page};
        break;
    }
    case Edit::Kind::AddTextLayer:
        // The words were read elsewhere (the OCR worker); writing them is
        // an ordinary edit, and replaying it never runs OCR again.
        (void)ops::add_text_layer(ctx_, *doc_, m.page, m.words);
        out.pages = {m.page};
        break;
    case Edit::Kind::CropBox:
        if (m.rects.empty()) {
            channel_.send(id, Failed{"no box to crop to"});
            return;
        }
        (void)ops::crop(ctx_, *doc_, m.pages, m.rects.front());
        out.pages = page_set(m.pages, doc_->page_count());
        break;
    // Organising pages: page numbers shift, so everything is redrawn.
    case Edit::Kind::RotatePages:
        (void)ops::rotate_pages(ctx_, *doc_, m.pages, m.degrees);
        out.all_pages = true;
        break;
    case Edit::Kind::DeletePages:
        (void)ops::delete_pages(ctx_, *doc_, m.pages);
        out.all_pages = true;
        break;
    case Edit::Kind::MovePages:
        (void)ops::move_pages(ctx_, *doc_, m.pages, m.page);
        out.all_pages = true;
        break;
    case Edit::Kind::InsertPages: {
        // The other file is parsed here, in the sandbox, like the document.
        Document source = Document::open_memory(ctx_, m.data.data(), m.data.size(), "pdf");
        if (source.needs_password()) {
            channel_.send(id, Failed{"the file to insert is password-protected; remove its password first"});
            return;
        }
        (void)ops::insert_pages(ctx_, *doc_, m.page, source, m.pages);
        out.all_pages = true;
        break;
    }
    case Edit::Kind::SetInfo:
        ops::set_info(ctx_, *doc_, m.name, m.text);
        break;  // no page changes
    case Edit::Kind::InsertBlank:
        if (m.rects.empty()) {
            channel_.send(id, Failed{"no size for the new page"});
            return;
        }
        ops::insert_blank_page(ctx_, *doc_, m.page, m.rects.front().x1, m.rects.front().y1);
        out.all_pages = true;
        break;
    }
    // Every cached display list and text layer may now be out of date.
    renderer_->clear_cache();
    text_pages_.clear();
    out.base_sizes = base_sizes();
    channel_.send(id, out);
}

void Session::on_save(std::uint64_t id, Frame& frame) {
    (void)decode_as<Save>(frame);
    if (!require_document(id)) {
        return;
    }
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to Save"});
        return;
    }
    // The descriptor stays owned by the frame and closes with it. The viewer
    // makes the file durable and renames it; the worker can do neither.
    doc_->save_fd(frame.fd.get(), SaveOptions{});
    struct stat st {};
    const std::uint64_t bytes =
        ::fstat(frame.fd.get(), &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
    channel_.send(id, Saved{bytes});
}

void Session::on_list_annots(std::uint64_t id) {
    if (!require_document(id)) {
        return;
    }
    channel_.send(id, AnnotList{ops::list_annotations(ctx_, *doc_)});
}

namespace {

ipc::CertRow to_row(const crypto::CertInfo& c) {
    ipc::CertRow row;
    row.subject = c.subject;
    row.common_name = c.common_name;
    row.issuer = c.issuer;
    row.serial = c.serial;
    row.sha256 = c.sha256;
    row.not_before = c.not_before;
    row.not_after = c.not_after;
    row.is_ca = c.is_ca;
    row.can_sign = c.can_sign;
    return row;
}

std::vector<ipc::RevocationRow> to_rows(const std::vector<crypto::RevocationCheck>& checks) {
    std::vector<ipc::RevocationRow> out;
    for (const crypto::RevocationCheck& c : checks) {
        ipc::RevocationRow row;
        row.subject = c.cert.common_name.empty() ? c.cert.subject : c.cert.common_name;
        row.status = static_cast<std::uint8_t>(c.status);
        row.source = c.source;
        row.data_time = c.data_time;
        row.revoked_at = c.revoked_at;
        row.problem = c.problem;
        out.push_back(std::move(row));
    }
    return out;
}

/// The viewer's trust store, as PEM. A damaged one is the viewer's problem,
/// not this document's: carry on with what parsed.
crypto::TrustStore trust_of(const std::string& pem) {
    crypto::TrustStore trust;
    if (!pem.empty()) {
        try {
            (void)trust.add_pem(pem);
        } catch (const Error&) {
        }
    }
    return trust;
}

std::vector<crypto::FetchedRevocation> from_rows(const std::vector<ipc::FetchedRow>& rows) {
    std::vector<crypto::FetchedRevocation> out;
    for (const ipc::FetchedRow& f : rows) {
        crypto::FetchedRevocation x;
        x.kind = f.kind == 0 ? crypto::RevocationQuery::Kind::Ocsp
                             : crypto::RevocationQuery::Kind::Crl;
        x.url = f.url;
        x.body = f.body;
        x.error = f.error;
        out.push_back(std::move(x));
    }
    return out;
}

std::vector<crypto::Bytes> blobs_of(const std::vector<ops::SignatureInfo>& sigs) {
    std::vector<crypto::Bytes> out;
    for (const ops::SignatureInfo& s : sigs) {
        if (s.range_ok) {
            out.push_back(s.contents);
        }
    }
    return out;
}

}  // namespace

void Session::on_list_revocation_queries(std::uint64_t id,
                                         const ipc::ListRevocationQueries& msg) {
    if (!require_document(id)) {
        return;
    }
    // The certificates come out of the document, so working out what to ask
    // about them happens here; the viewer only sends the bytes.
    const auto sigs = ops::list_signatures(ctx_, *doc_);
    const auto queries = crypto::revocation_queries(blobs_of(sigs), trust_of(msg.trust_pem),
                                                    ops::read_dss(ctx_, *doc_));
    RevocationQueryList out;
    for (const crypto::RevocationQuery& q : queries) {
        out.queries.push_back(ipc::QueryRow{
            static_cast<std::uint8_t>(q.kind == crypto::RevocationQuery::Kind::Ocsp ? 0 : 1),
            q.url, q.request, q.subject});
    }
    channel_.send(id, out);
}

void Session::on_add_validation_data(std::uint64_t id, ipc::Frame& frame) {
    const auto msg = decode_as<AddValidationData>(frame);
    if (!require_document(id)) {
        return;
    }
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to AddValidationData"});
        return;
    }
    // What the viewer fetched is hostile until parsed, and it is parsed here.
    const auto sigs = ops::list_signatures(ctx_, *doc_);
    const crypto::RevocationData embedded = ops::read_dss(ctx_, *doc_);
    const crypto::RevocationData data = crypto::validation_data(
        blobs_of(sigs), trust_of(msg.trust_pem), from_rows(msg.fetched), embedded);
    ValidationDataAdded out;
    if (!data.empty()) {
        ops::add_validation_data(ctx_, *doc_, data, frame.fd.get());
        out.certs = static_cast<std::uint32_t>(data.certs.size());
        out.ocsps = static_cast<std::uint32_t>(data.ocsps.size());
        out.crls = static_cast<std::uint32_t>(data.crls.size());
    }
    channel_.send(id, out);
}

void Session::on_prepare_doc_timestamp(std::uint64_t id, ipc::Frame& frame) {
    const auto msg = decode_as<PrepareDocTimestamp>(frame);
    if (!require_document(id)) {
        return;
    }
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to PrepareDocTimestamp"});
        return;
    }
    const auto prepared =
        ops::prepare_document_timestamp(ctx_, *doc_, msg.reserve, frame.fd.get());
    channel_.send(id, SignaturePrepared{prepared.range, prepared.field});
}

void Session::on_prepare_signature(std::uint64_t id, ipc::Frame& frame) {
    const auto msg = decode_as<PrepareSignature>(frame);
    if (!require_document(id)) {
        return;
    }
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to PrepareSignature"});
        return;
    }
    // The hole this leaves is filled by the viewer, which holds the key. The
    // worker never sees it, and could not use it if it did: it has no network
    // and no files of its own.
    const auto prepared = ops::prepare_signature(ctx_, *doc_, msg.request, frame.fd.get());
    channel_.send(id, SignaturePrepared{prepared.range, prepared.field});
}

void Session::on_list_signatures(std::uint64_t id, const ipc::ListSignatures& msg) {
    if (!require_document(id)) {
        return;
    }
    crypto::TrustStore trust = trust_of(msg.trust_pem);
    if (!msg.trusted_list.empty()) {
        try {
            trust.add_trusted_list(trustlist::decode(msg.trusted_list));
        } catch (const std::runtime_error&) {
            // A damaged cache is the viewer's to replace; verify without it.
        }
    }
    const auto sigs = ops::list_signatures(ctx_, *doc_);
    const crypto::RevocationData embedded = ops::read_dss(ctx_, *doc_);
    crypto::RevocationData online;
    if (!msg.online.empty()) {
        online = crypto::validation_data(blobs_of(sigs), trust, from_rows(msg.online), {});
    }
    SignatureList out;
    for (const ops::SignatureInfo& s : sigs) {
        ipc::SignatureRow row;
        row.field = s.field;
        row.page = s.page;
        row.rect = s.rect;
        row.subfilter = s.subfilter;
        row.name = s.name;
        row.reason = s.reason;
        row.location = s.location;
        row.claimed_time = s.claimed_time;
        row.range_ok = s.range_ok;
        row.range_problem = s.range_problem;
        row.covers_whole_revision = s.covers_whole_revision;
        row.changed_after_signing = s.changed_after_signing;
        row.later_signature_covers_changes = s.later_signature_covers_changes;
        row.certification = static_cast<std::uint8_t>(s.certification);
        row.locks = s.locks;
        row.changes_judged = s.changes_judged;
        row.changes_permitted = s.changes_permitted;
        row.change_problems = s.change_problems;
        row.document_timestamp = s.document_timestamp;
        row.only_validation_data_after = s.only_validation_data_after;
        if (s.range_ok && s.document_timestamp) {
            // A document timestamp: a token, not a signature. It has an
            // authority and a time, and no signer.
            const crypto::TimestampReport t = crypto::verify_document_timestamp(
                s.contents, ops::signed_bytes(ctx_, *doc_, s.range), trust, embedded, online);
            row.checked = true;
            row.intact = t.valid;
            row.problem = t.problem;
            row.trust = static_cast<std::uint8_t>(t.trust);
            row.has_timestamp = true;
            row.timestamp_valid = t.valid;
            row.timestamp_time = t.time;
            row.authority = to_row(t.authority);
            row.timestamp_trust = static_cast<std::uint8_t>(t.trust);
            row.timestamp_problem = t.problem;
            row.revocation = to_rows(t.revocation);
            row.timestamp_qualified = static_cast<std::uint8_t>(t.qualified.level);
            row.timestamp_qualified_detail = t.qualified.detail;
        } else if (s.range_ok) {
            // Hostile DER, parsed here rather than in the viewer. That is the
            // whole reason verification happens in the sandbox.
            const crypto::CmsReport r = crypto::verify_cms(
                s.contents, ops::signed_bytes(ctx_, *doc_, s.range), trust, 0, embedded, online);
            row.checked = true;
            row.intact = r.intact();
            row.problem = r.problem;
            row.digest = r.digest;
            row.signer = to_row(r.signer);
            for (const crypto::CertInfo& c : r.chain) {
                row.chain.push_back(to_row(c));
            }
            row.trust = static_cast<std::uint8_t>(r.trust);
            row.trust_detail = r.trust_detail;
            row.has_signing_certificate_v2 = r.has_signing_certificate_v2;
            row.has_signing_time_attribute = r.has_signing_time_attribute;
            if (r.timestamp) {
                row.has_timestamp = true;
                row.timestamp_valid = r.timestamp->valid;
                row.timestamp_time = r.timestamp->time;
                row.authority = to_row(r.timestamp->authority);
                row.timestamp_trust = static_cast<std::uint8_t>(r.timestamp->trust);
                row.timestamp_problem = r.timestamp->problem;
                row.timestamp_revocation = to_rows(r.timestamp->revocation);
                row.timestamp_qualified = static_cast<std::uint8_t>(r.timestamp->qualified.level);
                row.timestamp_qualified_detail = r.timestamp->qualified.detail;
            }
            row.revocation = to_rows(r.revocation);
            row.qualified = static_cast<std::uint8_t>(r.qualified.level);
            row.qualified_service = r.qualified.service;
            row.qualified_territory = r.qualified.territory;
            row.qualified_detail = r.qualified.detail;
        }
        out.rows.push_back(std::move(row));
    }
    channel_.send(id, out);
}

void Session::on_list_text_pages(std::uint64_t id) {
    if (!require_document(id)) {
        return;
    }
    TextPageList out;
    for (int p = 0; p < doc_->page_count(); ++p) {
        if (ops::page_has_text(ctx_, *doc_, p)) {
            out.pages.push_back(p);
        }
    }
    channel_.send(id, out);
}

void Session::on_list_fields(std::uint64_t id) {
    if (!require_document(id)) {
        return;
    }
    channel_.send(id, FieldList{ops::list_fields(ctx_, *doc_)});
}

// --- file tools ----------------------------------------------------------------
//
// The viewer runs these in a worker of their own (see protocol.hpp), so the
// document here is the file as it is on disk, not the one on screen.

void Session::on_get_info(std::uint64_t id) {
    if (!require_document(id)) {
        return;
    }
    DocInfo out;
    for (const char* key : {"format", "encryption", "info:Title", "info:Author", "info:Subject",
                            "info:Keywords", "info:Creator", "info:Producer", "info:CreationDate",
                            "info:ModDate"}) {
        if (auto value = doc_->metadata(key); value && !value->empty()) {
            out.fields.emplace_back(key, std::move(*value));
        }
    }
    channel_.send(id, out);
}

void Session::on_protect(std::uint64_t id, Frame& frame) {
    const Protect m = decode_as<Protect>(frame);
    if (!require_document(id)) {
        return;
    }
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to Protect"});
        return;
    }
    if (!doc_->is_pdf()) {
        channel_.send(id, Failed{"only a PDF can be password-protected"});
        return;
    }
    if (m.encrypt) {
        ops::EncryptOptions options;
        options.user_password = m.user_password;
        options.owner_password = m.owner_password;
        options.method = static_cast<ops::Encryption>(m.method);
        const auto bit = [&](int n) { return (m.permissions >> n & 1) != 0; };
        options.permissions = {bit(0), bit(1), bit(2), bit(3), bit(4), bit(5), bit(6)};
        ops::encrypt(ctx_, *doc_, frame.fd.get(), options);
    } else {
        ops::decrypt(ctx_, *doc_, frame.fd.get());
    }
    struct stat st {};
    const std::uint64_t bytes =
        ::fstat(frame.fd.get(), &st) == 0 ? static_cast<std::uint64_t>(st.st_size) : 0;
    channel_.send(id, Protected{bytes});
}

void Session::on_compress(std::uint64_t id, Frame& frame) {
    const Compress m = decode_as<Compress>(frame);
    if (!require_document(id)) {
        return;
    }
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to Compress"});
        return;
    }
    if (!doc_->is_pdf()) {
        channel_.send(id, Failed{"only a PDF can be compressed"});
        return;
    }
    ops::CompressOptions options;
    options.preset = static_cast<ops::CompressPreset>(m.preset);
    options.jpeg_quality = static_cast<int>(m.jpeg_quality);
    options.max_image_edge = static_cast<int>(m.max_image_edge);
    options.linearize = m.linearize;

    // Compression rewrites the document in place, so whatever happens it no
    // longer matches its file: close it, and nothing else is asked of it.
    struct CloseAfter {
        Session& s;
        ~CloseAfter() { s.close_document(); }
    } close_after{*this};
    const ops::CompressResult r = ops::compress(ctx_, *doc_, frame.fd.get(), options);
    channel_.send(id, Compressed{r.output_bytes, static_cast<std::uint32_t>(r.images_examined),
                                 static_cast<std::uint32_t>(r.images_recompressed)});
}

void Session::on_extract_pages(std::uint64_t id, Frame& frame) {
    const ExtractPages m = decode_as<ExtractPages>(frame);
    if (!require_document(id)) {
        return;
    }
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to ExtractPages"});
        return;
    }
    if (!doc_->is_pdf()) {
        channel_.send(id, Failed{"only a PDF can be split"});
        return;
    }
    const ops::PagesResult r = ops::extract(ctx_, *doc_, frame.fd.get(), m.ranges);
    channel_.send(id, PagesWritten{static_cast<std::uint32_t>(r.pages_written), r.output_bytes});
}

void Session::on_merge_begin(std::uint64_t id, const MergeBegin& m) {
    merger_.reset();
    ops::MergeOptions options;
    options.linearize = m.linearize;
    merger_ = std::make_unique<ops::Merger>(ctx_, options);
    channel_.send(id, MergeAdded{0});
}

void Session::on_merge_add(std::uint64_t id, Frame& frame) {
    const MergeAdd m = decode_as<MergeAdd>(frame);
    if (!merger_) {
        channel_.send(id, Failed{"no merge has been started"});
        return;
    }
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to MergeAdd"});
        return;
    }
    // add_fd takes the descriptor and closes it at once: a merge of many files
    // must never hold more than one open, under the sandbox's RLIMIT_NOFILE.
    const int pages = merger_->add_fd(frame.fd.release(), m.name.empty() ? "input" : m.name);
    channel_.send(id, MergeAdded{static_cast<std::uint32_t>(pages)});
}

void Session::on_merge_finish(std::uint64_t id, Frame& frame) {
    (void)decode_as<MergeFinish>(frame);
    if (!merger_) {
        channel_.send(id, Failed{"no merge has been started"});
        return;
    }
    if (!frame.fd) {
        channel_.send(id, Failed{"no file descriptor attached to MergeFinish"});
        return;
    }
    const ops::MergeResult r = merger_->finish_fd(frame.fd.get());
    merger_.reset();
    channel_.send(id, Merged{static_cast<std::uint32_t>(r.inputs_merged),
                             static_cast<std::uint32_t>(r.pages_written), r.output_bytes});
}

}  // namespace leht::worker
