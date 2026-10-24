#include "free_camera_math.h"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
int checks = 0;
void require(bool result, const char* message) {
    ++checks;
    if (!result) throw std::runtime_error(message);
}
bool near(double a, double b, double tolerance = 0.0002) { return std::fabs(a - b) <= tolerance; }
double length(const ce::CameraPose& p) { return std::sqrt(p.x*p.x+p.y*p.y+p.z*p.z); }
bool same(const ce::CameraPose& a, const ce::CameraPose& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z
        && a.pitch == b.pitch && a.yaw == b.yaw && a.roll == b.roll;
}

void stickAndTriggerInput() {
    auto v = ce::normalizeStick(0, 0, 7849);
    require(v.x == 0 && v.y == 0, "Neutral stick moved");
    v = ce::normalizeStick(7849, 0, 7849);
    require(v.x == 0 && v.y == 0, "Deadzone edge moved");
    v = ce::normalizeStick(7850, 0, 7849);
    require(v.x > 0 && v.x < 0.0001 && v.y == 0, "Deadzone was not remapped continuously");
    v = ce::normalizeStick(32767, 0, 7849);
    require(near(v.x, 1) && v.y == 0, "Positive stick endpoint was not full strength");
    v = ce::normalizeStick(-32768, 0, 7849);
    require(near(v.x, -1) && v.y == 0, "Negative stick endpoint was asymmetric");
    v = ce::normalizeStick(32767, 32767, 8689);
    require(near(std::hypot(v.x,v.y), 1) && near(v.x,v.y), "Diagonal stick exceeded unit length");
    v = ce::normalizeStick(6000, 6000, 7849);
    require(v.x > 0 && v.y > 0, "Radial deadzone incorrectly used per-axis thresholds");
    v = ce::normalizeStick(-32768, 32767, 40000);
    require(v.x == 0 && v.y == 0, "Invalid maximum deadzone produced movement");
    require(ce::normalizedTrigger(30) == 0 && ce::normalizedTrigger(0) == 0, "Trigger noise was not filtered");
    require(ce::normalizedTrigger(31) > 0 && ce::normalizedTrigger(31) < 0.005f, "Trigger threshold discontinuity");
    require(ce::normalizedTrigger(255) == 1, "Trigger endpoint was not full strength");
    require(ce::normalizedTrigger(255,255) == 0, "Invalid trigger threshold divided by zero");
}

void worldDirectionsAndSpeed() {
    ce::CameraPose pose{};
    ce::CameraMotion motion{};
    motion.forward = 1;
    require(ce::applyCameraMotion(pose,motion,.05,100), "Forward movement rejected");
    require(near(pose.x,5) && near(pose.y,0) && near(pose.z,0), "Yaw zero did not face +X");
    pose = {}; pose.yaw = 16384;
    ce::applyCameraMotion(pose,motion,.05,100);
    require(near(pose.x,0) && near(pose.y,5), "Quarter-turn yaw did not face +Y");
    pose = {}; motion = {}; motion.strafe = 1;
    ce::applyCameraMotion(pose,motion,.05,100);
    require(near(pose.x,0) && near(pose.y,5), "Right strafe at yaw zero had wrong direction");
    pose = {}; pose.pitch = 8192; motion = {}; motion.forward = 1;
    ce::applyCameraMotion(pose,motion,.05,100);
    require(near(pose.x,pose.z) && pose.z > 3 && near(length(pose),5), "Forward movement did not follow pitched view");
    pose = {}; pose.pitch = 8192; motion = {}; motion.up = 1;
    ce::applyCameraMotion(pose,motion,.05,100);
    require(near(pose.x,0) && near(pose.y,0) && near(pose.z,5), "Trigger lift was not world vertical");
    pose = {}; motion = {}; motion.forward = 1; motion.strafe = 1; motion.up = 1;
    ce::applyCameraMotion(pose,motion,.05,900);
    require(near(length(pose),45), "Combined movement exceeded selected speed");
    pose = {}; pose.pitch = 8192; motion.strafe = 0;
    ce::applyCameraMotion(pose,motion,.05,900);
    require(near(length(pose),45), "Tilted forward plus world-up exceeded selected speed");
    pose = {}; motion = {}; motion.forward = .5f;
    ce::applyCameraMotion(pose,motion,.05,300);
    require(near(pose.x,7.5), "Partial stick lost proportional speed");
    ce::CameraPose subdivided{};
    for(int i=0;i<5;++i) ce::applyCameraMotion(subdivided,motion,.01,300);
    require(near(subdivided.x,pose.x), "Translation depended on update frequency");
}

void anglesAndTiming() {
    ce::CameraPose pose{}; pose.yaw = 32760; pose.roll = 400;
    ce::CameraMotion motion{}; motion.yaw = 1;
    ce::applyCameraMotion(pose,motion,.05);
    require(pose.yaw < 0 && pose.yaw > -32768, "Positive yaw did not wrap at half-turn");
    require(pose.roll == 0, "Free camera retained roll");
    pose = {}; pose.yaw = -32760; motion.yaw = -1;
    ce::applyCameraMotion(pose,motion,.05);
    require(pose.yaw > 0 && pose.yaw < 32768, "Negative yaw did not wrap");
    require(ce::signedCameraAngle(std::numeric_limits<std::int32_t>::max()) == -1,
        "Large engine rotator was not normalized");
    pose = {}; motion = {}; motion.pitch = 1;
    for(int i=0;i<100;++i) ce::applyCameraMotion(pose,motion,.05);
    require(pose.pitch == ce::CameraPitchLimit, "Pitch did not stop below vertical");
    motion.pitch = -1;
    for(int i=0;i<100;++i) ce::applyCameraMotion(pose,motion,.05);
    require(pose.pitch == -ce::CameraPitchLimit, "Downward pitch limit failed");
    pose = {}; motion = {}; motion.forward = 1;
    ce::applyCameraMotion(pose,motion,30,300);
    require(near(pose.x,15), "Long stalled frame caused a camera jump");
    ce::CameraPose before = pose;
    require(ce::applyCameraMotion(pose,motion,0) && same(pose,before), "Zero delta changed the camera");
    require(!ce::applyCameraMotion(pose,motion,-1) && same(pose,before), "Negative delta changed the camera");
    require(!ce::applyCameraMotion(pose,motion,std::numeric_limits<double>::infinity()) && same(pose,before),
        "Infinite delta changed the camera");
    require(!ce::applyCameraMotion(pose,motion,std::numeric_limits<double>::quiet_NaN()) && same(pose,before),
        "NaN delta changed the camera");
}

void invalidInputIsAtomic() {
    ce::CameraPose pose{10,20,30,100,200,300}, before = pose;
    ce::CameraMotion motion{}; motion.forward = std::numeric_limits<float>::quiet_NaN();
    require(!ce::applyCameraMotion(pose,motion,.05) && same(pose,before), "NaN input partially committed pose");
    motion.forward = 1;
    require(!ce::applyCameraMotion(pose,motion,.05,0) && same(pose,before), "Zero speed changed pose");
    require(!ce::applyCameraMotion(pose,motion,.05,901) && same(pose,before), "Unbounded speed changed pose");
    require(!ce::applyCameraMotion(pose,motion,.05,std::numeric_limits<float>::infinity()) && same(pose,before),
        "Infinite speed changed pose");
    pose = {}; pose.x = ce::CameraPositionLimit; before = pose;
    require(!ce::applyCameraMotion(pose,motion,.05) && same(pose,before), "Out-of-range move partially committed pose");
    pose = {}; pose.z = std::numeric_limits<float>::infinity();
    require(!ce::validCameraPose(pose), "Invalid engine pose was accepted");
}
}

int main() {
    try {
        stickAndTriggerInput(); worldDirectionsAndSpeed(); anglesAndTiming(); invalidInputIsAtomic();
        std::cout << "Free camera math: " << checks << " checks passed.\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << "\n";
        return 1;
    }
}
