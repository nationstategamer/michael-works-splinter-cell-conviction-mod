// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include "checkpoint_request.h"
#include <iostream>
#include <limits>
#include <stdexcept>
namespace {
unsigned checks=0;
void require(bool pass,const char* message){++checks;if(!pass)throw std::runtime_error(message);}
ce::CheckpointRequestContext context(){return {7,1000,true,true,true,false,true};}
constexpr int sections[]={0,2,7};
}
int main(){try {
    using ce::CheckpointRequest;
    auto c=context();CheckpointRequest request;int selected=-1;
    require(request.queue(7,7,sections,3,c),"Confirmed valid section rejected");
    require(!request.queue(2,7,sections,3,c),"Duplicate request accepted");
    require(!request.take(c,&selected)&&request.pending(),"Open menu allowed execution");
    c.menuOpen=false;require(!request.take(c,&selected)&&request.pending(),"Held Cross allowed execution");
    c.neutral=true;require(request.take(c,&selected)&&selected==7&&request.loading(),"Neutral closed menu did not execute");
    require(!request.take(c,&selected),"Request executed twice");
    c.menuOpen=true;require(!request.queue(0,7,sections,3,c),"Request during transition accepted");
    request.cancel();require(request.loading(),"Cancel undid an already executed transition");
    request.complete();require(!request.loading()&&!request.pending(),"Completion retained ownership");
    for(unsigned gate=0;gate<4;++gate){
        c=context();bool* gates[]={&c.ready,&c.focused,&c.menuOpen,&c.saveProtected};*gates[gate]=false;
        CheckpointRequest p;require(!p.queue(0,7,sections,3,c),"Queue bypassed safety gate");
    }
    for(unsigned gate=0;gate<3;++gate){
        c=context();CheckpointRequest p;require(p.queue(2,7,sections,3,c),"Gate fixture queue failed");
        bool* gates[]={&c.ready,&c.focused,&c.saveProtected};*gates[gate]=false;c.menuOpen=false;c.neutral=true;
        require(!p.take(c,&selected)&&!p.pending()&&!p.loading(),"Execution bypassed changed safety gate");
    }
    c=context();CheckpointRequest stale;
    require(!stale.queue(0,6,sections,3,c),"Stale UI generation accepted");
    require(!stale.queue(1,7,sections,3,c),"Invented section accepted");
    require(!stale.queue(-1,7,sections,3,c),"Negative section accepted");
    require(!stale.queue(0,0,sections,3,c),"Empty generation accepted");
    require(!stale.queue(0,7,nullptr,3,c),"Missing catalog accepted");
    require(!stale.queue(0,7,sections,0,c),"Empty catalog accepted");
    require(!stale.queue(0,7,sections,33,c),"Unbounded catalog accepted");
    const int duplicate[]={0,0};require(!stale.queue(0,7,duplicate,2,c),"Ambiguous catalog accepted");
    const int invalid[]={0,-2};require(!stale.queue(0,7,invalid,2,c),"Invalid catalog accepted");
    require(stale.queue(0,7,sections,3,c),"Stale fixture queue failed");
    c.generation=8;c.menuOpen=false;c.neutral=true;
    require(!stale.take(c,&selected)&&!stale.pending(),"Map/profile transition retained old request");
    for(auto elapsed:{CheckpointRequest::lifetimeMs,CheckpointRequest::lifetimeMs+1}){
        c=context();CheckpointRequest p;require(p.queue(2,7,sections,3,c),"TTL fixture queue failed");
        c.nowMs+=elapsed;c.menuOpen=false;c.neutral=true;
        require(p.take(c,&selected)==(elapsed==CheckpointRequest::lifetimeMs),"Request TTL boundary incorrect");
    }
    for(auto timestamp:{std::uint64_t(999),std::uint64_t(0)}){
        c=context();CheckpointRequest p;require(p.queue(0,7,sections,3,c),"Clock fixture queue failed");
        c.nowMs=timestamp;c.menuOpen=false;c.neutral=true;
        require(!p.take(c,&selected)&&!p.pending(),"Backward clock authorized old request");
    }
    c=context();c.nowMs=std::numeric_limits<std::uint64_t>::max()-3;CheckpointRequest wrap;
    require(wrap.queue(0,7,sections,3,c),"Wrap fixture queue failed");c.nowMs=2;c.menuOpen=false;c.neutral=true;
    require(!wrap.take(c,&selected)&&!wrap.pending(),"Wrapped clock authorized request");
    c=context();CheckpointRequest cancelled;require(cancelled.queue(0,7,sections,3,c),"Cancel fixture queue failed");
    cancelled.cancel();c.menuOpen=false;c.neutral=true;require(!cancelled.take(c,&selected),"Cancelled request executed");
    c=context();CheckpointRequest pointer;require(pointer.queue(0,7,sections,3,c),"Pointer fixture queue failed");
    c.menuOpen=false;c.neutral=true;require(!pointer.take(c,nullptr)&&pointer.pending(),"Missing destination consumed request");
    std::cout<<"PASS: "<<checks<<" checkpoint request checks; context, save guard, catalog, input drain, stale/duplicate/expiry refusal.\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
