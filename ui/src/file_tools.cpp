// SPDX-License-Identifier: AGPL-3.0-or-later
#include "file_tools.hpp"

#include "worker_files.hpp"

#include "leht/ipc/process.hpp"
#include "leht/ipc/protocol.hpp"

#include <QFileInfo>
#include <QLocale>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <exception>

namespace ipc = leht::ipc;

namespace {

QString sizeText(std::uint64_t bytes) {
    return QLocale().formattedDataSize(static_cast<qint64>(bytes));
}

/// A descriptor this side opened for the worker to read. The worker gets its
/// own copy with the frame, so ours closes when the request has been sent.
class Input {
public:
    explicit Input(const QString& path)
        : fd_(::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC)) {}
    Input(const Input&) = delete;
    Input& operator=(const Input&) = delete;
    ~Input() {
        if (fd_ >= 0) {
            ::close(fd_);
        }
    }
    [[nodiscard]] int fd() const { return fd_; }

private:
    int fd_;
};

}  // namespace

FileTools::FileTools() = default;
FileTools::~FileTools() = default;

bool FileTools::cancelled() {
    if (!cancel_.exchange(false)) {
        return false;
    }
    emit failed(tr("Cancelled. Nothing was written."));
    return true;
}

std::unique_ptr<ipc::WorkerProcess> FileTools::startWorker() {
    cancel_ = false;
    try {
        auto proc = ipc::WorkerProcess::spawn(workerPath().toStdString());
        proc->handshake();
        return proc;
    } catch (const std::exception& e) {
        emit failed(tr("Could not start the document worker (%1): %2")
                        .arg(workerPath(), QString::fromUtf8(e.what())));
        return nullptr;
    }
}

template <typename Msg>
std::optional<ipc::Frame> FileTools::request(ipc::WorkerProcess& w, const Msg& msg, int fd,
                                             QString& error) {
    try {
        w.channel().send(nextId_++, msg, fd);
        auto reply = w.channel().recv(requestTimeout());
        if (!reply) {
            error = tr("the document worker stopped; the file may be damaged");
            return std::nullopt;
        }
        if (reply->type == ipc::MsgType::Failed) {
            error = QString::fromStdString(ipc::decode_as<ipc::Failed>(*reply).message);
            return std::nullopt;
        }
        return reply;
    } catch (const ipc::Timeout&) {
        error = tr("the document worker took too long and was stopped");
    } catch (const std::exception& e) {
        error = tr("the document worker failed (%1)").arg(QString::fromUtf8(e.what()));
    }
    // Whatever it was doing, this worker is finished with: do not leave it
    // running for its destructor to wait on.
    (void)w.kill();
    return std::nullopt;
}

bool FileTools::openInput(ipc::WorkerProcess& w, const QString& input, const QString& password) {
    const Input in(input);
    if (in.fd() < 0) {
        emit failed(tr("Could not open “%1”: %2")
                        .arg(QFileInfo(input).fileName(), QString::fromUtf8(std::strerror(errno))));
        return false;
    }
    QString error;
    auto reply = request(w, ipc::Open{QFileInfo(input).fileName().toStdString()}, in.fd(), error);
    if (reply && reply->type == ipc::MsgType::NeedsPassword) {
        if (password.isEmpty()) {
            emit passwordRequired(false);
            return false;
        }
        reply = request(w, ipc::Authenticate{password.toStdString()}, -1, error);
        if (reply && reply->type == ipc::MsgType::NeedsPassword) {
            emit passwordRequired(true);
            return false;
        }
    }
    if (!reply || reply->type != ipc::MsgType::Opened) {
        emit failed(tr("Could not open “%1”: %2")
                        .arg(QFileInfo(input).fileName(),
                             error.isEmpty() ? tr("unexpected answer from the worker") : error));
        return false;
    }
    // Opened is followed by the outline, which nothing here needs.
    try {
        (void)w.channel().recv(requestTimeout());
    } catch (const std::exception&) {
        emit failed(tr("Could not open “%1”: the document worker stopped")
                        .arg(QFileInfo(input).fileName()));
        return false;
    }
    return true;
}

void FileTools::combine(QStringList inputs, QString output, bool linearize) {
    auto w = startWorker();
    if (!w) {
        return;
    }
    const int total = static_cast<int>(inputs.size()) + 1;
    QString error;
    if (!request(*w, ipc::MergeBegin{linearize}, -1, error)) {
        emit failed(tr("Could not start combining: %1").arg(error));
        return;
    }

    int step = 0;
    for (const QString& path : inputs) {
        if (cancelled()) {
            return;
        }
        const QString name = QFileInfo(path).fileName();
        emit progress(step++, total, tr("Adding “%1”…").arg(name));
        const Input in(path);
        if (in.fd() < 0) {
            emit failed(tr("Could not open “%1”: %2")
                            .arg(name, QString::fromUtf8(std::strerror(errno))));
            return;
        }
        if (!request(*w, ipc::MergeAdd{name.toStdString()}, in.fd(), error)) {
            emit failed(tr("Could not add “%1”: %2").arg(name, error));
            return;
        }
    }
    if (cancelled()) {
        return;
    }

    emit progress(step, total, tr("Writing “%1”…").arg(QFileInfo(output).fileName()));
    Beside out(output);
    if (out.fd() < 0) {
        emit failed(tr("Could not write “%1”: %2").arg(output, out.error()));
        return;
    }
    const auto reply = request(*w, ipc::MergeFinish{}, out.fd(), error);
    if (!reply) {
        emit failed(tr("Could not write “%1”: %2").arg(output, error));
        return;
    }
    const ipc::Merged merged = ipc::decode_as<ipc::Merged>(*reply);
    if (const QString why = out.commit(); !why.isEmpty()) {
        emit failed(tr("Could not write “%1”: %2").arg(output, why));
        return;
    }
    emit finished(tr("Combined %n file(s) into %1 pages (%2).", nullptr,
                     static_cast<int>(merged.inputs))
                      .arg(merged.pages)
                      .arg(sizeText(merged.bytes)),
                  {output});
}

void FileTools::compress(QString input, QString password, QString output, int preset,
                         int quality, bool linearize) {
    auto w = startWorker();
    if (!w || !openInput(*w, input, password)) {
        return;
    }
    if (cancelled()) {
        return;
    }
    emit progress(0, 1, tr("Reducing “%1”…").arg(QFileInfo(input).fileName()));

    Beside out(output);
    if (out.fd() < 0) {
        emit failed(tr("Could not write “%1”: %2").arg(output, out.error()));
        return;
    }
    ipc::Compress msg;
    msg.preset = static_cast<std::uint8_t>(preset);
    msg.jpeg_quality = static_cast<std::uint32_t>(quality);
    msg.linearize = linearize;
    QString error;
    const auto reply = request(*w, msg, out.fd(), error);
    if (!reply) {
        emit failed(tr("Could not reduce “%1”: %2").arg(QFileInfo(input).fileName(), error));
        return;
    }
    if (cancelled()) {
        return;
    }
    const ipc::Compressed result = ipc::decode_as<ipc::Compressed>(*reply);
    const auto before = static_cast<std::uint64_t>(QFileInfo(input).size());
    if (result.bytes >= before) {
        // A tool called Reduce must never hand back a bigger file.
        emit finished(tr("“%1” is already as small as this setting can make it (%2). "
                         "Nothing was written.")
                          .arg(QFileInfo(input).fileName(), sizeText(before)),
                      {});
        return;
    }
    if (const QString why = out.commit(); !why.isEmpty()) {
        emit failed(tr("Could not write “%1”: %2").arg(output, why));
        return;
    }
    const int saved = static_cast<int>(100.0 - 100.0 * static_cast<double>(result.bytes) /
                                                   static_cast<double>(before));
    emit finished(tr("Reduced from %1 to %2 (%3% smaller).")
                      .arg(sizeText(before), sizeText(result.bytes))
                      .arg(saved),
                  {output});
}

void FileTools::split(QString input, QString password, QStringList ranges, QStringList outputs) {
    if (ranges.size() != outputs.size() || ranges.isEmpty()) {
        emit failed(tr("Nothing to split."));
        return;
    }
    auto w = startWorker();
    if (!w || !openInput(*w, input, password)) {
        return;
    }

    // Each part is complete when it is renamed into place. A split that
    // fails part-way says which parts it did write.
    QStringList written;
    const int total = static_cast<int>(ranges.size());
    const auto fail = [&](const QString& message) {
        emit failed(written.isEmpty()
                        ? message
                        : tr("%1\n\nThese parts were written before that: %2")
                              .arg(message, written.join(QStringLiteral(", "))));
    };
    for (int i = 0; i < total; ++i) {
        if (cancel_.exchange(false)) {
            fail(tr("Cancelled."));
            return;
        }
        const QString& output = outputs[i];
        emit progress(i, total, tr("Writing “%1”…").arg(QFileInfo(output).fileName()));
        Beside out(output);
        if (out.fd() < 0) {
            fail(tr("Could not write “%1”: %2").arg(output, out.error()));
            return;
        }
        QString error;
        if (!request(*w, ipc::ExtractPages{ranges[i].toStdString()}, out.fd(), error)) {
            fail(tr("Could not write pages %1 to “%2”: %3")
                     .arg(ranges[i], QFileInfo(output).fileName(), error));
            return;
        }
        if (const QString why = out.commit(); !why.isEmpty()) {
            fail(tr("Could not write “%1”: %2").arg(output, why));
            return;
        }
        written << QFileInfo(output).fileName();
    }
    emit finished(tr("Split into %n file(s).", nullptr, total), outputs);
}
