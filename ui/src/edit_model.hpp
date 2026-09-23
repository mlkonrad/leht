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
    QColor color;            ///< free text: its colour
};

/// One form field, for the Form panel. `type` is a leht::ops::FieldType cast
/// to int.
struct FieldRow {
    QString name;
    int type = 0;
    QString value;
    QStringList options;
    int page = 0;
    bool readOnly = false;
};

/// What the Sign dialog collected. The password lives only as long as the
/// signing request: it is handed to leht::crypto and wiped there.
struct SignSpec {
    QString p12Path;
    QString pkcs11Uri;  ///< a key on an ID card or token instead of p12Path
    QString password;   ///< the .p12 password, or the card's PIN
    QString field;      ///< an existing empty signature field, or empty
    int page = 0;
    QRectF rect;        ///< empty means an invisible signature
    QString name, reason, location;
    QString tsaUrl;
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
};

Q_DECLARE_METATYPE(AnnotRow)
Q_DECLARE_METATYPE(QVector<AnnotRow>)
Q_DECLARE_METATYPE(FieldRow)
Q_DECLARE_METATYPE(QVector<FieldRow>)
Q_DECLARE_METATYPE(SignSpec)
Q_DECLARE_METATYPE(SigRow)
Q_DECLARE_METATYPE(QVector<SigRow>)
