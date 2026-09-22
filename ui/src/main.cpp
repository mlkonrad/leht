// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_window.hpp"
#include "outline_model.hpp"

#include <QApplication>
#include <QIcon>

int main(int argc, char** argv) {
    // Ties windows to the installed .desktop file: the Wayland app_id, and so
    // the icon and grouping in docks and task switchers. Set BEFORE the
    // application exists: Qt registers this ID with the desktop portal during
    // start-up, and once anything else has talked to the portal the connection
    // already has an ID ("Connection already associated with an application ID").
    QGuiApplication::setDesktopFileName(QStringLiteral(LEHT_APP_ID));
    QApplication app(argc, argv);
    qRegisterMetaType<OutlineRow>();
    qRegisterMetaType<QVector<OutlineRow>>();
    QApplication::setApplicationName(QStringLiteral("Leht"));
    QApplication::setApplicationDisplayName(QStringLiteral("Leht"));
    QApplication::setWindowIcon(
        QIcon::fromTheme(QStringLiteral(LEHT_APP_ID), QIcon(QStringLiteral(":/leht/leht.svg"))));

    MainWindow window;
    window.show();

    // A path on the command line opens immediately: `leht-viewer file.pdf`.
    const QStringList args = QApplication::arguments();
    if (args.size() > 1) {
        window.openPath(args.at(1));
    }

    return QApplication::exec();
}
