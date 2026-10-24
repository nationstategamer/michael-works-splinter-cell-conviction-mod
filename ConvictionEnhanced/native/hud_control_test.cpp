#include "hud_control.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
static_assert(sizeof(std::uintptr_t)==4, "HUD fixtures require the game's x86 layout");
constexpr std::uintptr_t renderAllCaller=0x458b08,renderOneCaller=0x458bad;
constexpr std::uintptr_t movieVtable=0xcde5d4;
constexpr unsigned layerBits[]={1,2,4,8,16,32,64};
constexpr std::uintptr_t callbackVtables[]={
    0xdb3598,0xdb8aac,0xdb2974,0xdb344c,0xdb4fd4,0xdb460c,0xdb5d6c,0xdb8dc4
};
const char* clipNames[]={"unused","Reticle","Markers","Inventory","Buttons","Objectives","Status","Subtitles"};

struct FakeMovie {
    std::uintptr_t vtable{};
    unsigned char padding[0x54]{};
    std::uintptr_t callbacks{};
    int count{},capacity{};
};
struct FakeCallback {
    std::uintptr_t vtable{},movie{},unused{},paths{};
    int count{},capacity{};
};
struct FakePath {
    const char* data{};
    int count{},capacity{};
    unsigned char padding[0x4c]{};
};
static_assert(offsetof(FakeMovie,callbacks)==0x58 && offsetof(FakeMovie,count)==0x5c && offsetof(FakeMovie,capacity)==0x60,
    "Movie callback array layout changed");
static_assert(offsetof(FakeCallback,movie)==4 && offsetof(FakeCallback,paths)==0xc && offsetof(FakeCallback,count)==0x10 && offsetof(FakeCallback,capacity)==0x14,
    "Callback owner/path layout changed");
static_assert(sizeof(FakePath)==0x58 && offsetof(FakePath,count)==4 && offsetof(FakePath,capacity)==8,
    "Native path-entry layout changed");

using Key=std::pair<std::uintptr_t,std::string>;
struct Write {Key key;int value,depth;};
std::map<Key,int> visible;
std::vector<Write> writes;
std::function<void()> drawing;
std::string throwingGetPath;
int checks=0,reads=0,drawDepth=0,outsideWrites=0;

void require(bool ok,const char* message) {
    ++checks;
    if(!ok)throw std::runtime_error(message);
}
Key key(void* movie,const std::string& path) {return {reinterpret_cast<std::uintptr_t>(movie),path};}
int getVisible(void* movie,const char* path) {
    ++reads;
    if(throwingGetPath==path)throw std::runtime_error("Synthetic getter failure");
    const auto found=visible.find(key(movie,path));
    return found==visible.end()?-1:found->second;
}
void setVisible(void* movie,const char* path,int value) {
    if(drawDepth<=0)++outsideWrites;
    const auto which=key(movie,path);
    writes.push_back({which,value,drawDepth});
    visible[which]=value;
}
void invokeDraw() {drawing();}
void draw(const std::function<void()>& action,bool gameplay=true) {
    auto previous=std::move(drawing);
    drawing=action;
    ++drawDepth;
    try {ce::testHudDraw(invokeDraw,gameplay,getVisible,setVisible);}
    catch(...) {--drawDepth;drawing=std::move(previous);throw;}
    --drawDepth;
    drawing=std::move(previous);
}
void beginCase() {
    ce::restoreHud();
    visible.clear();writes.clear();throwingGetPath.clear();reads=0;
}
void hide(unsigned bits) {
    for(unsigned bit:layerBits)ce::setHudLayerHidden(static_cast<ce::HudLayer>(bit),(bits&bit)!=0);
}

struct Fixture {
    FakeMovie movie{};
    std::array<std::uintptr_t,8> callbackPointers{};
    std::array<FakeCallback,8> callbacks{};
    std::array<FakePath,8> paths{};
    std::array<std::array<char,190>,8> names{};

    Fixture() {
        movie.vtable=movieVtable;
        movie.callbacks=reinterpret_cast<std::uintptr_t>(callbackPointers.data());
        movie.count=movie.capacity=static_cast<int>(callbacks.size());
        for(std::size_t i=0;i<callbacks.size();++i){
            callbackPointers[i]=reinterpret_cast<std::uintptr_t>(&callbacks[i]);
            callbacks[i].vtable=callbackVtables[i];
            callbacks[i].movie=reinterpret_cast<std::uintptr_t>(&movie);
            callbacks[i].paths=reinterpret_cast<std::uintptr_t>(&paths[i]);
            callbacks[i].count=callbacks[i].capacity=i?1:0;
            std::memcpy(names[i].data(),clipNames[i],std::strlen(clipNames[i])+1);
            paths[i].data=names[i].data();
            paths[i].count=paths[i].capacity=static_cast<int>(std::strlen(clipNames[i]))+1;
            if(i)visible[key(&movie,path(i))]=1;
        }
    }
    Fixture(const Fixture&)=delete;
    Fixture& operator=(const Fixture&)=delete;
    std::string path(std::size_t index) const {return std::string("_root.")+names[index].data()+"._visible";}
    int value(std::size_t index) const {
        const auto found=visible.find({reinterpret_cast<std::uintptr_t>(&movie),path(index)});
        return found==visible.end()?-1:found->second;
    }
};

std::vector<unsigned char> snapshot(const Fixture& f) {
    const auto first=reinterpret_cast<const unsigned char*>(&f);
    return {first,first+sizeof(f)};
}
void unchanged(const Fixture& f,const std::vector<unsigned char>& before) {
    require(std::memcmp(&f,before.data(),sizeof(f))==0,"HUD filtering wrote native movie/callback/path memory");
}
void allVisible(const Fixture& f) {
    for(std::size_t i=1;i<8;++i)require(f.value(i)==1,"A clip was not restored to its original visible value");
}

void initializationAndNoFiltering() {
    require(!ce::hudStatus().installed,"HUD started installed without initialization");
    ce::setHudLayerHidden(ce::HudLayer::Crosshair,true);ce::setAllHudHidden(true);
    require(!ce::hudStatus().allHidden && ce::hudStatus().hiddenMask==0,"Setters bypassed initialization gating");
    require(ce::initializeHudControl(),"HUD test initialization failed");
    auto status=ce::hudStatus();
    require(status.installed && status.supportedMask==127 && !status.allHidden && status.hiddenMask==0,
        "HUD defaults or test support mask are incorrect");
    require(status.observations==0 && status.suppressed==0 && status.observedMask==0,"HUD diagnostics did not start empty");
    beginCase();Fixture f;const auto before=snapshot(f);
    draw([&]{require(ce::testHudQuery(&f.movie,renderAllCaller,7)==7,"Default filtering changed the natural result");allVisible(f);});
    require(writes.empty() && reads==0,"Default HUD settings accessed clip visibility");
    status=ce::hudStatus();
    require(status.observations==1 && status.observedMask==127 && status.suppressed==0,"Natural observation diagnostics are incorrect");
    unchanged(f,before);
    ce::setHudLayerHidden(static_cast<ce::HudLayer>(0),true);
    ce::setHudLayerHidden(static_cast<ce::HudLayer>(3),true);
    ce::setHudLayerHidden(static_cast<ce::HudLayer>(128),true);
    require(ce::hudStatus().hiddenMask==0 && !ce::hudStatus().allHidden,"Invalid layer masks changed choices");
}

void independentLayersAndRestore() {
    for(std::size_t selected=1;selected<8;++selected){
        beginCase();Fixture f;const auto before=snapshot(f);hide(layerBits[selected-1]);
        require(writes.empty(),"Changing a layer wrote game state outside drawing");
        const auto suppressed=ce::hudStatus().suppressed;
        draw([&]{
            require(ce::testHudQuery(&f.movie,renderOneCaller,7)==7,"Individual hiding suppressed the entire movie");
            for(std::size_t i=1;i<8;++i)require(f.value(i)==(i==selected?0:1),"Individual hiding affected the wrong layer");
        });
        allVisible(f);unchanged(f,before);
        require(writes.size()==2 && writes[0].value==0 && writes[1].value==1 && writes[0].key==writes[1].key,
            "Individual clip did not have one bounded hide/restore pair");
        require(ce::hudStatus().suppressed==suppressed+1,"Individual suppression counter did not match the hidden clip");
        require(ce::hudStatus().hiddenMask==layerBits[selected-1],"Drawing changed the selected layer");
    }
    beginCase();Fixture f;hide(1|4);ce::setAllHudHidden(true);const auto before=snapshot(f);
    require(ce::hudStatus().allHidden && ce::hudStatus().hiddenMask==5,"Master hide discarded individual choices");
    draw([&]{require(ce::testHudQuery(&f.movie,renderAllCaller,7)==0,"Master hide did not suppress the HUD movie");allVisible(f);});
    require(writes.empty() && reads==0,"Master hide wrote or read individual clip state");
    unchanged(f,before);ce::setAllHudHidden(false);
    require(!ce::hudStatus().allHidden && ce::hudStatus().hiddenMask==5,"Disabling master did not preserve individual choices");
    draw([&]{ce::testHudQuery(&f.movie,renderAllCaller,1);require(f.value(1)==0 && f.value(3)==0 && f.value(2)==1,"Individual choices did not resume after master hide");});
    allVisible(f);
    writes.clear();ce::setAllHudHidden(true);ce::restoreHud();ce::restoreHud();
    require(!ce::hudStatus().allHidden && ce::hudStatus().hiddenMask==0 && writes.empty(),"Restore all did not clear choices without touching game state");
    draw([&]{require(ce::testHudQuery(&f.movie,renderAllCaller,1)==1,"Restored HUD remained filtered");});
    require(writes.empty(),"Restored HUD wrote clip state");
}

void unsupportedContextsAndMovies() {
    beginCase();Fixture f;hide(127);ce::setAllHudHidden(true);const auto before=snapshot(f);
    const auto observations=ce::hudStatus().observations,suppressed=ce::hudStatus().suppressed;
    require(ce::testHudQuery(&f.movie,renderAllCaller,7)==7,"Query outside a render scope was filtered");
    draw([&]{require(ce::testHudQuery(&f.movie,renderAllCaller,7)==7,"Non-gameplay draw was filtered");},false);
    draw([&]{
        require(ce::testHudQuery(&f.movie,renderAllCaller+1,7)==7,"Non-render caller was filtered");
        require(ce::testHudQuery(&f.movie,0,7)==7,"Unknown caller was filtered");
        require(ce::testHudQuery(&f.movie,renderAllCaller,0)==0,"Naturally hidden movie changed result");
    });
    require(ce::hudStatus().observations==observations && ce::hudStatus().suppressed==suppressed,"Rejected context changed HUD observation/suppression counters");
    require(reads==0 && writes.empty(),"Rejected context touched visibility");unchanged(f,before);

    const std::function<void(Fixture&)> corruptions[]={
        [](Fixture& x){x.movie.vtable=0;},
        [](Fixture& x){x.movie.callbacks=0;},
        [](Fixture& x){x.movie.count=0;},
        [](Fixture& x){x.movie.count=-1;},
        [](Fixture& x){x.movie.count=129;x.movie.capacity=129;},
        [](Fixture& x){x.movie.capacity=x.movie.count-1;},
        [](Fixture& x){x.movie.capacity=257;},
        [](Fixture& x){x.callbackPointers[7]=0;},
        [](Fixture& x){x.callbacks[7].movie=0;},
        [](Fixture& x){x.callbacks[7].vtable=0x123456;}, // Unknown/frontend callback.
    };
    for(const auto& corrupt:corruptions){
        beginCase();Fixture bad;hide(127);ce::setAllHudHidden(true);corrupt(bad);
        const auto bytes=snapshot(bad);const auto obs=ce::hudStatus().observations;
        draw([&]{require(ce::testHudQuery(&bad.movie,renderAllCaller,7)==7,"Malformed or frontend movie was filtered");});
        require(ce::hudStatus().observations==obs && reads==0 && writes.empty(),"Malformed movie was observed or touched before full validation");
        unchanged(bad,bytes);
    }
    draw([]{require(ce::testHudQuery(nullptr,renderOneCaller,7)==7,"Null movie was filtered");});

    beginCase();Fixture root;root.movie.count=root.movie.capacity=1;hide(127);
    draw([&]{require(ce::testHudQuery(&root.movie,renderOneCaller,7)==7,"Root-only HUD movie was hidden by an individual choice");});
    ce::setAllHudHidden(true);
    draw([&]{require(ce::testHudQuery(&root.movie,renderOneCaller,7)==0,"Known root-only HUD movie ignored the master switch");});
    require(reads==0 && writes.empty(),"Root-only HUD movie accessed nonexistent layer paths");
}

void malformedAndMissingPaths() {
    const std::function<void(Fixture&)> corruptions[]={
        [](Fixture& x){x.callbacks[1].count=-1;},
        [](Fixture& x){x.callbacks[1].count=65;x.callbacks[1].capacity=65;},
        [](Fixture& x){x.callbacks[1].capacity=0;},
        [](Fixture& x){x.callbacks[1].capacity=129;},
        [](Fixture& x){x.callbacks[1].paths=0;},
        [](Fixture& x){x.callbacks[1].count=0;x.callbacks[1].capacity=0;x.callbacks[1].paths=0;},
        [](Fixture& x){x.paths[1].data=nullptr;},
        [](Fixture& x){x.paths[1].count=1;},
        [](Fixture& x){x.paths[1].count=177;x.paths[1].capacity=177;},
        [](Fixture& x){x.paths[1].capacity=x.paths[1].count-1;},
        [](Fixture& x){x.paths[1].capacity=4097;},
        [](Fixture& x){x.names[1][x.paths[1].count-1]='X';},
        [](Fixture& x){x.names[1][0]='[';},
        [](Fixture& x){x.names[1][2]=0;},
    };
    for(const auto& corrupt:corruptions){
        beginCase();Fixture f;hide(1);corrupt(f);const auto before=snapshot(f);const auto state=visible;
        draw([&]{require(ce::testHudQuery(&f.movie,renderAllCaller,7)==7,"Bad path changed the movie result");});
        require(reads==0 && writes.empty() && visible==state,"Invalid clip path reached a visibility getter or setter");
        unchanged(f,before);
    }
    beginCase();Fixture f;hide(1|2|4);visible[key(&f.movie,f.path(1))]=0;visible.erase(key(&f.movie,f.path(2)));visible[key(&f.movie,f.path(3))]=2;
    const auto state=visible;
    draw([&]{require(ce::testHudQuery(&f.movie,renderAllCaller,7)==7,"Missing/game-hidden clips changed the movie result");});
    require(reads==3 && writes.empty() && visible==state,"Missing, false or nonboolean clip was changed or forced visible");

    beginCase();Fixture longPath;hide(1);
    std::memset(longPath.names[1].data(),'A',175);longPath.names[1][175]=0;
    longPath.paths[1].count=176;longPath.paths[1].capacity=4096;
    visible[key(&longPath.movie,longPath.path(1))]=1;
    draw([&]{ce::testHudQuery(&longPath.movie,renderAllCaller,1);require(longPath.value(1)==0,"Longest supported path was not filtered");});
    require(longPath.value(1)==1 && writes.size()==2,"Longest supported path was not restored");
}

void repeatedAndNestedRendering() {
    beginCase();Fixture f;hide(1);const auto before=snapshot(f);
    draw([&]{
        ce::testHudQuery(&f.movie,renderAllCaller,1);
        ce::testHudQuery(&f.movie,renderOneCaller,1);
        require(f.value(1)==0 && writes.size()==1,"Repeated query duplicated hiding or prematurely restored a clip");
        ce::restoreHud();
        require(f.value(1)==0 && writes.size()==1,"Restore all wrote game visibility during the active draw");
    });
    require(f.value(1)==1 && writes.size()==2 && ce::hudStatus().hiddenMask==0,"Active draw failed to restore after choices were cleared");
    unchanged(f,before);

    beginCase();Fixture nested;hide(1);
    draw([&]{
        ce::testHudQuery(&nested.movie,renderAllCaller,1);
        require(nested.value(1)==0 && nested.value(3)==1,"Outer draw did not hide only its selection");
        hide(4);
        draw([&]{
            ce::testHudQuery(&nested.movie,renderOneCaller,1);
            require(nested.value(1)==0 && nested.value(3)==0,"Nested draw lost outer state or its own selection");
        });
        require(nested.value(1)==0 && nested.value(3)==1,"Nested scope restored an outer-owned clip");
        ce::testHudQuery(&nested.movie,renderAllCaller,1);
        require(nested.value(3)==1,"Outer scope stopped using its captured selection");
    });
    allVisible(nested);
    require(writes.size()==4 && writes[0].depth==1 && writes[1].depth==2 && writes[2].depth==2 && writes[3].depth==1,
        "Nested hide/restore ordering escaped its owning scope");
    require(writes[0].key==writes[3].key && writes[1].key==writes[2].key,"Nested restoration used another scope's clip");

    beginCase();Fixture owner;Fixture other;hide(1);
    draw([&]{
        ce::testHudQuery(&owner.movie,renderAllCaller,1);
        require(owner.value(1)==0 && other.value(1)==1,"Visibility was shared between different movie instances");
    });
    allVisible(owner);allVisible(other);
}

void exceptionRestoration() {
    beginCase();Fixture f;hide(1|4);bool caught=false;const auto before=snapshot(f);
    try {
        draw([&]{
            ce::testHudQuery(&f.movie,renderAllCaller,1);
            require(f.value(1)==0 && f.value(3)==0,"Exception fixture was not hidden before draw failure");
            throw std::runtime_error("Synthetic render failure");
        });
    }catch(const std::runtime_error& error){caught=std::string(error.what())=="Synthetic render failure";}
    require(caught,"Draw exception did not propagate after cleanup");allVisible(f);unchanged(f,before);
    require(writes.size()==4 && writes[0].key==writes[3].key && writes[1].key==writes[2].key,
        "Draw exception did not restore clips in reverse order");
    const auto count=writes.size();
    require(ce::testHudQuery(&f.movie,renderAllCaller,1)==1 && writes.size()==count,"Exception left a stale active render scope");
    draw([&]{ce::testHudQuery(&f.movie,renderAllCaller,1);require(f.value(1)==0,"Subsequent draw did not recover its test callbacks");});
    allVisible(f);
}

void getterFailureRestoresAndDisables() {
    // Last because a backend fault intentionally disables HUD controls for the session.
    beginCase();Fixture f;hide(1|2);throwingGetPath=f.path(2);const auto before=snapshot(f);
    draw([&]{
        require(ce::testHudQuery(&f.movie,renderAllCaller,7)==7,"Getter failure changed the natural movie result");
        require(f.value(1)==0,"Earlier temporary hide was not retained until safe scope cleanup");
    });
    allVisible(f);unchanged(f,before);
    const auto status=ce::hudStatus();
    require(!status.installed && status.supportedMask==0 && status.hiddenMask==0 && !status.allHidden,
        "Getter failure did not disable filtering and clear choices");
    require(writes.size()==2 && writes[0].value==0 && writes[1].value==1,"Getter failure failed to restore prior temporary writes");
    writes.clear();throwingGetPath.clear();ce::setHudLayerHidden(ce::HudLayer::Crosshair,true);ce::setAllHudHidden(true);
    draw([&]{require(ce::testHudQuery(&f.movie,renderAllCaller,7)==7,"Faulted HUD continued filtering");});
    require(writes.empty() && ce::hudStatus().hiddenMask==0 && !ce::hudStatus().allHidden,"Faulted HUD accepted new choices or writes");
}
}

int main() {
    try {
        initializationAndNoFiltering();
        independentLayersAndRestore();
        unsupportedContextsAndMovies();
        malformedAndMissingPaths();
        repeatedAndNestedRendering();
        exceptionRestoration();
        getterFailureRestoresAndDisables();
        require(outsideWrites==0 && drawDepth==0,"A visibility write escaped the render scope");
        std::cout<<"PASS: "<<checks<<" HUD checks; bounded native fixtures, selection isolation, master/restore, invalid-context refusal, nested/exception cleanup and write scope.\n";
        return 0;
    }catch(const std::exception& error){
        std::cerr<<"FAIL after "<<checks<<" HUD checks: "<<error.what()<<'\n';
        return 1;
    }
}
