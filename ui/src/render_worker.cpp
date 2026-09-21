// SPDX-License-Identifier: AGPL-3.0-or-later
#include "render_worker.hpp"

#include "leht/ipc/protocol.hpp"
#include "leht/page_cache.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <limits>
#include <set>
#include <system_error>
#include <tuple>
#include <utility>

namespace ipc = leht::ipc;

namespace {

/// Renders outside the scroll generation (thumbnails, printing) carry the
/// newest possible generation, so the worker never treats them as stale.
constexpr quint64 kNoGeneration = std::numeric_limits<quint64>::max();

/// The largest zoom the worker accepts; see ipc/src/protocol.cpp.
constexpr double kMaxZoom = 64.0;

/// Copies a leht::Bitmap (RGB, own stride) into a self-owning QImage. The copy
/// is deliberate: the QImage travels to the GUI thread by value through a
/// queued signal, so it must own its data.
QImage toQImage(const leht::Bitmap& bmp) {
    if (bmp.empty() || bmp.channels != 3) {
        return {};
    }
    const QImage view(bmp.pixels.data(), bmp.width, bmp.height, bmp.stride,
                      QImage::Format_RGB888);
    return view.copy();
}

QVector<QRectF> toRects(const std::vector<leht::TextQuad>& quads) {
    QVector<QRectF> boxes;
    boxes.reserve(static_cast<int>(quads.size()));
    for (const leht::TextQuad& q : quads) {
        boxes.push_back(QRectF(q.min_x(), q.min_y(), q.max_x() - q.min_x(),
                               q.max_y() - q.min_y()));
    }
    return boxes;
}

/// Where leht-worker lives, in order of preference: an explicit override, next
/// to the running binary (a relocated install or an app bundle), the install
/// location, and the build tree (tests and running from the build directory).
QString workerPath() {
    const QString env = qEnvironmentVariable("LEHT_WORKER_PATH");
    if (!env.isEmpty()) {
        return env;
    }
    for (const QString& candidate :
         {QCoreApplication::applicationDirPath() + QStringLiteral("/leht-worker"),
          QStringLiteral(LEHT_WORKER_INSTALLED_PATH),
          QStringLiteral(LEHT_WORKER_BUILD_PATH)}) {
        if (QFileInfo(candidate).isExecutable()) {
            return candidate;
        }
    }
    return QStringLiteral(LEHT_WORKER_BUILD_PATH);
}

/// Files that crashed a worker, for the rest of the session. Keyed on file
/// identity and version, so an edited file gets a fresh chance.
using FileKey = std::tuple<dev_t, ino_t, off_t, std::int64_t>;

QMutex g_quarantineMutex;
std::set<FileKey>& quarantine() {
    static std::set<FileKey> files;
    return files;
}

std::optional<FileKey> fileKey(const QString& path) {
    struct stat st {};
    if (::stat(QFile::encodeName(path).constData(), &st) != 0) {
        return std::nullopt;
    }
    return FileKey{st.st_dev, st.st_ino, st.st_size,
                   static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 +
                       st.st_mtim.tv_nsec};
}

bool isQuarantined(const QString& path) {
    const auto key = fileKey(path);
    const QMutexLocker lock(&g_quarantineMutex);
    return key && quarantine().contains(*key);
}

void addToQuarantine(const QString& path) {
    if (const auto key = fileKey(path)) {
        const QMutexLocker lock(&g_quarantineMutex);
        quarantine().insert(*key);
    }
}

const QString kCrashedMessage = QStringLiteral(
    "Leht could not open this file safely: it crashed the document parser. "
    "The file may be damaged or deliberately malformed.");

}  // namespace

RenderWorker::RenderWorker() = default;

RenderWorker::~RenderWorker() {
    closeDocument();
}

void RenderWorker::setGeneration(quint64 generation) {
    generation_.storeRelease(generation);
    // Tell the worker too, so it drops queued renders and aborts a running one.
    // Channel::send is safe to call concurrently with this thread's I/O.
    if (auto proc = proc_.load()) {
        try {
            proc->channel().send(0, ipc::Cancel{generation});
        } catch (const std::exception&) {
            // The worker has gone; this thread will notice on its next receive.
        }
    }
}

qint64 RenderWorker::workerPid() const {
    const auto proc = proc_.load();
    return proc ? proc->pid() : 0;
}

void RenderWorker::closeDocument() {
    proc_.store(nullptr);  // the last reference reaps the worker process
    password_.reset();
    baseSizes_.clear();
    poisoned_.clear();
    crashes_ = 0;
    if (cache_) {
        cache_->clear();
    }
}

bool RenderWorker::startWorker() {
    try {
        std::shared_ptr<ipc::WorkerProcess> proc =
            ipc::WorkerProcess::spawn(workerPath().toStdString());
        proc->handshake();
        proc_.store(std::move(proc));
        return true;
    } catch (const std::exception& e) {
        emit failed(tr("Could not start the document worker (%1): %2")
                        .arg(workerPath(), QString::fromUtf8(e.what())));
        return false;
    }
}

template <typename Msg>
bool RenderWorker::sendRequest(const Msg& msg, int fd) {
    auto proc = proc_.load();
    if (!proc) {
        return false;
    }
    try {
        proc->channel().send(nextId_++, msg, fd);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<ipc::Frame> RenderWorker::receive() {
    auto proc = proc_.load();
    if (!proc) {
        return std::nullopt;
    }
    try {
        return proc->channel().recv();
    } catch (const ipc::ProtocolError&) {
        // Malformed output from the process that just parsed an untrusted file:
        // assume it is compromised, and never read from it again.
        proc->kill();
        return std::nullopt;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void RenderWorker::workerLost(Phase phase, int page) {
    proc_.store(nullptr);
    ++crashes_;

    if (phase == Phase::Open || crashes_ >= 2) {
        addToQuarantine(path_);
        closeDocument();
        emit failed(kCrashedMessage);
        return;
    }
    if (phase == Phase::Page && page >= 0) {
        poisoned_.insert(page);  // this page stays blank from now on
    }
    // Restore the document in a fresh worker so the other pages keep working.
    if (!startWorker() || !openInWorker(/*silent=*/true)) {
        closeDocument();
    }
}

void RenderWorker::publishOpened(const ipc::Opened& result, const ipc::Outline& outline) {
    baseSizes_.clear();
    baseSizes_.reserve(static_cast<int>(result.base_sizes.size()));
    for (const leht::PageSize& s : result.base_sizes) {
        baseSizes_.push_back(QSize(s.width, s.height));
    }
    emit opened(static_cast<int>(baseSizes_.size()), baseSizes_);

    QVector<OutlineRow> rows;
    rows.reserve(static_cast<int>(outline.rows.size()));
    for (const ipc::OutlineRow& r : outline.rows) {
        rows.push_back(OutlineRow{r.depth, QString::fromStdString(r.title), r.page, r.y});
    }
    emit outlineReady(rows);
}

bool RenderWorker::openInWorker(bool silent) {
    const int fd = ::open(QFile::encodeName(path_).constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        emit failed(QString::fromUtf8(std::strerror(errno)));
        return false;
    }
    const bool sent = sendRequest(ipc::Open{QFileInfo(path_).fileName().toStdString()}, fd);
    ::close(fd);  // the worker has its own copy

    auto reply = sent ? receive() : std::nullopt;
    if (reply && reply->type == ipc::MsgType::NeedsPassword && password_) {
        // A respawned worker: re-unlock with the password the user gave.
        if (!sendRequest(ipc::Authenticate{*password_})) {
            reply.reset();
        } else {
            reply = receive();
        }
    }
    if (!reply) {
        workerLost(Phase::Open, -1);
        return false;
    }

    try {
        switch (reply->type) {
        case ipc::MsgType::NeedsPassword:
            emit passwordRequired(false);
            return false;
        case ipc::MsgType::Failed:
            emit failed(QString::fromStdString(ipc::decode_as<ipc::Failed>(*reply).message));
            return false;
        case ipc::MsgType::Opened: {
            const ipc::Opened result = ipc::decode_as<ipc::Opened>(*reply);
            auto outlineFrame = receive();
            if (!outlineFrame) {
                workerLost(Phase::Open, -1);
                return false;
            }
            const ipc::Outline outline = ipc::decode_as<ipc::Outline>(*outlineFrame);
            if (!silent) {
                publishOpened(result, outline);
            }
            return true;
        }
        default:
            throw ipc::ProtocolError("unexpected reply to Open");
        }
    } catch (const ipc::ProtocolError&) {
        workerLost(Phase::Open, -1);
        return false;
    }
}

void RenderWorker::open(const QString& path) {
    closeDocument();
    path_ = path;
    if (!cache_) {
        cache_ = std::make_unique<leht::PageCache>();
    }

    if (isQuarantined(path)) {
        emit failed(kCrashedMessage);
        return;
    }
    // A fresh worker per document: whatever one file did to a worker cannot
    // reach the next file opened.
    if (!startWorker()) {
        return;
    }
    (void)openInWorker(/*silent=*/false);
}

void RenderWorker::authenticate(const QString& password) {
    if (!proc_.load()) {
        return;
    }
    const std::string pw = password.toStdString();
    if (!sendRequest(ipc::Authenticate{pw})) {
        workerLost(Phase::Open, -1);
        return;
    }
    auto reply = receive();
    if (!reply) {
        workerLost(Phase::Open, -1);
        return;
    }
    try {
        if (reply->type == ipc::MsgType::NeedsPassword) {
            emit passwordRequired(true);  // wrong password; ask again
            return;
        }
        if (reply->type == ipc::MsgType::Failed) {
            emit failed(QString::fromStdString(ipc::decode_as<ipc::Failed>(*reply).message));
            return;
        }
        const ipc::Opened result = ipc::decode_as<ipc::Opened>(*reply);
        auto outlineFrame = receive();
        if (!outlineFrame) {
            workerLost(Phase::Open, -1);
            return;
        }
        password_ = pw;
        publishOpened(result, ipc::decode_as<ipc::Outline>(*outlineFrame));
    } catch (const ipc::ProtocolError&) {
        workerLost(Phase::Open, -1);
    }
}

void RenderWorker::render(int page, double zoom, int rotation, quint64 generation) {
    if (!proc_.load() || page < 0 || poisoned_.contains(page)) {
        return;
    }
    // Drop stale work: if the view has moved on, this page is no longer wanted.
    if (generation < generation_.loadAcquire()) {
        return;
    }
    const auto z = static_cast<float>(zoom);
    if (!(z > 0.0F) || zoom > kMaxZoom) {
        return;
    }

    // The page cache lives on this side of the boundary, so a hit costs no
    // round trip to the worker. Keyed by (page, zoom, rotation).
    if (auto hit = cache_->get(page, z, rotation)) {
        emit rendered(page, zoom, rotation, generation, toQImage(*hit));
        return;
    }

    if (!sendRequest(ipc::Render{page, z, rotation, generation})) {
        workerLost(Phase::Page, page);
        return;
    }
    auto reply = receive();
    if (!reply) {
        workerLost(Phase::Page, page);
        return;
    }
    try {
        if (reply->type == ipc::MsgType::RenderSkipped) {
            (void)ipc::decode_as<ipc::RenderSkipped>(*reply);
            return;  // stale, cancelled, or a bad page: leave it blank
        }
        ipc::Rendered r = ipc::decode_as<ipc::Rendered>(*reply);
        if (r.page != page) {
            throw ipc::ProtocolError("render reply for the wrong page");
        }
        const QImage image = toQImage(r.bitmap);
        cache_->put(page, z, rotation, std::move(r.bitmap));
        if (!image.isNull()) {
            emit rendered(page, zoom, rotation, generation, image);
        }
    } catch (const ipc::ProtocolError&) {
        if (auto proc = proc_.load()) {
            proc->kill();
        }
        workerLost(Phase::Page, page);
    }
}

void RenderWorker::search(const QString& needle) {
    if (!proc_.load()) {
        return;
    }
    if (needle.isEmpty()) {
        emit searchFinished(0);
        return;
    }
    if (!sendRequest(ipc::Search{needle.toStdString()})) {
        workerLost(Phase::Search, -1);
        emit searchFinished(0);
        return;
    }

    int total = 0;
    for (;;) {
        auto reply = receive();
        if (!reply) {
            workerLost(Phase::Search, -1);
            break;
        }
        try {
            if (reply->type == ipc::MsgType::SearchDone ||
                reply->type == ipc::MsgType::Failed) {
                break;
            }
            const ipc::PageMatches m = ipc::decode_as<ipc::PageMatches>(*reply);
            const QVector<QRectF> boxes = toRects(m.quads);
            total += static_cast<int>(boxes.size());
            emit pageMatches(m.page, boxes);
        } catch (const ipc::ProtocolError&) {
            if (auto proc = proc_.load()) {
                proc->kill();
            }
            workerLost(Phase::Search, -1);
            break;
        }
    }
    emit searchFinished(total);
}

void RenderWorker::selectRegion(int page, QPointF aBase, QPointF bBase, int mode) {
    if (!proc_.load() || page < 0 || poisoned_.contains(page) || mode < 0 ||
        mode > static_cast<int>(leht::SelectMode::Lines)) {
        return;
    }
    ipc::Select s;
    s.page = page;
    s.ax = static_cast<float>(aBase.x());
    s.ay = static_cast<float>(aBase.y());
    s.bx = static_cast<float>(bBase.x());
    s.by = static_cast<float>(bBase.y());
    s.mode = static_cast<leht::SelectMode>(mode);
    if (!sendRequest(s)) {
        workerLost(Phase::Page, page);
        return;
    }
    auto reply = receive();
    if (!reply) {
        workerLost(Phase::Page, page);
        return;
    }
    try {
        if (reply->type == ipc::MsgType::Failed) {
            return;  // no selection rather than an error
        }
        const ipc::SelectionResult sel = ipc::decode_as<ipc::SelectionResult>(*reply);
        emit selectionReady(page, toRects(sel.quads), QString::fromStdString(sel.text));
    } catch (const ipc::ProtocolError&) {
        if (auto proc = proc_.load()) {
            proc->kill();
        }
        workerLost(Phase::Page, page);
    }
}

QImage RenderWorker::renderUncached(int page, double zoom) {
    if (!proc_.load() || page < 0 || poisoned_.contains(page) || !(zoom > 0.0) ||
        zoom > kMaxZoom) {
        return {};
    }
    if (!sendRequest(ipc::Render{page, static_cast<float>(zoom), 0, kNoGeneration})) {
        workerLost(Phase::Page, page);
        return {};
    }
    auto reply = receive();
    if (!reply) {
        workerLost(Phase::Page, page);
        return {};
    }
    try {
        if (reply->type == ipc::MsgType::RenderSkipped) {
            return {};
        }
        return toQImage(ipc::decode_as<ipc::Rendered>(*reply).bitmap);
    } catch (const ipc::ProtocolError&) {
        if (auto proc = proc_.load()) {
            proc->kill();
        }
        workerLost(Phase::Page, page);
        return {};
    }
}

void RenderWorker::renderThumbnail(int page, int targetWidth) {
    if (page < 0 || page >= baseSizes_.size() || targetWidth <= 0) {
        return;
    }
    const int baseWidth = baseSizes_[page].width();
    if (baseWidth <= 0) {
        return;
    }
    // Rendered outside the page cache: a thumbnail must not evict the
    // full-size pages the reader is actually looking at.
    const QImage img = renderUncached(page, static_cast<double>(targetWidth) / baseWidth);
    if (!img.isNull()) {
        emit thumbnailReady(page, img);
    }
}

QImage RenderWorker::renderAt(int page, double zoom) {
    return renderUncached(page, zoom);
}
