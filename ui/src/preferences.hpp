// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QDialog>
#include <QVector>

class QCheckBox;
class QComboBox;
class QLineEdit;
class QListWidget;
class QSpinBox;

/// Settings the viewer keeps in QSettings, gathered in one window. The keys
/// are the ones the dialogs have always remembered on their own ("signing/tsa",
/// "ocr/dpi"…), so a value set in either place shows in the other.
namespace prefs {
inline constexpr const char* kAuthor = "annotations/author";
inline constexpr const char* kDefaultZoom = "view/defaultZoom";  ///< "width" | "page" | "actual"
/// The toolbar shows (default true), and its quick buttons are labelled
/// (default true; false is icons only).
inline constexpr const char* kShowToolbar = "appearance/showToolbar";
inline constexpr const char* kToolbarLabels = "appearance/toolbarLabels";
/// Where the toolbar's quick tools sit: "left" (after undo and redo, the
/// default) or "centre".
inline constexpr const char* kToolbarAlign = "appearance/toolbarAlign";
/// Reopen, at start-up, the documents open when Leht last closed. Off by default.
inline constexpr const char* kRestoreTabs = "general/restoreTabs";
inline constexpr const char* kTrusted = "trustedCertificates";
}  // namespace prefs

class PreferencesDialog : public QDialog {
    Q_OBJECT

public:
    enum class Page { General, Signing, Trust, Recognition, Network };

    /// `ocrLanguages` are the installed ones; empty hides the Recognition page.
    PreferencesDialog(QWidget* parent, const QStringList& ocrLanguages, Page first = Page::General);

signals:
    /// After OK: something that is drawn changed (icons, toolbar style).
    void appearanceChanged();
    /// After OK: the list of trusted certificates changed, so signatures
    /// should be checked again.
    void trustChanged();

private:
    void save();

    QLineEdit* author_ = nullptr;
    QComboBox* zoom_ = nullptr;
    QCheckBox* restoreTabs_ = nullptr;
    QCheckBox* showToolbar_ = nullptr;
    QComboBox* toolbarLabels_ = nullptr;
    QComboBox* toolbarAlign_ = nullptr;
    QCheckBox* themeIcons_ = nullptr;

    QLineEdit* signName_ = nullptr;
    QLineEdit* signLocation_ = nullptr;
    QCheckBox* useTsa_ = nullptr;
    QLineEdit* tsa_ = nullptr;
    QCheckBox* ltv_ = nullptr;

    QListWidget* trusted_ = nullptr;

    QVector<QCheckBox*> languages_;
    QSpinBox* dpi_ = nullptr;
};
