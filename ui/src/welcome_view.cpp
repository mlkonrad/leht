// SPDX-License-Identifier: AGPL-3.0-or-later
#include "welcome_view.hpp"

#include "icons.hpp"
#include "recent_files.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QPainter>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QUrl>

#include <algorithm>
#include <QVBoxLayout>

QStringList droppedFiles(const QMimeData* mime) {
    QStringList paths;
    if (mime == nullptr || !mime->hasUrls()) {
        return paths;
    }
    for (const QUrl& url : mime->urls()) {
        if (!url.isLocalFile() || !QFileInfo(url.toLocalFile()).isFile()) {
            return {};
        }
        paths << url.toLocalFile();
    }
    return paths;
}

namespace {

/// A task on the welcome view: an icon, a bold title and a line saying what it
/// is for. A button, so it takes focus, Space/Enter and screen readers.
class TaskCard final : public QAbstractButton {
public:
    TaskCard(const QIcon& icon, const QString& title, const QString& blurb, QWidget* parent)
        : QAbstractButton(parent), blurb_(blurb) {
        setIcon(icon);
        setText(title);
        setAccessibleName(title);
        setAccessibleDescription(blurb);
        setToolTip(blurb);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    QSize sizeHint() const override {
        const QFontMetrics fm(font());
        return {fm.horizontalAdvance(blurb_) + 90, std::max(64, fm.height() * 2 + 30)};
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette& pal = palette();
        const bool lit = underMouse() || hasFocus() || isDown();
        QPen pen(lit ? pal.color(QPalette::Highlight) : pal.color(QPalette::Mid), hasFocus() ? 2.0 : 1.0);
        p.setPen(pen);
        p.setBrush(isDown() ? pal.color(QPalette::AlternateBase) : pal.color(QPalette::Base));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);

        const int iconSize = 28;
        const QRect iconRect(14, (height() - iconSize) / 2, iconSize, iconSize);
        icon().paint(&p, iconRect, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);

        QFont bold = font();
        bold.setWeight(QFont::DemiBold);
        const QFontMetrics fb(bold);
        const QFontMetrics fm(font());
        const int x = iconRect.right() + 14;
        const int w = width() - x - 12;
        const int top = (height() - fb.height() - fm.height() - 2) / 2;
        p.setFont(bold);
        p.setPen(pal.color(QPalette::Text));
        p.drawText(QRect(x, top, w, fb.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fb.elidedText(text(), Qt::ElideRight, w));
        p.setFont(font());
        // Secondary text: the text colour a third of the way to the
        // background, which every theme gets right (PlaceholderText is often
        // left dark in dark palettes).
        const QColor t = pal.color(QPalette::Text);
        const QColor b = pal.color(QPalette::Base);
        p.setPen(QColor::fromRgbF(t.redF() * 0.62f + b.redF() * 0.38f, t.greenF() * 0.62f + b.greenF() * 0.38f,
                                  t.blueF() * 0.62f + b.blueF() * 0.38f));
        p.drawText(QRect(x, top + fb.height() + 2, w, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(blurb_, Qt::ElideRight, w));
    }

private:
    QString blurb_;
};

QString homeRelative(const QString& dir) {
    const QString home = QDir::homePath();
    if (dir == home) {
        return QStringLiteral("~");
    }
    if (dir.startsWith(home + QLatin1Char('/'))) {
        return QStringLiteral("~") + dir.mid(home.size());
    }
    return dir;
}

}  // namespace

WelcomeView::WelcomeView(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("welcomeView"));
    setAcceptDrops(true);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    auto* page = new QWidget(scroll);
    scroll->setWidget(page);
    auto* centre = new QHBoxLayout(page);
    centre->addStretch(1);
    auto* column = new QWidget(page);
    column->setMaximumWidth(760);
    column->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    centre->addWidget(column, 4);
    centre->addStretch(1);

    auto* layout = new QVBoxLayout(column);
    layout->setSpacing(14);
    layout->setContentsMargins(24, 32, 24, 32);

    // Heading: the mark, the name, the tagline (docs/branding.md).
    auto* heading = new QHBoxLayout;
    auto* mark = new QLabel(column);
    mark->setPixmap(QApplication::windowIcon().pixmap(56, 56));
    heading->addWidget(mark);
    auto* titles = new QVBoxLayout;
    auto* name = new QLabel(tr("Leht"), column);
    QFont big = name->font();
    big.setPointSizeF(big.pointSizeF() * 2.0);
    big.setWeight(QFont::DemiBold);
    name->setFont(big);
    titles->addWidget(name);
    titles->addWidget(new QLabel(tr("Every PDF, one page at a time."), column));
    heading->addLayout(titles);
    heading->addStretch(1);
    layout->addLayout(heading);

    // The drop zone holds the main button: open, or drop files anywhere here.
    dropFrame_ = new QFrame(column);
    dropFrame_->setObjectName(QStringLiteral("dropZone"));
    auto* drop = new QVBoxLayout(dropFrame_);
    drop->setContentsMargins(20, 22, 20, 22);
    auto* open = new QPushButton(tr("Open a PDF…"), dropFrame_);
    open->setObjectName(QStringLiteral("welcomeOpen"));
    open->setDefault(true);
    open->setMinimumHeight(40);
    open->setToolTip(tr("Open a PDF (%1)").arg(QKeySequence(QKeySequence::Open).toString(QKeySequence::NativeText)));
    connect(open, &QPushButton::clicked, this, &WelcomeView::openRequested);
    drop->addWidget(open, 0, Qt::AlignHCenter);
    dropHint_ = new QLabel(tr("or drop PDFs and images here — several at once to combine them"), dropFrame_);
    dropHint_->setAlignment(Qt::AlignCenter);
    dropHint_->setWordWrap(true);
    drop->addWidget(dropHint_);
    layout->addWidget(dropFrame_);

    // Task cards: start from what you want done, not from a menu.
    auto* tasksTitle = new QLabel(tr("What would you like to do?"), column);
    QFont section = tasksTitle->font();
    section.setWeight(QFont::DemiBold);
    tasksTitle->setFont(section);
    layout->addSpacing(6);
    layout->addWidget(tasksTitle);
    const struct {
        const char* id;
        const char* icon;
        const char* title;
        const char* blurb;
    } kTasks[] = {
        {kSign, "signature", QT_TR_NOOP("Sign a document"), QT_TR_NOOP("With your ID card or a certificate file")},
        {kFill, "text-cursor-input", QT_TR_NOOP("Fill in a form"), QT_TR_NOOP("Type into the form's fields")},
        {kCombine, "combine", QT_TR_NOOP("Combine files"), QT_TR_NOOP("PDFs and images into one PDF")},
        {kOcr, "scan-text", QT_TR_NOOP("Make a scan searchable"), QT_TR_NOOP("Recognize the text on scanned pages")},
        {kReduce, "minimize-2", QT_TR_NOOP("Reduce file size"), QT_TR_NOOP("A smaller copy, for e-mail or the web")},
        {kVerify, "shield-check", QT_TR_NOOP("Check signatures"), QT_TR_NOOP("Who signed, and whether it changed since")},
    };
    auto* grid = new QGridLayout;
    grid->setSpacing(10);
    int i = 0;
    for (const auto& t : kTasks) {
        auto* card = new TaskCard(icons::named(QLatin1String(t.icon)), tr(t.title), tr(t.blurb), column);
        card->setObjectName(QStringLiteral("task_%1").arg(QLatin1String(t.id)));
        const QString id = QLatin1String(t.id);
        connect(card, &QAbstractButton::clicked, this, [this, id] { emit taskChosen(id); });
        grid->addWidget(card, i / 2, i % 2);
        ++i;
    }
    layout->addLayout(grid);

    // Recent files.
    recentTitle_ = new QLabel(tr("Recent files"), column);
    recentTitle_->setFont(section);
    layout->addSpacing(6);
    layout->addWidget(recentTitle_);
    recent_ = new QListWidget(column);
    recent_->setObjectName(QStringLiteral("recentFiles"));
    recent_->setIconSize(QSize(22, 22));
    recent_->setUniformItemSizes(true);
    recent_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(recent_, &QListWidget::itemActivated, this, [this](QListWidgetItem* item) {
        emit openPath(item->data(Qt::UserRole).toString());
    });
    connect(recent_, &QListWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        QListWidgetItem* item = recent_->itemAt(at);
        QMenu menu(this);
        if (item != nullptr) {
            const QString path = item->data(Qt::UserRole).toString();
            menu.addAction(tr("Open"), this, [this, path] { emit openPath(path); });
            menu.addAction(tr("Remove from List"), this, [this, path] {
                recent::remove(path);
                refresh();
            });
            menu.addSeparator();
        }
        menu.addAction(tr("Clear List"), this, [this] {
            recent::clear();
            refresh();
        });
        menu.exec(recent_->viewport()->mapToGlobal(at));
    });
    layout->addWidget(recent_);
    noRecent_ = new QLabel(tr("Files you open will be listed here."), column);
    noRecent_->setEnabled(false);
    layout->addWidget(noRecent_);
    layout->addStretch(1);

    restyle();
    refresh();
}

void WelcomeView::refresh() {
    recent_->clear();
    const QIcon fileIcon = icons::named(QStringLiteral("file-text"));
    for (const QString& path : recent::files()) {
        const QFileInfo info(path);
        auto* item = new QListWidgetItem(fileIcon, QStringLiteral("%1   —   %2").arg(
                                                       info.fileName(), homeRelative(info.absolutePath())),
                                         recent_);
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        item->setData(Qt::AccessibleTextRole, tr("%1, in %2").arg(info.fileName(), homeRelative(info.absolutePath())));
    }
    const bool any = recent_->count() > 0;
    // As tall as its rows, so a short list does not leave an empty box.
    const int rows = std::min(recent_->count(), recent::kMax);
    recent_->setFixedHeight(rows * std::max(recent_->sizeHintForRow(0), 26) + 12);
    recent_->setVisible(any);
    noRecent_->setVisible(!any);
}

void WelcomeView::restyle() {
    // Palette roles only, so light and dark themes both work.
    setStyleSheet(QStringLiteral(
        "QFrame#dropZone { border: 2px dashed palette(mid); border-radius: 10px; }"
        "QFrame#dropZone[active=\"true\"] { border-color: palette(highlight);"
        "  background: palette(alternate-base); }"
        "QPushButton#welcomeOpen { background: palette(highlight); color: palette(highlighted-text);"
        "  border: none; border-radius: 6px; padding: 8px 22px; font-weight: 600; }"
        "QPushButton#welcomeOpen:focus { outline: none; border: 2px solid palette(highlighted-text); }"
        "QListWidget#recentFiles { border: 1px solid palette(mid); border-radius: 8px; padding: 4px; }"));
}

void WelcomeView::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange) {
        // The card icons are drawn in the palette's colour when painted; the
        // list's cached ones need making again.
        refresh();
    }
    QWidget::changeEvent(event);
}

void WelcomeView::setDropHighlight(bool on) {
    dropFrame_->setProperty("active", on);
    dropFrame_->style()->unpolish(dropFrame_);
    dropFrame_->style()->polish(dropFrame_);
    dropHint_->setText(on ? tr("Drop to open — several files are combined into one PDF")
                          : tr("or drop PDFs and images here — several at once to combine them"));
}

void WelcomeView::dragEnterEvent(QDragEnterEvent* event) {
    if (!droppedFiles(event->mimeData()).isEmpty()) {
        setDropHighlight(true);
        event->acceptProposedAction();
    }
}

void WelcomeView::dragLeaveEvent(QDragLeaveEvent* event) {
    setDropHighlight(false);
    QWidget::dragLeaveEvent(event);
}

void WelcomeView::dropEvent(QDropEvent* event) {
    setDropHighlight(false);
    const QStringList paths = droppedFiles(event->mimeData());
    if (!paths.isEmpty()) {
        event->acceptProposedAction();
        emit filesDropped(paths);
    }
}
