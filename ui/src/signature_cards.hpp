// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QVector>
#include <QWidget>

#include "edit_model.hpp"

class QVBoxLayout;

/// What a signature is worth, for a reader: a verdict and one sentence.
struct SignatureVerdict {
    enum class Level { Valid, Warning, Broken };
    Level level = Level::Valid;
    QString headline;  ///< "Valid", "Changed after signing", ...
    QString sentence;  ///< says what that means, in plain words
};

/// The verdict on `row`, the same judgement `leht verify` makes (exit codes
/// 4 broken, 5 untrusted, 6 changed after signing, 7 changed in a way a
/// certification or lock forbids), in words.
[[nodiscard]] SignatureVerdict judgeSignature(const SigRow& row);

/// The Signatures tab's cards: one per signature, its verdict as a coloured
/// shield and a sentence, who signed and when, whether it is qualified; the
/// technical details stay in the tree below them.
class SignatureCards : public QWidget {
    Q_OBJECT

public:
    explicit SignatureCards(QWidget* parent = nullptr);

    void setRows(const QVector<SigRow>& rows, bool haveTrustedList);
    [[nodiscard]] int count() const { return cards_; }

signals:
    void showPage(int page);
    /// The signer is not trusted: offer the trusted-certificates settings.
    void trustRequested();

private:
    QVBoxLayout* layout_ = nullptr;
    int cards_ = 0;
};
