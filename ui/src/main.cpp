// SPDX-License-Identifier: AGPL-3.0-or-later
#include "main_window.hpp"
#include "outline_model.hpp"
#include "single_instance.hpp"

#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QTimer>

#include <memory>

int main(int argc, char** argv) {
    // Ties windows to the installed .desktop file: the Wayland app_id, and so
    // the icon and grouping in docks and task switchers. Set BEFORE the
    // application exists: Qt registers this ID with the desktop portal during
    // start-up, and once anything else has talked to the portal the connection
    // already has an ID ("Connection already associated with an application ID").
    QGuiApplication::setDesktopFileName(QStringLiteral(LEHT_APP_ID));
    // Read before Qt starts: it is this launch's to hand on, not Qt's to use up.
    const QByteArray activationToken = qgetenv("XDG_ACTIVATION_TOKEN");
    QApplication app(argc, argv);
    qRegisterMetaType<OutlineRow>();
    qRegisterMetaType<QVector<OutlineRow>>();
    QApplication::setApplicationName(QStringLiteral("Leht"));
    QApplication::setApplicationDisplayName(QStringLiteral("Leht"));
    QApplication::setWindowIcon(
        QIcon::fromTheme(QStringLiteral(LEHT_APP_ID), QIcon(QStringLiteral(":/leht/leht.svg"))));

    // `leht-viewer a.pdf b.pdf`: each opens as a tab. `--new-window` starts
    // a Leht of its own even when one is running.
    QStringList paths;
    bool ownWindow = false;
    const QStringList args = QApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--new-window")) {
            ownWindow = true;
        } else {
            paths << QFileInfo(args.at(i)).absoluteFilePath();  // against this launch's directory
        }
    }

    // One Leht per user: if one runs, it takes the files and this launch ends.
    std::unique_ptr<SingleInstance> instance;
    if (!ownWindow) {
        const QString name = SingleInstance::defaultName();
        if (SingleInstance::sendToRunning(name, paths, activationToken)) {
            return 0;
        }
        instance = std::make_unique<SingleInstance>(name);
        if (!instance->listen()) {
            // Another launch got there first: hand over to it after all, or
            // (if it does not answer) carry on alone.
            if (SingleInstance::sendToRunning(name, paths, activationToken)) {
                return 0;
            }
            instance.reset();
        } else {
            QObject::connect(instance.get(), &SingleInstance::filesReceived, &MainWindow::openHandedOver);
        }
    }

    MainWindow window;
    window.show();
    for (const QString& path : paths) {
        window.openDocument(path);
    }
    // The first time Leht runs: a short tour of the start screen.
    QTimer::singleShot(500, &window, [&window] { window.showFirstRunHints(); });

    return QApplication::exec();
}
