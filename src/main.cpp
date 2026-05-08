// Circuit Capacity Calculator — minimal scaffold entry point.
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
    QApplication::setApplicationName(QStringLiteral("circuit_capacity_calculator"));
    QApplication::setOrganizationName(QStringLiteral("circuit_capacity_calculator"));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("Circuit Capacity Calculator — scaffold"));
    window.resize(960, 640);

    auto* placeholder = new QLabel(
        QStringLiteral("Circuit Capacity Calculator — native build OK.\n"
                       "Replace src/main.cpp with the real UI."),
        &window);
    placeholder->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(placeholder);

    window.show();
    return QApplication::exec();
}
