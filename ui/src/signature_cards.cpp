// SPDX-License-Identifier: AGPL-3.0-or-later
#include "signature_cards.hpp"

#include "icons.hpp"
#include "properties_dialog.hpp"  // parsePdfDate
#include "leht/crypto/crypto.hpp"

#include <QDateTime>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

namespace {

bool trusted(int trust) {
    return static_cast<leht::crypto::Trust>(trust) == leht::crypto::Trust::Trusted;
}

QString when(qint64 unixSeconds) {
    return QLocale().toString(QDateTime::fromSecsSinceEpoch(unixSeconds), QLocale::LongFormat);
}

QColor colourOf(SignatureVerdict::Level level) {
    switch (level) {
        case SignatureVerdict::Level::Broken: return {200, 30, 30};
        case SignatureVerdict::Level::Warning: return {210, 130, 0};
        case SignatureVerdict::Level::Valid: break;
    }
    return {30, 150, 60};
}

}  // namespace

SignatureVerdict judgeSignature(const SigRow& row) {
    using L = SignatureVerdict::Level;
    const auto tr = [](const char* s) { return SignatureCards::tr(s); };
    if (!row.rangeOk || !row.intact) {
        const QString why = !row.rangeOk ? row.rangeProblem : row.problem;
        return {L::Broken, tr("Broken"),
                tr("The signed part of the document has been altered, or the signature is damaged. "
                   "Do not rely on it.") +
                    (why.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(why))};
    }
    if (row.changesJudged && !row.changesPermitted) {
        return {L::Broken, tr("Changed in a forbidden way"),
                tr("The document was changed after signing in a way this signature does not allow: %1.")
                    .arg(row.changeProblems.join(QStringLiteral("; ")))};
    }
    if (static_cast<leht::crypto::Trust>(row.trust) == leht::crypto::Trust::Revoked) {
        return {L::Broken, tr("Certificate revoked"),
                tr("The signature is intact, but the signer's certificate was revoked: the signer's "
                   "authority withdrew it.")};
    }
    if (row.changedAfterSigning && !row.laterSignatureCoversChanges && !row.onlyValidationDataAfter) {
        return {L::Warning, tr("Changed after signing"),
                tr("The signature is intact, but the document was added to afterwards. What was signed "
                   "has not changed; what came later is not covered by it.")};
    }
    if (!trusted(row.trust)) {
        return {L::Warning, row.documentTimestamp ? tr("Authority not trusted") : tr("Signer not trusted"),
                row.documentTimestamp
                    ? tr("The timestamp is intact, but Leht does not know the authority that made it.")
                    : tr("The signature is intact and the document unchanged, but Leht cannot trace the "
                         "signer's certificate to an authority it trusts.")};
    }
    if (row.documentTimestamp) {
        return {L::Valid, tr("Valid"),
                tr("Proves that the document, up to this point, existed on %1.").arg(when(row.timestampTime))};
    }
    return {L::Valid, tr("Valid"), tr("The signature is valid, and the document has not changed since it "
                                      "was signed.")};
}

SignatureCards::SignatureCards(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("signatureCards"));
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(8);
}

void SignatureCards::setRows(const QVector<SigRow>& rows, bool haveTrustedList) {
    while (QLayoutItem* item = layout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    cards_ = 0;
    for (const SigRow& row : rows) {
        const SignatureVerdict v = judgeSignature(row);
        const QColor colour = colourOf(v.level);

        auto* card = new QFrame(this);
        card->setObjectName(QStringLiteral("signatureCard"));
        card->setFrameShape(QFrame::StyledPanel);
        card->setStyleSheet(QStringLiteral("QFrame#signatureCard { border: 1px solid rgba(%1, %2, %3, 140);"
                                           " border-left: 4px solid rgb(%1, %2, %3); border-radius: 6px;"
                                           " background: rgba(%1, %2, %3, 18); }")
                                .arg(colour.red()).arg(colour.green()).arg(colour.blue()));
        auto* body = new QVBoxLayout(card);
        body->setContentsMargins(10, 8, 10, 8);
        body->setSpacing(4);

        auto* top = new QHBoxLayout;
        auto* shield = new QLabel(card);
        shield->setPixmap(icons::tinted(v.level == SignatureVerdict::Level::Broken    ? QStringLiteral("shield-x")
                                        : v.level == SignatureVerdict::Level::Warning ? QStringLiteral("shield-alert")
                                                                                      : QStringLiteral("shield-check"),
                                        colour, 22, devicePixelRatioF()));
        top->addWidget(shield, 0, Qt::AlignTop);
        auto* titles = new QVBoxLayout;
        const QString who = row.documentTimestamp ? tr("Document timestamp")
                            : !row.signerCommonName.isEmpty() ? row.signerCommonName
                            : !row.name.isEmpty() ? row.name
                                                  : row.field;
        auto* name = new QLabel(who, card);
        QFont bold = name->font();
        bold.setWeight(QFont::DemiBold);
        name->setFont(bold);
        name->setWordWrap(true);
        titles->addWidget(name);
        auto* headline = new QLabel(v.headline, card);
        QPalette pal = headline->palette();
        pal.setColor(QPalette::WindowText, colour);
        headline->setPalette(pal);
        headline->setFont(bold);
        titles->addWidget(headline);
        top->addLayout(titles, 1);
        body->addLayout(top);

        auto* sentence = new QLabel(v.sentence, card);
        sentence->setWordWrap(true);
        sentence->setMinimumWidth(0);
        body->addWidget(sentence);

        // When: a trusted timestamp proves it; otherwise the signer's own claim.
        QString facts;
        if (!row.documentTimestamp) {
            if (row.hasTimestamp && row.timestampValid) {
                facts = tr("Signed %1, proved by a timestamp.").arg(when(row.timestampTime));
            } else if (!row.claimedTime.isEmpty()) {
                const QDateTime claimed = parsePdfDate(row.claimedTime);
                facts = tr("Says it was signed %1; nothing proves when.")
                            .arg(claimed.isValid()
                                     ? QLocale().toString(claimed.toLocalTime(), QLocale::LongFormat)
                                     : row.claimedTime);
            }
            if (!row.reason.isEmpty()) {
                facts += QLatin1Char(' ') + tr("Reason: %1.").arg(row.reason);
            }
            if (row.certification != 0) {
                facts += QLatin1Char(' ') + tr("It certifies the document.");
            }
        }
        if (!facts.isEmpty()) {
            auto* line = new QLabel(facts.trimmed(), card);
            line->setWordWrap(true);
            body->addWidget(line);
        }

        auto* badges = new QHBoxLayout;
        if (row.intact && !row.documentTimestamp) {
            QString q;
            switch (row.qualified) {
                case 2: q = tr("Qualified electronic signature"); break;
                case 3: q = tr("Qualified electronic seal"); break;
                case 4: q = tr("Advanced, qualified certificate"); break;
                case 1: q = tr("Not qualified"); break;
                default: q = haveTrustedList ? QString() : tr("Qualification not checked"); break;
            }
            if (!q.isEmpty()) {
                auto* badge = new QLabel(q, card);
                badge->setObjectName(QStringLiteral("qualifiedBadge"));
                badge->setToolTip(row.qualifiedService.isEmpty() ? row.qualifiedDetail
                                                                 : tr("%1 (%2)").arg(row.qualifiedService,
                                                                                     row.qualifiedDetail));
                const bool yes = row.qualified >= 2;
                badge->setStyleSheet(yes ? QStringLiteral("QLabel { border: 1px solid rgb(30,120,200); color: "
                                                          "rgb(30,120,200); border-radius: 8px; padding: 1px 8px; }")
                                         : QStringLiteral("QLabel { border: 1px solid palette(mid); border-radius: "
                                                          "8px; padding: 1px 8px; }"));
                badges->addWidget(badge);
            }
        }
        badges->addStretch(1);
        body->addLayout(badges);
        auto* buttons = new QHBoxLayout;
        buttons->setSpacing(4);
        if (row.page >= 0) {
            auto* show = new QPushButton(icons::named(QStringLiteral("eye")), tr("Show"), card);
            show->setFlat(true);
            show->setToolTip(tr("Go to the page the signature is on"));
            const int page = row.page;
            connect(show, &QPushButton::clicked, this, [this, page] { emit showPage(page); });
            buttons->addWidget(show);
        }
        if (row.intact && row.rangeOk && !trusted(row.trust) &&
            static_cast<leht::crypto::Trust>(row.trust) != leht::crypto::Trust::Revoked) {
            auto* trust = new QPushButton(icons::named(QStringLiteral("key-round")), tr("Trusted Certificates…"), card);
            trust->setFlat(true);
            trust->setToolTip(tr("If you know where the signer's certificate authority comes from, add it"));
            connect(trust, &QPushButton::clicked, this, &SignatureCards::trustRequested);
            buttons->addWidget(trust);
        }
        buttons->addStretch(1);
        body->addLayout(buttons);
        card->setAccessibleName(tr("%1: %2").arg(who, v.headline));
        card->setAccessibleDescription(v.sentence);
        layout_->addWidget(card);
        ++cards_;
    }
}
