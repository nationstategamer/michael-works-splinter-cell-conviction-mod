// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include "exploration_policy.h"
#include "free_camera_math.h"
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
unsigned checks=0;
void require(bool condition,const char* message) {
    ++checks;if(!condition)throw std::runtime_error(message);
}
using ce::ExplorationAction;
using ce::ExplorationContext;
using ce::ExplorationOperation;
using ce::ExplorationPolicy;
using ce::ExplorationPolicyReason;
using ce::ExplorationPosition;
constexpr ExplorationPosition Destination{100,200,300};
constexpr ExplorationPosition Origin{10,20,30};
ExplorationContext context() {
    ExplorationContext result{};
    result.identity={7,0x1000,0x2000,0x3000};result.nowMs=1000;
    result.supported=true;result.focused=true;result.menuOpen=true;
    result.saveProtected=true;result.currentSafe=true;result.currentPosition=Origin;
    return result;
}
void released(ExplorationContext& value) {
    value.menuOpen=false;value.neutral=true;value.inputDraining=false;++value.nowMs;
}
ExplorationOperation peek(const ExplorationPolicy& policy) {
    ExplorationOperation result{};
    require(policy.pendingOperation(&result),"Pending operation unavailable");
    return result;
}
ExplorationOperation dispatch(ExplorationPolicy& policy,ExplorationContext& value) {
    auto pending=peek(policy);released(value);ExplorationOperation result{};
    require(policy.take(value,pending.token,true,&result),"Valid native dispatch rejected");
    return result;
}
void saveAnchor(ExplorationPolicy& policy,ExplorationContext& value) {
    value.menuOpen=true;value.neutral=true;value.inputDraining=false;
    require(policy.replaceReturnPoint(value),"Explicit safe return point rejected");
}
ExplorationPosition anchor(const ExplorationPolicy& policy) {
    ExplorationPosition result{};
    require(policy.returnPoint(&result),"Saved return point unavailable");return result;
}
void lifecycle() {
    ExplorationPolicy p;auto c=context();ExplorationOperation result{};
    require(!p.hasReturnPoint()&&!p.pending()&&!p.executing(),"Fresh state owns work");
    require(!p.returnPoint(nullptr)&&!p.pendingOperation(nullptr),"Null getters succeeded");
    require(!p.queueReturn(7,c)&&p.lastReason()==ExplorationPolicyReason::NoReturnPoint,"Invented return accepted");
    auto target=Destination;
    require(p.queueTeleport(target,7,c),"Confirmed camera target rejected");
    target={999,888,777};auto queued=peek(p);
    require(queued.target==Destination&&queued.identity==c.identity,"Camera target not copied immutably");
    require(queued.token!=0&&queued.action==ExplorationAction::Teleport,"Invalid queued operation");
    require(!p.queueTeleport(target,7,c)&&p.pending(),"Duplicate request replaced pending work");
    require(!p.take(c,queued.token,true,&result)&&p.pending(),"Menu-open move executed");
    c.menuOpen=false;
    require(!p.take(c,queued.token,true,&result)&&p.pending(),"Held Cross moved player");
    c.neutral=true;c.inputDraining=true;
    require(!p.take(c,queued.token,true,&result)&&p.pending(),"Input drain moved player");
    c.inputDraining=false;c.currentPosition={11,22,33};
    require(p.take(c,queued.token,true,&result)&&p.executing()&&!p.pending(),"Released move did not execute");
    require(result.target==Destination&&result.token==queued.token,"Dispatched target changed");
    require(anchor(p)==c.currentPosition,"Return point was not captured immediately before move");
    require(!p.take(c,queued.token,true,&result),"Move dispatched twice");
    c.menuOpen=true;
    require(!p.queueReturn(7,c),"In-flight move allowed a new operation");
    p.cancel();require(p.executing(),"Cancel released in-flight ownership");
    require(!p.complete(queued.token+1,true)&&p.executing(),"Wrong completion token ended move");
    require(p.complete(queued.token,true)&&!p.executing(),"Matching completion rejected");
    require(p.lastReason()==ExplorationPolicyReason::Completed,"Completion reason missing");
    require(p.hasReturnPoint(),"Successful move discarded return point");
    require(!p.complete(queued.token,true),"Completion accepted twice");
}
void queueGates() {
    for(unsigned gate=0;gate<6;++gate) {
        ExplorationPolicy p;auto c=context();
        switch(gate){case 0:c.supported=false;break;case 1:c.focused=false;break;
            case 2:c.menuOpen=false;break;case 3:c.saveProtected=false;break;
            case 4:c.transition=true;break;default:c.currentSafe=false;break;}
        require(!p.queueTeleport(Destination,7,c),"Teleport queue bypassed safety gate");
        require(!p.pending()&&!p.hasReturnPoint(),"Rejected queue captured state");
    }
    for(unsigned field=0;field<4;++field) {
        ExplorationPolicy p;auto c=context();
        switch(field){case 0:c.identity.generation=0;break;case 1:c.identity.controller=0;break;
            case 2:c.identity.pawn=0;break;default:c.identity.level=0;break;}
        require(!p.queueTeleport(Destination,7,c),"Missing identity accepted");
    }
    for(auto generation:{std::uint64_t(0),std::uint64_t(6),std::uint64_t(8)}) {
        ExplorationPolicy p;auto c=context();
        require(!p.queueTeleport(Destination,generation,c),"Stale UI epoch accepted");
        require(p.lastReason()==ExplorationPolicyReason::StaleGeneration,"Epoch refusal reason lost");
    }
    for(unsigned gate=0;gate<5;++gate) {
        ExplorationPolicy p;auto c=context();require(p.queueTeleport(Destination,7,c),"Changed gate fixture failed");
        auto op=peek(p);released(c);
        switch(gate){case 0:c.supported=false;break;case 1:c.focused=false;break;
            case 2:c.saveProtected=false;break;case 3:c.transition=true;break;
            default:c.currentSafe=false;break;}
        ExplorationOperation ignored{};
        require(!p.take(c,op.token,true,&ignored)&&!p.pending()&&!p.executing(),"Changed native safety gate bypassed");
        require(!p.hasReturnPoint(),"Changed safety gate captured an anchor");
    }
}
void coordinatesAndLanding() {
    static_assert(ce::ExplorationPositionLimit==ce::CameraPositionLimit,"Coordinate bounds must match camera");
    const float invalid[]={std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),-std::numeric_limits<float>::infinity(),
        std::nextafter(ce::ExplorationPositionLimit,std::numeric_limits<float>::infinity()),
        -std::nextafter(ce::ExplorationPositionLimit,std::numeric_limits<float>::infinity())};
    for(float value:invalid)for(unsigned axis=0;axis<3;++axis) {
        ExplorationPosition bad=Destination;
        if(axis==0)bad.x=value;else if(axis==1)bad.y=value;else bad.z=value;
        ExplorationPolicy p;auto c=context();
        require(!p.queueTeleport(bad,7,c)&&!p.pending(),"Non-finite/out-of-bounds camera accepted");
        c.currentPosition=bad;
        require(!p.queueTeleport(Destination,7,c),"Invalid current position accepted");
    }
    for(float limit:{ce::ExplorationPositionLimit,-ce::ExplorationPositionLimit}) {
        ExplorationPolicy p;auto c=context();
        require(p.queueTeleport({limit,limit,limit},7,c),"Finite bound rejected by pure policy");
        auto queued=peek(p);released(c);ExplorationOperation ignored{};
        require(!p.take(c,queued.token,false,&ignored),"Coordinates alone authorized landing");
        require(!p.pending()&&!p.hasReturnPoint()&&p.lastReason()==ExplorationPolicyReason::InvalidLanding,"Rejected landing captured an anchor or stayed pending");
    }
    ExplorationPolicy p;auto c=context();require(p.queueTeleport(Destination,7,c),"Landing fixture failed");
    auto op=peek(p);ExplorationOperation ignored{};
    require(!p.take(c,op.token,false,&ignored)&&p.pending(),"Unvalidated open-menu target consumed request");
    released(c);c.currentPosition.x=std::numeric_limits<float>::quiet_NaN();
    require(!p.take(c,op.token,true,&ignored)&&!p.pending()&&!p.hasReturnPoint(),"Late-invalid current position moved/captured");
}
void anchorsAndFailure() {
    ExplorationPolicy p;auto c=context();
    require(p.queueTeleport(Destination,7,c),"Failure fixture queue failed");
    auto first=dispatch(p,c);require(p.complete(first.token,false),"Failed native result not completed");
    require(p.lastReason()==ExplorationPolicyReason::NativeFailure&&anchor(p)==Origin,"Native failure lost safe recovery anchor");
    c.menuOpen=true;c.currentPosition={40,50,60};
    require(p.queueTeleport({400,500,600},7,c),"Second outbound queue failed");
    auto second=dispatch(p,c);require(second.token!=first.token,"Request token reused");
    require(anchor(p)==Origin,"Second outbound overwrote original return point");
    require(p.complete(second.token,true),"Second outbound completion failed");
    c.menuOpen=true;
    require(p.queueReturn(7,c),"Saved return rejected");auto back=peek(p);
    require(back.action==ExplorationAction::Return&&back.target==Origin,"Return destination was not saved point");
    released(c);ExplorationOperation result{};
    require(!p.take(c,back.token,false,&result)&&!p.pending(),"Blocked return landing was accepted");
    require(anchor(p)==Origin,"Blocked return discarded safe point");
    c.menuOpen=true;
    require(p.queueReturn(7,c),"Return retry rejected");back=dispatch(p,c);
    require(p.complete(back.token,false)&&anchor(p)==Origin,"Failed native return lost safe point");
    c.menuOpen=true;require(p.queueReturn(7,c),"Final return retry rejected");
    back=dispatch(p,c);require(p.complete(back.token,true)&&anchor(p)==Origin,"Return success discarded anchor");
    c.menuOpen=true;c.neutral=true;c.currentPosition={70,80,90};
    require(p.replaceReturnPoint(c)&&anchor(p)==c.currentPosition,"Explicit safe replacement failed");
    require(p.queueTeleport(Destination,7,c),"Explicit anchor outbound queue failed");
    auto custom=dispatch(p,c);require(anchor(p)==ExplorationPosition{70,80,90},"Automatic capture overwrote explicit anchor");
    require(p.complete(custom.token,true),"Explicit anchor move completion failed");
}
void replacementGates() {
    for(unsigned gate=0;gate<8;++gate) {
        ExplorationPolicy p;auto c=context();saveAnchor(p,c);
        c.currentPosition={1000,2000,3000};
        switch(gate){case 0:c.supported=false;break;case 1:c.focused=false;break;
            case 2:c.saveProtected=false;break;case 3:c.transition=true;break;
            case 4:c.currentSafe=false;break;case 5:c.menuOpen=false;break;
            case 6:c.neutral=false;break;default:c.inputDraining=true;break;}
        require(!p.replaceReturnPoint(c),"Explicit replacement bypassed safety gate");
        if(gate!=0&&gate!=3)require(anchor(p)==Origin,"Failed replacement changed valid anchor");
        else require(!p.hasReturnPoint(),"Unsupported transition retained anchor");
    }
    ExplorationPolicy p;auto c=context();saveAnchor(p,c);
    require(p.queueTeleport(Destination,7,c),"Busy replacement fixture failed");
    c.currentPosition={1,2,3};require(!p.replaceReturnPoint(c)&&anchor(p)==Origin,"Pending request allowed replacement");
    auto op=dispatch(p,c);c.menuOpen=true;c.neutral=true;
    require(!p.replaceReturnPoint(c)&&anchor(p)==Origin,"In-flight move allowed replacement");
    require(p.complete(op.token,true),"Replacement fixture completion failed");
}
void identityAndReentrancy() {
    for(unsigned field=0;field<4;++field) {
        ExplorationPolicy p;auto c=context();saveAnchor(p,c);
        require(p.queueTeleport(Destination,7,c),"Identity fixture queue failed");
        switch(field){case 0:++c.identity.generation;break;case 1:++c.identity.controller;break;
            case 2:++c.identity.pawn;break;default:++c.identity.level;break;}
        p.update(c);
        require(!p.pending()&&!p.hasReturnPoint(),"Scene/checkpoint/pawn change retained old state");
        require(!p.queueReturn(c.identity.generation,c),"Old point returned across identity");
    }
    ExplorationPolicy p;auto c=context();require(p.queueTeleport(Destination,7,c),"Reentrant fixture failed");
    auto op=dispatch(p,c);++c.identity.generation;c.currentPosition={400,500,600};p.update(c);
    require(p.executing()&&!p.hasReturnPoint(),"Reentrant identity change released native call or kept old anchor");
    c.menuOpen=true;
    require(!p.queueTeleport(Destination,c.identity.generation,c),"Reentrant observation queued overlapping native move");
    require(!p.complete(op.token,true)&&!p.executing(),"Old native result revived invalidated scope");
    require(p.lastReason()==ExplorationPolicyReason::InFlightInvalidated&&!p.hasReturnPoint(),"Invalidated completion revived anchor");
    require(p.queueTeleport(Destination,c.identity.generation,c),"New scene did not recover after old native call returned");
    auto fresh=dispatch(p,c);require(anchor(p)==ExplorationPosition{400,500,600},"New scene captured stale return point");
    require(p.complete(fresh.token,true),"New scene completion failed");
    for(unsigned gate=0;gate<2;++gate) {
        ExplorationPolicy q;auto d=context();require(q.queueTeleport(Destination,7,d),"Focus fixture failed");
        auto moving=dispatch(q,d);if(!gate)d.focused=false;else d.saveProtected=false;q.update(d);
        require(q.executing()&&q.hasReturnPoint(),"Temporary safety loss discarded safe anchor or native ownership");
        require(!q.complete(moving.token,true)&&q.hasReturnPoint(),"Temporary safety loss accepted stale native result");
        d.focused=true;d.saveProtected=true;d.menuOpen=true;
        require(q.queueReturn(7,d),"Recovered same scene could not use safe point");
    }
}
void expiryValidationAndCancel() {
    for(auto elapsed:{ExplorationPolicy::lifetimeMs,ExplorationPolicy::lifetimeMs+1}) {
        ExplorationPolicy p;auto c=context();saveAnchor(p,c);
        require(p.queueTeleport(Destination,7,c),"Expiry fixture failed");auto op=peek(p);
        c.nowMs+=elapsed;c.menuOpen=false;c.neutral=true;ExplorationOperation result{};
        require(p.take(c,op.token,true,&result)==(elapsed==ExplorationPolicy::lifetimeMs),"TTL boundary incorrect");
        require(p.hasReturnPoint(),"Expiry discarded existing anchor");
    }
    for(auto timestamp:{std::uint64_t(0),std::uint64_t(999)}) {
        ExplorationPolicy p;auto c=context();require(p.queueTeleport(Destination,7,c),"Clock fixture failed");
        auto op=peek(p);c.nowMs=timestamp;c.menuOpen=false;c.neutral=true;ExplorationOperation result{};
        require(!p.take(c,op.token,true,&result)&&!p.pending(),"Backward clock authorized move");
        require(p.lastReason()==ExplorationPolicyReason::ClockChanged,"Clock refusal reason lost");
    }
    ExplorationPolicy wrap;auto c=context();c.nowMs=std::numeric_limits<std::uint64_t>::max()-2;
    require(wrap.queueTeleport(Destination,7,c),"Wrap fixture failed");auto op=peek(wrap);
    c.nowMs=1;c.menuOpen=false;c.neutral=true;ExplorationOperation result{};
    require(!wrap.take(c,op.token,true,&result)&&!wrap.pending(),"Wrapped clock authorized move");
    ExplorationPolicy p;c=context();require(p.queueTeleport(Destination,7,c),"Token fixture failed");
    auto old=peek(p);p.cancel();require(!p.pending()&&!p.hasReturnPoint(),"Cancel captured an anchor");
    require(p.queueTeleport({9,8,7},7,c),"Replacement queue failed");auto next=peek(p);released(c);
    require(!p.take(c,old.token,true,&result)&&p.pending()&&!p.hasReturnPoint(),"Stale native landing query authorized/replaced new request");
    require(p.lastReason()==ExplorationPolicyReason::StaleValidation,"Stale landing token reason lost");
    require(!p.take(c,next.token,true,nullptr)&&p.pending(),"Missing output consumed request");
    require(p.take(c,next.token,true,&result)&&result.target==ExplorationPosition{9,8,7},"Current validated token failed");
    c.nowMs+=ExplorationPolicy::lifetimeMs+100;p.update(c);
    require(p.executing()&&p.complete(result.token,true),"Pending TTL incorrectly expired in-flight native call");
    c.menuOpen=true;require(p.queueReturn(7,c),"Focus cancel fixture failed");c.focused=false;p.update(c);
    require(!p.pending()&&p.hasReturnPoint(),"Focus loss retained pending move or lost anchor");
}
}
int main() {try {
    lifecycle();queueGates();coordinatesAndLanding();anchorsAndFailure();
    replacementGates();identityAndReentrancy();expiryValidationAndCancel();
    std::cout<<"PASS: "<<checks<<" exploration policy checks; protected immutable requests, native landing gate, input drain, safe anchors, retries and scene invalidation.\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}}
