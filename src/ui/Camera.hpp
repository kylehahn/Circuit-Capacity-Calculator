#pragma once

#include <QMatrix4x4>
#include <QVector3D>

namespace ccc::ui {

// Z-up orbit camera.
//   yawDeg   — rotation around +Z, increasing counter-clockwise from +X
//   pitchDeg — angle above the XY plane, [-89, +89]
struct Camera {
    QVector3D target{0.0f, 0.0f, 0.0f};
    float distance = 200.0f;
    float yawDeg   = -60.0f;
    float pitchDeg = 35.0f;
    float fovDeg   = 40.0f;
    float nearZ    = 0.5f;
    float farZ     = 4000.0f;

    QVector3D position() const;
    QVector3D forward()  const;
    QVector3D right()    const;
    QVector3D up()       const;

    QMatrix4x4 viewMatrix() const;
    QMatrix4x4 projectionMatrix(float aspect) const;

    void orbit(float dYawDeg, float dPitchDeg);
    void pan(float dxScreenPx, float dyScreenPx, int viewportPxHeight);
    void zoom(float wheelSteps);                         // +1 = zoom in
    void fit(const QVector3D& center, float radius);
    void setPresetTop();
    void setPresetIso();
    void setPresetFront();
    void setPresetSide();
};

}  // namespace ccc::ui
