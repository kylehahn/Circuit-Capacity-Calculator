#include "ui/MainWindow.hpp"

#include "core/Bem.hpp"
#include "core/FastCapIo.hpp"
#include "io/GerberProject.hpp"
#include "io/KicadPcbIo.hpp"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QPainter>
#include <QPixmap>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <fstream>
#include <cmath>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <vector>

namespace {

int writeBcuSigCapMatrix(const QString& gerberDir,
                         const QString& kicadPath,
                         const QString& csvPath,
                         const QString& solverName,
                         double panelSize) {
    try {
        auto model = !gerberDir.isEmpty()
            ? ccc::io::readGerberProject(gerberDir.toStdString(), kicadPath.toStdString())
            : ccc::io::readKicadPcb(kicadPath.toStdString());
        const std::regex sigRe(R"(^/SIG\d+$)");
        const bool wholeImportedNet = !kicadPath.isEmpty();
        std::map<std::string, ccc::core::ConductorRefs> refsByNet;
        for (const auto& p : model.pads) {
            if ((wholeImportedNet || p.layer == "B.Cu") && std::regex_match(p.net, sigRe)) {
                refsByNet[p.net].padIds.push_back(p.id);
            }
        }
        for (const auto& t : model.traces) {
            if ((wholeImportedNet || t.layer == "B.Cu") && std::regex_match(t.net, sigRe)) {
                refsByNet[t.net].traceIds.push_back(t.id);
            }
        }
        for (const auto& z : model.zones) {
            if ((wholeImportedNet || z.layerId == "B.Cu") && std::regex_match(z.net, sigRe)) {
                refsByNet[z.net].zoneIds.push_back(z.id);
            }
        }
        if (refsByNet.size() != 16) return 5;

        auto sigNumber = [](const std::string& net) {
            return std::stoi(net.substr(4));
        };
        std::vector<std::string> nets;
        for (const auto& [net, _] : refsByNet) nets.push_back(net);
        std::sort(nets.begin(), nets.end(), [&](const auto& a, const auto& b) {
            return sigNumber(a) < sigNumber(b);
        });

        const QFileInfo outInfo(csvPath);
        if (!outInfo.absolutePath().isEmpty()) QDir().mkpath(outInfo.absolutePath());
        std::ofstream csv(csvPath.toStdString());
        if (!csv) return 7;
        csv << "solver,net_a,net_b,panels_a,panels_b,cm_f,cm_pf,"
               "assemble_ms,solve_ms,iters_a,iters_b,residual_a,residual_b\n";

        ccc::core::BemOptions opts;
        opts.panelSize = panelSize;
        opts.panelSizePad = panelSize;
        opts.panelSizeFpc = panelSize;
        opts.panelSizeTrace = panelSize;
        opts.epsEff = 1.0;
        opts.imageShield = false;
        opts.solver = ccc::core::BemSolver::DirectLU;
        opts.iterTol = 1e-8;
        opts.iterMaxIters = 2000;

        ccc::core::FastCapEnvironmentOptions fcEnv;
        const QString solverLower = solverName.toLower();
        fcEnv.solver = solverLower.contains(QStringLiteral("faster"))
            ? ccc::core::ExternalCapSolver::FasterCapAdaptive
            : ccc::core::ExternalCapSolver::FastCapFixed;
        fcEnv.includeGroundNets = true;
        fcEnv.includeDielectricStack = true;
        fcEnv.environmentMarginMm = 20.0;
        fcEnv.environmentPanelSize = std::max(1.0, panelSize * 2.0);
        fcEnv.dielectricPanelSize = std::max(2.0, fcEnv.environmentPanelSize * 2.0);

        int computed = 0;
        double minCm = std::numeric_limits<double>::infinity();
        double maxCm = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < nets.size(); ++i) {
            for (std::size_t j = i + 1; j < nets.size(); ++j) {
                const auto r = ccc::core::computeMutualCapacitanceFastCapWithEnvironment(
                    model, refsByNet[nets[i]], refsByNet[nets[j]], nets[i], nets[j], opts, fcEnv);
                if (!std::isfinite(r.Cm)) return 8;
                minCm = std::min(minCm, r.Cm);
                maxCm = std::max(maxCm, r.Cm);
                ++computed;
                csv << ccc::core::externalCapSolverName(fcEnv.solver) << ','
                    << nets[i] << ',' << nets[j] << ','
                    << r.NA << ',' << r.NB << ','
                    << r.Cm << ',' << r.Cm * 1.0e12 << ','
                    << r.assembleMs << ',' << r.solveMs << ','
                    << r.iters[0] << ',' << r.iters[1] << ','
                    << r.residual[0] << ',' << r.residual[1] << '\n';
            }
        }
        if (computed != 120) return 9;
        if (!(maxCm > minCm * 1.10)) return 10;
        return 0;
    } catch (...) {
        return 11;
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("circuit_capacity_calculator"));
    QApplication::setOrganizationName(QStringLiteral("circuit_capacity_calculator"));

    const QStringList args = app.arguments();
    auto argValue = [&args](const QString& name) {
        const int i = args.indexOf(name);
        if (i >= 0 && i + 1 < args.size()) return args[i + 1];
        return QString();
    };

    const QString gerberDir  = argValue(QStringLiteral("--open-gerber"));
    const QString kicadPath  = argValue(QStringLiteral("--open-kicad"));
    const QString soloLayer  = argValue(QStringLiteral("--solo-layer"));
    const QString selectNet  = argValue(QStringLiteral("--select-net"));
    const QString selectCapNets = argValue(QStringLiteral("--select-cap-nets"));
    const QString screenshot = argValue(QStringLiteral("--screenshot"));
    const QString sigMatrix  = argValue(QStringLiteral("--sig-cap-matrix"));
    const QString solverName = argValue(QStringLiteral("--solver")).isEmpty()
                                   ? QStringLiteral("fastcap")
                                   : argValue(QStringLiteral("--solver"));
    const double sigPanelSize = argValue(QStringLiteral("--sig-panel-size")).isEmpty()
                                    ? 1.0
                                    : argValue(QStringLiteral("--sig-panel-size")).toDouble();
    const bool quitAfterScreenshot = args.contains(QStringLiteral("--quit-after-screenshot"));

    if (!sigMatrix.isEmpty()) {
        if (gerberDir.isEmpty() && kicadPath.isEmpty()) return 2;
        return writeBcuSigCapMatrix(gerberDir, kicadPath, sigMatrix, solverName, sigPanelSize);
    }

    ccc::ui::MainWindow w;
    bool opened = true;
    if (!gerberDir.isEmpty() || !kicadPath.isEmpty()) {
        opened = w.openPcbProject(gerberDir, kicadPath, true);
    }
    if (!opened) return 2;
    if (!soloLayer.isEmpty()) w.showOnlyLayer(soloLayer);
    if (!selectNet.isEmpty() && !w.selectNet(selectNet)) return 4;
    if (!selectCapNets.isEmpty()) {
        const QStringList nets = selectCapNets.split(QStringLiteral(","));
        if (nets.size() != 2 || !w.selectCapNets(nets[0].trimmed(), nets[1].trimmed())) return 4;
    }

    w.show();
    if (!screenshot.isEmpty()) {
        QTimer::singleShot(1800, &w, [&w, screenshot, quitAfterScreenshot]() {
            const QFileInfo outInfo(screenshot);
            if (!outInfo.absolutePath().isEmpty()) {
                QDir().mkpath(outInfo.absolutePath());
            }
            QByteArray format = outInfo.suffix().toLatin1().toUpper();
            if (format.isEmpty()) format = "BMP";
            bool supported = false;
            for (const auto& f : QImageWriter::supportedImageFormats()) {
                if (QString::fromLatin1(f).compare(QString::fromLatin1(format),
                                                   Qt::CaseInsensitive) == 0) {
                    supported = true;
                    break;
                }
            }
            if (!supported) format = "BMP";
            w.repaint();
            QApplication::processEvents();
            const QPixmap shot = w.grab();
            const bool shotNull = shot.isNull();
            bool saved = !shot.isNull()
                         && shot.save(outInfo.absoluteFilePath(), format.constData());
            if (!saved) {
                QImage image(w.size() * w.devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
                image.setDevicePixelRatio(w.devicePixelRatioF());
                image.fill(Qt::transparent);
                QPainter painter(&image);
                w.render(&painter);
                painter.end();
                saved = image.save(outInfo.absoluteFilePath(), format.constData());
            }
            if (!saved) {
                std::ofstream log((outInfo.absoluteFilePath() + QStringLiteral(".txt")).toStdString());
                log << "window=" << w.width() << "x" << w.height() << "\n";
                log << "devicePixelRatio=" << w.devicePixelRatioF() << "\n";
                log << "format=" << format.constData() << "\n";
                log << "grabNull=" << (shotNull ? "true" : "false") << "\n";
                log << "grabSize=" << shot.width() << "x" << shot.height() << "\n";
                log << "formats=";
                for (const auto& f : QImageWriter::supportedImageFormats()) {
                    log << f.constData() << ",";
                }
                log << "\n";
            }
            if (quitAfterScreenshot) QApplication::exit(saved ? 0 : 3);
        });
    }
    return QApplication::exec();
}
