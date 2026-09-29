// SPDX-License-Identifier: AGPL-3.0-or-later
#include "preferences.hpp"

#include "icons.hpp"
#include "page_dialogs.hpp"

#include <algorithm>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStackedWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace {

QLabel* note(const QString& text, QWidget* parent) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

QWidget* pageWidget(QWidget* parent, QVBoxLayout*& layout) {
    auto* page = new QWidget(parent);
    layout = new QVBoxLayout(page);
    layout->setContentsMargins(12, 0, 0, 0);
    return page;
}

}  // namespace

PreferencesDialog::PreferencesDialog(QWidget* parent, const QStringList& ocrLanguages, Page first)
    : QDialog(parent) {
    setObjectName(QStringLiteral("preferences"));
    setWindowTitle(tr("Preferences"));
    resize(680, 460);
    QSettings settings;

    auto* sections = new QListWidget(this);
    sections->setObjectName(QStringLiteral("preferenceSections"));
    sections->setIconSize(QSize(18, 18));
    sections->setMaximumWidth(190);
    auto* stack = new QStackedWidget(this);
    const auto addSection = [&](Page id, const char* icon, const QString& title, QWidget* page) {
        auto* item = new QListWidgetItem(icons::named(QLatin1String(icon)), title, sections);
        item->setData(Qt::UserRole, static_cast<int>(id));
        stack->addWidget(page);
    };

    // General.
    QVBoxLayout* layout = nullptr;
    QWidget* general = pageWidget(stack, layout);
    auto* form = new QFormLayout;
    author_ = new QLineEdit(settings.value(QLatin1String(prefs::kAuthor)).toString(), general);
    author_->setPlaceholderText(tr("Your name"));
    author_->setToolTip(tr("Shown as the author of the comments, highlights and notes you add"));
    form->addRow(tr("Name on comments:"), author_);
    zoom_ = new QComboBox(general);
    zoom_->addItem(tr("Fit width"), QStringLiteral("width"));
    zoom_->addItem(tr("Fit page"), QStringLiteral("page"));
    zoom_->addItem(tr("Actual size"), QStringLiteral("actual"));
    zoom_->setCurrentIndex(std::max(0, zoom_->findData(settings.value(QLatin1String(prefs::kDefaultZoom),
                                                                       QStringLiteral("width")))));
    form->addRow(tr("Open documents at:"), zoom_);
    restoreTabs_ = new QCheckBox(tr("Reopen the documents that were open when Leht closed"), general);
    restoreTabs_->setObjectName(QStringLiteral("restoreTabs"));
    restoreTabs_->setChecked(settings.value(QLatin1String(prefs::kRestoreTabs), false).toBool());
    restoreTabs_->setToolTip(tr("Each on the page it was left at. Files opened from elsewhere open as well."));
    form->addRow(tr("Start-up:"), restoreTabs_);
    showToolbar_ = new QCheckBox(tr("Show the toolbar"), general);
    showToolbar_->setObjectName(QStringLiteral("showToolbar"));
    showToolbar_->setChecked(settings.value(QLatin1String(prefs::kShowToolbar), true).toBool());
    showToolbar_->setToolTip(tr("Without it, the page number and zoom show in the status bar"));
    form->addRow(tr("Toolbar:"), showToolbar_);
    toolbarLabels_ = new QComboBox(general);
    toolbarLabels_->setObjectName(QStringLiteral("toolbarLabels"));
    toolbarLabels_->addItem(tr("Icons and labels"), true);
    toolbarLabels_->addItem(tr("Icons only"), false);
    toolbarLabels_->setCurrentIndex(settings.value(QLatin1String(prefs::kToolbarLabels), true).toBool() ? 0 : 1);
    toolbarLabels_->setEnabled(showToolbar_->isChecked());
    connect(showToolbar_, &QCheckBox::toggled, toolbarLabels_, &QWidget::setEnabled);
    form->addRow(tr("Toolbar buttons:"), toolbarLabels_);
    themeIcons_ = new QCheckBox(tr("Use the desktop's icon theme where it has an icon"), general);
    themeIcons_->setChecked(settings.value(QLatin1String(icons::kUseThemeKey), false).toBool());
    themeIcons_->setToolTip(tr("Leht's own icons are used otherwise, and for anything the theme lacks"));
    form->addRow(tr("Icons:"), themeIcons_);
    layout->addLayout(form);
    layout->addStretch(1);
    addSection(Page::General, "settings", tr("General"), general);

    // Signing: the defaults the Sign dialog starts from.
    QWidget* signing = pageWidget(stack, layout);
    layout->addWidget(note(tr("What the Sign dialog fills in for you. You can still change any of "
                              "it each time you sign."), signing));
    form = new QFormLayout;
    signName_ = new QLineEdit(settings.value(QStringLiteral("signing/name")).toString(), signing);
    signName_->setPlaceholderText(tr("Taken from the certificate when empty"));
    form->addRow(tr("Name on the signature:"), signName_);
    signLocation_ = new QLineEdit(settings.value(QStringLiteral("signing/location")).toString(), signing);
    signLocation_->setPlaceholderText(tr("e.g. Tallinn"));
    form->addRow(tr("Location:"), signLocation_);
    useTsa_ = new QCheckBox(tr("Add a trusted timestamp"), signing);
    useTsa_->setChecked(settings.value(QStringLiteral("signing/useTsa"), false).toBool());
    useTsa_->setToolTip(tr("Proves when you signed. Goes online to the timestamp authority below, "
                           "sending only a fingerprint of the signature."));
    form->addRow(tr("By default:"), useTsa_);
    tsa_ = new QLineEdit(settings.value(QStringLiteral("signing/tsa")).toString(), signing);
    tsa_->setPlaceholderText(QStringLiteral("https://…"));
    form->addRow(tr("Timestamp authority:"), tsa_);
    ltv_ = new QCheckBox(tr("Add long-term validation data"), signing);
    ltv_->setChecked(settings.value(QStringLiteral("signing/ltv"), false).toBool());
    ltv_->setToolTip(tr("Embeds what is needed to check the signature after its certificates "
                        "expire (PAdES B-LT). Needs a timestamp."));
    form->addRow(QString(), ltv_);
    const auto syncTsa = [this] {
        tsa_->setEnabled(useTsa_->isChecked());
        ltv_->setEnabled(useTsa_->isChecked());
    };
    connect(useTsa_, &QCheckBox::toggled, this, syncTsa);
    syncTsa();
    layout->addLayout(form);

    // Smart-ID and Mobile-ID: SK serves only relying parties it knows.
    auto* sk = new QGroupBox(tr("Smart-ID and Mobile-ID"), signing);
    auto* skLayout = new QVBoxLayout(sk);
    skLayout->addWidget(note(tr("Signing with a phone goes through SK ID Solutions, which "
                                "answers only relying parties it has registered. Leht uses SK's "
                                "demo environment, where the name DEMO works for everyone; "
                                "leave these empty for it."),
                             sk));
    auto* skForm = new QFormLayout;
    const auto rpField = [&](const char* key, const QString& placeholder) {
        auto* edit = new QLineEdit(settings.value(QLatin1String(key)).toString(), sk);
        edit->setPlaceholderText(placeholder);
        return edit;
    };
    smartIdRpName_ = rpField(prefs::kSmartIdRpName, QStringLiteral("DEMO"));
    smartIdRpUuid_ = rpField(prefs::kSmartIdRpUuid,
                             QStringLiteral("00000000-0000-4000-8000-000000000000"));
    mobileIdRpName_ = rpField(prefs::kMobileIdRpName, QStringLiteral("DEMO"));
    mobileIdRpUuid_ = rpField(prefs::kMobileIdRpUuid,
                              QStringLiteral("00000000-0000-0000-0000-000000000000"));
    skForm->addRow(tr("Smart-ID relying party:"), smartIdRpName_);
    skForm->addRow(tr("Its UUID:"), smartIdRpUuid_);
    skForm->addRow(tr("Mobile-ID relying party:"), mobileIdRpName_);
    skForm->addRow(tr("Its UUID:"), mobileIdRpUuid_);
    skLayout->addLayout(skForm);
    layout->addWidget(sk);
    layout->addStretch(1);
    addSection(Page::Signing, "signature", tr("Signing"), signing);

    // Trusted certificates, besides the system's.
    QWidget* trust = pageWidget(stack, layout);
    layout->addWidget(note(tr("Signatures are trusted when their certificate leads to one of your "
                              "system's certificate authorities, or to a certificate listed here. "
                              "Add one only if you know where it came from."), trust));
    trusted_ = new QListWidget(trust);
    trusted_->setObjectName(QStringLiteral("trustedCertificates"));
    trusted_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    for (const QString& path : settings.value(QLatin1String(prefs::kTrusted)).toStringList()) {
        auto* item = new QListWidgetItem(icons::named(QStringLiteral("key-round")),
                                         QFileInfo(path).fileName(), trusted_);
        item->setData(Qt::UserRole, path);
        item->setToolTip(QFileInfo::exists(path) ? path : tr("%1 (missing)").arg(path));
    }
    layout->addWidget(trusted_);
    auto* trustButtons = new QHBoxLayout;
    auto* addCert = new QPushButton(icons::named(QStringLiteral("file-plus")), tr("Add…"), trust);
    auto* removeCert = new QPushButton(icons::named(QStringLiteral("trash-2")), tr("Remove"), trust);
    removeCert->setEnabled(false);
    trustButtons->addWidget(addCert);
    trustButtons->addWidget(removeCert);
    trustButtons->addStretch(1);
    layout->addLayout(trustButtons);
    connect(trusted_, &QListWidget::itemSelectionChanged, this,
            [this, removeCert] { removeCert->setEnabled(!trusted_->selectedItems().isEmpty()); });
    connect(addCert, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, tr("Trust a certificate"), QString(),
            tr("Certificates (*.pem *.crt *.cer);;All files (*)"));
        if (path.isEmpty()) {
            return;
        }
        for (int i = 0; i < trusted_->count(); ++i) {
            if (trusted_->item(i)->data(Qt::UserRole).toString() == path) {
                return;
            }
        }
        auto* item = new QListWidgetItem(icons::named(QStringLiteral("key-round")),
                                         QFileInfo(path).fileName(), trusted_);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
    });
    connect(removeCert, &QPushButton::clicked, this, [this] { qDeleteAll(trusted_->selectedItems()); });
    addSection(Page::Trust, "key-round", tr("Trusted Certificates"), trust);

    // Text recognition, when this build has it.
    if (!ocrLanguages.isEmpty()) {
        QWidget* ocr = pageWidget(stack, layout);
        layout->addWidget(note(tr("The languages Recognize Text starts with. More can be installed "
                                  "from your distribution's Tesseract language packs."), ocr));
        const QStringList wanted = settings.value(QStringLiteral("ocr/languages")).toStringList();
        for (const QString& code : ocrLanguages) {
            auto* box = new QCheckBox(ocrLanguageName(code), ocr);
            box->setProperty("code", code);
            box->setChecked(wanted.isEmpty() ? (code == QLatin1String("est") || code == QLatin1String("eng"))
                                             : wanted.contains(code));
            layout->addWidget(box);
            languages_.push_back(box);
        }
        form = new QFormLayout;
        dpi_ = new QSpinBox(ocr);
        dpi_->setRange(150, 600);
        dpi_->setSingleStep(50);
        dpi_->setSuffix(tr(" dpi"));
        dpi_->setValue(settings.value(QStringLiteral("ocr/dpi"), 300).toInt());
        form->addRow(tr("Resolution:"), dpi_);
        layout->addLayout(form);
        layout->addStretch(1);
        addSection(Page::Recognition, "scan-text", tr("Text Recognition"), ocr);
    }

    // Network: nothing to set, a promise to read.
    QWidget* network = pageWidget(stack, layout);
    auto* promise = new QLabel(network);
    promise->setWordWrap(true);
    promise->setTextFormat(Qt::RichText);
    promise->setText(tr(
        "<p><b>Your documents never leave this computer.</b></p>"
        "<p>Leht goes online only when you ask it to, each time, and says where it is "
        "connecting in the status bar while it does:</p>"
        "<ul>"
        "<li><b>Timestamps</b> when signing: a fingerprint of the signature goes to the "
        "timestamp authority.</li>"
        "<li><b>Revocation checks</b> and <b>long-term validation</b>: certificate "
        "serial numbers go to the certificate authority's OCSP or CRL service.</li>"
        "<li><b>EU Trusted List</b> updates: the published list is downloaded.</li>"
        "<li><b>Smart-ID and Mobile-ID</b> signing: the fingerprint to sign, and your "
        "personal code or phone number, go to SK ID Solutions.</li>"
        "</ul>"
        "<p>Nothing is sent in the background, and there is no telemetry.</p>"));
    layout->addWidget(promise);
    layout->addStretch(1);
    addSection(Page::Network, "globe", tr("Network & Privacy"), network);

    auto* body = new QHBoxLayout;
    body->addWidget(sections);
    body->addWidget(stack, 1);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* outer = new QVBoxLayout(this);
    outer->addLayout(body, 1);
    outer->addWidget(buttons);

    connect(sections, &QListWidget::currentRowChanged, stack, &QStackedWidget::setCurrentIndex);
    for (int i = 0; i < sections->count(); ++i) {
        if (sections->item(i)->data(Qt::UserRole).toInt() == static_cast<int>(first)) {
            sections->setCurrentRow(i);
        }
    }
    if (sections->currentRow() < 0) {
        sections->setCurrentRow(0);
    }
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        static const QRegularExpression uuid(
            QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
        for (QLineEdit* edit : {smartIdRpUuid_, mobileIdRpUuid_}) {
            // SK compares UUIDs as written, and only in lower case.
            if (!edit->text().trimmed().isEmpty() && !uuid.match(edit->text().trimmed()).hasMatch()) {
                QMessageBox::warning(this, windowTitle(),
                                     tr("A relying party's UUID is written like "
                                        "00000000-0000-0000-0000-000000000000, in lower case."));
                return;
            }
        }
        if (useTsa_->isChecked() && !QUrl(tsa_->text().trimmed()).isValid()) {
            QMessageBox::warning(this, windowTitle(), tr("The timestamp authority's address is not a URL."));
            return;
        }
        if (!languages_.isEmpty() &&
            std::none_of(languages_.begin(), languages_.end(), [](QCheckBox* b) { return b->isChecked(); })) {
            QMessageBox::warning(this, windowTitle(), tr("Choose at least one text recognition language."));
            return;
        }
        save();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

void PreferencesDialog::save() {
    QSettings settings;
    settings.setValue(QLatin1String(prefs::kAuthor), author_->text().trimmed());
    settings.setValue(QLatin1String(prefs::kDefaultZoom), zoom_->currentData());
    settings.setValue(QLatin1String(prefs::kRestoreTabs), restoreTabs_->isChecked());

    const bool labels = toolbarLabels_->currentData().toBool();
    const bool appearance =
        settings.value(QLatin1String(prefs::kShowToolbar), true).toBool() != showToolbar_->isChecked() ||
        settings.value(QLatin1String(prefs::kToolbarLabels), true).toBool() != labels ||
        settings.value(QLatin1String(icons::kUseThemeKey), false).toBool() != themeIcons_->isChecked();
    settings.setValue(QLatin1String(prefs::kShowToolbar), showToolbar_->isChecked());
    settings.setValue(QLatin1String(prefs::kToolbarLabels), labels);
    settings.setValue(QLatin1String(icons::kUseThemeKey), themeIcons_->isChecked());

    settings.setValue(QStringLiteral("signing/name"), signName_->text().trimmed());
    settings.setValue(QStringLiteral("signing/location"), signLocation_->text().trimmed());
    settings.setValue(QStringLiteral("signing/useTsa"), useTsa_->isChecked());
    settings.setValue(QStringLiteral("signing/tsa"), tsa_->text().trimmed());
    settings.setValue(QStringLiteral("signing/ltv"), ltv_->isChecked());
    settings.setValue(QLatin1String(prefs::kSmartIdRpName), smartIdRpName_->text().trimmed());
    settings.setValue(QLatin1String(prefs::kSmartIdRpUuid), smartIdRpUuid_->text().trimmed());
    settings.setValue(QLatin1String(prefs::kMobileIdRpName), mobileIdRpName_->text().trimmed());
    settings.setValue(QLatin1String(prefs::kMobileIdRpUuid), mobileIdRpUuid_->text().trimmed());

    QStringList trusted;
    for (int i = 0; i < trusted_->count(); ++i) {
        trusted << trusted_->item(i)->data(Qt::UserRole).toString();
    }
    const bool trustEdited = trusted != settings.value(QLatin1String(prefs::kTrusted)).toStringList();
    settings.setValue(QLatin1String(prefs::kTrusted), trusted);

    if (!languages_.isEmpty()) {
        QStringList codes;
        for (QCheckBox* box : languages_) {
            if (box->isChecked()) {
                codes << box->property("code").toString();
            }
        }
        settings.setValue(QStringLiteral("ocr/languages"), codes);
        settings.setValue(QStringLiteral("ocr/dpi"), dpi_->value());
    }

    if (appearance) {
        emit appearanceChanged();
    }
    if (trustEdited) {
        emit trustChanged();
    }
}
