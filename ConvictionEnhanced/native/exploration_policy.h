#pragma once
// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include <cmath>
#include <cstdint>

namespace ce {
struct ExplorationPosition {
    float x=0,y=0,z=0;
    bool operator==(const ExplorationPosition& other) const noexcept {
        return x==other.x&&y==other.y&&z==other.z;
    }
};
inline constexpr float ExplorationPositionLimit=10000000.0f;
inline bool validExplorationPosition(const ExplorationPosition& point) noexcept {
    return std::isfinite(point.x)&&std::isfinite(point.y)&&std::isfinite(point.z)
        &&std::fabs(point.x)<=ExplorationPositionLimit
        &&std::fabs(point.y)<=ExplorationPositionLimit
        &&std::fabs(point.z)<=ExplorationPositionLimit;
}
struct ExplorationIdentity {
    // generation is the backend's scene/checkpoint-state epoch, not a frame
    // count. Checkpoint/part loads and relevant script-state changes advance it.
    std::uint64_t generation=0;
    std::uintptr_t controller=0,pawn=0,level=0;
    bool valid() const noexcept {return generation&&controller&&pawn&&level;}
    bool operator==(const ExplorationIdentity& other) const noexcept {
        return generation==other.generation&&controller==other.controller
            &&pawn==other.pawn&&level==other.level;
    }
};
struct ExplorationContext {
    ExplorationIdentity identity{};
    std::uint64_t nowMs=0;
    bool supported=false,focused=false,menuOpen=false,neutral=false;
    bool inputDraining=false,saveProtected=false,transition=false,currentSafe=false;
    ExplorationPosition currentPosition{};
};
enum class ExplorationAction { Teleport, Return };
struct ExplorationOperation {
    ExplorationAction action=ExplorationAction::Teleport;
    std::uint64_t token=0;
    ExplorationIdentity identity{};
    ExplorationPosition target{};
};
enum class ExplorationPolicyReason {
    None,Queued,WaitingForInput,Busy,Unavailable,Unfocused,SaveProtection,
    Transition,StaleGeneration,IdentityChanged,InvalidCoordinate,UnsafeCurrent,
    NoReturnPoint,InvalidLanding,Expired,ClockChanged,Cancelled,
    StaleValidation,MissingOutput,InFlightInvalidated,StaleCompletion,
    NativeFailure,Completed,ReturnSaved
};

// Pure state only: no native calls, IO, controller polling or locks. The caller
// serializes every access. Coordinate validation never proves a landing safe:
// the native backend must freshly validate the exact pending target and token.
class ExplorationPolicy {
public:
    static constexpr std::uint64_t lifetimeMs=5000;

    void update(const ExplorationContext& context) noexcept {
        if(!context.supported||context.transition||!context.identity.valid()) {
            invalidate(context.transition?ExplorationPolicyReason::Transition
                                        :ExplorationPolicyReason::Unavailable);
            return;
        }
        if(!bound_||!(context.identity==identity_)) {
            const bool changed=bound_;
            invalidate(changed?ExplorationPolicyReason::IdentityChanged
                              :ExplorationPolicyReason::None);
            identity_=context.identity;bound_=true;
        }
        if(!context.focused||!context.saveProtected) {
            const auto why=!context.focused?ExplorationPolicyReason::Unfocused
                                          :ExplorationPolicyReason::SaveProtection;
            if(pending_){pending_=false;reason_=why;}
            if(executing_)executionValid_=false;
            return;
        }
        if(pending_&&(context.nowMs<queuedMs_||context.nowMs-queuedMs_>lifetimeMs)) {
            reason_=context.nowMs<queuedMs_?ExplorationPolicyReason::ClockChanged
                                         :ExplorationPolicyReason::Expired;
            pending_=false;
        }
    }

    bool queueTeleport(const ExplorationPosition& target,std::uint64_t uiGeneration,
        const ExplorationContext& context) noexcept {
        update(context);
        if(!canQueue(uiGeneration,context))return false;
        if(!validExplorationPosition(target)){reason_=ExplorationPolicyReason::InvalidCoordinate;return false;}
        return queue(ExplorationAction::Teleport,target,context);
    }
    bool queueReturn(std::uint64_t uiGeneration,const ExplorationContext& context) noexcept {
        update(context);
        if(!canQueue(uiGeneration,context))return false;
        if(!anchorValid_){reason_=ExplorationPolicyReason::NoReturnPoint;return false;}
        return queue(ExplorationAction::Return,anchor_,context);
    }
    bool pendingOperation(ExplorationOperation* operation) const noexcept {
        if(!pending_||!operation)return false;
        *operation=queued_;return true;
    }

    bool take(const ExplorationContext& context,std::uint64_t validatedToken,
        bool landingValidated,ExplorationOperation* operation) noexcept {
        update(context);
        if(!pending_)return false;
        if(!operation){reason_=ExplorationPolicyReason::MissingOutput;return false;}
        if(validatedToken!=queued_.token){reason_=ExplorationPolicyReason::StaleValidation;return false;}
        if(!safe(context)) {pending_=false;return false;}
        if(!(queued_.identity==context.identity)) {
            pending_=false;reason_=ExplorationPolicyReason::IdentityChanged;return false;
        }
        if(context.menuOpen||!context.neutral||context.inputDraining) {
            reason_=ExplorationPolicyReason::WaitingForInput;return false;
        }
        if(!context.currentSafe) {
            pending_=false;reason_=ExplorationPolicyReason::UnsafeCurrent;return false;
        }
        if(!validExplorationPosition(context.currentPosition)
            ||!validExplorationPosition(queued_.target)) {
            pending_=false;reason_=ExplorationPolicyReason::InvalidCoordinate;return false;
        }
        if(!landingValidated) {
            pending_=false;reason_=ExplorationPolicyReason::InvalidLanding;return false;
        }
        // Capture the current, freshly native-validated pre-move position once.
        // It survives a rejected native move and later outbound moves/returns.
        if(queued_.action==ExplorationAction::Teleport&&!anchorValid_) {
            anchor_=context.currentPosition;anchorValid_=true;
        }
        if(queued_.action==ExplorationAction::Return&&!anchorValid_) {
            pending_=false;reason_=ExplorationPolicyReason::NoReturnPoint;return false;
        }
        *operation=queued_;inFlight_=queued_;pending_=false;
        executing_=true;executionValid_=true;reason_=ExplorationPolicyReason::None;
        return true;
    }

    // A native call can reenter observation. If identity/focus/protection changed,
    // update that fresh context before completion; an old result cannot revive
    // an invalidated return point. Busy remains held until this matching reply.
    bool complete(std::uint64_t token,bool success) noexcept {
        if(!executing_||token!=inFlight_.token) {
            reason_=ExplorationPolicyReason::StaleCompletion;return false;
        }
        const bool current=executionValid_&&bound_&&(inFlight_.identity==identity_);
        executing_=false;executionValid_=false;inFlight_={};
        reason_=!current?ExplorationPolicyReason::InFlightInvalidated
                       :success?ExplorationPolicyReason::Completed
                               :ExplorationPolicyReason::NativeFailure;
        return current;
    }
    void cancel() noexcept {
        if(pending_){pending_=false;reason_=ExplorationPolicyReason::Cancelled;}
    }
    bool replaceReturnPoint(const ExplorationContext& context) noexcept {
        update(context);
        if(pending_||executing_){reason_=ExplorationPolicyReason::Busy;return false;}
        if(!safe(context))return false;
        if(!context.menuOpen||!context.neutral||context.inputDraining) {
            reason_=ExplorationPolicyReason::WaitingForInput;return false;
        }
        if(!context.currentSafe){reason_=ExplorationPolicyReason::UnsafeCurrent;return false;}
        if(!validExplorationPosition(context.currentPosition)) {
            reason_=ExplorationPolicyReason::InvalidCoordinate;return false;
        }
        anchor_=context.currentPosition;anchorValid_=true;
        reason_=ExplorationPolicyReason::ReturnSaved;return true;
    }
    bool hasReturnPoint() const noexcept {return anchorValid_&&bound_;}
    bool returnPoint(ExplorationPosition* point) const noexcept {
        if(!hasReturnPoint()||!point)return false;
        *point=anchor_;return true;
    }
    bool pending() const noexcept {return pending_;}
    bool executing() const noexcept {return executing_;}
    ExplorationPolicyReason lastReason() const noexcept {return reason_;}
    const ExplorationIdentity& identity() const noexcept {return identity_;}

private:
    bool safe(const ExplorationContext& context) noexcept {
        if(context.transition){reason_=ExplorationPolicyReason::Transition;return false;}
        if(!context.supported||!context.identity.valid()||!bound_) {
            reason_=ExplorationPolicyReason::Unavailable;return false;
        }
        if(!context.focused){reason_=ExplorationPolicyReason::Unfocused;return false;}
        if(!context.saveProtected){reason_=ExplorationPolicyReason::SaveProtection;return false;}
        return true;
    }
    bool canQueue(std::uint64_t generation,const ExplorationContext& context) noexcept {
        if(pending_||executing_){reason_=ExplorationPolicyReason::Busy;return false;}
        if(!safe(context))return false;
        if(!generation||generation!=context.identity.generation) {
            reason_=ExplorationPolicyReason::StaleGeneration;return false;
        }
        if(!context.menuOpen){reason_=ExplorationPolicyReason::WaitingForInput;return false;}
        if(!context.currentSafe){reason_=ExplorationPolicyReason::UnsafeCurrent;return false;}
        if(!validExplorationPosition(context.currentPosition)) {
            reason_=ExplorationPolicyReason::InvalidCoordinate;return false;
        }
        return true;
    }
    bool queue(ExplorationAction action,const ExplorationPosition& target,
        const ExplorationContext& context) noexcept {
        ++nextToken_;if(!nextToken_)++nextToken_;
        queued_={action,nextToken_,context.identity,target};queuedMs_=context.nowMs;
        pending_=true;reason_=ExplorationPolicyReason::Queued;return true;
    }
    void invalidate(ExplorationPolicyReason reason) noexcept {
        bound_=false;identity_={};anchorValid_=false;anchor_={};pending_=false;
        if(executing_)executionValid_=false;
        reason_=reason;
    }
    ExplorationIdentity identity_{};
    ExplorationPosition anchor_{};
    ExplorationOperation queued_{},inFlight_{};
    std::uint64_t queuedMs_=0,nextToken_=0;
    bool bound_=false,anchorValid_=false,pending_=false,executing_=false,executionValid_=false;
    ExplorationPolicyReason reason_=ExplorationPolicyReason::None;
};
}
