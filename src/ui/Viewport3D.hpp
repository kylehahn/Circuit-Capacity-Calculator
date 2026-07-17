#pragma once

#include "Camera.hpp"
#include "core/Model.hpp"

#include <QColor>
#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QOpenGLWidget>
#include <QPoint>
#include <QString>
#include <QVector3D>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ccc::ui {

// Identifies one selectable thing in the scene. `sub` is used for sub-objects
// (e.g. an individual waypoint of a trace).
struct SceneRef {
    QString kind;     // "pad" | "trace" | "fpc" | "waypoint" | "zone" | ""
    QString id;       // for waypoint: the parent trace id
    int     sub = -1; // for waypoint: index into Trace::waypoints
    bool isValid() const { return !kind.isEmpty() && !id.isEmpty(); }
    bool operator==(const SceneRef&) const = default;
};

class Viewport3D : public QOpenGLWidget, protected QOpenGLFunctions_3_3_Core {
    Q_OBJECT
public:
    enum class Mode {
        Select,
        AddPad,
        DeletePad,
        AddTrace,
        EditTrace,
        MoveFpc,
        CapMeasure,        // pick two electrically-connected conductors -> BEM
    };

    enum class ViewMode { Two2D, Three3D };

    explicit Viewport3D(QWidget* parent = nullptr);
    ~Viewport3D() override;

    void setModel(ccc::core::Model* model);
    void rebuildScene();
    void fitView();
    void setLayerVisible(const QString& layerId, bool visible);

    void presetIso();
    void presetTop();
    void presetFront();
    void presetSide();

    void setMode(Mode m);
    Mode mode() const { return mode_; }

    void setViewMode(ViewMode v);
    ViewMode viewMode() const { return viewMode_; }

    void setSnapEnabled(bool e)  { snapEnabled_ = e; }
    void setSnapSize(double mm)  { snapSize_ = std::max(0.01, mm); }
    bool snapEnabled() const     { return snapEnabled_; }
    double snapSize() const      { return snapSize_; }

    void setGridVisible(bool v)  { gridVisible_ = v; update(); }
    bool gridVisible() const     { return gridVisible_; }

    SceneRef selection() const   { return selection_; }
    void clearSelection();
    bool selectNet(const QString& netName);
    bool selectCapNets(const QString& netA, const QString& netB);

    struct CapPick {
        std::vector<QString> padIds;
        std::vector<QString> fpcIds;
        std::vector<QString> traceIds;
        std::vector<QString> zoneIds;
        bool empty() const {
            return padIds.empty() && fpcIds.empty()
                && traceIds.empty() && zoneIds.empty();
        }
    };
    CapPick capPickA() const { return capPickA_; }
    CapPick capPickB() const { return capPickB_; }
    std::pair<QString, QString> selectedCapNets() const;
    void resetCapPicks();

signals:
    void selectionChanged(SceneRef ref);
    void capSelectionChanged(QString netA, QString netB);
    void modelEdited();
    void cursorMoved(double xMm, double yMm);
    void modeChanged(int newMode);
    void viewModeChanged(int newViewMode);
    void capacitanceRequested();

protected:
    void initializeGL() override;
    void resizeGL(int w, int h) override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*)  override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*)      override;
    void keyPressEvent(QKeyEvent*)     override;

private:
    struct Vertex { float px, py, pz, nx, ny, nz; };
    struct Mesh {
        std::vector<Vertex>   vertices;
        std::vector<uint32_t> indices;
    };
    struct GpuMesh {
        QOpenGLBuffer vbo{QOpenGLBuffer::VertexBuffer};
        QOpenGLBuffer ibo{QOpenGLBuffer::IndexBuffer};
        int  indexCount = 0;
        bool ready = false;
        void upload(QOpenGLFunctions_3_3_Core* gl, const Mesh& m);
        void release();
    };
    struct Batch {
        GpuMesh*   mesh = nullptr;
        QMatrix4x4 model;
        QColor     color{180,180,180,255};
        SceneRef   ref;
    };

    GpuMesh meshBox_;
    GpuMesh meshCylinder_;
    GpuMesh meshSphere_;

    QOpenGLShaderProgram     program_;
    QOpenGLVertexArrayObject vao_;
    GLint uMVP_   = -1;
    GLint uModel_ = -1;
    GLint uColor_ = -1;
    GLint uLight_ = -1;

    Camera                camera_;
    std::vector<Batch>    batches_;
    ccc::core::Model*     model_ = nullptr;
    SceneRef              selection_;
    SceneRef              hover_;

    Mode     mode_     = Mode::Select;
    ViewMode viewMode_ = ViewMode::Two2D;

    double traceRenderWidthFloor_ = 0.05;

    bool   marqueeActive_   = false;
    QPoint marqueeStart_;
    QPoint marqueeEnd_;
    bool   marqueeMoved_    = false;

    std::vector<SceneRef> selections_;

    bool   snapEnabled_  = true;
    double snapSize_     = 0.5;
    bool   gridVisible_  = true;
    GpuMesh meshGrid_;
    // Zone fills: split per copper layer, each rendered with its own
    // colorForKicadLayer() colour. Uploaded lazily in paintGL.
    std::map<std::string, GpuMesh> zoneFillMeshes_;
    std::map<std::string, Mesh>    zoneFillsCpu_;
    bool    zoneFillsDirty_ = false;
    bool    gridDirty_   = true;

    // Per-layer merged geometry. Without this, KiCad imports (~1900 segments
    // + 33k zone-outline edges) would emit 35 000+ individual draw-calls per
    // paintGL pass. We bake every trace and zone-outline edge as flat
    // top-face quads into one Mesh per copper layer; that becomes one batch
    // per (layer, category) regardless of element count.
    std::map<std::string, GpuMesh> traceMeshes_;
    std::map<std::string, Mesh>    traceCpu_;
    std::map<std::string, GpuMesh> zoneOutlineMeshes_;
    std::map<std::string, Mesh>    zoneOutlineCpu_;
    bool    mergedMeshesDirty_ = false;

    // Per-net merged meshes built once at PCB-import time. Each entry holds
    // the merged top-face geometry of every pad/trace/zone in that net.
    // Used as a single overlay when the user clicks any element so the whole
    // net visually appears as ONE object. This realises the user-facing
    // meaning of "combine same-net conductors into one".
    std::map<std::string, GpuMesh> netHighlightMeshes_;
    std::map<std::string, Mesh>    netHighlightCpu_;
    std::string                    highlightedNet_;

    // True when the model has changed (load, edit) and the cached static
    // meshes need re-baking. Selection / hover / mode changes do NOT set
    // this — that's the entire point of the cache.
    bool    staticMeshesDirty_ = true;

    CapPick capPickA_;
    CapPick capPickB_;

    struct TraceDraft {
        QString fromId;
        std::vector<std::pair<double,double>> waypoints;
        bool hasCursor = false;
        double cursorX = 0.0, cursorY = 0.0;
    };
    std::optional<TraceDraft> traceDraft_;

    QPoint lastMouseScreen_;
    Qt::MouseButton activeBtn_ = Qt::NoButton;
    bool dragging_ = false;
    SceneRef dragRef_;
    double dragOffsetX_ = 0;
    double dragOffsetY_ = 0;

    void buildPrototypeMeshes();
    void rebuildBatches();
    void buildStaticAndNetMeshes();
    std::string netForRef(const SceneRef& r) const;
    QMatrix4x4 mvp(const QMatrix4x4& model) const;
    bool screenToPlaneXY(QPoint px, double planeZ, double& outX, double& outY) const;
    bool screenToSensorXY(QPoint px, double& outX, double& outY) const;
    SceneRef pickAt(QPoint px) const;
    static Mesh makeBoxMesh(float w, float h, float d);
    static Mesh makeCylinderMesh(int segments);
    static Mesh makeSphereMesh(int latBands, int lonBands);

    double sensorTopZ() const;
    double sensorBottomZ() const;
    double layerTopZ(const QString& id) const;
    double layerBottomZ(const QString& id) const;

    QString nextPadId() const;
    QString nextTraceId() const;
    void    addPadAt(double x, double y);
    void    deleteSceneRef(const SceneRef& ref);
    void    finalizeTraceTo(const SceneRef& target);
    void    cancelTraceDraft();
    void    insertWaypointAt(const QString& traceId, double x, double y);

    double                         snapVal(double v) const;
    std::pair<double, double>      snapPt(double x, double y) const;
    std::pair<double, double>      snap8Dir(double fromX, double fromY,
                                            double toX,   double toY) const;
    bool                           traceDraftAnchor(double& outX, double& outY) const;
    std::vector<std::pair<double,double>> tracePolylinePoints(const ccc::core::Trace& t) const;

    bool handlePressSelect(QMouseEvent*, const SceneRef& hit, double wx, double wy, bool xyValid);
    bool handlePressAddPad(QMouseEvent*, double wx, double wy, bool xyValid);
    bool handlePressDelete(QMouseEvent*, const SceneRef& hit);
    bool handlePressAddTrace(QMouseEvent*, const SceneRef& hit, double wx, double wy, bool xyValid);
    bool handlePressEditTrace(QMouseEvent*, const SceneRef& hit, double wx, double wy, bool xyValid);
    bool handlePressMoveFpc(QMouseEvent*, const SceneRef& hit, double wx, double wy, bool xyValid);
    bool handlePressCapMeasure(QMouseEvent*, const SceneRef& hit);

    CapPick findConductorOf(const SceneRef& ref) const;
    QString netForPick(const CapPick& pick) const;
    void emitCapSelectionChanged();
    bool refInPick(const SceneRef& ref, const CapPick& c) const;

    void buildGridMesh();

    void finishMarqueeSelection();
    bool refInSelections(const SceneRef& r) const;
    void drawMarqueeOverlay();
};

}  // namespace ccc::ui
