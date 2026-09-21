// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_window.hpp"

#include <QApplication>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("Leht"));
    QApplication::setApplicationDisplayName(QStringLiteral("Leht"));

    MainWindow window;
    window.show();

    // A path on the command line opens immediately: `leht-viewer file.pdf`.
    const QStringList args = QApplication::arguments();
    if (args.size() > 1) {
        window.openPath(args.at(1));
    }

    return QApplication::exec();
}
