#pragma once

#include "Viewport3D.hpp"
#include "core/Model.hpp"

#include <QLabel>
#include <QMainWindow>
#include <QString>
#include <QUndoStack>

#include <atomic>
#include <memory>

class QAction;
class QActionGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFrame;
class QListWidget;
class QProgressBar;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace ccc::ui {

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);

    // Used by ModelSnapshotCmd to restore a saved Model state.
    void restoreModelState(const ccc::core::Model& m);

    // Scriptable import hooks used by visual regression runs.
    bool openPcbProject(const QString& gerberDir,
                        const QString& kicadPcbPath,
                        bool quiet = false);
    bool openKiCadPcb(const QString& path, bool quiet = false);
    bool openGerberProject(const QString& dir, bool quiet = false);
    void showOnlyLayer(const QString& layerId);
    bool selectNet(const QString& netName);
    bool selectCapNets(const QString& netA, const QString& netB);

private slots:
    void onNew();
    void onOpen();
    void onSave();
    void onSaveAs();
    void onSelectionChanged(SceneRef ref);
    void onModelEdited();
    void onCursorMoved(double xMm, double yMm);
    void onModeChanged(int mode);

private:
    void setupUi();
    void setupMenus();
    void setupModeToolbar();
    void setupDocks();
    void setupStatusBar();
    void rebuildLayerPanel();
    void refreshStatus();
    void setDirty(bool d);
    void setCurrentFile(const QString& path);
    bool maybeSave();
    bool saveTo(const QString& path);
    void loadFrom(const QString& path);
    void selectMode(Viewport3D::Mode m);
    void runPcbImportDialog();

    // Solo a single layer (hide everything else). Empty soloId clears solo.
    void applySolo(const QString& soloId);

    // Open the BEM result dialog for the current pair of cap-mode picks.
    void runCapacitanceDialog();

    // Standalone "pick two nets and compute" dialog. Kept for menu access;
    // viewport selection now uses the left FasterCap panel.
    void runNetCapacitanceDialog();
    void runNetCapacitanceDialogForNets(const QString& netA,
                                        const QString& netB,
                                        bool autoStart,
                                        int initialSolverIndex = -1);
    void refreshFastCapAvailability();
    void updateCapSelectionPanel();
    void startSelectedFastCapCompute();
    void showBoardInfoDialog();

    ccc::core::Model model_     = ccc::core::Model::makeDefault();
    ccc::core::Model lastKnown_ = model_;

    Viewport3D*     viewport_     = nullptr;
    QWidget*        layerPanel_   = nullptr;
    QVBoxLayout*    layerLayout_  = nullptr;
    QCheckBox*      snapToggle_   = nullptr;
    QDoubleSpinBox* snapSpin_     = nullptr;
    QActionGroup*   modeGroup_    = nullptr;

    QFrame*         capPanel_ = nullptr;
    QLabel*         capNetAValue_ = nullptr;
    QLabel*         capNetBValue_ = nullptr;
    QLabel*         capResult_ = nullptr;
    QProgressBar*   capProgressBar_ = nullptr;
    QPushButton*    capComputeButton_ = nullptr;
    QPushButton*    capCancelButton_ = nullptr;
    QComboBox*      capSolverCombo_ = nullptr;
    QDoubleSpinBox* capPanelSpin_ = nullptr;
    QDoubleSpinBox* capTracePanelSpin_ = nullptr;
    QDoubleSpinBox* capEpsSpin_ = nullptr;
    QString         capNetA_;
    QString         capNetB_;
    bool            capComputeRunning_ = false;
    bool            fastCapAvailable_ = false;
    bool            fasterCapAvailable_ = false;
    std::shared_ptr<std::atomic<bool>> capStopFlag_;

    QLabel* lblCounts_    = nullptr;
    QLabel* lblCursor_    = nullptr;
    QLabel* lblSelection_ = nullptr;
    QLabel* lblModeHint_  = nullptr;

    QUndoStack* undo_ = nullptr;
    QString currentPath_;
    bool    dirty_ = false;
    QString soloId_;        // empty == no solo
};

}  // namespace ccc::ui
