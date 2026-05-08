// sensor3d — minimal scaffold entry point.
//
// This empty Qt window exists so the build can be validated end-to-end on a
// fresh machine before any real UI is wired up. The first real milestone
// replaces this with src/ui/MainWindow + src/ui/Viewport3D.

#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QString>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("sensor3d"));
    QApplication::setOrganizationName(QStringLiteral("sensor3d"));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("sensor3d — scaffold"));
    window.resize(960, 640);

    auto* placeholder = new QLabel(
        QStringLiteral("sensor3d native build OK.\n"
                       "Replace src/main.cpp with the real UI."),
        &window);
    placeholder->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(placeholder);

    window.show();
    return QApplication::exec();
}
