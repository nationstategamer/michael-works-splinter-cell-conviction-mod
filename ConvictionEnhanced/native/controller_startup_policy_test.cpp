#include "controller_startup_policy.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using ce::ControllerStartupDecision;
using ce::ControllerStartupIdentity;
using ce::ControllerStartupPolicy;
using ce::ControllerStartupSnapshot;
int checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
ControllerStartupSnapshot ready(std::uint64_t time=1000) {
    ControllerStartupSnapshot sample{};
    sample.nowMs=time;sample.identity={1,2,3,4};
    sample.supported=sample.sessionSolo=sample.onGameThread=sample.foreground=true;
    sample.profileLoaded=sample.savedUseController=sample.rawPadReady=sample.nativePadReady=true;
    return sample;
}
void decision(ControllerStartupPolicy& policy,const ControllerStartupSnapshot& sample,
    ControllerStartupDecision expected,const char* message) {
    require(policy.observe(sample)==expected,message);
}

void stableRepairAndOneAttempt() {
    ControllerStartupPolicy policy;
    auto sample=ready(0);
    require(!policy.completed()&&!policy.attempted(),"New policy started completed");
    require(!policy.markAttempt(),"Unobserved policy allowed a native attempt");
    decision(policy,sample,ControllerStartupDecision::None,"First observation authorized repair");
    sample.nowMs=249;
    decision(policy,sample,ControllerStartupDecision::None,"Stability interval ended too early");
    sample.nowMs=250;
    decision(policy,sample,ControllerStartupDecision::Repair,"Stable disabled runtime did not authorize repair");
    require(!policy.completed()&&!policy.attempted(),"Offering repair consumed the attempt before the caller marked it");
    require(policy.markAttempt(),"Eligible repair could not mark its native attempt");
    require(policy.completed()&&policy.attempted(),"Attempt was not permanently completed before native work");
    require(!policy.markAttempt(),"A second native attempt was authorized");
    // Simulate native failure: both runtime flags remain off. No retry occurs.
    sample.nowMs=5000;
    decision(policy,sample,ControllerStartupDecision::None,"Failed native activation caused a retry");
    sample.identity={5,6,7,8};sample.nowMs=6000;
    decision(policy,sample,ControllerStartupDecision::None,"New identity bypassed the process-wide attempt limit");
    sample.nowMs=7000;
    decision(policy,sample,ControllerStartupDecision::None,"A later identity retried the completed operation");
}

void alreadyEnabledAndUserChoice() {
    ControllerStartupPolicy policy;
    auto sample=ready();sample.controllerEnabled=sample.viewportEnabled=true;
    decision(policy,sample,ControllerStartupDecision::None,"Already-enabled status bypassed stability");
    sample.nowMs+=250;
    decision(policy,sample,ControllerStartupDecision::AlreadyEnabled,"Stable enabled state was not completed");
    require(policy.completed()&&!policy.attempted(),"Already-enabled state counted as a native attempt");
    require(!policy.markAttempt(),"Already-enabled completion allowed native activation");
    // Later user changes must never be undone by a startup-only policy.
    sample.savedUseController=false;sample.controllerEnabled=sample.viewportEnabled=false;sample.nowMs+=500;
    decision(policy,sample,ControllerStartupDecision::None,"User disabling controller restarted a completed startup check");
    sample.savedUseController=true;sample.nowMs+=500;
    decision(policy,sample,ControllerStartupDecision::None,"Re-enabling the option restarted a completed startup check");

    ControllerStartupPolicy disabled;
    sample=ready();sample.savedUseController=false;
    decision(disabled,sample,ControllerStartupDecision::None,"Saved disabled preference was overridden");
    sample.nowMs+=5000;
    decision(disabled,sample,ControllerStartupDecision::None,"Waiting overrode the disabled preference");
    require(!disabled.completed()&&!disabled.markAttempt(),"Disabled preference marked or offered an attempt");
    sample.savedUseController=true;sample.nowMs+=1;
    decision(disabled,sample,ControllerStartupDecision::None,"User enabling option inherited disabled waiting time");
    sample.nowMs+=250;
    decision(disabled,sample,ControllerStartupDecision::Repair,"Later saved enabled choice could not become eligible");
    sample.savedUseController=false;
    decision(disabled,sample,ControllerStartupDecision::None,"Disabling after an offer retained the offer");
    require(!disabled.markAttempt(),"Stale offer survived the user disabling the option");
}

void everyReadinessGate() {
    bool ControllerStartupSnapshot::* gates[]={
        &ControllerStartupSnapshot::supported,&ControllerStartupSnapshot::sessionSolo,
        &ControllerStartupSnapshot::onGameThread,&ControllerStartupSnapshot::foreground,
        &ControllerStartupSnapshot::profileLoaded,&ControllerStartupSnapshot::savedUseController,
        &ControllerStartupSnapshot::rawPadReady,&ControllerStartupSnapshot::nativePadReady
    };
    for(const auto gate:gates) {
        ControllerStartupPolicy policy;
        auto sample=ready();
        decision(policy,sample,ControllerStartupDecision::None,"Gate test initialization failed");
        sample.nowMs+=249;sample.*gate=false;
        decision(policy,sample,ControllerStartupDecision::None,"A failed readiness gate allowed repair");
        sample.nowMs+=1000;
        decision(policy,sample,ControllerStartupDecision::None,"A prolonged failed gate allowed repair");
        require(!policy.markAttempt(),"Failed gate left a native attempt authorized");
        sample.*gate=true;sample.nowMs+=1;
        decision(policy,sample,ControllerStartupDecision::None,"Restored gate inherited old stability");
        sample.nowMs+=249;
        decision(policy,sample,ControllerStartupDecision::None,"Restored gate stabilized too soon");
        sample.nowMs+=1;
        decision(policy,sample,ControllerStartupDecision::Repair,"Restored valid gate never became eligible");
    }
}

void identityAndPartialActivation() {
    std::uintptr_t ControllerStartupIdentity::* fields[]={&ControllerStartupIdentity::engine,
        &ControllerStartupIdentity::viewport,&ControllerStartupIdentity::controller,&ControllerStartupIdentity::profile};
    for(const auto field:fields) {
        ControllerStartupPolicy policy;
        auto sample=ready();
        decision(policy,sample,ControllerStartupDecision::None,"Identity test initialization failed");
        sample.nowMs+=250;sample.identity.*field+=100;
        decision(policy,sample,ControllerStartupDecision::None,"Changed identity inherited stable authorization");
        sample.nowMs+=250;
        decision(policy,sample,ControllerStartupDecision::Repair,"New stable identity was rejected");
        sample.identity.*field=0;
        decision(policy,sample,ControllerStartupDecision::None,"Null identity retained a repair offer");
        require(!policy.markAttempt(),"Null identity allowed a stale native attempt");
    }
    for(int direction=0;direction<2;++direction) {
        ControllerStartupPolicy policy;
        auto sample=ready();
        decision(policy,sample,ControllerStartupDecision::None,"Partial-state test initialization failed");
        sample.controllerEnabled=direction==0;sample.viewportEnabled=direction!=0;sample.nowMs+=500;
        decision(policy,sample,ControllerStartupDecision::None,"Mixed runtime flags forced native activation");
        sample.nowMs+=500;
        decision(policy,sample,ControllerStartupDecision::None,"Prolonged partial activation forced repair");
        require(!policy.markAttempt(),"Partial runtime state allowed a native attempt");
        sample.controllerEnabled=sample.viewportEnabled=false;sample.nowMs+=1;
        decision(policy,sample,ControllerStartupDecision::None,"Settled flags inherited partial-state stability");
        sample.nowMs+=250;
        decision(policy,sample,ControllerStartupDecision::Repair,"Settled stable disabled runtime never became eligible");
    }
}

void backwardAndWrappedClock() {
    ControllerStartupPolicy backward;
    auto sample=ready(1000);
    decision(backward,sample,ControllerStartupDecision::None,"Backward-clock test initialization failed");
    sample.nowMs=1200;
    decision(backward,sample,ControllerStartupDecision::None,"Clock test stabilized early");
    sample.nowMs=1100;
    decision(backward,sample,ControllerStartupDecision::None,"Backward time authorized repair");
    sample.nowMs=1349;
    decision(backward,sample,ControllerStartupDecision::None,"Backward time failed to restart stability");
    sample.nowMs=1350;
    decision(backward,sample,ControllerStartupDecision::Repair,"Stable time after reset was rejected");

    ControllerStartupPolicy wrapped;
    sample=ready(std::numeric_limits<std::uint64_t>::max()-100);
    decision(wrapped,sample,ControllerStartupDecision::None,"Wrap test initialization failed");
    sample.nowMs=10;
    decision(wrapped,sample,ControllerStartupDecision::None,"Wrapped subtraction authorized repair");
    sample.nowMs=259;
    decision(wrapped,sample,ControllerStartupDecision::None,"Wrapped clock retained old stability");
    sample.nowMs=260;
    decision(wrapped,sample,ControllerStartupDecision::Repair,"Fresh interval after wrap was rejected");
}

void consistentPairTransitionsRestartStability() {
    for(int direction=0;direction<2;++direction) {
        ControllerStartupPolicy policy;
        auto sample=ready(1000);
        sample.controllerEnabled=sample.viewportEnabled=direction==0;
        decision(policy,sample,ControllerStartupDecision::None,"Pair-transition test initialization failed");
        sample.nowMs=1249;
        sample.controllerEnabled=sample.viewportEnabled=direction!=0;
        decision(policy,sample,ControllerStartupDecision::None,"Changed consistent pair inherited waiting time");
        sample.nowMs=1250;
        decision(policy,sample,ControllerStartupDecision::None,"Pair changed at 249 ms authorized action at 250 ms");
        require(!policy.markAttempt(),"Changed pair prematurely authorized a native attempt");
        sample.nowMs=1498;
        decision(policy,sample,ControllerStartupDecision::None,"Changed pair stabilized before its own interval");
        sample.nowMs=1499;
        decision(policy,sample,direction==0?ControllerStartupDecision::Repair:ControllerStartupDecision::AlreadyEnabled,
            "Stable changed pair did not reach its correct decision");
    }
}
}

int main() {
    try {
        stableRepairAndOneAttempt();alreadyEnabledAndUserChoice();everyReadinessGate();
        identityAndPartialActivation();backwardAndWrappedClock();
        consistentPairTransitionsRestartStability();
        std::cout<<"Controller startup policy: "<<checks<<" checks passed.\n";return 0;
    } catch(const std::exception& error) {
        std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<"\n";return 1;
    }
}
