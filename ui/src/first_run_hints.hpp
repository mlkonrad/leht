// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QPointer>
#include <QString>
#include <QVector>
#include <QWidget>

class QLabel;
class QPushButton;

/// A few callouts the first time someone uses Leht, each pointing at a part of
/// the window: "Next" moves on, "Got it" ends the tour, and it never comes
/// back (QSettings "firstRun/hintsShown").
class FirstRunHints : public QWidget {
    Q_OBJECT

public:
    struct Hint {
        QPointer<QWidget> target;  ///< what it points at; a gone or hidden one is skipped
        QString title;
        QString text;
    };

    /// Shows the tour over `window` unless the QSettings flag `key` says it
    /// was shown before (or `force`). Returns nullptr when there is nothing to show.
    static FirstRunHints* showOnce(QWidget* window, QVector<Hint> hints, const QString& key, bool force = false);
    /// The two tours: the start screen, and the first document.
    static constexpr const char* kWelcomeKey = "firstRun/welcomeHintsShown";
    static constexpr const char* kDocumentKey = "firstRun/documentHintsShown";

protected:
    void paintEvent(QPaintEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    FirstRunHints(QWidget* window, QVector<Hint> hints);
    void showHint(int index);
    void place();
    void finish();

    QVector<Hint> hints_;
    int index_ = -1;
    QWidget* bubble_ = nullptr;
    QLabel* title_ = nullptr;
    QLabel* text_ = nullptr;
    QLabel* step_ = nullptr;
    QPushButton* next_ = nullptr;
    QRect targetRect_;  ///< the target, in this overlay's coordinates
};
