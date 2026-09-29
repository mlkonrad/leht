// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QColor>
#include <QMetaType>
#include <QPolygonF>
#include <QRectF>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include <cmath>

/// One annotation, as the GUI sees it: for hit-testing (erasing) and listing.
/// Built on the worker thread from ops::AnnotInfo. Geometry is in base
/// coordinates, like every other box the view draws.
struct AnnotRow {
    int id = 0;
    int page = 0;
    QString type;
    QRectF rect;
    QString contents;
    bool movable = false;    ///< the Move tool may pick it up
    bool resizable = false;  ///< ...and resize it (a note's icon cannot be)
    double fontSize = 0;     ///< free text: its size in points
    QColor color;            ///< its colour (free text: its text's)
    QString author;
    double opacity = 1;
    double lineWidth = 0;    ///< ink, shapes and lines; 0 otherwise
    bool styleable = false;  ///< its colour and the rest can be changed
};

/// One place a form field shows on a page.
struct FieldWidget {
    int page = 0;
    QRectF rect;      ///< in base coordinates
    QString onState;  ///< checkbox and radio: the state a click on it sets
};

/// One form field, for the Form panel and the outlines on the page. `type` is
/// a leht::ops::FieldType cast to int.
struct FieldRow {
    QString name;
    int type = 0;
    QString value;
    QStringList options;
    int page = 0;
    bool readOnly = false;
    QRectF rect;  ///< of the first widget, in base coordinates
    bool required = false;
    int maxLength = 0;  ///< text: most characters allowed, 0 = no limit
    QVector<FieldWidget> widgets;  ///< every widget; the first is `page`/`rect`
    bool multiline = false;        ///< text: takes several lines
    bool editableChoice = false;   ///< choice: also takes typed text
};

/// The order a reader fills a form in: page, then rows top to bottom (a few
/// points of slack, so fields on one line stay together), then left to right.
/// The Form panel lists fields in it, and Tab on the page follows it.
inline bool fieldReadsBefore(const FieldRow& a, const FieldRow& b) {
    if (a.page != b.page) {
        return a.page < b.page;
    }
    const double ay = std::round(a.rect.top() / 6.0);
    const double by = std::round(b.rect.top() / 6.0);
    if (ay != by) {
        return ay < by;
    }
    return a.rect.left() < b.rect.left();
}

/// What the Sign dialog collected. The password lives only as long as the
/// signing request: it is handed to leht::crypto and wiped there.
struct SignSpec {
    QString p12Path;
    QString pkcs11Uri;  ///< a key on an ID card or token instead of p12Path
    QString password;   ///< the .p12 password, or the card's PIN
    int certify = 0;    ///< certify the document at this DocMDP level; 0: an ordinary signature
    QString field;      ///< an existing empty signature field, or empty
    int page = 0;
    QRectF rect;        ///< empty means an invisible signature
    QString name, reason, location;
    QString tsaUrl;
    /// Then embed validation data and a document timestamp (PAdES B-LTA).
    /// Needs tsaUrl; fetches OCSP and CRL data over the network.
    bool ltv = false;
    QByteArray image;                 ///< PNG/JPEG bytes for the appearance
    QVector<QPolygonF> strokes;       ///< a drawn signature
    QSizeF strokesCanvas;
    QStringList lines;                ///< text shown beside the graphic
};

/// The PKCS#11 library to find card keys in. Empty means every module the
/// system has registered with p11-kit, which includes OpenSC; the environment
/// variable LEHT_PKCS11_MODULE names one that did not register itself (and
/// the tests' SoftHSM).
inline QString pkcs11Module() { return qEnvironmentVariable("LEHT_PKCS11_MODULE"); }

/// One signature, as the panel shows it: strings and flags only. The viewer
/// never parses a certificate or a CMS blob -- the worker did that, inside its
/// sandbox -- so nothing here needs interpreting.
struct SigRow {
    QString field;
    int page = -1;
    QRectF rect;
    QString subfilter, name, reason, location, claimedTime;
    bool rangeOk = false;
    QString rangeProblem;
    bool changedAfterSigning = false;
    bool laterSignatureCoversChanges = false;
    int certification = 0;       ///< DocMDP level this signature certifies with; 0 none
    QString locks;               ///< the fields it locks, in words
    bool changesJudged = false;  ///< a certification or lock applies to later changes
    bool changesPermitted = true;
    QStringList changeProblems;
    bool checked = false;
    bool intact = false;
    QString problem;
    QString digest;
    QString signer, signerCommonName, issuer, serial, fingerprint;
    qint64 notBefore = 0, notAfter = 0;
    QStringList chain;
    int trust = 0;  ///< leht::crypto::Trust
    QString trustDetail;
    bool hasTimestamp = false;
    bool timestampValid = false;
    qint64 timestampTime = 0;
    QString authority;
    int timestampTrust = 0;
    QString timestampProblem;
    /// A document timestamp (B-LTA): the timestamp fields and `intact` say
    /// it all; it has no signer.
    bool documentTimestamp = false;
    /// Added to after signing, but only with validation data: unchanged.
    bool onlyValidationDataAfter = false;
    /// Each chain certificate's revocation status, in words; empty when there
    /// was no revocation data to check against.
    QStringList revocation;
    QStringList timestampRevocation;
    bool revoked = false;  ///< one of them was revoked before the signing time
    /// The EU trusted lists' verdict (crypto::QualifiedReport::Level: 0 not
    /// checked, 1 not qualified, 2 QES, 3 qualified seal, 4 advanced with a
    /// qualified certificate), and why.
    int qualified = 0;
    QString qualifiedDetail;
    QString qualifiedService;  ///< the qualified CA, with its country
    bool timestampQualified = false;
};

Q_DECLARE_METATYPE(AnnotRow)
Q_DECLARE_METATYPE(QVector<AnnotRow>)
Q_DECLARE_METATYPE(FieldRow)
Q_DECLARE_METATYPE(QVector<FieldRow>)
Q_DECLARE_METATYPE(SignSpec)
Q_DECLARE_METATYPE(SigRow)
Q_DECLARE_METATYPE(QVector<SigRow>)
