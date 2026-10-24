#pragma once
// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include <cstddef>
#include <cstdint>

namespace ce {
struct CheckpointRequestContext {
    std::uint64_t generation=0,nowMs=0;
    bool ready=false,focused=false,menuOpen=false,neutral=false,saveProtected=false;
};
// A confirmed UI choice becomes a single native operation only after menu
// ownership drains. The native backend supplies freshly validated context.
class CheckpointRequest {
public:
    static constexpr std::uint64_t lifetimeMs=5000;
    bool queue(int section,std::uint64_t generation,const int* ids,std::size_t count,
        const CheckpointRequestContext& context) noexcept {
        if(pending_||loading_||!safe(context)||!context.menuOpen||!generation
            ||generation!=context.generation||!ids||!count||count>32)return false;
        bool found=false;
        for(std::size_t i=0;i<count;++i) {
            if(ids[i]<0)return false;
            if(ids[i]==section)found=true;
            for(std::size_t j=0;j<i;++j)if(ids[i]==ids[j])return false;
        }
        if(!found)return false;
        generation_=generation;queuedMs_=context.nowMs;section_=section;pending_=true;
        return true;
    }
    bool take(const CheckpointRequestContext& context,int* section) noexcept {
        if(!pending_||!section)return false;
        if(!safe(context)||context.generation!=generation_||context.nowMs<queuedMs_
            ||context.nowMs-queuedMs_>lifetimeMs) {cancel();return false;}
        if(context.menuOpen||!context.neutral)return false;
        pending_=false;loading_=true;*section=section_;return true;
    }
    void cancel() noexcept {pending_=false;}
    void complete() noexcept {pending_=false;loading_=false;}
    bool pending() const noexcept {return pending_;}
    bool loading() const noexcept {return loading_;}
    int section() const noexcept {return section_;}
private:
    static bool safe(const CheckpointRequestContext& c) noexcept {
        return c.ready&&c.focused&&c.saveProtected;
    }
    std::uint64_t generation_=0,queuedMs_=0;
    int section_=-1;
    bool pending_=false,loading_=false;
};
}
