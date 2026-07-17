#include "MainWindow.hpp"

#include "core/Bem.hpp"
#include "core/FastCapIo.hpp"
#include "io/GerberProject.hpp"
#include "io/GlbIo.hpp"
#include "io/JsonIo.hpp"
#include "io/KicadPcbIo.hpp"

#include <QAbstractSpinBox>
#include <QAbstractItemView>
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDialog>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QFuture>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QProgressDialog>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStatusBar>
#include <QHeaderView>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QUndoCommand>
#include <QUndoStack>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>
#include <QtConcurrent/QtConcurrentRun>

#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <fstream>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

namespace ccc::ui {

// ---------------------------------------------------------------------------
//  Snapshot-based undo command
// ---------------------------------------------------------------------------
namespace {
class ModelSnapshotCmd : public QUndoCommand {
public:
    ModelSnapshotCmd(MainWindow* mw, ccc::core::Model before, ccc::core::Model after, const QString& text)
        : QUndoCommand(text),
          mw_(mw),
          before_(std::move(before)),
          after_(std::move(after)) {}
    void undo() override { mw_->restoreModelState(before_); }
    void redo() override {
        if (firstRedo_) { firstRedo_ = false; return; }
        mw_->restoreModelState(after_);
    }
private:
    MainWindow*       mw_;
    ccc::core::Model  before_;
    ccc::core::Model  after_;
    bool              firstRedo_ = true;
};

// Dispatch save/load by file extension. .glb -> binary glTF; everything else -> JSON.
void writeAny(const ccc::core::Model& m, const QString& path) {
    if (path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive))
        ccc::io::writeModelGlb(m, path.toStdString());
    else
        ccc::io::writeModelFile(m, path.toStdString());
}
ccc::core::Model readAny(const QString& path) {
    if (path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive))
        return ccc::io::readModelGlb(path.toStdString());
    return ccc::io::readModelFile(path.toStdString());
}

ccc::core::ConductorRefs refsForNet(const ccc::core::Model& model, const QString& net) {
    ccc::core::ConductorRefs refs;
    const std::string target = net.toStdString();
    for (const auto& p : model.pads) {
        if (p.net == target) refs.padIds.push_back(p.id);
    }
    for (const auto& t : model.traces) {
        if (t.net == target) refs.traceIds.push_back(t.id);
    }
    for (const auto& z : model.zones) {
        if (z.net == target) refs.zoneIds.push_back(z.id);
    }
    return refs;
}

bool refsEmpty(const ccc::core::ConductorRefs& refs) {
    return refs.padIds.empty() && refs.fpcIds.empty()
        && refs.traceIds.empty() && refs.zoneIds.empty();
}

constexpr int kCapModeFusion = 100;

bool capModeIsFusion(int mode) {
    return mode == kCapModeFusion;
}

ccc::core::ExternalCapSolver solverForCapMode(int mode) {
    return mode == int(ccc::core::ExternalCapSolver::FasterCapAdaptive)
        ? ccc::core::ExternalCapSolver::FasterCapAdaptive
        : ccc::core::ExternalCapSolver::FastCapFixed;
}

QString labelForCapMode(int mode) {
    if (capModeIsFusion(mode)) return QStringLiteral("Fusion sweep");
    return QString::fromStdString(
        ccc::core::externalCapSolverName(solverForCapMode(mode)));
}

bool capModeAvailable(int mode, bool fastCapAvailable, bool fasterCapAvailable) {
    if (capModeIsFusion(mode)) return fastCapAvailable && fasterCapAvailable;
    return solverForCapMode(mode) == ccc::core::ExternalCapSolver::FasterCapAdaptive
        ? fasterCapAvailable
        : fastCapAvailable;
}

QString percentText(double value) {
    return QStringLiteral("%1%").arg(value * 100.0, 0, 'f', 2);
}

QString fusionResultHtml(const ccc::core::FastCapFusionResult& fusion,
                         const QString& netA,
                         const QString& netB) {
    auto cap = [](double value) {
        return QString::fromStdString(ccc::core::formatCapacitance(value));
    };
    QString html = QStringLiteral(
        "<div><b>Fusion C<sub>m</sub> check</b></div>"
        "<div style='color:#555'>%1 &harr; %2</div>"
        "<table cellspacing='0' cellpadding='2'>"
        "<tr><th align='left'>Backend</th><th align='right'>Panel</th>"
        "<th align='right'>C<sub>m</sub></th><th align='right'>Solve</th></tr>")
        .arg(netA.toHtmlEscaped())
        .arg(netB.toHtmlEscaped());
    for (const auto& point : fusion.fastCapSweep) {
        html += QStringLiteral(
            "<tr><td>FastCap</td><td align='right'>%1 mm</td>"
            "<td align='right'>%2</td><td align='right'>%3 ms</td></tr>")
            .arg(point.panelSizeMm, 0, 'f', 1)
            .arg(cap(point.result.Cm))
            .arg(point.result.solveMs, 0, 'f', 0);
    }
    html += QStringLiteral(
        "<tr><td>FasterCap ref</td><td align='right'>%1 mm</td>"
        "<td align='right'>%2</td><td align='right'>%3 ms</td></tr></table>")
        .arg(fusion.fasterCapReference.panelSizeMm, 0, 'f', 1)
        .arg(cap(fusion.fasterCapReference.result.Cm))
        .arg(fusion.fasterCapReference.result.solveMs, 0, 'f', 0);

    const bool fastStable = fusion.fastCapFineRelativeDelta <= 0.02;
    const bool refClose = fusion.fasterCapReferenceRelativeDelta <= 0.05;
    const QString verdict = fastStable && refClose
        ? QStringLiteral("FastCap 0.5 mm looks usable for fast sweeps.")
        : QStringLiteral("Use finer FastCap panels or FasterCap reference for this pair.");
    html += QStringLiteral(
        "<div style='color:#666'>FastCap 0.5->0.2 delta: %1<br/>"
        "FastCap 0.5 vs FasterCap ref: %2<br/>%3</div>")
        .arg(percentText(fusion.fastCapFineRelativeDelta))
        .arg(percentText(fusion.fasterCapReferenceRelativeDelta))
        .arg(verdict);
    return html;
}
}  // namespace

// ---------------------------------------------------------------------------
//  MainWindow
// ---------------------------------------------------------------------------
MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    undo_ = new QUndoStack(this);
    undo_->setUndoLimit(80);

    setupUi();
    setupMenus();
    setupModeToolbar();
    setupDocks();
    setupStatusBar();
    refreshFastCapAvailability();

    viewport_->setModel(&model_);
    rebuildLayerPanel();
    refreshStatus();
    setCurrentFile({});
    selectMode(Viewport3D::Mode::Select);
    resize(1280, 820);
    setWindowTitle(QStringLiteral("Circuit Capacity Calculator"));

    // Initial state: 2D top view, fit to model. Done after setModel so fitView
    // has geometry to work with.
    viewport_->setViewMode(Viewport3D::ViewMode::Two2D);
    viewport_->presetTop();
    viewport_->fitView();
}

void MainWindow::setupUi() {
    viewport_ = new Viewport3D(this);
    setCentralWidget(viewport_);

    connect(viewport_, &Viewport3D::selectionChanged, this, &MainWindow::onSelectionChanged);
    connect(viewport_, &Viewport3D::capSelectionChanged,
            this, [this](const QString&, const QString&) { updateCapSelectionPanel(); });
    connect(viewport_, &Viewport3D::modelEdited,      this, &MainWindow::onModelEdited);
    connect(viewport_, &Viewport3D::cursorMoved,      this, &MainWindow::onCursorMoved);
    connect(viewport_, &Viewport3D::modeChanged,      this, &MainWindow::onModeChanged);
    connect(viewport_, &Viewport3D::capacitanceRequested,
            this, [this]{
                updateCapSelectionPanel();
            });

    auto* tb = addToolBar(QStringLiteral("View"));
    tb->setObjectName(QStringLiteral("ViewToolbar"));
    tb->setMovable(false);
    auto add = [&](const QString& label, std::function<void()> cb) {
        auto* a = tb->addAction(label);
        connect(a, &QAction::triggered, this, cb);
    };
    // 2D/3D toggle. Checkable so it shows the current mode at a glance.
    auto* a2D = tb->addAction(QStringLiteral("2D"));
    a2D->setCheckable(true);
    a2D->setChecked(true);
    connect(a2D, &QAction::toggled, this, [this](bool on) {
        viewport_->setViewMode(on ? Viewport3D::ViewMode::Two2D
                                  : Viewport3D::ViewMode::Three3D);
    });
    tb->addSeparator();
    add(QStringLiteral("Iso"),   [this]{ viewport_->presetIso();   });
    add(QStringLiteral("Top"),   [this]{ viewport_->presetTop();   });
    add(QStringLiteral("Front"), [this]{ viewport_->presetFront(); });
    add(QStringLiteral("Side"),  [this]{ viewport_->presetSide();  });
    add(QStringLiteral("Fit"),   [this]{ viewport_->fitView();     });
    tb->addSeparator();
    auto* boardInfo = tb->addAction(QStringLiteral("Board Info"));
    boardInfo->setToolTip(QStringLiteral("Show stackup, copper thickness, and dielectric constants"));
    connect(boardInfo, &QAction::triggered, this, &MainWindow::showBoardInfoDialog);

    // Keep the toolbar checkbox in sync if view mode is changed elsewhere.
    connect(viewport_, &Viewport3D::viewModeChanged, this, [a2D](int v) {
        const bool is2D = (v == int(Viewport3D::ViewMode::Two2D));
        if (a2D->isChecked() != is2D) a2D->setChecked(is2D);
    });
}

void MainWindow::setupMenus() {
    auto* fileMenu = menuBar()->addMenu(QStringLiteral("&File"));

    auto* aNew  = fileMenu->addAction(QStringLiteral("&New"));
    aNew->setShortcut(QKeySequence::New);
    connect(aNew, &QAction::triggered, this, &MainWindow::onNew);

    auto* aOpen = fileMenu->addAction(QStringLiteral("&Open..."));
    aOpen->setShortcut(QKeySequence::Open);
    connect(aOpen, &QAction::triggered, this, &MainWindow::onOpen);

    auto* aOpenPcb = fileMenu->addAction(QStringLiteral("Open &PCB..."));
    connect(aOpenPcb, &QAction::triggered, this, &MainWindow::runPcbImportDialog);

    auto* aOpenKicad = fileMenu->addAction(QStringLiteral("Open &KiCad PCB..."));
    aOpenKicad->setVisible(false);
    connect(aOpenKicad, &QAction::triggered, this, [this]() {
        if (!maybeSave()) return;
        const QString p = QFileDialog::getOpenFileName(
            this, QStringLiteral("Open KiCad PCB"),
            QString(), QStringLiteral("KiCad PCB (*.kicad_pcb)"));
        if (p.isEmpty()) return;

        // KiCad imports for large 4-layer boards can take several seconds ??        // parsing, viewport mesh baking, ear-clip triangulation of zone
        // pours, and GPU upload. Show a determinate progress bar with
        // stages so the user knows what's happening and roughly how far
        // along we are.
        QProgressDialog busy(QStringLiteral("Loading KiCad PCB..."),
                             QString() /* no Cancel ??parse is non-cancellable */,
                             0, 100, this);
        busy.setWindowTitle(QStringLiteral("Importing PCB"));
        busy.setWindowModality(Qt::WindowModal);
        busy.setMinimumDuration(0);   // show immediately for any non-trivial work
        busy.setAutoClose(false);
        busy.setAutoReset(false);
        auto step = [&busy](int pct, const QString& label) {
            busy.setValue(pct);
            busy.setLabelText(label);
            QApplication::processEvents();
        };
        step(0, QStringLiteral("Reading %1...").arg(QFileInfo(p).fileName()));

        try {
            step(5, QStringLiteral("Parsing S-expressions (layers, nets, "
                                   "footprints, segments, zones)..."));
            ccc::core::Model m = ccc::io::readKicadPcb(p.toStdString());

            step(35, QStringLiteral(
                "Parsed: %1 pads 쨌 %2 traces 쨌 %3 zones 쨌 %4 layers")
                .arg(int(m.pads.size())).arg(int(m.traces.size()))
                .arg(int(m.zones.size())).arg(int(m.layers.size())));

            step(45, QStringLiteral("Swapping in new model..."));
            const auto before = std::move(model_);
            model_ = std::move(m);
            lastKnown_ = model_;
            soloId_.clear();

            step(55, QStringLiteral("Building viewport scene (this is the "
                                    "slow part for big boards)..."));
            viewport_->setModel(&model_);
            viewport_->clearSelection();

            step(65, QStringLiteral(
                "Baking per-layer trace + zone meshes (%1 segments, %2 zone "
                "islands)...")
                .arg(int(model_.traces.size())).arg(int(model_.zones.size())));
            viewport_->rebuildScene();        // does the merged-mesh bake

            step(85, QStringLiteral("Switching to 2D top view & fitting..."));
            viewport_->setViewMode(Viewport3D::ViewMode::Two2D);
            viewport_->presetTop();
            viewport_->fitView();

            step(95, QStringLiteral("Refreshing layer panel & status..."));
            rebuildLayerPanel();
            refreshStatus();
            setCurrentFile({});   // not associated with any save path
            setDirty(true);       // imported, not saved yet
            (void)before;

            step(100, QStringLiteral("Done."));
            busy.close();
        } catch (const std::exception& ex) {
            busy.close();
            QMessageBox::critical(this, QStringLiteral("KiCad PCB import failed"),
                                   QString::fromUtf8(ex.what()));
        }
    });

    auto* aOpenGerber = fileMenu->addAction(QStringLiteral("Open &Gerber Project..."));
    aOpenGerber->setVisible(false);
    connect(aOpenGerber, &QAction::triggered, this, [this]() {
        if (!maybeSave()) return;
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("Open Gerber Project"));
        if (dir.isEmpty()) return;

        QProgressDialog busy(QStringLiteral("Loading Gerber project..."),
                             QString(), 0, 100, this);
        busy.setWindowTitle(QStringLiteral("Importing Gerbers"));
        busy.setWindowModality(Qt::WindowModal);
        busy.setMinimumDuration(0);
        busy.setAutoClose(false);
        busy.setAutoReset(false);
        auto step = [&busy](int pct, const QString& label) {
            busy.setValue(pct);
            busy.setLabelText(label);
            QApplication::processEvents();
        };

        try {
            step(5, QStringLiteral("Parsing Gerber, drill, and gbrjob files..."));
            ccc::core::Model m = ccc::io::readGerberProject(dir.toStdString());

            step(45, QStringLiteral(
                "Parsed: %1 copper islands - %2 object candidates - %3 layers")
                .arg(int(m.zones.size()))
                .arg([&m] {
                    std::set<std::string> nets;
                    for (const auto& z : m.zones) nets.insert(z.net);
                    return int(nets.size());
                }())
                .arg(int(m.layers.size())));

            step(55, QStringLiteral("Swapping in Gerber object model..."));
            const auto before = std::move(model_);
            model_ = std::move(m);
            lastKnown_ = model_;
            soloId_.clear();

            step(65, QStringLiteral("Baking object viewport meshes..."));
            viewport_->setModel(&model_);
            viewport_->clearSelection();
            viewport_->rebuildScene();

            step(85, QStringLiteral("Switching to 2D top view & fitting..."));
            viewport_->setViewMode(Viewport3D::ViewMode::Two2D);
            viewport_->presetTop();
            viewport_->fitView();

            step(95, QStringLiteral("Refreshing layer panel & status..."));
            rebuildLayerPanel();
            refreshStatus();
            setCurrentFile({});
            setDirty(true);
            (void)before;

            step(100, QStringLiteral("Done."));
            busy.close();
        } catch (const std::exception& ex) {
            busy.close();
            QMessageBox::critical(this, QStringLiteral("Gerber import failed"),
                                  QString::fromUtf8(ex.what()));
        }
    });

    auto* aSave = fileMenu->addAction(QStringLiteral("&Save"));
    aSave->setShortcut(QKeySequence::Save);
    connect(aSave, &QAction::triggered, this, &MainWindow::onSave);

    auto* aSaveAs = fileMenu->addAction(QStringLiteral("Save &As..."));
    aSaveAs->setShortcut(QKeySequence::SaveAs);
    connect(aSaveAs, &QAction::triggered, this, &MainWindow::onSaveAs);

    fileMenu->addSeparator();
    auto* aNetCap = fileMenu->addAction(QStringLiteral("Compute Net &Capacitance..."));
    aNetCap->setShortcut(QKeySequence(QStringLiteral("Ctrl+M")));
    connect(aNetCap, &QAction::triggered, this, &MainWindow::runNetCapacitanceDialog);

    fileMenu->addSeparator();
    auto* aQuit = fileMenu->addAction(QStringLiteral("&Quit"));
    aQuit->setShortcut(QKeySequence::Quit);
    connect(aQuit, &QAction::triggered, this, &QMainWindow::close);

    auto* editMenu = menuBar()->addMenu(QStringLiteral("&Edit"));
    auto* aUndo = undo_->createUndoAction(this, QStringLiteral("&Undo"));
    aUndo->setShortcut(QKeySequence::Undo);
    auto* aRedo = undo_->createRedoAction(this, QStringLiteral("&Redo"));
    aRedo->setShortcuts({QKeySequence::Redo, QKeySequence(QStringLiteral("Ctrl+Y"))});
    editMenu->addAction(aUndo);
    editMenu->addAction(aRedo);

    auto* fastCapMenu = menuBar()->addMenu(QStringLiteral("External &Solver"));
    auto* aFastCapCompute = fastCapMenu->addAction(QStringLiteral("Compute Selected &Nets"));
    auto* aFastCapStatus = fastCapMenu->addAction(QStringLiteral("Check &Installation..."));
    auto* aFastCapFolder = fastCapMenu->addAction(QStringLiteral("Open Solver &Folder"));

    auto refreshFastCapMenu = [this, aFastCapCompute, aFastCapFolder]() {
        refreshFastCapAvailability();
        aFastCapCompute->setEnabled(fastCapAvailable_ || fasterCapAvailable_);
        aFastCapCompute->setToolTip(
            (fastCapAvailable_ || fasterCapAvailable_)
                ? QStringLiteral("Compute the currently selected net pair with the selected external solver.")
                : QStringLiteral("Install external/fastcap/fastcap.exe or external/fastercap/FasterCap.exe."));
        aFastCapFolder->setEnabled(true);
    };
    connect(fastCapMenu, &QMenu::aboutToShow, this, refreshFastCapMenu);
    refreshFastCapMenu();

    connect(aFastCapCompute, &QAction::triggered, this, [this]() {
        const auto [netA, netB] = viewport_->selectedCapNets();
        const bool hasPair = !netA.isEmpty() && !netB.isEmpty() && netA != netB;
        if (!hasPair) {
            QMessageBox::information(
                this,
                QStringLiteral("External Solver"),
                QStringLiteral("Select one net, then Shift-click another net first."));
            return;
        }
        startSelectedFastCapCompute();
    });
    connect(aFastCapStatus, &QAction::triggered, this, [this]() {
        const QString fastExe = QString::fromStdString(ccc::core::defaultFastCapExecutable());
        const QString fasterExe = QString::fromStdString(ccc::core::defaultFasterCapExecutable());
        refreshFastCapAvailability();
        QMessageBox::information(
            this,
            QStringLiteral("External Solver Installation"),
            QStringLiteral("FastCap fixed panels: %1\n%2\n\n"
                           "FasterCap adaptive: %3\n%4")
                .arg(fastCapAvailable_ ? QStringLiteral("available")
                                        : QStringLiteral("missing"))
                .arg(fastExe)
                .arg(fasterCapAvailable_ ? QStringLiteral("available")
                                          : QStringLiteral("missing"))
                .arg(fasterExe));
    });
    connect(aFastCapFolder, &QAction::triggered, this, []() {
        const QFileInfo info(QString::fromStdString(ccc::core::defaultFastCapExecutable()));
        QDesktopServices::openUrl(QUrl::fromLocalFile(info.absolutePath()));
    });
}

void MainWindow::runPcbImportDialog() {
    if (!maybeSave()) return;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Open PCB"));
    auto* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(12, 12, 12, 12);
    root->setSpacing(10);

    auto* note = new QLabel(QStringLiteral(
        "Use Gerber for copper geometry. Optionally choose the matching KiCad PCB "
        "file to provide stackup thickness and dielectric constants."), &dlg);
    note->setWordWrap(true);
    root->addWidget(note);

    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);

    auto* gerberRow = new QWidget(&dlg);
    auto* gerberLayout = new QHBoxLayout(gerberRow);
    gerberLayout->setContentsMargins(0, 0, 0, 0);
    gerberLayout->setSpacing(6);
    auto* gerberEdit = new QLineEdit(gerberRow);
    gerberEdit->setPlaceholderText(QStringLiteral("Gerber folder, e.g. jlcpcb/gerber"));
    auto* gerberBrowse = new QPushButton(QStringLiteral("Browse..."), gerberRow);
    gerberLayout->addWidget(gerberEdit, 1);
    gerberLayout->addWidget(gerberBrowse);
    form->addRow(QStringLiteral("Gerber folder"), gerberRow);

    auto* kicadRow = new QWidget(&dlg);
    auto* kicadLayout = new QHBoxLayout(kicadRow);
    kicadLayout->setContentsMargins(0, 0, 0, 0);
    kicadLayout->setSpacing(6);
    auto* kicadEdit = new QLineEdit(kicadRow);
    kicadEdit->setPlaceholderText(QStringLiteral("Matching .kicad_pcb file for stackup"));
    auto* kicadBrowse = new QPushButton(QStringLiteral("Browse..."), kicadRow);
    kicadLayout->addWidget(kicadEdit, 1);
    kicadLayout->addWidget(kicadBrowse);
    form->addRow(QStringLiteral("KiCad PCB"), kicadRow);

    root->addLayout(form);

    connect(gerberBrowse, &QPushButton::clicked, &dlg, [&]() {
        const QString dir = QFileDialog::getExistingDirectory(
            &dlg, QStringLiteral("Choose Gerber Folder"), gerberEdit->text());
        if (!dir.isEmpty()) gerberEdit->setText(dir);
    });
    connect(kicadBrowse, &QPushButton::clicked, &dlg, [&]() {
        const QString path = QFileDialog::getOpenFileName(
            &dlg, QStringLiteral("Choose KiCad PCB"), kicadEdit->text(),
            QStringLiteral("KiCad PCB (*.kicad_pcb)"));
        if (!path.isEmpty()) kicadEdit->setText(path);
    });

    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    auto* openButton = new QPushButton(QStringLiteral("Open"), &dlg);
    auto* cancelButton = new QPushButton(QStringLiteral("Cancel"), &dlg);
    buttons->addWidget(openButton);
    buttons->addWidget(cancelButton);
    root->addLayout(buttons);

    connect(cancelButton, &QPushButton::clicked, &dlg, &QDialog::reject);
    connect(openButton, &QPushButton::clicked, &dlg, [&]() {
        const QString gerberDir = gerberEdit->text().trimmed();
        const QString kicadPath = kicadEdit->text().trimmed();
        if (gerberDir.isEmpty() && kicadPath.isEmpty()) {
            QMessageBox::information(&dlg, QStringLiteral("Open PCB"),
                                     QStringLiteral("Choose a Gerber folder or a KiCad PCB file."));
            return;
        }
        dlg.accept();
    });

    dlg.resize(720, 170);
    if (dlg.exec() != QDialog::Accepted) return;

    openPcbProject(gerberEdit->text().trimmed(), kicadEdit->text().trimmed(), false);
}

bool MainWindow::openPcbProject(const QString& gerberDir,
                                const QString& kicadPcbPath,
                                bool quiet) {
    const QString gerber = gerberDir.trimmed();
    const QString kicad = kicadPcbPath.trimmed();
    if (gerber.isEmpty() && kicad.isEmpty()) return false;
    if (gerber.isEmpty()) return openKiCadPcb(kicad, quiet);

    std::unique_ptr<QProgressDialog> busy;
    if (!quiet) {
        busy = std::make_unique<QProgressDialog>(
            QStringLiteral("Loading PCB project..."),
            QString(), 0, 100, this);
        busy->setWindowTitle(QStringLiteral("Importing PCB"));
        busy->setWindowModality(Qt::WindowModal);
        busy->setMinimumDuration(0);
        busy->setAutoClose(false);
        busy->setAutoReset(false);
    }
    auto step = [&busy](int pct, const QString& label) {
        if (!busy) return;
        busy->setValue(pct);
        busy->setLabelText(label);
        QApplication::processEvents();
    };

    try {
        step(5, kicad.isEmpty()
                    ? QStringLiteral("Parsing Gerber, drill, and job files...")
                    : QStringLiteral("Parsing Gerber geometry and explicit KiCad stackup..."));
        ccc::core::Model m = ccc::io::readGerberProject(gerber.toStdString(),
                                                        kicad.toStdString());

        step(45, QStringLiteral("Installing imported PCB model..."));
        model_ = std::move(m);
        lastKnown_ = model_;
        soloId_.clear();

        step(65, QStringLiteral("Baking viewport meshes..."));
        viewport_->setModel(&model_);
        viewport_->clearSelection();
        viewport_->rebuildScene();

        step(85, QStringLiteral("Switching to top view..."));
        viewport_->setViewMode(Viewport3D::ViewMode::Two2D);
        viewport_->presetTop();
        viewport_->fitView();

        step(95, QStringLiteral("Refreshing UI..."));
        rebuildLayerPanel();
        refreshStatus();
        setCurrentFile({});
        setDirty(true);

        step(100, QStringLiteral("Done."));
        if (busy) busy->close();
        return true;
    } catch (const std::exception& ex) {
        if (busy) busy->close();
        if (!quiet) {
            QMessageBox::critical(this, QStringLiteral("PCB import failed"),
                                  QString::fromUtf8(ex.what()));
        }
        return false;
    }
}

bool MainWindow::openKiCadPcb(const QString& path, bool quiet) {
    if (path.isEmpty()) return false;

    std::unique_ptr<QProgressDialog> busy;
    if (!quiet) {
        busy = std::make_unique<QProgressDialog>(
            QStringLiteral("Loading KiCad PCB..."),
            QString(), 0, 100, this);
        busy->setWindowTitle(QStringLiteral("Importing PCB"));
        busy->setWindowModality(Qt::WindowModal);
        busy->setMinimumDuration(0);
        busy->setAutoClose(false);
        busy->setAutoReset(false);
    }
    auto step = [&busy](int pct, const QString& label) {
        if (!busy) return;
        busy->setValue(pct);
        busy->setLabelText(label);
        QApplication::processEvents();
    };

    try {
        step(5, QStringLiteral("Parsing KiCad PCB..."));
        ccc::core::Model m = ccc::io::readKicadPcb(path.toStdString());

        step(45, QStringLiteral("Installing imported model..."));
        model_ = std::move(m);
        lastKnown_ = model_;
        soloId_.clear();

        step(65, QStringLiteral("Baking viewport meshes..."));
        viewport_->setModel(&model_);
        viewport_->clearSelection();
        viewport_->rebuildScene();

        step(85, QStringLiteral("Switching to top view..."));
        viewport_->setViewMode(Viewport3D::ViewMode::Two2D);
        viewport_->presetTop();
        viewport_->fitView();

        step(95, QStringLiteral("Refreshing UI..."));
        rebuildLayerPanel();
        refreshStatus();
        setCurrentFile({});
        setDirty(true);

        step(100, QStringLiteral("Done."));
        if (busy) busy->close();
        return true;
    } catch (const std::exception& ex) {
        if (busy) busy->close();
        if (!quiet) {
            QMessageBox::critical(this, QStringLiteral("KiCad PCB import failed"),
                                  QString::fromUtf8(ex.what()));
        }
        return false;
    }
}

bool MainWindow::openGerberProject(const QString& dir, bool quiet) {
    if (dir.isEmpty()) return false;

    std::unique_ptr<QProgressDialog> busy;
    if (!quiet) {
        busy = std::make_unique<QProgressDialog>(
            QStringLiteral("Loading Gerber project..."),
            QString(), 0, 100, this);
        busy->setWindowTitle(QStringLiteral("Importing Gerbers"));
        busy->setWindowModality(Qt::WindowModal);
        busy->setMinimumDuration(0);
        busy->setAutoClose(false);
        busy->setAutoReset(false);
    }
    auto step = [&busy](int pct, const QString& label) {
        if (!busy) return;
        busy->setValue(pct);
        busy->setLabelText(label);
        QApplication::processEvents();
    };

    try {
        step(5, QStringLiteral("Parsing Gerber, drill, and job files..."));
        ccc::core::Model m = ccc::io::readGerberProject(dir.toStdString());

        step(45, QStringLiteral("Installing Gerber object model..."));
        model_ = std::move(m);
        lastKnown_ = model_;
        soloId_.clear();

        step(65, QStringLiteral("Baking object viewport meshes..."));
        viewport_->setModel(&model_);
        viewport_->clearSelection();
        viewport_->rebuildScene();

        step(85, QStringLiteral("Switching to top view..."));
        viewport_->setViewMode(Viewport3D::ViewMode::Two2D);
        viewport_->presetTop();
        viewport_->fitView();

        step(95, QStringLiteral("Refreshing UI..."));
        rebuildLayerPanel();
        refreshStatus();
        setCurrentFile({});
        setDirty(true);

        step(100, QStringLiteral("Done."));
        if (busy) busy->close();
        return true;
    } catch (const std::exception& ex) {
        if (busy) busy->close();
        if (!quiet) {
            QMessageBox::critical(this, QStringLiteral("Gerber import failed"),
                                  QString::fromUtf8(ex.what()));
        }
        return false;
    }
}

void MainWindow::showOnlyLayer(const QString& layerId) {
    applySolo(layerId);
    viewport_->presetTop();
    viewport_->fitView();
}

bool MainWindow::selectNet(const QString& netName) {
    return viewport_ && viewport_->selectNet(netName);
}

bool MainWindow::selectCapNets(const QString& netA, const QString& netB) {
    return viewport_ && viewport_->selectCapNets(netA, netB);
}

void MainWindow::setupModeToolbar() {
    auto* tb = addToolBar(QStringLiteral("Edit Mode"));
    tb->setObjectName(QStringLiteral("ModeToolbar"));
    tb->setMovable(false);

    modeGroup_ = new QActionGroup(this);
    modeGroup_->setExclusive(true);

    struct ModeDef { QString text; QString shortcut; Viewport3D::Mode mode; };
    const std::array<ModeDef, 6> modes = {{
        { QStringLiteral("&Select"),       QStringLiteral("S"), Viewport3D::Mode::Select     },
        { QStringLiteral("Add &Pad"),      QStringLiteral("A"), Viewport3D::Mode::AddPad     },
        { QStringLiteral("&Delete"),       QStringLiteral("D"), Viewport3D::Mode::DeletePad  },
        { QStringLiteral("Add &Trace"),    QStringLiteral("T"), Viewport3D::Mode::AddTrace   },
        { QStringLiteral("&Edit Trace"),   QStringLiteral("E"), Viewport3D::Mode::EditTrace  },
        { QStringLiteral("Move &FPC"),     QStringLiteral("F"), Viewport3D::Mode::MoveFpc    },
    }};
    for (const auto& m : modes) {
        auto* a = tb->addAction(m.text);
        a->setCheckable(true);
        a->setShortcut(QKeySequence(m.shortcut));
        a->setData(int(m.mode));
        modeGroup_->addAction(a);
        connect(a, &QAction::triggered, this, [this, mode = m.mode]{ selectMode(mode); });
    }
    updateCapSelectionPanel();
}

void MainWindow::setupDocks() {
    auto* dock = new QDockWidget(QStringLiteral("Layers"), this);
    dock->setObjectName(QStringLiteral("LayoutDock"));
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto* root = new QWidget(dock);
    auto* root_layout = new QVBoxLayout(root);
    root_layout->setContentsMargins(8, 8, 8, 8);
    root_layout->setSpacing(8);

    auto* layerBox = new QGroupBox(QStringLiteral("Copper Layers"), root);
    auto* layerBoxLayout = new QVBoxLayout(layerBox);
    layerBoxLayout->setContentsMargins(6, 6, 6, 6);
    layerBoxLayout->setSpacing(6);

    // Layer panel: rebuilt whenever the model changes. Wrapped in a scroll
    // area so the dock height stays bounded even with many layers (e.g. 8+
    // copper layers from a KiCad import).
    layerPanel_  = new QWidget;
    layerLayout_ = new QVBoxLayout(layerPanel_);
    layerLayout_->setContentsMargins(0, 0, 0, 0);
    layerLayout_->setSpacing(4);
    auto* layerScroll = new QScrollArea(layerBox);
    layerScroll->setWidgetResizable(true);
    layerScroll->setFrameShape(QFrame::NoFrame);
    // Keep the layer controls compact; the detailed stackup table lives in
    // Board Info so this panel can stay focused on visibility.
    layerScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    layerScroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layerScroll->setMinimumHeight(160);
    layerScroll->setMinimumWidth(270);
    layerScroll->setWidget(layerPanel_);
    layerBoxLayout->addWidget(layerScroll, /*stretch=*/1);

    capPanel_ = new QFrame(root);
    capPanel_->setFrameShape(QFrame::StyledPanel);
    auto* capLayout = new QVBoxLayout(capPanel_);
    capLayout->setContentsMargins(8, 6, 8, 8);
    capLayout->setSpacing(6);
    capLayout->addWidget(new QLabel(QStringLiteral("<b>Capacitance</b>"), capPanel_));

    auto* capForm = new QFormLayout;
    capForm->setContentsMargins(0, 0, 0, 0);
    capNetAValue_ = new QLabel(QStringLiteral("-"), capPanel_);
    capNetBValue_ = new QLabel(QStringLiteral("-"), capPanel_);
    capNetAValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    capNetBValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    capForm->addRow(QStringLiteral("Net A"), capNetAValue_);
    capForm->addRow(QStringLiteral("Net B"), capNetBValue_);

    capSolverCombo_ = new QComboBox(capPanel_);
    capSolverCombo_->addItem(QStringLiteral("FastCap (fixed panels)"),
                             int(ccc::core::ExternalCapSolver::FastCapFixed));
    capSolverCombo_->addItem(QStringLiteral("FasterCap (adaptive)"),
                             int(ccc::core::ExternalCapSolver::FasterCapAdaptive));
    capSolverCombo_->addItem(QStringLiteral("Fusion sweep + reference"),
                             kCapModeFusion);
    capSolverCombo_->setToolTip(QStringLiteral(
        "FastCap is usually faster and uses the generated panels directly. "
        "FasterCap adaptively refines, which can be slower. Fusion runs "
        "FastCap at 1.0/0.5/0.2 mm, then checks 0.5 mm against FasterCap."));
    capForm->addRow(QStringLiteral("Solver"), capSolverCombo_);

    capPanelSpin_ = new QDoubleSpinBox(capPanel_);
    capPanelSpin_->setRange(0.001, 5.0);
    capPanelSpin_->setDecimals(4);
    capPanelSpin_->setSingleStep(0.1);
    capPanelSpin_->setSuffix(QStringLiteral(" mm"));
    capPanelSpin_->setValue(0.5);
    capPanelSpin_->setToolTip(QStringLiteral("Panel size for pads and copper zones"));
    capForm->addRow(QStringLiteral("Zone panel"), capPanelSpin_);

    capTracePanelSpin_ = new QDoubleSpinBox(capPanel_);
    capTracePanelSpin_->setRange(0.0001, 5.0);
    capTracePanelSpin_->setDecimals(5);
    capTracePanelSpin_->setSingleStep(0.05);
    capTracePanelSpin_->setSuffix(QStringLiteral(" mm"));
    capTracePanelSpin_->setValue(0.2);
    capTracePanelSpin_->setToolTip(QStringLiteral("Panel size for traces"));
    capForm->addRow(QStringLiteral("Trace panel"), capTracePanelSpin_);

    capEpsSpin_ = new QDoubleSpinBox(capPanel_);
    capEpsSpin_->setRange(1.0, 100.0);
    capEpsSpin_->setDecimals(2);
    capEpsSpin_->setSingleStep(0.1);
    capEpsSpin_->setValue(4.5);
    capEpsSpin_->setToolTip(QStringLiteral(
        "Fallback eps_r when no explicit stackup dielectric is available"));
    capForm->addRow(QStringLiteral("Fallback eps_r"), capEpsSpin_);
    capLayout->addLayout(capForm);

    auto* capButtonRow = new QHBoxLayout;
    capComputeButton_ = new QPushButton(QStringLiteral("Compute Cap"), capPanel_);
    capCancelButton_ = new QPushButton(QStringLiteral("Cancel"), capPanel_);
    capCancelButton_->setEnabled(false);
    capButtonRow->addWidget(capComputeButton_);
    capButtonRow->addWidget(capCancelButton_);
    capButtonRow->addStretch(1);
    capLayout->addLayout(capButtonRow);

    capProgressBar_ = new QProgressBar(capPanel_);
    capProgressBar_->setRange(0, 0);
    capProgressBar_->setTextVisible(false);
    capProgressBar_->setVisible(false);
    capLayout->addWidget(capProgressBar_);

    capResult_ = new QLabel(capPanel_);
    capResult_->setTextFormat(Qt::RichText);
    capResult_->setWordWrap(true);
    capResult_->setMinimumHeight(32);
    capResult_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    capLayout->addWidget(capResult_);

    connect(capComputeButton_, &QPushButton::clicked,
            this, &MainWindow::startSelectedFastCapCompute);
    connect(capSolverCombo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this]() { updateCapSelectionPanel(); });
    connect(capCancelButton_, &QPushButton::clicked, this, [this]() {
        if (capStopFlag_) capStopFlag_->store(true);
        if (capCancelButton_) capCancelButton_->setEnabled(false);
        if (capResult_) {
            capResult_->setText(QStringLiteral(
                "<span style='color:#888'>Cancelling&hellip;</span>"));
        }
    });
    capPanel_->setVisible(false);
    capPanel_->setMaximumHeight(380);
    capPanel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    root_layout->addWidget(capPanel_);
    root_layout->addWidget(layerBox, /*stretch=*/1);

    root_layout->addSpacing(4);
    auto* snapBox = new QFrame(root);
    snapBox->setFrameShape(QFrame::StyledPanel);
    auto* snapL = new QVBoxLayout(snapBox);
    snapL->setContentsMargins(8, 6, 8, 6);
    snapL->addWidget(new QLabel(QStringLiteral("<b>Snap</b>")));
    snapToggle_ = new QCheckBox(QStringLiteral("Snap to grid"), snapBox);
    snapToggle_->setChecked(viewport_->snapEnabled());
    connect(snapToggle_, &QCheckBox::toggled, this, [this](bool on){ viewport_->setSnapEnabled(on); });
    snapL->addWidget(snapToggle_);
    auto* gridToggle = new QCheckBox(QStringLiteral("Show grid"), snapBox);
    gridToggle->setChecked(viewport_->gridVisible());
    connect(gridToggle, &QCheckBox::toggled, this, [this](bool on){ viewport_->setGridVisible(on); });
    snapL->addWidget(gridToggle);
    auto* form = new QFormLayout;
    snapSpin_ = new QDoubleSpinBox(snapBox);
    snapSpin_->setDecimals(2);
    snapSpin_->setRange(0.05, 20.0);
    snapSpin_->setSingleStep(0.1);
    snapSpin_->setValue(viewport_->snapSize());
    snapSpin_->setSuffix(QStringLiteral(" mm"));
    connect(snapSpin_, QOverload<double>::of(&QDoubleSpinBox::valueChanged),
            this, [this](double v){ viewport_->setSnapSize(v); });
    form->addRow(QStringLiteral("Grid step:"), snapSpin_);
    snapL->addLayout(form);
    root_layout->addWidget(snapBox);

    dock->setWidget(root);
    // Default dock width that fits the rows without clipping. Users can
    // still drag the splitter to make it narrower, but the start size now
    // matches what the rows need so the panel doesn't open mid-cut.
    dock->setMinimumWidth(380);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
    resizeDocks({dock}, {380}, Qt::Horizontal);
}

void MainWindow::setupStatusBar() {
    lblCounts_    = new QLabel(this);
    lblModeHint_  = new QLabel(this);
    lblSelection_ = new QLabel(QStringLiteral("(none)"), this);
    lblCursor_    = new QLabel(QStringLiteral("x: 0.0  y: 0.0  mm"), this);
    statusBar()->addWidget(lblCounts_, 0);
    statusBar()->addWidget(lblModeHint_, 1);
    statusBar()->addPermanentWidget(lblSelection_);
    statusBar()->addPermanentWidget(lblCursor_);
}

void MainWindow::rebuildLayerPanel() {
    if (!layerLayout_) return;
    // Clear existing rows
    while (auto* it = layerLayout_->takeAt(0)) {
        if (auto* w = it->widget()) w->deleteLater();
        delete it;
    }

    auto* buttonRow = new QWidget(layerPanel_);
    auto* buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(0, 0, 0, 2);
    buttonLayout->setSpacing(6);
    auto* allBtn = new QPushButton(QStringLiteral("All"), buttonRow);
    auto* topBtn = new QPushButton(QStringLiteral("Top"), buttonRow);
    auto* bottomBtn = new QPushButton(QStringLiteral("Bottom"), buttonRow);
    allBtn->setToolTip(QStringLiteral("Show all copper layers"));
    topBtn->setToolTip(QStringLiteral("Show only F.Cu"));
    bottomBtn->setToolTip(QStringLiteral("Show only B.Cu"));
    buttonLayout->addWidget(allBtn);
    buttonLayout->addWidget(topBtn);
    buttonLayout->addWidget(bottomBtn);
    layerLayout_->addWidget(buttonRow);
    connect(allBtn, &QPushButton::clicked, this, [this]() { applySolo(QString()); });
    connect(topBtn, &QPushButton::clicked, this, [this]() { applySolo(QStringLiteral("F.Cu")); });
    connect(bottomBtn, &QPushButton::clicked, this, [this]() { applySolo(QStringLiteral("B.Cu")); });

    auto addRow = [&](const QString& id, const QString& name,
                      double thickness, double permittivity, bool isCond, bool visible) {
        auto* row = new QFrame(layerPanel_);
        row->setFrameShape(QFrame::StyledPanel);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(6, 4, 6, 4);
        h->setSpacing(6);

        auto* vis = new QCheckBox(row);
        vis->setChecked(visible);
        vis->setToolTip(QStringLiteral("Show/hide this layer"));
        connect(vis, &QCheckBox::toggled, this, [this, id](bool on) {
            if (id == QLatin1String("glass")) model_.glass.visible = on;
            else if (auto* l = model_.findLayer(id.toStdString())) l->visible = on;
            viewport_->setLayerVisible(id, on);
            // Visibility itself isn't undoable (matches typical 3D apps).
            soloId_.clear();
        });
        h->addWidget(vis);

        auto* lbl = new QLabel(name, row);
        lbl->setMinimumWidth(68);
        h->addWidget(lbl);

        const QString thicknessText =
            thickness < 1.0
                ? QStringLiteral("%1 um").arg(thickness * 1000.0, 0, 'f', 0)
                : QStringLiteral("%1 mm").arg(thickness, 0, 'f', 3);
        auto* thicknessLabel = new QLabel(thicknessText, row);
        thicknessLabel->setMinimumWidth(54);
        thicknessLabel->setToolTip(QStringLiteral(
            "Copper thickness. Open Board Info for the full stackup."));
        thicknessLabel->setStyleSheet(QStringLiteral("color:#667085;"));
        h->addWidget(thicknessLabel);

        if (isCond) {
            auto* tag = new QLabel(QStringLiteral("<i>metal</i>"), row);
            tag->setToolTip(QStringLiteral("Conductive layer; eps_r unused"));
            tag->setMinimumWidth(38);
            h->addWidget(tag);
        } else {
            auto* ebox = new QDoubleSpinBox(row);
            ebox->setDecimals(2);
            ebox->setRange(1.0, 100.0);
            ebox->setSingleStep(0.1);
            ebox->setValue(permittivity);
            ebox->setPrefix(QStringLiteral("eps_r "));
            ebox->setToolTip(QStringLiteral("Relative dielectric constant"));
            ebox->setMaximumWidth(90);
            connect(ebox, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, id](double v) {
                if (id == QLatin1String("glass")) model_.glass.permittivity = v;
                else if (auto* l = model_.findLayer(id.toStdString())) l->permittivity = v;
                onModelEdited();
            });
            h->addWidget(ebox);
        }

        auto* solo = new QPushButton(QStringLiteral("Solo"), row);
        solo->setCheckable(true);
        solo->setChecked(soloId_ == id);
        solo->setMaximumWidth(44);
        solo->setToolTip(QStringLiteral("Show only this layer"));
        connect(solo, &QPushButton::toggled, this, [this, id](bool on) {
            applySolo(on ? id : QString());
        });
        h->addWidget(solo);

        layerLayout_->addWidget(row);
    };

    int conductorCount = 0;
    int dielectricCount = 0;
    for (const auto& l : model_.layers) {
        if (l.isConductor) {
            ++conductorCount;
            addRow(QString::fromStdString(l.id), QString::fromStdString(l.name),
                   l.thickness, l.permittivity, l.isConductor, l.visible);
        } else {
            ++dielectricCount;
        }
    }

    auto* note = new QLabel(layerPanel_);
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#667085; padding:4px 2px;"));
    note->setText(QStringLiteral(
        "%1 copper layers. %2 dielectric layers are in Board Info.")
        .arg(conductorCount)
        .arg(dielectricCount));
    layerLayout_->addWidget(note);
    layerLayout_->addStretch(1);
}

void MainWindow::applySolo(const QString& soloId) {
    soloId_ = soloId;
    auto setVis = [&](const QString& id, bool& v, bool target) {
        v = target;
        viewport_->setLayerVisible(id, target);
    };
    if (soloId.isEmpty()) {
        const bool hasGlass = model_.glass.thickness > 0.0
                              && model_.glass.width > 0.0
                              && model_.glass.height > 0.0;
        setVis(QStringLiteral("glass"), model_.glass.visible, hasGlass);
        for (auto& l : model_.layers) {
            bool v = l.isConductor;
            setVis(QString::fromStdString(l.id), v, v);
            l.visible = v;
        }
    } else {
        bool gv = (soloId == QLatin1String("glass"));
        setVis(QStringLiteral("glass"), model_.glass.visible, gv);
        for (auto& l : model_.layers) {
            const QString lid = QString::fromStdString(l.id);
            bool v = (lid == soloId);
            setVis(lid, v, v);
            l.visible = v;
        }
    }
    rebuildLayerPanel();
}

void MainWindow::refreshStatus() {
    lblCounts_->setText(QStringLiteral(
            "Pads: %1   Traces: %2   FPC: %3   Zones: %4")
                        .arg(model_.pads.size())
                        .arg(model_.traces.size())
                        .arg(model_.fpcPads.size())
                        .arg(model_.zones.size()));
}

void MainWindow::setDirty(bool d) {
    dirty_ = d;
    QString title = QStringLiteral("Circuit Capacity Calculator");
    if (!currentPath_.isEmpty()) title += QStringLiteral(" - %1").arg(QFileInfo(currentPath_).fileName());
    if (d) title += QStringLiteral(" *");
    setWindowTitle(title);
}

void MainWindow::setCurrentFile(const QString& path) {
    currentPath_ = path;
    setDirty(false);
}

void MainWindow::selectMode(Viewport3D::Mode m) {
    viewport_->setMode(m);
    viewport_->setFocus();
    if (modeGroup_) {
        for (auto* a : modeGroup_->actions()) {
            if (a->data().toInt() == int(m)) { a->setChecked(true); break; }
        }
    }
    onModeChanged(int(m));
}

void MainWindow::showBoardInfoDialog() {
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Board Info"));
    auto* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    int copperCount = 0;
    int dielectricCount = 0;
    double stackThickness = 0.0;
    for (const auto& l : model_.layers) {
        stackThickness += l.thickness;
        if (l.isConductor) ++copperCount;
        else ++dielectricCount;
    }

    auto* summary = new QLabel(&dlg);
    summary->setTextFormat(Qt::RichText);
    summary->setWordWrap(true);
    summary->setStyleSheet(QStringLiteral(
        "QLabel { background:#f6f8fa; border:1px solid #d8dee4;"
        " border-radius:6px; padding:8px 10px; }"));
    summary->setText(QStringLiteral(
        "<b>%1</b><br>"
        "Board size: %2 mm x %3 mm<br>"
        "Stack thickness: %4 mm - Copper layers: %5 - Dielectric layers: %6<br>"
        "<span style='color:#667085'>Gerber imports use Gerber copper geometry; "
        "when a KiCad PCB file is selected in Open PCB, its physical stackup is used for capacitance extraction.</span>")
        .arg(QString::fromStdString(model_.meta.type.empty() ? "Model" : model_.meta.type))
        .arg(model_.glass.width, 0, 'f', 2)
        .arg(model_.glass.height, 0, 'f', 2)
        .arg(stackThickness, 0, 'f', 3)
        .arg(copperCount)
        .arg(dielectricCount));
    root->addWidget(summary);

    auto* table = new QTableWidget(&dlg);
    table->setColumnCount(5);
    table->setHorizontalHeaderLabels({
        QStringLiteral("Layer"),
        QStringLiteral("Role"),
        QStringLiteral("Thickness"),
        QStringLiteral("eps_r"),
        QStringLiteral("Visible")
    });
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setAlternatingRowColors(true);
    table->verticalHeader()->setVisible(false);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);

    auto addItem = [table](int row, int col, const QString& text) {
        auto* item = new QTableWidgetItem(text);
        if (col != 0) item->setTextAlignment(Qt::AlignCenter);
        table->setItem(row, col, item);
    };
    auto appendLayer = [&](const QString& name, bool conductor, double thickness,
                           double eps, bool visible) {
        const int row = table->rowCount();
        table->insertRow(row);
        addItem(row, 0, name);
        addItem(row, 1, conductor ? QStringLiteral("Copper") : QStringLiteral("Dielectric"));
        addItem(row, 2, QStringLiteral("%1 mm").arg(thickness, 0, 'f', 3));
        addItem(row, 3, conductor ? QStringLiteral("-") : QString::number(eps, 'f', 2));
        addItem(row, 4, visible ? QStringLiteral("Yes") : QStringLiteral("No"));
    };

    const bool hasGlass = model_.glass.thickness > 0.0
                          && model_.glass.width > 0.0
                          && model_.glass.height > 0.0;
    if (hasGlass) {
        appendLayer(QStringLiteral("Glass"), false,
                    model_.glass.thickness, model_.glass.permittivity,
                    model_.glass.visible);
    }
    for (const auto& l : model_.layers) {
        appendLayer(QString::fromStdString(l.name.empty() ? l.id : l.name),
                    l.isConductor, l.thickness, l.permittivity, l.visible);
    }
    table->resizeRowsToContents();
    root->addWidget(table, 1);

    auto* row = new QHBoxLayout;
    row->addStretch(1);
    auto* close = new QPushButton(QStringLiteral("Close"), &dlg);
    row->addWidget(close);
    root->addLayout(row);
    connect(close, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.resize(680, 460);
    dlg.exec();
}

void MainWindow::refreshFastCapAvailability() {
    fastCapAvailable_ = ccc::core::fastCapAvailable();
    fasterCapAvailable_ = ccc::core::fasterCapAvailable();
    updateCapSelectionPanel();
}

void MainWindow::updateCapSelectionPanel() {
    if (!capPanel_ || !viewport_) return;
    const auto [netA, netB] = viewport_->selectedCapNets();
    const bool ready = !netA.isEmpty() && !netB.isEmpty() && netA != netB;
    if (ready && !capComputeRunning_) {
        capNetA_ = netA;
        capNetB_ = netB;
    } else if (!capComputeRunning_) {
        capNetA_.clear();
        capNetB_.clear();
    }

    capPanel_->setVisible(ready || capComputeRunning_);
    if (capNetAValue_) capNetAValue_->setText(capNetA_.isEmpty() ? QStringLiteral("-") : capNetA_);
    if (capNetBValue_) capNetBValue_->setText(capNetB_.isEmpty() ? QStringLiteral("-") : capNetB_);

    const int capMode = capSolverCombo_
        ? capSolverCombo_->currentData().toInt()
        : int(ccc::core::ExternalCapSolver::FastCapFixed);
    const bool solverAvailable = capModeAvailable(capMode, fastCapAvailable_, fasterCapAvailable_);
    if (capComputeButton_) {
        capComputeButton_->setText(capModeIsFusion(capMode)
            ? QStringLiteral("Run Fusion")
            : QStringLiteral("Compute Cap"));
    }
    if (capComputeButton_) {
        capComputeButton_->setEnabled(ready && solverAvailable && !capComputeRunning_);
    }
    if (capResult_ && ready && !capComputeRunning_) {
        capResult_->setText(
            solverAvailable
                ? QStringLiteral("<span style='color:#555'>%1 ready.</span>")
                    .arg(labelForCapMode(capMode))
                : QStringLiteral("<span style='color:#c8261a'>%1 executable is not available.</span>")
                    .arg(labelForCapMode(capMode)));
    }
}

void MainWindow::onModeChanged(int mode) {
    static const std::array<QString, 7> hints = {
        QStringLiteral("Select: click a net, then Shift-click another net to open the capacitance panel."),
        QStringLiteral("Add Pad: click empty space on the sensor plane to drop a 12 mm pad."),
        QStringLiteral("Delete: click any pad / trace / FPC to remove."),
        QStringLiteral("Add Trace: click start pad, click empty for waypoints, click target pad/FPC. Esc cancels."),
        QStringLiteral("Edit Trace: click trace to select, click line to insert a waypoint, drag handles."),
        QStringLiteral("Move FPC: drag an FPC pad to reposition."),
        QStringLiteral("Cap panel: select two different nets first."),
    };
    if (mode >= 0 && size_t(mode) < hints.size()) lblModeHint_->setText(hints[mode]);
}

void MainWindow::onNew() {
    if (!maybeSave()) return;
    model_     = ccc::core::Model::makeDefault();
    lastKnown_ = model_;
    undo_->clear();
    soloId_.clear();
    viewport_->setModel(&model_);
    rebuildLayerPanel();
    refreshStatus();
    setCurrentFile({});
}

void MainWindow::onOpen() {
    if (!maybeSave()) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open"), {},
        QStringLiteral("Sensor models (*.glb *.ccc *.json);;GLB (*.glb);;Circuit-Capacity (*.ccc *.json);;All files (*)"));
    if (path.isEmpty()) return;
    loadFrom(path);
}

void MainWindow::onSave() {
    if (currentPath_.isEmpty()) { onSaveAs(); return; }
    saveTo(currentPath_);
}

void MainWindow::onSaveAs() {
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Save As"), {},
        QStringLiteral("GLB (*.glb);;Circuit-Capacity (*.ccc);;JSON (*.json)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QStringLiteral(".glb"), Qt::CaseInsensitive) &&
        !path.endsWith(QStringLiteral(".ccc"), Qt::CaseInsensitive) &&
        !path.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive)) {
        path += QStringLiteral(".glb");
    }
    if (saveTo(path)) setCurrentFile(path);
}

bool MainWindow::saveTo(const QString& path) {
    try {
        writeAny(model_, path);
        setDirty(false);
        statusBar()->showMessage(QStringLiteral("Saved %1").arg(path), 3000);
        return true;
    } catch (const std::exception& e) {
        QMessageBox::critical(this, QStringLiteral("Save failed"), QString::fromUtf8(e.what()));
        return false;
    }
}

void MainWindow::loadFrom(const QString& path) {
    try {
        model_     = readAny(path);
        lastKnown_ = model_;
        undo_->clear();
        soloId_.clear();
        viewport_->setModel(&model_);
        rebuildLayerPanel();
        refreshStatus();
        setCurrentFile(path);
        statusBar()->showMessage(QStringLiteral("Loaded %1").arg(path), 3000);
    } catch (const std::exception& e) {
        QMessageBox::critical(this, QStringLiteral("Load failed"), QString::fromUtf8(e.what()));
    }
}

bool MainWindow::maybeSave() {
    if (capComputeRunning_) {
        QMessageBox::information(
            this,
            QStringLiteral("FasterCap"),
            QStringLiteral("Wait for the current FasterCap calculation to finish or cancel it first."));
        return false;
    }
    if (!dirty_) return true;
    const auto ans = QMessageBox::warning(this, QStringLiteral("Unsaved changes"),
        QStringLiteral("Save changes before continuing?"),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);
    if (ans == QMessageBox::Cancel)  return false;
    if (ans == QMessageBox::Discard) return true;
    onSave();
    return !dirty_;
}

void MainWindow::onSelectionChanged(SceneRef ref) {
    updateCapSelectionPanel();
    if (!ref.isValid()) {
        lblSelection_->setText(QStringLiteral("(none)"));
        return;
    }
    // Look up the element's net + layer for display.
    QString netStr, layerStr;
    if (ref.kind == QLatin1String("pad")) {
        for (const auto& p : model_.pads)
            if (QString::fromStdString(p.id) == ref.id) {
                netStr = QString::fromStdString(p.net);
                layerStr = QString::fromStdString(p.layer);
                break;
            }
    } else if (ref.kind == QLatin1String("trace") || ref.kind == QLatin1String("waypoint")) {
        for (const auto& t : model_.traces)
            if (QString::fromStdString(t.id) == ref.id) {
                netStr = QString::fromStdString(t.net);
                layerStr = QString::fromStdString(t.layer);
                break;
            }
    } else if (ref.kind == QLatin1String("zone")) {
        for (const auto& z : model_.zones)
            if (QString::fromStdString(z.id) == ref.id) {
                netStr = QString::fromStdString(z.net);
                layerStr = QString::fromStdString(z.layerId);
                break;
            }
    }
    QString suffix;
    if (!netStr.isEmpty())   suffix += QStringLiteral("  net=%1").arg(netStr);
    if (!layerStr.isEmpty()) suffix += QStringLiteral("  layer=%1").arg(layerStr);
    lblSelection_->setText(QStringLiteral("Selected: %1 %2%3%4")
                           .arg(ref.kind, ref.id)
                           .arg(ref.sub >= 0 ? QStringLiteral(":%1").arg(ref.sub) : QString())
                           .arg(suffix));
}

void MainWindow::onModelEdited() {
    if (lastKnown_ != model_) {
        ccc::core::Model before = lastKnown_;
        ccc::core::Model after  = model_;
        lastKnown_ = after;
        undo_->push(new ModelSnapshotCmd(this, std::move(before), std::move(after), QStringLiteral("Edit")));
    }
    setDirty(true);
    refreshStatus();
}

void MainWindow::onCursorMoved(double x, double y) {
    lblCursor_->setText(QStringLiteral("x: %1  y: %2  mm").arg(x, 0, 'f', 2).arg(y, 0, 'f', 2));
}

void MainWindow::restoreModelState(const ccc::core::Model& m) {
    model_     = m;
    lastKnown_ = m;
    soloId_.clear();
    viewport_->clearSelection();
    viewport_->rebuildScene();
    rebuildLayerPanel();
    refreshStatus();
    setDirty(true);
}

// ----------------------------------------------------------------------------
//  BEM dialog -- live panel-count + estimated time + background solve
// ----------------------------------------------------------------------------
namespace {

// Empirical performance model. Calibrated against an actual measurement at
// N=14772 (assemble 261.7 ms, solve 13348.4 ms) on a multi-core desktop.
//   K_a = 1.5e-6 ms / N^2  -> N=14772 predicts ~327 ms (actual 262 ms)
//   K_s = 5.0e-9 ms / N^3  -> N=14772 predicts ~16.1 s (actual 13.3 s)
// Total = K_a * N^2 (assemble, OpenMP) + K_s * N^3 (LU solve, partial pivoting).
constexpr double kAssembleCoef = 1.5e-6;   // ms per N^2
constexpr double kSolveCoef    = 5.0e-9;   // ms per N^3

double estimatedMs(int N, bool useGpu = false, bool iterative = false,
                   int iterEstimate = 100) {
    const double n = double(N);
    if (iterative) {
        // BiCGStab: each iter is one matvec (~O(N^2) ops). 2 RHS = 2 solves.
        // Per matvec we use the same coefficient as direct assemble (one row
        // sweep over Green's function). 2 matvecs per BiCGStab iter (for p
        // and s in the algorithm) -> 4 matvecs per RHS approximately.
        constexpr double kMatvecCoef = 1.5e-6;  // ms / N^2
        const double oneMatvec = kMatvecCoef * n * n;
        return 2.0 * iterEstimate * 2.0 * oneMatvec;   // 2 RHS x ~2 matvec/iter
    }
    constexpr double kGpuSpeedup = 8.0;
    const double assembleMs = kAssembleCoef * n * n;
    const double solveMs    = kSolveCoef    * n * n * n;
    return assembleMs + (useGpu ? solveMs / kGpuSpeedup : solveMs);
}

QString formatDuration(double ms) {
    if (ms < 1.0)    return QStringLiteral("< 1 ms");
    if (ms < 1000.0) return QStringLiteral("~%1 ms").arg(int(std::round(ms)));
    if (ms < 60000.0) {
        const double s = ms / 1000.0;
        return QStringLiteral("~%1 s").arg(s, 0, 'f', s < 10 ? 1 : 0);
    }
    const int sec  = int(std::round(ms / 1000.0));
    const int mins = sec / 60;
    const int rem  = sec % 60;
    return QStringLiteral("~%1m %2s").arg(mins).arg(rem);
}

// Color code: green <0.5 s, yellow <5 s, orange <30 s, red beyond.
QString severityColor(double ms) {
    if (ms < 500.0)   return QStringLiteral("#1a8a1a");
    if (ms < 5000.0)  return QStringLiteral("#b88300");
    if (ms < 30000.0) return QStringLiteral("#cc6a00");
    return QStringLiteral("#c8261a");
}

struct BemJob {
    int                       NA = 0;
    int                       NB = 0;
    ccc::core::BemResult      result;
    QString                   netA;
    QString                   netB;
    QString                   solver;
    QString                   resultHtml;
    QString                   error;        // empty == success
    bool                      ok = false;
};

struct CapProgressState {
    std::mutex mutex;
    QString phase;
    QString detail;
    int step = 0;
    int totalSteps = 0;
    int panelsA = 0;
    int panelsB = 0;
    int panelsEnvironment = 0;
    int dielectricPanels = 0;
    std::size_t outputBytes = 0;
    double solverElapsedSeconds = 0.0;
};

}  // namespace

// =============================================================================
//  DEAD CODE ??legacy element-pick capacitance dialog.
//
//  Kept compiled for now; NOT reachable from any menu, shortcut, or viewport
//  signal as of this revision. The current net workflow uses the left
//  FasterCap panel instead. Reasons:
//    * The live-preview lambda calls panelizeConductor() on the UI thread
//      every 80 ms, which freezes the GUI for seconds on KiCad-scale nets.
//    * Element picking is awkward on a 1894-segment PCB ??net-based
//      selection is the right primitive for the KiCad workflow.
//
//  Do NOT re-hook this without first moving the live preview onto a
//  worker thread. If you decide to delete it: prefer truncate-and-rewrite
//  via heredoc rather than surgical Edit, per CLAUDE.md "File-truncation
//  gotcha" ??this function is in the danger zone of MainWindow.cpp.
// =============================================================================
void MainWindow::runCapacitanceDialog() {
    const auto a = viewport_->capPickA();
    const auto b = viewport_->capPickB();
    if (a.empty() || b.empty()) return;

    auto toCore = [](const Viewport3D::CapPick& p) {
        ccc::core::ConductorRefs c;
        for (const auto& s : p.padIds)   c.padIds.push_back  (s.toStdString());
        for (const auto& s : p.fpcIds)   c.fpcIds.push_back  (s.toStdString());
        for (const auto& s : p.traceIds) c.traceIds.push_back(s.toStdString());
        for (const auto& s : p.zoneIds)  c.zoneIds.push_back (s.toStdString());
        return c;
    };
    const auto refsA = toCore(a);
    const auto refsB = toCore(b);

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("BEM Capacitance"));
    dlg.setModal(true);

    auto* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    // ---- Header: conductor summary ------------------------------------------
    auto* header = new QLabel(&dlg);
    header->setTextFormat(Qt::RichText);
    header->setStyleSheet(QStringLiteral(
        "QLabel { background:#f4f6f8; border:1px solid #d8dde2;"
        " border-radius:6px; padding:8px 10px; }"));
    auto fmtConductor = [](const QString& name, const Viewport3D::CapPick& p) {
        return QStringLiteral(
            "<b>%1</b> &nbsp; <span style='color:#555'>"
            "%2&nbsp;pad &middot; %3&nbsp;FPC &middot; %4&nbsp;trace</span>")
            .arg(name)
            .arg(p.padIds.size()).arg(p.fpcIds.size()).arg(p.traceIds.size());
    };
    header->setText(fmtConductor(QStringLiteral("Conductor A"), a) +
                    QStringLiteral("<br>") +
                    fmtConductor(QStringLiteral("Conductor B"), b));
    root->addWidget(header);

    // ---- Settings group -----------------------------------------------------
    auto* settingsBox = new QGroupBox(QStringLiteral("Solver settings"), &dlg);
    auto* form = new QFormLayout(settingsBox);
    form->setLabelAlignment(Qt::AlignRight);

    auto makePanelSpin = [&](double dflt, const QString& tip) {
        auto* s = new QDoubleSpinBox(settingsBox);
        s->setRange(0.0001, 5.0);
        s->setDecimals(5);
        s->setStepType(QAbstractSpinBox::AdaptiveDecimalStepType);
        s->setValue(dflt);
        s->setSuffix(QStringLiteral(" mm"));
        s->setToolTip(tip);
        return s;
    };
    // Multi-scale geometries (large pads + tiny traces) need different panel
    // sizes per element type. Pads & FPCs have smooth charge distribution and
    // converge with relatively few panels; traces need fine resolution to
    // resolve proximity effects.
    auto* panelSpinPad = makePanelSpin(0.10, QStringLiteral(
        "Panel side length used for pads. Pads are large flat conductors\n"
        "with smooth charge distribution: ~30-100 panels/side is plenty.\n"
        "0.1 mm on a 12 mm pad gives ~120 panels/side, ~11 000 in disc."));
    auto* panelSpinFpc = makePanelSpin(0.10, QStringLiteral(
        "Panel side length used for FPC pads. Same physics as pads."));
    auto* panelSpinTrace = makePanelSpin(0.005, QStringLiteral(
        "Panel side length used for traces. Traces are narrow so they need\n"
        "fine perpendicular resolution to capture proximity effects.\n"
        "5 um (0.005 mm) on a 100 um trace gives 20 panels across width."));
    // Preset buttons: pre-fill all fields for one of three accuracy budgets.
    auto* presetRow = new QHBoxLayout;
    auto* btnQuick = new QPushButton(QStringLiteral("Quick"),       settingsBox);
    auto* btnConv  = new QPushButton(QStringLiteral("Convergence"), settingsBox);
    auto* btnPrec  = new QPushButton(QStringLiteral("Precise"),     settingsBox);
    btnQuick->setToolTip(QStringLiteral(
        "Fast scan, geometry comparison.\n"
        "Pad/FPC 0.5 mm, Trace 1 um. Direct LU. ~1 second."));
    btnConv->setToolTip(QStringLiteral(
        "Convergence-grade accuracy (~1%).\n"
        "Pad/FPC 0.2 mm, Trace 0.5 um. Direct LU. ~30 seconds."));
    btnPrec->setToolTip(QStringLiteral(
        "Reference-grade precision (~0.1%).\n"
        "Pad/FPC 0.05 mm, Trace 0.1 um. BiCGStab, tight tolerance.\n"
        "Budget around 1 hour for typical 1cm-pad + um-trace geometry."));
    presetRow->addWidget(btnQuick);
    presetRow->addWidget(btnConv);
    presetRow->addWidget(btnPrec);
    presetRow->addStretch(1);
    form->addRow(QStringLiteral("Preset"), presetRow);

    form->addRow(QStringLiteral("Pad panel"),   panelSpinPad);
    form->addRow(QStringLiteral("FPC panel"),   panelSpinFpc);
    form->addRow(QStringLiteral("Trace panel"), panelSpinTrace);

    auto* epsSpin = new QDoubleSpinBox(settingsBox);
    epsSpin->setRange(1.0, 100.0);
    epsSpin->setDecimals(2);
    epsSpin->setSingleStep(0.1);
    epsSpin->setValue(4.0);
    epsSpin->setToolTip(QStringLiteral(
        "Effective relative permittivity of the dielectric stack between\n"
        "the two conductors. Use 1.0 for free space, 4.0 for FR-4-ish."));
    form->addRow(QStringLiteral("Effective eps_r"), epsSpin);

    auto* imgChk = new QCheckBox(
        QStringLiteral("Include grounded-shield image"), settingsBox);
    imgChk->setChecked(true);
    imgChk->setToolTip(QStringLiteral(
        "Mirror each panel about z = -glass.thickness with opposite sign.\n"
        "Use when there is a ground plane below the glass."));
    form->addRow(QString(), imgChk);

    // GPU acceleration. Only enabled if the binary was built with
    // -DCCC_ENABLE_CUDA=ON AND a CUDA device is visible.
    auto* gpuChk = new QCheckBox(
        QStringLiteral("Use GPU (cuSOLVER) for LU solve"), settingsBox);
    const bool gpuOn = ccc::core::gpuAvailable();
    gpuChk->setEnabled(gpuOn);
    gpuChk->setChecked(gpuOn);
    if (gpuOn) {
        gpuChk->setToolTip(QStringLiteral(
            "Run the dense LU on the GPU via cuSOLVER. Direct LU only.\n"
            "Matrix assemble stays on CPU."));
    } else {
        gpuChk->setToolTip(QStringLiteral(
            "Disabled: this build has no CUDA support, or no CUDA device is "
            "visible.\nTo enable, install the CUDA Toolkit and rebuild with "
            "-DCCC_ENABLE_CUDA=ON."));
    }
    form->addRow(QString(), gpuChk);

    // Solver type: dense direct LU vs matrix-free BiCGStab. Iterative needs
    // O(N) memory instead of O(N^2), so it's the only sane choice for large N.
    auto* solverCombo = new QComboBox(settingsBox);
    solverCombo->addItem(QStringLiteral("Direct LU  (fast, O(N\xC2\xB2) memory)"));
    solverCombo->addItem(QStringLiteral("Iterative BiCGStab  (matrix-free, O(N) memory)"));
    solverCombo->setCurrentIndex(0);
    solverCombo->setToolTip(QStringLiteral(
        "Direct LU: fastest for small/medium N, but stores the full N x N "
        "influence matrix (e.g. ~126 GB at N=130000).\n"
        "BiCGStab: never builds the matrix; computes A*x on the fly each "
        "iteration. O(N) memory, ~50-200 iters to converge, slower per iter."));
    form->addRow(QStringLiteral("Solver"), solverCombo);

    auto* tolSpin = new QDoubleSpinBox(settingsBox);
    tolSpin->setDecimals(8);
    tolSpin->setRange(1e-10, 1e-2);
    tolSpin->setValue(1e-6);
    tolSpin->setSingleStep(1e-7);
    tolSpin->setToolTip(QStringLiteral(
        "BiCGStab convergence tolerance (relative residual ||r||/||b||).\n"
        "1e-6 is plenty for capacitance values; loosen for speed."));
    form->addRow(QStringLiteral("Iter tol"), tolSpin);

    auto* maxIterSpin = new QDoubleSpinBox(settingsBox);
    maxIterSpin->setDecimals(0);
    maxIterSpin->setRange(10, 100000);
    maxIterSpin->setValue(500);
    maxIterSpin->setToolTip(QStringLiteral(
        "BiCGStab maximum iterations per RHS. If the solver returns at this "
        "limit without converging, the residual is reported in the result."));
    form->addRow(QStringLiteral("Iter max"), maxIterSpin);

    // Disable iter-only fields when DirectLU is selected.
    auto syncIterFields = [solverCombo, tolSpin, maxIterSpin, gpuChk]() {
        const bool iter = (solverCombo->currentIndex() == 1);
        tolSpin->setEnabled(iter);
        maxIterSpin->setEnabled(iter);
        // GPU only meaningful for DirectLU.
        if (iter) gpuChk->setChecked(false);
        gpuChk->setEnabled(!iter && ccc::core::gpuAvailable());
    };
    syncIterFields();
    QObject::connect(solverCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        &dlg, [syncIterFields](int){ syncIterFields(); });

    // Wire the preset buttons. Each one writes the field values; valueChanged
    // signals on the spinboxes / combos update the live preview automatically.
    auto applyPreset = [panelSpinPad, panelSpinFpc, panelSpinTrace,
                        solverCombo, tolSpin, maxIterSpin, syncIterFields](
                            double pad, double trace,
                            int solverIdx, double tol, int iterMax) {
        panelSpinPad   ->setValue(pad);
        panelSpinFpc   ->setValue(pad);
        panelSpinTrace ->setValue(trace);
        solverCombo    ->setCurrentIndex(solverIdx);
        tolSpin        ->setValue(tol);
        maxIterSpin    ->setValue(iterMax);
        syncIterFields();
    };
    QObject::connect(btnQuick, &QPushButton::clicked, &dlg, [applyPreset]() {
        // Pad/FPC 0.5 mm; trace 1 um; Direct LU.
        applyPreset(0.5, 0.001, /*solver=*/0, /*tol=*/1e-6, /*iter=*/500);
    });
    QObject::connect(btnConv, &QPushButton::clicked, &dlg, [applyPreset]() {
        // Pad/FPC 0.2 mm; trace 0.5 um; Direct LU.
        applyPreset(0.2, 0.0005, /*solver=*/0, /*tol=*/1e-6, /*iter=*/500);
    });
    QObject::connect(btnPrec, &QPushButton::clicked, &dlg, [applyPreset]() {
        // Pad/FPC 0.05 mm; trace 0.1 um; BiCGStab tight tolerance.
        applyPreset(0.05, 0.0001, /*solver=*/1, /*tol=*/1e-8, /*iter=*/2000);
    });

    root->addWidget(settingsBox);

    // ---- Live preview line --------------------------------------------------
    auto* preview = new QLabel(&dlg);
    preview->setTextFormat(Qt::RichText);
    preview->setWordWrap(true);
    preview->setStyleSheet(QStringLiteral(
        "QLabel { background:#fcfcfc; border:1px solid #e2e6ea;"
        " border-radius:6px; padding:8px 10px; }"));
    root->addWidget(preview);

    // ---- Compute / Cancel / Close ------------------------------------------
    auto* btnRow = new QHBoxLayout;
    auto* btnRun    = new QPushButton(QStringLiteral("Compute"), &dlg);
    btnRun->setDefault(true);
    btnRun->setMinimumWidth(110);
    auto* btnCancel = new QPushButton(QStringLiteral("Cancel"), &dlg);
    btnCancel->setEnabled(false);
    auto* btnClose  = new QPushButton(QStringLiteral("Close"), &dlg);
    btnRow->addWidget(btnRun);
    btnRow->addWidget(btnCancel);
    btnRow->addStretch(1);
    btnRow->addWidget(btnClose);
    root->addLayout(btnRow);

    // ---- Result panel -------------------------------------------------------
    auto* resultBox = new QGroupBox(QStringLiteral("Result"), &dlg);
    auto* resultLay = new QVBoxLayout(resultBox);
    auto* result = new QLabel(QStringLiteral(
        "<span style='color:#888'>"
        "Adjust panel size, then click <b>Compute</b>.</span>"), resultBox);
    result->setTextFormat(Qt::RichText);
    result->setWordWrap(true);
    result->setMinimumHeight(80);
    result->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    resultLay->addWidget(result);
    root->addWidget(resultBox, 1);

    // ---- Live panel-count + ETA --------------------------------------------
    auto* debounce = new QTimer(&dlg);
    debounce->setSingleShot(true);
    debounce->setInterval(80);

    auto recomputePreview = [&, panelSpinPad, panelSpinFpc, panelSpinTrace,
                             preview, gpuChk, solverCombo]() {
        const double pPad   = panelSpinPad->value();
        const double pFpc   = panelSpinFpc->value();
        const double pTrace = panelSpinTrace->value();
        int NA = 0, NB = 0;
        QString warn;
        try {
            const auto pa = ccc::core::panelizeConductor(refsA, model_, pPad, pFpc, pTrace);
            const auto pb = ccc::core::panelizeConductor(refsB, model_, pPad, pFpc, pTrace);
            NA = int(pa.size());
            NB = int(pb.size());
        } catch (const std::exception& e) {
            warn = QString::fromUtf8(e.what());
        }
        const int N = NA + NB;
        const bool useGpu    = gpuChk->isEnabled() && gpuChk->isChecked();
        const bool iterative = (solverCombo->currentIndex() == 1);
        const double et = estimatedMs(N, useGpu, iterative);
        const QString col = severityColor(et);

        QString html;
        if (!warn.isEmpty()) {
            html = QStringLiteral(
                "<span style='color:#c8261a'>Cannot panelise: %1</span>")
                .arg(warn.toHtmlEscaped());
        } else {
            html = QStringLiteral(
                "Panels &nbsp;<b>A=%1</b> &middot; <b>B=%2</b> &middot; "
                "<b>total %3</b>"
                "&nbsp;&nbsp;|&nbsp;&nbsp;"
                "Estimated time "
                "<span style='color:%4; font-weight:600'>%5</span>")
                .arg(NA).arg(NB).arg(N)
                .arg(col).arg(formatDuration(et));
            // Memory hint depends on solver:
            //   DirectLU  : N x N matrix (8*N^2 bytes)
            //   BiCGStab  : ~10 vectors of N doubles (80*N bytes)
            const double memMB = iterative
                ? (80.0 * double(N) / (1024.0 * 1024.0))
                : (8.0  * double(N) * double(N) / (1024.0 * 1024.0));
            if (memMB > 4096.0) {
                html += QStringLiteral(
                    "<br><span style='color:#c8261a'>"
                    "Needs ~%1 GB of RAM. May fail to allocate.</span>")
                    .arg(memMB / 1024.0, 0, 'f', 1);
            } else if (memMB > 512.0) {
                html += QStringLiteral(
                    "<br><span style='color:#cc6a00'>"
                    "Needs ~%1 MB of RAM.</span>")
                    .arg(memMB, 0, 'f', 0);
            } else if (et > 30000.0) {
                html += QStringLiteral(
                    "<br><span style='color:#cc6a00'>"
                    "This will take a while. Coffee?</span>");
            }
        }
        preview->setText(html);
    };
    QObject::connect(debounce, &QTimer::timeout, &dlg, recomputePreview);

    auto schedulePreview = [debounce] { debounce->start(); };
    QObject::connect(panelSpinPad,
        QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        &dlg, schedulePreview);
    QObject::connect(panelSpinFpc,
        QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        &dlg, schedulePreview);
    QObject::connect(panelSpinTrace,
        QOverload<double>::of(&QDoubleSpinBox::valueChanged),
        &dlg, schedulePreview);
    QObject::connect(gpuChk, &QCheckBox::toggled, &dlg,
        [&recomputePreview](bool){ recomputePreview(); });
    QObject::connect(solverCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        &dlg, [&recomputePreview](int){ recomputePreview(); });
    recomputePreview();

    // ---- Background solve --------------------------------------------------
    auto* watcher = new QFutureWatcher<BemJob>(&dlg);
    auto* elapsedTimer = new QTimer(&dlg);
    elapsedTimer->setInterval(100);
    auto runStart = std::make_shared<QElapsedTimer>();
    auto stopFlag = std::make_shared<std::atomic<bool>>(false);

    QObject::connect(elapsedTimer, &QTimer::timeout, &dlg,
        [result, runStart]() {
            const double ms = double(runStart->elapsed());
            result->setText(QStringLiteral(
                "<span style='color:#444'>Computing&hellip; "
                "<span style='color:#888'>elapsed %1</span></span>")
                .arg(formatDuration(ms)));
        });

    QObject::connect(watcher, &QFutureWatcher<BemJob>::finished, &dlg,
        [watcher, result, btnRun, btnCancel, elapsedTimer]() {
            elapsedTimer->stop();
            btnRun->setEnabled(true);
            btnCancel->setEnabled(false);
            const auto job = watcher->result();
            if (!job.ok) {
                const bool wasCancel =
                    job.error.contains(QStringLiteral("cancelled"),
                                       Qt::CaseInsensitive);
                if (wasCancel) {
                    result->setText(QStringLiteral(
                        "<span style='color:#888'>Cancelled by user.</span>"));
                } else {
                    result->setText(QStringLiteral(
                        "<span style='color:#c8261a'>Error: %1</span>")
                        .arg(job.error.toHtmlEscaped()));
                }
                return;
            }
            const auto& r = job.result;
            const QString cm = QString::fromStdString(
                ccc::core::formatCapacitance(r.Cm));
            const QString ca = QString::fromStdString(
                ccc::core::formatCapacitance(r.CselfA));
            const QString cb = QString::fromStdString(
                ccc::core::formatCapacitance(r.CselfB));
            const double total = r.assembleMs + r.solveMs;
            QString iterRow;
            if (r.iters[0] > 0 || r.iters[1] > 0) {
                iterRow = QStringLiteral(
                    "<tr><td style='color:#666'>BiCGStab</td>"
                        "<td>%1 iters (rel resid %2) "
                        "&middot; %3 iters (%4)</td></tr>")
                    .arg(r.iters[0])
                    .arg(r.residual[0], 0, 'e', 1)
                    .arg(r.iters[1])
                    .arg(r.residual[1], 0, 'e', 1);
            }
            result->setText(QStringLiteral(
                "<div style='font-size:13pt; margin-bottom:6px'>"
                "Mutual capacitance "
                "<b style='color:#1a4ea0'>C<sub>m</sub> = %1</b></div>"
                "<table cellspacing='2' cellpadding='2'>"
                "<tr><td style='color:#666'>Panels</td>"
                    "<td>A&nbsp;%2&nbsp; &middot; &nbsp;B&nbsp;%3"
                    "&nbsp; &middot; &nbsp;total&nbsp;%4</td></tr>"
                "<tr><td style='color:#666'>Self-cap</td>"
                    "<td>C<sub>self,A</sub>&nbsp;%5"
                    "&nbsp; &middot; &nbsp;C<sub>self,B</sub>&nbsp;%6</td></tr>"
                "%7"
                "<tr><td style='color:#666'>Timing</td>"
                    "<td>assemble&nbsp;%8&nbsp;ms"
                    "&nbsp; &middot; &nbsp;solve&nbsp;%9&nbsp;ms"
                    "&nbsp; &middot; &nbsp;total&nbsp;%10&nbsp;ms</td></tr>"
                "</table>")
                .arg(cm).arg(r.NA).arg(r.NB).arg(r.NA + r.NB)
                .arg(ca).arg(cb)
                .arg(iterRow)
                .arg(r.assembleMs, 0, 'f', 1)
                .arg(r.solveMs,    0, 'f', 1)
                .arg(total,        0, 'f', 1));
        });

    auto launch = [&, watcher, runStart, elapsedTimer, btnRun, btnCancel,
                   result, panelSpinPad, panelSpinFpc, panelSpinTrace,
                   epsSpin, imgChk, gpuChk, solverCombo,
                   tolSpin, maxIterSpin, stopFlag]() {
        stopFlag->store(false);

        ccc::core::BemOptions opts;
        opts.panelSize       = panelSpinTrace->value();
        opts.panelSizePad    = panelSpinPad->value();
        opts.panelSizeFpc    = panelSpinFpc->value();
        opts.panelSizeTrace  = panelSpinTrace->value();
        opts.epsEff          = epsSpin->value();
        opts.imageShield     = imgChk->isChecked();
        opts.useGpu          = gpuChk->isEnabled() && gpuChk->isChecked();
        opts.solver          = (solverCombo->currentIndex() == 1)
                                 ? ccc::core::BemSolver::BiCGStab
                                 : ccc::core::BemSolver::DirectLU;
        opts.iterTol         = tolSpin->value();
        opts.iterMaxIters    = int(maxIterSpin->value());
        opts.stopFlag        = stopFlag.get();

        const auto refsAcopy = refsA;
        const auto refsBcopy = refsB;
        ccc::core::Model modelCopy = model_;

        btnRun->setEnabled(false);
        btnCancel->setEnabled(true);
        result->setText(QStringLiteral(
            "<span style='color:#444'>Computing&hellip; elapsed 0 ms</span>"));
        runStart->restart();
        elapsedTimer->start();

        QFuture<BemJob> fut = QtConcurrent::run(
            [opts, refsAcopy, refsBcopy, modelCopy, stopFlag]() -> BemJob {
                BemJob j;
                try {
                    const auto pa = ccc::core::panelizeConductor(
                        refsAcopy, modelCopy,
                        opts.panelSizePad, opts.panelSizeFpc, opts.panelSizeTrace);
                    if (stopFlag->load())
                        throw std::runtime_error("BEM cancelled");
                    const auto pb = ccc::core::panelizeConductor(
                        refsBcopy, modelCopy,
                        opts.panelSizePad, opts.panelSizeFpc, opts.panelSizeTrace);
                    if (stopFlag->load())
                        throw std::runtime_error("BEM cancelled");
                    j.NA = int(pa.size());
                    j.NB = int(pb.size());
                    j.result = ccc::core::computeMutualCapacitance(
                        pa, pb, modelCopy, opts);
                    j.ok = true;
                } catch (const std::exception& e) {
                    j.ok = false;
                    j.error = QString::fromUtf8(e.what());
                }
                return j;
            });
        watcher->setFuture(fut);
    };

    QObject::connect(btnRun,    &QPushButton::clicked, &dlg, launch);
    QObject::connect(btnCancel, &QPushButton::clicked, &dlg,
        [watcher, result, stopFlag, btnCancel]() {
            if (watcher->isRunning()) {
                stopFlag->store(true);
                btnCancel->setEnabled(false);
                result->setText(QStringLiteral(
                    "<span style='color:#888'>Cancelling&hellip;</span>"));
            }
        });
    QObject::connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.resize(560, 600);
    dlg.exec();

    if (watcher->isRunning()) {
        stopFlag->store(true);
        watcher->waitForFinished();
    }
}

void MainWindow::startSelectedFastCapCompute() {
    if (!viewport_ || !capPanel_ || capComputeRunning_) return;
    updateCapSelectionPanel();
    if (capNetA_.isEmpty() || capNetB_.isEmpty() || capNetA_ == capNetB_) return;

    const int capMode = capSolverCombo_
        ? capSolverCombo_->currentData().toInt()
        : int(ccc::core::ExternalCapSolver::FastCapFixed);
    const auto selectedSolver = solverForCapMode(capMode);
    const bool fusionMode = capModeIsFusion(capMode);
    const bool solverAvailable = capModeAvailable(capMode, fastCapAvailable_, fasterCapAvailable_);
    const QString solverLabel = labelForCapMode(capMode);

    if (!solverAvailable) {
        if (capResult_) {
            capResult_->setText(QStringLiteral(
                "<span style='color:#c8261a'>%1 executable is not available.</span>")
                .arg(solverLabel));
        }
        return;
    }

    auto refsAheap = std::make_shared<ccc::core::ConductorRefs>(
        refsForNet(model_, capNetA_));
    auto refsBheap = std::make_shared<ccc::core::ConductorRefs>(
        refsForNet(model_, capNetB_));
    if (refsEmpty(*refsAheap) || refsEmpty(*refsBheap)) {
        if (capResult_) {
            capResult_->setText(QStringLiteral(
                "<span style='color:#c8261a'>One of the selected nets has no copper.</span>"));
        }
        return;
    }

    ccc::core::BemOptions opts;
    opts.panelSize = capTracePanelSpin_ ? capTracePanelSpin_->value() : 0.2;
    opts.panelSizePad = capPanelSpin_ ? capPanelSpin_->value() : 0.5;
    opts.panelSizeFpc = opts.panelSizePad;
    opts.panelSizeTrace = opts.panelSize;
    opts.epsEff = capEpsSpin_ ? capEpsSpin_->value() : 4.5;
    opts.imageShield = false;
    opts.useGpu = false;
    opts.solver = ccc::core::BemSolver::DirectLU;
    opts.iterTol = 1e-6;
    opts.iterMaxIters = 1000;

    capStopFlag_ = std::make_shared<std::atomic<bool>>(false);
    opts.stopFlag = capStopFlag_.get();
    capComputeRunning_ = true;
    if (viewport_) viewport_->setEnabled(false);
    if (layerPanel_) layerPanel_->setEnabled(false);
    if (capComputeButton_) capComputeButton_->setEnabled(false);
    if (capCancelButton_) capCancelButton_->setEnabled(true);
    if (capProgressBar_) capProgressBar_->setVisible(true);
    if (capResult_) {
        capResult_->setText(QStringLiteral(
            "<span style='color:#444'>%1 computing&hellip;</span>")
            .arg(solverLabel));
    }

    auto* watcher = new QFutureWatcher<BemJob>(this);
    auto* timer = new QTimer(watcher);
    timer->setInterval(200);
    auto runStart = std::make_shared<QElapsedTimer>();
    const QString netA = capNetA_;
    const QString netB = capNetB_;
    auto stopFlag = capStopFlag_;
    const ccc::core::Model* modelPtr = &model_;
    auto progressState = std::make_shared<CapProgressState>();

    connect(timer, &QTimer::timeout, this, [this, runStart, solverLabel, progressState]() {
        if (!capResult_) return;
        const double s = double(runStart->elapsed()) / 1000.0;
        QString phase;
        QString detail;
        int step = 0;
        int totalSteps = 0;
        int panelsA = 0;
        int panelsB = 0;
        int panelsEnvironment = 0;
        int dielectricPanels = 0;
        std::size_t outputBytes = 0;
        double solverElapsed = 0.0;
        {
            std::lock_guard<std::mutex> lock(progressState->mutex);
            phase = progressState->phase;
            detail = progressState->detail;
            step = progressState->step;
            totalSteps = progressState->totalSteps;
            panelsA = progressState->panelsA;
            panelsB = progressState->panelsB;
            panelsEnvironment = progressState->panelsEnvironment;
            dielectricPanels = progressState->dielectricPanels;
            outputBytes = progressState->outputBytes;
            solverElapsed = progressState->solverElapsedSeconds;
        }
        if (phase.isEmpty()) phase = QStringLiteral("Preparing geometry");
        const QString stepText = totalSteps > 0
            ? QStringLiteral(" step %1/%2").arg(step).arg(totalSteps)
            : QString();
        capResult_->setText(QStringLiteral(
            "<div><b>%1%2</b> - %3 s</div>"
            "<div style='color:#555'>%4</div>"
            "<div style='color:#666'>Panels A=%5, B=%6, env=%7, dielectric=%8<br/>"
            "Solver output %9 bytes, solver active %10 s</div>")
            .arg(phase.toHtmlEscaped())
            .arg(stepText)
            .arg(s, 0, 'f', 1)
            .arg(detail.toHtmlEscaped())
            .arg(panelsA)
            .arg(panelsB)
            .arg(panelsEnvironment)
            .arg(dielectricPanels)
            .arg(qulonglong(outputBytes))
            .arg(solverElapsed, 0, 'f', 1));
    });

    connect(watcher, &QFutureWatcher<BemJob>::finished, this,
            [this, watcher, timer, netA, netB, capMode, solverLabel]() {
        timer->stop();
        const auto job = watcher->result();
        watcher->deleteLater();
        capComputeRunning_ = false;
        capStopFlag_.reset();
        if (viewport_) viewport_->setEnabled(true);
        if (layerPanel_) layerPanel_->setEnabled(true);
        if (capProgressBar_) capProgressBar_->setVisible(false);
        const auto [currentA, currentB] = viewport_ ? viewport_->selectedCapNets()
                                                    : std::pair<QString, QString>{};
        const bool ready = !currentA.isEmpty() && !currentB.isEmpty()
                           && currentA != currentB;
        const bool solverStillAvailable = capModeAvailable(
            capMode, fastCapAvailable_, fasterCapAvailable_);
        if (capComputeButton_) {
            capComputeButton_->setEnabled(ready && solverStillAvailable);
        }
        if (capCancelButton_) capCancelButton_->setEnabled(false);
        if (!capResult_) return;

        if (!job.ok) {
            capResult_->setText(QStringLiteral(
                "<span style='color:#c8261a'>Error: %1</span>")
                .arg(job.error.toHtmlEscaped()));
            return;
        }
        if (!job.resultHtml.isEmpty()) {
            capResult_->setText(job.resultHtml);
            return;
        }

        const auto& r = job.result;
        const QString cm = QString::fromStdString(ccc::core::formatCapacitance(r.Cm));
        capResult_->setText(QStringLiteral(
            "<div><b>C<sub>m</sub> = %1</b></div>"
            "<div style='color:#555'>%2 &harr; %3</div>"
            "<div style='color:#666'>Panels A=%4, B=%5<br/>"
            "%6 %7 ms - local GND + stackup dielectric</div>")
            .arg(cm)
            .arg(netA.toHtmlEscaped())
            .arg(netB.toHtmlEscaped())
            .arg(r.NA)
            .arg(r.NB)
            .arg(solverLabel.toHtmlEscaped())
            .arg(r.solveMs, 0, 'f', 0));
    });

    runStart->start();
    timer->start();
    QFuture<BemJob> fut = QtConcurrent::run(
        [opts, refsAheap, refsBheap, modelPtr, stopFlag, netA, netB,
         selectedSolver, solverLabel, fusionMode, progressState]() -> BemJob {
            BemJob j;
            j.netA = netA;
            j.netB = netB;
            j.solver = solverLabel;
            try {
                ccc::core::FastCapEnvironmentOptions fcEnv;
                fcEnv.includeGroundNets = true;
                fcEnv.includeDielectricStack = true;
                fcEnv.environmentMarginMm = 20.0;
                fcEnv.environmentPanelSize = std::max(1.0, opts.panelSizePad * 2.0);
                fcEnv.dielectricPanelSize = std::max(2.0, fcEnv.environmentPanelSize * 2.0);
                fcEnv.solverTimeoutSeconds = 0.0;
                fcEnv.progressCallback = [progressState](const ccc::core::FastCapProgress& p) {
                    std::lock_guard<std::mutex> lock(progressState->mutex);
                    progressState->phase = QString::fromStdString(p.phase);
                    progressState->detail = QString::fromStdString(p.detail);
                    progressState->step = p.step;
                    progressState->totalSteps = p.totalSteps;
                    progressState->panelsA = p.panelsA;
                    progressState->panelsB = p.panelsB;
                    progressState->panelsEnvironment = p.panelsEnvironment;
                    progressState->dielectricPanels = p.dielectricPanels;
                    progressState->outputBytes = p.outputBytes;
                    progressState->solverElapsedSeconds = p.elapsedSeconds;
                };
                if (fusionMode) {
                    const auto fusion = ccc::core::computeMutualCapacitanceFusion(
                        *modelPtr, *refsAheap, *refsBheap,
                        netA.toStdString(), netB.toStdString(), opts, fcEnv);
                    j.result = fusion.fasterCapReference.result;
                    j.resultHtml = fusionResultHtml(fusion, netA, netB);
                } else {
                    fcEnv.solver = selectedSolver;
                    j.result = ccc::core::computeMutualCapacitanceFastCapWithEnvironment(
                        *modelPtr, *refsAheap, *refsBheap,
                        netA.toStdString(), netB.toStdString(), opts, fcEnv);
                }
                j.NA = j.result.NA;
                j.NB = j.result.NB;
                j.ok = true;
            } catch (const std::exception& e) {
                j.ok = false;
                j.error = QString::fromUtf8(e.what());
            }
            return j;
        });
    watcher->setFuture(fut);
}

// ============================================================================
//  Net-only Cap dialog: pick two nets from combo boxes, compute their mutual
//  capacitance via external FasterCap. Bypasses the viewport entirely.
// ============================================================================
void MainWindow::runNetCapacitanceDialog() {
    runNetCapacitanceDialogForNets({}, {}, false);
}

void MainWindow::runNetCapacitanceDialogForNets(const QString& initialNetA,
                                                const QString& initialNetB,
                                                bool autoStart,
                                                int initialSolverIndex) {
    (void)initialSolverIndex;
    // Collect unique non-empty nets from the model.
    std::set<QString> netSet;
    for (const auto& p : model_.pads)   if (!p.net.empty()) netSet.insert(QString::fromStdString(p.net));
    for (const auto& t : model_.traces) if (!t.net.empty()) netSet.insert(QString::fromStdString(t.net));
    for (const auto& z : model_.zones)  if (!z.net.empty()) netSet.insert(QString::fromStdString(z.net));
    if (netSet.size() < 2) {
        QMessageBox::information(this, QStringLiteral("Net Capacitance"),
            QStringLiteral("Need at least 2 named nets in the model. "
                           "Open a KiCad PCB or Gerber project first."));
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("Net-to-Net Capacitance"));
    auto* root = new QVBoxLayout(&dlg);
    root->setContentsMargins(14, 14, 14, 14);
    root->setSpacing(10);

    // ---- Net selection -----------------------------------------------------
    auto* netBox = new QGroupBox(QStringLiteral("Nets"), &dlg);
    auto* netForm = new QFormLayout(netBox);
    auto* comboA = new QComboBox(netBox);
    auto* comboB = new QComboBox(netBox);
    for (const auto& n : netSet) { comboA->addItem(n); comboB->addItem(n); }
    comboA->setCurrentIndex(0);
    comboB->setCurrentIndex(int(netSet.size() > 1 ? 1 : 0));
    auto setComboToNet = [](QComboBox* combo, const QString& net) {
        if (net.isEmpty()) return false;
        const int idx = combo->findText(net);
        if (idx < 0) return false;
        combo->setCurrentIndex(idx);
        return true;
    };
    const bool initialAOk = setComboToNet(comboA, initialNetA);
    const bool initialBOk = setComboToNet(comboB, initialNetB);
    if (comboA->currentText() == comboB->currentText()) {
        for (int i = 0; i < comboB->count(); ++i) {
            if (comboB->itemText(i) != comboA->currentText()) {
                comboB->setCurrentIndex(i);
                break;
            }
        }
    }
    const bool shouldAutoStart = autoStart && initialAOk && initialBOk
        && comboA->currentText() != comboB->currentText();
    auto* btnPickA = new QPushButton(QStringLiteral("Pick from viewport"), netBox);
    auto* btnPickB = new QPushButton(QStringLiteral("Pick from viewport"), netBox);
    auto* rowA = new QHBoxLayout; rowA->addWidget(comboA, 1); rowA->addWidget(btnPickA);
    auto* rowB = new QHBoxLayout; rowB->addWidget(comboB, 1); rowB->addWidget(btnPickB);
    netForm->addRow(QStringLiteral("Net A"), rowA);
    netForm->addRow(QStringLiteral("Net B"), rowB);
    root->addWidget(netBox);

    // Helper: hide dialog, listen for one selection change in the viewport,
    // grab that element's net, set the corresponding combo, restore dialog.
    //
    // Caveat: dlg.exec() is application-modal. Just calling hide() does NOT
    // always release modality on every Qt build ??the viewport then ignores
    // mouse input and the GUI looks frozen. We explicitly drop modality
    // before hide and restore it on re-show.
    auto pickNetInto = [this, &dlg](QComboBox* combo) {
        dlg.setWindowModality(Qt::NonModal);
        dlg.hide();
        auto conn = std::make_shared<QMetaObject::Connection>();
        *conn = QObject::connect(viewport_, &Viewport3D::selectionChanged,
            &dlg, [this, conn, combo, &dlg](SceneRef ref) {
                QObject::disconnect(*conn);
                std::string net;
                const std::string id = ref.id.toStdString();
                if (ref.kind == QLatin1String("pad")) {
                    for (const auto& p : model_.pads) if (p.id == id) { net = p.net; break; }
                } else if (ref.kind == QLatin1String("trace") ||
                           ref.kind == QLatin1String("waypoint")) {
                    for (const auto& t : model_.traces) if (t.id == id) { net = t.net; break; }
                } else if (ref.kind == QLatin1String("zone")) {
                    for (const auto& z : model_.zones) if (z.id == id) { net = z.net; break; }
                }
                if (!net.empty()) {
                    const int idx = combo->findText(QString::fromStdString(net));
                    if (idx >= 0) combo->setCurrentIndex(idx);
                }
                dlg.setWindowModality(Qt::ApplicationModal);
                dlg.show();
                dlg.activateWindow();
                dlg.raise();
            });
    };
    QObject::connect(btnPickA, &QPushButton::clicked, &dlg, [pickNetInto, comboA]() {
        pickNetInto(comboA);
    });
    QObject::connect(btnPickB, &QPushButton::clicked, &dlg, [pickNetInto, comboB]() {
        pickNetInto(comboB);
    });

    // ---- Solver settings (minimal) -----------------------------------------
    auto* setBox = new QGroupBox(QStringLiteral("Solver"), &dlg);
    auto* setForm = new QFormLayout(setBox);
    auto* psPadSpin   = new QDoubleSpinBox(setBox);
    psPadSpin->setRange(0.001, 5.0); psPadSpin->setDecimals(4);
    psPadSpin->setSingleStep(0.1); psPadSpin->setSuffix(QStringLiteral(" mm"));
    psPadSpin->setValue(0.5);   // PCB-scale default
    auto* psTrSpin    = new QDoubleSpinBox(setBox);
    psTrSpin->setRange(0.0001, 5.0); psTrSpin->setDecimals(5);
    psTrSpin->setSingleStep(0.05); psTrSpin->setSuffix(QStringLiteral(" mm"));
    psTrSpin->setValue(0.2);    // 200 um default for PCB tracks

    // Three preset accuracy levels for PCB-scale geometry.
    auto* presetRow = new QHBoxLayout;
    auto* btnQuick = new QPushButton(QStringLiteral("Quick"),       setBox);
    auto* btnConv  = new QPushButton(QStringLiteral("Convergence"), setBox);
    auto* btnPrec  = new QPushButton(QStringLiteral("Precise"),     setBox);
    btnQuick->setToolTip(QStringLiteral(
        "Fast scan for PCB-scale nets.\n"
        "Pad/Zone 1.0 mm, Trace 0.5 mm. Seconds."));
    btnConv->setToolTip(QStringLiteral(
        "Convergence-grade (1% accuracy).\n"
        "Pad/Zone 0.3 mm, Trace 0.1 mm. Tens of seconds to a few minutes."));
    btnPrec->setToolTip(QStringLiteral(
        "Reference precision.\n"
        "Pad/Zone 0.1 mm, Trace 0.03 mm. May take hours."));
    presetRow->addWidget(btnQuick);
    presetRow->addWidget(btnConv);
    presetRow->addWidget(btnPrec);
    presetRow->addStretch(1);
    QObject::connect(btnQuick, &QPushButton::clicked, &dlg,
        [psPadSpin, psTrSpin]() { psPadSpin->setValue(1.0);  psTrSpin->setValue(0.5); });
    QObject::connect(btnConv,  &QPushButton::clicked, &dlg,
        [psPadSpin, psTrSpin]() { psPadSpin->setValue(0.3);  psTrSpin->setValue(0.1); });
    QObject::connect(btnPrec,  &QPushButton::clicked, &dlg,
        [psPadSpin, psTrSpin]() { psPadSpin->setValue(0.1);  psTrSpin->setValue(0.03); });
    setForm->addRow(QStringLiteral("Preset"), presetRow);
    auto* epsSpin = new QDoubleSpinBox(setBox);
    epsSpin->setRange(1.0, 100.0); epsSpin->setDecimals(2);
    epsSpin->setSingleStep(0.1); epsSpin->setValue(4.5);
    auto* solverLabel = new QLabel(QStringLiteral("FasterCap (external)"), setBox);
    solverLabel->setToolTip(QStringLiteral(
        "Uses external/fastercap/FasterCap.exe via subprocess and parses "
        "the CAPACITANCE MATRIX output."));
    if (!fastCapAvailable_) {
        solverLabel->setText(QStringLiteral("FasterCap (missing)"));
        solverLabel->setStyleSheet(QStringLiteral("color:#c8261a"));
    }
    setForm->addRow(QStringLiteral("Pad/FPC/Zone panel"), psPadSpin);
    setForm->addRow(QStringLiteral("Trace panel"),        psTrSpin);
    setForm->addRow(QStringLiteral("Effective eps_r"),    epsSpin);
    setForm->addRow(QStringLiteral("Solver"),             solverLabel);
    root->addWidget(setBox);

    // ---- Buttons + result --------------------------------------------------
    auto* btnRow = new QHBoxLayout;
    auto* btnRun   = new QPushButton(QStringLiteral("Compute"), &dlg);
    btnRun->setDefault(true);
    auto* btnCancel = new QPushButton(QStringLiteral("Cancel"), &dlg);
    btnCancel->setEnabled(false);
    auto* btnClose = new QPushButton(QStringLiteral("Close"), &dlg);
    btnRow->addWidget(btnRun);
    btnRow->addWidget(btnCancel);
    btnRow->addStretch(1);
    btnRow->addWidget(btnClose);
    root->addLayout(btnRow);

    auto* resultBox = new QGroupBox(QStringLiteral("Result"), &dlg);
    auto* resultLay = new QVBoxLayout(resultBox);
    auto* result = new QLabel(QStringLiteral(
        "<span style='color:#888'>Pick two nets, then click <b>Compute</b>.</span>"),
        resultBox);
    result->setTextFormat(Qt::RichText);
    result->setWordWrap(true);
    result->setMinimumHeight(80);
    result->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    resultLay->addWidget(result);
    root->addWidget(resultBox, 1);

    // ---- Background BEM ----------------------------------------------------
    auto* watcher  = new QFutureWatcher<BemJob>(&dlg);
    auto* timer    = new QTimer(&dlg);
    timer->setInterval(200);
    auto runStart  = std::make_shared<QElapsedTimer>();
    auto stopFlag  = std::make_shared<std::atomic<bool>>(false);

    QObject::connect(timer, &QTimer::timeout, &dlg, [result, runStart]() {
        const double s = double(runStart->elapsed()) / 1000.0;
        result->setText(QStringLiteral(
            "<span style='color:#444'>Computing&hellip; %1 s</span>")
            .arg(s, 0, 'f', 1));
    });

    QObject::connect(watcher, &QFutureWatcher<BemJob>::finished, &dlg,
        [watcher, result, btnRun, btnCancel, timer]() {
            timer->stop();
            btnRun->setEnabled(true);
            btnCancel->setEnabled(false);
            const auto job = watcher->result();
            if (!job.ok) {
                result->setText(QStringLiteral(
                    "<span style='color:#c8261a'>Error: %1</span>")
                    .arg(job.error.toHtmlEscaped()));
                return;
            }
            const auto& r = job.result;
            const QString cm = QString::fromStdString(ccc::core::formatCapacitance(r.Cm));
            const QString pairLine = (!job.netA.isEmpty() && !job.netB.isEmpty())
                                         ? QStringLiteral(
                                               "<div style='color:#555; margin-top:3px'>"
                                               "%1 &harr; %2 &nbsp;|&nbsp; %3</div>")
                                               .arg(job.netA.toHtmlEscaped())
                                               .arg(job.netB.toHtmlEscaped())
                                               .arg(job.solver.toHtmlEscaped())
                                         : QString();
            QString iterLine;
            if (r.iters[0] > 0 || r.iters[1] > 0) {
                iterLine = QStringLiteral(
                    "<div style='color:#666; margin-top:4px'>"
                    "BiCGStab: %1 iters (rel resid %2), "
                    "%3 iters (%4)</div>")
                    .arg(r.iters[0])
                    .arg(r.residual[0], 0, 'e', 1)
                    .arg(r.iters[1])
                    .arg(r.residual[1], 0, 'e', 1);
            }
            result->setText(QStringLiteral(
                "<div style='font-size:14pt'>"
                "Mutual capacitance "
                "<b style='color:#1a4ea0'>C<sub>m</sub> = %1</b></div>"
                "%2"
                "<div style='color:#666; margin-top:4px'>"
                "Panels: A=%3, B=%4, total %5 &nbsp;|&nbsp; "
                "assemble %6 ms, solve %7 ms</div>"
                "%8")
                .arg(cm)
                .arg(pairLine)
                .arg(r.NA).arg(r.NB).arg(r.NA + r.NB)
                .arg(r.assembleMs, 0, 'f', 0).arg(r.solveMs, 0, 'f', 0)
                .arg(iterLine));
        });

    auto launch = [&, watcher, runStart, timer, btnRun, btnCancel,
                   result, comboA, comboB, psPadSpin, psTrSpin, epsSpin,
                   stopFlag]() {
        const QString netA = comboA->currentText();
        const QString netB = comboB->currentText();
        if (netA == netB) {
            result->setText(QStringLiteral(
                "<span style='color:#c8261a'>Net A and Net B must be different.</span>"));
            return;
        }
        if (!fastCapAvailable_) {
            result->setText(QStringLiteral(
                "<span style='color:#c8261a'>FasterCap executable is not available.</span>"));
            return;
        }
        // Build ConductorRefs from net membership.
        ccc::core::ConductorRefs refsA, refsB;
        const std::string nA = netA.toStdString(), nB = netB.toStdString();
        for (const auto& p : model_.pads) {
            if (p.net == nA) refsA.padIds.push_back(p.id);
            else if (p.net == nB) refsB.padIds.push_back(p.id);
        }
        for (const auto& t : model_.traces) {
            if (t.net == nA) refsA.traceIds.push_back(t.id);
            else if (t.net == nB) refsB.traceIds.push_back(t.id);
        }
        for (const auto& z : model_.zones) {
            if (z.net == nA) refsA.zoneIds.push_back(z.id);
            else if (z.net == nB) refsB.zoneIds.push_back(z.id);
        }
        if ((refsA.padIds.empty() && refsA.traceIds.empty() && refsA.zoneIds.empty())
            || (refsB.padIds.empty() && refsB.traceIds.empty() && refsB.zoneIds.empty())) {
            result->setText(QStringLiteral(
                "<span style='color:#c8261a'>One of the selected nets has no "
                "elements in the model.</span>"));
            return;
        }
        stopFlag->store(false);
        ccc::core::BemOptions opts;
        opts.panelSize       = psTrSpin->value();
        opts.panelSizePad    = psPadSpin->value();
        opts.panelSizeFpc    = psPadSpin->value();
        opts.panelSizeTrace  = psTrSpin->value();
        opts.epsEff          = epsSpin->value();
        opts.imageShield     = false;
        opts.useGpu          = false;
        const QString solverName = QStringLiteral("FasterCap (external)");
        opts.solver          = ccc::core::BemSolver::DirectLU;
        opts.iterTol         = 1e-6;
        opts.iterMaxIters    = 1000;
        opts.stopFlag        = stopFlag.get();

        // NOTE: Do NOT copy `model_` on the UI thread. KiCad imports can carry
        // megabytes of zone-outline vertices; copying twice (local copy +
        // lambda capture) was the cause of the multi-second "Not Responding"
        // freeze right after Compute. We pass the model by pointer instead.
        // Safe because dlg.exec() is application-modal (no other File>Open
        // can land while compute runs) and dlg.exec() returns only after
        // waitForFinished() has reaped the worker (see end of this function).
        const ccc::core::Model* modelPtr = &model_;
        auto refsAheap = std::make_shared<ccc::core::ConductorRefs>(std::move(refsA));
        auto refsBheap = std::make_shared<ccc::core::ConductorRefs>(std::move(refsB));

        btnRun->setEnabled(false);
        btnCancel->setEnabled(true);
        result->setText(QStringLiteral("<span style='color:#444'>Computing&hellip;</span>"));
        runStart->restart();
        timer->start();
        QFuture<BemJob> fut = QtConcurrent::run(
            [opts, refsAheap, refsBheap, modelPtr, stopFlag,
             netA, netB, solverName]() -> BemJob {
                BemJob j;
                j.netA = netA;
                j.netB = netB;
                j.solver = solverName;
                try {
                    ccc::core::FastCapEnvironmentOptions fcEnv;
                    fcEnv.includeGroundNets = true;
                    fcEnv.includeDielectricStack = true;
                    fcEnv.environmentMarginMm = 20.0;
                    fcEnv.environmentPanelSize = std::max(1.0, opts.panelSizePad * 2.0);
                    fcEnv.dielectricPanelSize = std::max(2.0, fcEnv.environmentPanelSize * 2.0);
                    j.result = ccc::core::computeMutualCapacitanceFastCapWithEnvironment(
                        *modelPtr, *refsAheap, *refsBheap,
                        netA.toStdString(), netB.toStdString(), opts, fcEnv);
                    j.NA = j.result.NA;
                    j.NB = j.result.NB;
                    j.ok = true;
                } catch (const std::exception& e) {
                    j.ok = false; j.error = QString::fromUtf8(e.what());
                }
                return j;
            });
        watcher->setFuture(fut);
    };

    QObject::connect(btnRun,    &QPushButton::clicked, &dlg, launch);
    QObject::connect(btnCancel, &QPushButton::clicked, &dlg,
        [watcher, stopFlag, result, btnCancel]() {
            if (watcher->isRunning()) {
                stopFlag->store(true);
                btnCancel->setEnabled(false);
                result->setText(QStringLiteral(
                    "<span style='color:#888'>Cancelling&hellip;</span>"));
            }
        });
    QObject::connect(btnClose, &QPushButton::clicked, &dlg, &QDialog::accept);

    dlg.resize(480, 520);
    if (shouldAutoStart) {
        QTimer::singleShot(0, &dlg, launch);
    }
    dlg.exec();
    if (watcher->isRunning()) {
        stopFlag->store(true);
        watcher->waitForFinished();
    }
}

}  // namespace ccc::ui
