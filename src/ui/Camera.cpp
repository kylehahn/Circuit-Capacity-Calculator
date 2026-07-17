#include "Camera.hpp"

#include <algorithm>
#include <cmath>

namespace ccc::ui {

namespace {
constexpr float kDeg2Rad = 0.0174532925199432957f;
}

QVector3D Camera::position() const {
    const float yr = yawDeg * kDeg2Rad;
    const float pr = pitchDeg * kDeg2Rad;
    const float cp = std::cos(pr), sp = std::sin(pr);
    const float cy = std::cos(yr), sy = std::sin(yr);
    return target + QVector3D(distance * cp * cy, distance * cp * sy, distance * sp);
}
QVector3D Camera::forward() const { return (target - position()).normalized(); }
QVector3D Camera::right()   const { return QVector3D::crossProduct(forward(), QVector3D(0,0,1)).normalized(); }
QVector3D Camera::up()      const { return QVector3D::crossProduct(right(), forward()).normalized(); }

QMatrix4x4 Camera::viewMatrix() const {
    QMatrix4x4 v;
    v.lookAt(position(), target, QVector3D(0, 0, 1));
    return v;
}
QMatrix4x4 Camera::projectionMatrix(float aspect) const {
    QMatrix4x4 p;
    p.perspective(fovDeg, aspect, nearZ, farZ);
    return p;
}

void Camera::orbit(float dYaw, float dPitch) {
    yawDeg   += dYaw;
    pitchDeg = std::clamp(pitchDeg + dPitch, -89.0f, 89.0f);
}
void Camera::pan(float dxs, float dys, int vpH) {
    // Empirically calibrated against user feedback on a top-down 2D
    // KiCad-import view: Y `+=` matches the user's expectation, X needs
    // `-=`. Same convention applies in 3D pan (Ctrl+RMB).
    const float worldPerPx = (2.0f * distance * std::tan(fovDeg * 0.5f * kDeg2Rad)) / std::max(1, vpH);
    target -= right() * (dxs * worldPerPx);
    target += up()    * (dys * worldPerPx);
}
void Camera::zoom(float steps) {
    distance = std::clamp(distance * std::pow(0.85f, steps), 1.0f, 5000.0f);
}
void Camera::fit(const QVector3D& ctr, float radius) {
    target = ctr;
    distance = std::max(1.0f, radius / std::tan(fovDeg * 0.5f * kDeg2Rad) * 1.2f);
}
void Camera::setPresetTop()   { yawDeg = -90.0f; pitchDeg =  89.0f; }
void Camera::setPresetIso()   { yawDeg = -60.0f; pitchDeg =  35.0f; }
void Camera::setPresetFront() { yawDeg = -90.0f; pitchDeg =   0.0f; }
void Camera::setPresetSide()  { yawDeg =   0.0f; pitchDeg =   0.0f; }

}  // namespace ccc::ui
