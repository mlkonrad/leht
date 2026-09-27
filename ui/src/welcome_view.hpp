// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QWidget>

class QFrame;
class QLabel;
class QListWidget;
class QListWidgetItem;

/// What the window shows with no document open: open a file, start from a
/// task, or pick up a recent file. Files dropped on it are offered upwards.
class WelcomeView : public QWidget {
    Q_OBJECT

public:
    /// The tasks on the cards, by the id taskChosen() gives.
    static constexpr const char* kSign = "sign";
    static constexpr const char* kFill = "fill";
    static constexpr const char* kCombine = "combine";
    static constexpr const char* kOcr = "ocr";
    static constexpr const char* kReduce = "reduce";
    static constexpr const char* kVerify = "verify";

    explicit WelcomeView(QWidget* parent = nullptr);

    /// Re-reads the recent files (call when it is shown again).
    void refresh();

signals:
    void openRequested();  ///< the "Open a PDF" button
    void openPath(const QString& path);  ///< a recent file
    void taskChosen(const QString& task);
    void filesDropped(const QStringList& paths);

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void setDropHighlight(bool on);
    void restyle();

    QFrame* dropFrame_ = nullptr;
    QLabel* dropHint_ = nullptr;
    QListWidget* recent_ = nullptr;
    QLabel* recentTitle_ = nullptr;
    QLabel* noRecent_ = nullptr;
};

/// The local files among a drop's URLs, or none if anything else is in it.
[[nodiscard]] QStringList droppedFiles(const class QMimeData* mime);
