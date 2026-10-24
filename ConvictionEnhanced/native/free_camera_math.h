#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace ce {

// Unreal uses 65536 rotator units per turn and a world-space Z-up coordinate
// system. Positive pitch looks up; positive yaw turns from +X toward +Y.
struct CameraPose {
    float x = 0, y = 0, z = 0;
    std::int32_t pitch = 0, yaw = 0, roll = 0;
};

struct CameraMotion {
    float strafe = 0, forward = 0, up = 0;
    float yaw = 0, pitch = 0;
};

struct StickVector { float x = 0, y = 0; };

inline constexpr std::array<float, 3> CameraSpeeds{100.0f, 300.0f, 900.0f};
inline constexpr double CameraMaxDeltaSeconds = 0.05;
inline constexpr double CameraLookUnitsPerSecond = 16384.0; // 90 degrees/s
inline constexpr std::int32_t CameraPitchLimit = 16202; // Just under 89 degrees.
inline constexpr float CameraPositionLimit = 10000000.0f;

inline StickVector normalizeStick(std::int16_t x, std::int16_t y, int deadzone) noexcept {
    const double dx = x, dy = y;
    const double magnitude = std::hypot(dx, dy);
    const double threshold = static_cast<double>(std::clamp(deadzone, 0, 32767));
    if (magnitude <= threshold || threshold == 32767.0) return {};
    const double strength = std::min(1.0, (magnitude - threshold) / (32767.0 - threshold));
    return {static_cast<float>(dx * strength / magnitude),
            static_cast<float>(dy * strength / magnitude)};
}

inline float normalizedTrigger(std::uint8_t value, int threshold = 30) noexcept {
    threshold = std::clamp(threshold, 0, 255);
    if (value <= threshold || threshold == 255) return 0;
    return static_cast<float>(value - threshold) / static_cast<float>(255 - threshold);
}

inline bool validCameraPose(const CameraPose& pose) noexcept {
    return std::isfinite(pose.x) && std::isfinite(pose.y) && std::isfinite(pose.z)
        && std::fabs(pose.x) <= CameraPositionLimit
        && std::fabs(pose.y) <= CameraPositionLimit
        && std::fabs(pose.z) <= CameraPositionLimit;
}

inline std::int32_t signedCameraAngle(std::int32_t angle) noexcept {
    // Unsigned masking avoids implementation-defined signed shifts or narrowing.
    const auto value = static_cast<std::uint32_t>(angle) & 0xffffu;
    return value >= 32768u ? static_cast<std::int32_t>(value) - 65536
                           : static_cast<std::int32_t>(value);
}

// Call once per camera update with a monotonic delta. The caller must reset its
// clock on activation, input ownership changes and focus changes. This routine
// cannot touch game state and commits the whole candidate pose only if valid.
inline bool applyCameraMotion(CameraPose& pose, const CameraMotion& input,
                              double deltaSeconds, float speed = CameraSpeeds[1]) noexcept {
    if (!validCameraPose(pose) || !std::isfinite(deltaSeconds) || deltaSeconds < 0
        || !std::isfinite(speed) || speed <= 0 || speed > CameraSpeeds.back()
        || !std::isfinite(input.strafe) || !std::isfinite(input.forward)
        || !std::isfinite(input.up) || !std::isfinite(input.yaw)
        || !std::isfinite(input.pitch)) return false;
    if (deltaSeconds == 0) return true;

    constexpr double Pi = 3.14159265358979323846;
    constexpr double RadiansPerUnit = 2.0 * Pi / 65536.0;
    const double dt = std::min(deltaSeconds, CameraMaxDeltaSeconds);
    CameraPose next = pose;
    const double yawStep = std::clamp(static_cast<double>(input.yaw), -1.0, 1.0)
        * CameraLookUnitsPerSecond * dt;
    const double pitchStep = std::clamp(static_cast<double>(input.pitch), -1.0, 1.0)
        * CameraLookUnitsPerSecond * dt;
    next.yaw = signedCameraAngle(signedCameraAngle(pose.yaw) + static_cast<std::int32_t>(std::lround(yawStep)));
    next.pitch = std::clamp(signedCameraAngle(pose.pitch) + static_cast<std::int32_t>(std::lround(pitchStep)),
                            -CameraPitchLimit, CameraPitchLimit);
    next.roll = 0;

    const double yaw = next.yaw * RadiansPerUnit;
    const double pitch = next.pitch * RadiansPerUnit;
    const double forward = std::clamp(static_cast<double>(input.forward), -1.0, 1.0);
    const double strafe = std::clamp(static_cast<double>(input.strafe), -1.0, 1.0);
    const double up = std::clamp(static_cast<double>(input.up), -1.0, 1.0);
    double dx = std::cos(pitch) * std::cos(yaw) * forward - std::sin(yaw) * strafe;
    double dy = std::cos(pitch) * std::sin(yaw) * forward + std::cos(yaw) * strafe;
    double dz = std::sin(pitch) * forward + up;
    // Normalize the resulting world vector: world-up and tilted forward need
    // not be orthogonal, and no stick/trigger combination should exceed speed.
    const double length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length > 1.0) { dx /= length; dy /= length; dz /= length; }
    const double distance = speed * dt;
    next.x = static_cast<float>(pose.x + dx * distance);
    next.y = static_cast<float>(pose.y + dy * distance);
    next.z = static_cast<float>(pose.z + dz * distance);
    if (!validCameraPose(next)) return false;
    pose = next;
    return true;
}

} // namespace ce
