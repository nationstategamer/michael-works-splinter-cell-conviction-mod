#pragma once
#include <cstdint>

namespace ce {
struct ControllerStartupIdentity {
    std::uintptr_t engine=0,viewport=0,controller=0,profile=0;
    bool operator==(const ControllerStartupIdentity& other) const noexcept {
        return engine==other.engine&&viewport==other.viewport
            &&controller==other.controller&&profile==other.profile;
    }
    bool valid() const noexcept {return engine&&viewport&&controller&&profile;}
};

struct ControllerStartupSnapshot {
    std::uint64_t nowMs=0;
    ControllerStartupIdentity identity{};
    bool supported=false,sessionSolo=false,onGameThread=false,foreground=false;
    bool profileLoaded=false,savedUseController=false,rawPadReady=false,nativePadReady=false;
    bool controllerEnabled=false,viewportEnabled=false;
};

enum class ControllerStartupDecision { None, AlreadyEnabled, Repair };

// Own one instance for the process. The caller serializes access and performs
// all native work; this policy only decides when a single attempt is eligible.
class ControllerStartupPolicy {
public:
    static constexpr std::uint64_t stabilityMs=250;

    ControllerStartupDecision observe(const ControllerStartupSnapshot& sample) noexcept {
        repairOffered_=false;
        if(completed_)return ControllerStartupDecision::None;
        const bool eligible=sample.supported&&sample.sessionSolo&&sample.onGameThread
            &&sample.foreground&&sample.profileLoaded&&sample.savedUseController
            &&sample.rawPadReady&&sample.nativePadReady&&sample.identity.valid();
        // Partial native activation may be in progress. Wait for a consistent
        // pair, then start a fresh stability interval without forcing either bit.
        if(!eligible||sample.controllerEnabled!=sample.viewportEnabled) {
            waiting_=false;
            return ControllerStartupDecision::None;
        }
        // An identity transition or backward/wrapped clock cannot inherit time
        // from an earlier eligible object. Tick zero is a valid starting point.
        if(!waiting_||!(sample.identity==identity_)||sample.controllerEnabled!=stableEnabled_
            ||sample.nowMs<lastObservedMs_) {
            identity_=sample.identity;
            stableEnabled_=sample.controllerEnabled;
            stableSinceMs_=sample.nowMs;
            lastObservedMs_=sample.nowMs;
            waiting_=true;
            return ControllerStartupDecision::None;
        }
        lastObservedMs_=sample.nowMs;
        if(sample.nowMs-stableSinceMs_<stabilityMs)return ControllerStartupDecision::None;
        if(sample.controllerEnabled) {
            completed_=true;
            return ControllerStartupDecision::AlreadyEnabled;
        }
        repairOffered_=true;
        return ControllerStartupDecision::Repair;
    }

    // Call immediately before the native apply operation. Completion precedes
    // that call, so even a failed native operation cannot cause repeated writes.
    bool markAttempt() noexcept {
        if(completed_||!repairOffered_)return false;
        repairOffered_=false;
        attempted_=true;
        completed_=true;
        return true;
    }
    bool completed() const noexcept {return completed_;}
    bool attempted() const noexcept {return attempted_;}

private:
    ControllerStartupIdentity identity_{};
    std::uint64_t stableSinceMs_=0,lastObservedMs_=0;
    bool waiting_=false,stableEnabled_=false,repairOffered_=false,completed_=false,attempted_=false;
};
}
