#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include <windows.h>
#include <bcrypt.h>
#include <intrin.h>
#include <cstring>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "MinHook.h"
#include "checkpoint_control.h"
#include "checkpoint_request.h"
#include "checkpoint_archive.h"
#include "checkpoint_library.h"
#include "practice_storage.h"
namespace ce { namespace {
SRWLOCK checkpointLock=SRWLOCK_INIT;
struct Lock {Lock(){AcquireSRWLockExclusive(&checkpointLock);}~Lock(){ReleaseSRWLockExclusive(&checkpointLock);}};
struct ArchiveRef {std::uintptr_t object=0,data=0;unsigned size=0;};
struct Sample {
 std::uintptr_t engine=0,viewport=0,controller=0,level=0,manager=0,profile=0,episodeObject=0;
 int profileId=-1,act=-1,episode=-1,current=-1;
 char profileName[128]{},account[65]{},mission[96]{},map[260]{};
 ArchiveRef initial{},last{};std::uintptr_t savedInfo=0,savedData=0;unsigned savedSize=0;
 bool archivesReady=false;unsigned char difficulty=0;int controllerPreference=0;
 unsigned partCount=0;CheckpointPart parts[2]{};char partMaps[2][260]{};std::uintptr_t partObjects[2]{};
};
struct Snapshot {int token=-1;CheckpointArchiveInfo info{};CheckpointLibraryRecord record;bool initialSaved=false,normalSaved=false;};
struct Input {HWND window=nullptr;unsigned connected=0;bool menuOpen=false,neutral=false,protectedSave=false;std::uint64_t time=0;};
using RestoreFn=int(__thiscall*)(void*,const void*,int);
RestoreFn originalRestore=nullptr;
CheckpointStatus status;CheckpointRequest request;Input input;unsigned queuedMask=0;
Sample observed{},owner{},armedIdentity{};
std::vector<std::shared_ptr<const Snapshot>> snapshots;
std::shared_ptr<const Snapshot> armedSnapshot;
std::string profileKey;
bool haveObservation=false,haveOwner=false,faulted=false,libraryReady=false,libraryFault=false,armed=false,inRestore=false;
std::uint64_t lastObservation=0,lastCapture=0,loadStarted=0;std::uintptr_t base=0;
struct PartGroup {int first;const char* maps[2];};
constexpr PartGroup partGroups[]={
 {7,{"_Single\\08_ThirdEchelon_A.unr","_Single\\08_ThirdEchelon_B.unr"}},
 {9,{"_Single\\09_AbandonedReservoir_A.unr","_Single\\09_AbandonedReservoir_B.unr"}},
 {11,{"_Single\\10_StreetsAndGarden_A.unr","_Single\\10_StreetsAndGarden_B.unr"}},
 {13,{"_Single\\11_WhiteHouse_A.unr","_Single\\11_WhiteHouse_B.unr"}}
};
struct PartJob {Sample source{};int target=-1;char map[260]{};unsigned mask=0;};
bool partBindingsReady=false,partQueued=false,partTraveling=false,inPartDispatch=false;
PartJob activePart{};std::uint64_t partStarted=0,partTargetSeen=0;
#ifdef CE_TEST_HOST
std::uint64_t testTime=1000;bool testFocus=true,testStorage=true;unsigned dispatches=0;int lastDispatch=-1;
std::uintptr_t testCaller=0;unsigned testOriginalCalls=0;bool testUsedOwned=false,testReentered=false,testPostMap=true;int testOriginalResult=1,testPostId=-999;unsigned testAnchorCopies=0;bool testAnchorOk=true,testMaskInOriginal=false;
int testEpisode=0;bool testPartMaps=true,testArchives=true;unsigned testDifficulty=1,testProfile=1,testPartFailures=0,testPartCalls=0,testPartArrivalFlags=0;int testPartLast=-1;bool testPartReentry=false;
std::vector<unsigned char> testArrivalBytes;unsigned char testArrivalInfo[32]{},testArrivalArchive[0x48]{};
int __fastcall testOriginal(void*,void*,const void* data,int) {++testOriginalCalls;testUsedOwned=armedSnapshot&&data==armedSnapshot->record.bytes.data();testReentered=checkpointStatus().loading;if(testMaskInOriginal)pollCheckpointInput(reinterpret_cast<HWND>(1),2,false,true,true);return testOriginalResult;}
std::uint64_t now() noexcept {return testTime;}
#else
std::uint64_t now() noexcept {return GetTickCount64();}
#endif
bool fresh(std::uint64_t t) noexcept {return t&&now()>=t&&now()-t<=1000;}
void generation() noexcept {if(++status.generation==0)++status.generation;}
bool sameOwner(const Sample& a,const Sample& b) noexcept {
 return a.profile==b.profile&&a.profileId==b.profileId&&a.act==b.act&&a.episode==b.episode&&std::strcmp(a.profileName,b.profileName)==0&&std::strcmp(a.account,b.account)==0&&std::strcmp(a.map,b.map)==0;
}
bool sameView(const Sample& a,const Sample& b) noexcept {return sameOwner(a,b)&&a.engine==b.engine&&a.viewport==b.viewport&&a.controller==b.controller&&a.level==b.level&&a.manager==b.manager&&a.episodeObject==b.episodeObject;}
bool focusedWindow(HWND window) noexcept {
#ifdef CE_TEST_HOST
 (void)window;return testFocus;
#else
 DWORD pid=0;return window&&GetWindowThreadProcessId(window,&pid)&&pid==GetCurrentProcessId()&&GetForegroundWindow()==window;
#endif
}
bool focused() noexcept {return focusedWindow(input.window);}
bool protectedSave() noexcept {
#ifdef CE_TEST_HOST
 return testStorage;
#else
 const auto storage=practiceStorageStatus();return storage.installed&&storage.active&&storage.healthy;
#endif
}
CheckpointRequestContext context() noexcept {
 return {status.generation,now(),status.installed&&!faulted&&!libraryFault&&status.available&&haveObservation&&fresh(lastObservation)&&fresh(input.time)&&input.connected!=0&&(!request.pending()||input.connected==queuedMask),focused(),input.menuOpen,input.neutral,input.protectedSave&&protectedSave()};
}
bool bytes(std::uintptr_t p,const unsigned char* v,std::size_t n) noexcept {
 __try{return std::memcmp(reinterpret_cast<const void*>(p),v,n)==0;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool bindings() noexcept {
 const unsigned char tick[]={0x8b,0x86,0xa0,2,0,0,0xff,0x70,0x40,0x8b,0x16,0xff,0x70,0x3c,0x8b,0xce,0xff,0x92,0x4c,2,0,0};
 const unsigned char accessor[]={0x8b,0x81,0x64,2,0,0,0xc3};
 const unsigned char anchor[]={0x56,0x8b,0xf1,0x8b,0x8e,0xa0,2,0,0,0x85,0xc9,0x74,0x0d,0x8b,0x01,0x6a,0x01};
 return bytes(base+0x39f7f2,tick,sizeof(tick))&&bytes(base+0x3953d8,accessor,sizeof(accessor))&&bytes(base+0x39a15c,anchor,sizeof(anchor));
}
bool partBindings() noexcept {
 const unsigned char replay[]={0x55,0x8b,0xec,0xe8,0x1f,0xf3,0xa0,0xff,0xff,0x75,0x14,0x8b,0x10,0x8b,0xc8,0xff};
 const unsigned char tail[]={0x5d,0xc2,0x10,0};const unsigned char flag[]={0x81,0x88,0xfc,1,0,0,0,0,0x40,0};
 return bytes(base+0x983822,replay,sizeof(replay))&&bytes(base+0x9838b2,tail,sizeof(tail))&&bytes(base+0x979c38,flag,sizeof(flag));
}
bool copyString(std::uintptr_t address,char* output,unsigned capacity) noexcept {
 __try {const auto data=*reinterpret_cast<const std::uintptr_t*>(address);const int size=*reinterpret_cast<const int*>(address+4),cap=*reinterpret_cast<const int*>(address+8);
  if(!data||size<2||size>260||static_cast<unsigned>(size)>capacity||cap<size||cap>4096)return false;const auto text=reinterpret_cast<const unsigned char*>(data);if(text[size-1]!=0)return false;
  for(int i=0;i<size-1;++i)if(text[i]<32||text[i]>126)return false;std::memcpy(output,text,size);return true;
 } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool equalIgnoreCase(const char* a,const char* b) noexcept {while(*a&&*b){char x=*a++,y=*b++;if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return false;}return *a==*b;}
const PartGroup* partGroup(int episode) noexcept {for(const auto& group:partGroups)if(episode==group.first||episode==group.first+1)return &group;return nullptr;}
void fixturePartMetadata(Sample* sample) noexcept {
 const auto* group=partGroup(sample->episode);if(!group)return;sample->partCount=2;
 for(unsigned i=0;i<2;++i){sample->parts[i].episode=group->first+static_cast<int>(i);std::snprintf(sample->parts[i].label,sizeof(sample->parts[i].label),"Part %c",'A'+i);std::snprintf(sample->partMaps[i],sizeof(sample->partMaps[i]),"%s",group->maps[i]);sample->partObjects[i]=100+i;}
}
bool campaignMap(const char* map) noexcept {
 char lower[260]{};const auto n=std::strlen(map);if(n<13||n>=sizeof(lower))return false;
 for(std::size_t i=0;i<n;++i)lower[i]=map[i]>='A'&&map[i]<='Z'?static_cast<char>(map[i]+32):map[i];
 if(std::memcmp(lower,"_single\\",8)!=0||std::strcmp(lower+n-4,".unr")!=0)return false;
 for(std::size_t i=8;i<n-4;++i)if(!((lower[i]>='a'&&lower[i]<='z')||(lower[i]>='0'&&lower[i]<='9')||lower[i]=='_'||lower[i]=='-'))return false;return true;
}
bool readArchive(std::uintptr_t object,ArchiveRef* out) noexcept {
 __try {if(!object||*reinterpret_cast<const std::uintptr_t*>(object)!=base+0xcb77d4)return false;
  const auto data=*reinterpret_cast<const std::uintptr_t*>(object+0x3c);const int size=*reinterpret_cast<const int*>(object+0x40),capacity=*reinterpret_cast<const int*>(object+0x44);
  if(!data||size<9||static_cast<std::size_t>(size)>MaxCheckpointCompressed||capacity<size||static_cast<std::size_t>(capacity)>MaxCheckpointExpanded)return false;
  *out={object,data,static_cast<unsigned>(size)};return true;
 } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
enum class Inspection {Unavailable,OtherViewport,Ready};
Inspection inspect(void* self,Sample* s,const char** why) noexcept {
 __try {
  auto at=[](std::uintptr_t p){return *reinterpret_cast<const std::uintptr_t*>(p);};auto integer=[](std::uintptr_t p){return *reinterpret_cast<const int*>(p);};
  if(at(base+0xfa3200)||at(base+0xfa321c)){*why="Waiting for native map loading to finish";return Inspection::Unavailable;}
  const auto first=at(base+0xfa3178)==0?at(base+0xfa3370):0;s->engine=first?first:at(base+0xfa336c);if(!s->engine)return Inspection::Unavailable;
  const auto e=s->engine,client=at(e+0x44);if(!client||integer(client+0x30)<1||integer(client+0x30)>8||!at(client+0x2c))return Inspection::Unavailable;
  s->viewport=at(at(client+0x2c));if(!s->viewport)return Inspection::Unavailable;s->controller=at(s->viewport+0x30);
  if(s->controller!=reinterpret_cast<std::uintptr_t>(self))return Inspection::OtherViewport;
  if(!s->controller||at(s->controller+0x430)!=s->viewport||at(s->viewport+0x18)!=client)return Inspection::Unavailable;
  if(at(e+0x1fc)&0x100){*why="Recorded checkpoints are unavailable in split-screen";return Inspection::Unavailable;}
  const auto connection=at(base+0x101f8c0);if(!connection)return Inspection::Unavailable;const auto block=at(connection+0x284);
  if(!block||integer(block+4)!=4||integer(block+8)<4||integer(block+8)>64||std::memcmp(reinterpret_cast<const void*>(at(block+12)),"SOLO",4)!=0){*why="Recorded checkpoints require a solo campaign session";return Inspection::Unavailable;}
  s->level=at(e+0x19c);if(!s->level||at(s->controller+0x134)!=s->level||!copyString(s->level+0x6c,s->map,sizeof(s->map))||!campaignMap(s->map))return Inspection::Unavailable;
  const auto table=at(e);if(at(table+0x1f0)!=base+0x3953d8||at(table+0x24c)!=base+0x3a1f02){*why="Checkpoint functions differ from supported build";return Inspection::Unavailable;}
  s->manager=at(e+0x264);if(!s->manager)return Inspection::Unavailable;const auto m=s->manager;s->profileId=integer(m+0x230);
  if(s->profileId<0||s->profileId>3||integer(m+0x234)!=-1)return Inspection::Unavailable;s->profile=at(m+0xa8+4*s->profileId);
  if(!s->profile||!copyString(s->profile,s->profileName,sizeof(s->profileName))||!practiceAccountKey(s->account)){*why="Waiting for a verified account and player profile";return Inspection::Unavailable;}
  const auto root=m+8;s->act=integer(root+4);s->episode=integer(root+8);const int acts=integer(root+0x78);const auto actData=at(root+0x74);
  if(s->act!=0||acts<1||acts>8||!actData||s->episode<0)return Inspection::Unavailable;const auto act=at(actData);if(!act||integer(act+0x28)==1)return Inspection::Unavailable;
  const int episodes=integer(act+0x1c);const auto episodeData=at(act+0x18);if(episodes<1||episodes>64||s->episode>=episodes||!episodeData)return Inspection::Unavailable;
  s->episodeObject=at(episodeData+4*s->episode);if(!s->episodeObject)return Inspection::Unavailable;char map[260]{};
  if(!copyString(s->episodeObject,s->mission,sizeof(s->mission))||!copyString(s->episodeObject+0x18,map,sizeof(map))||!equalIgnoreCase(s->map,map)){*why="Current mission and player profile selection differ";return Inspection::Unavailable;}
  s->difficulty=*reinterpret_cast<const unsigned char*>(s->profile+0x154);s->controllerPreference=integer(s->profile+0x24);
  const auto* group=partGroup(s->episode);
  if(group&&episodes>group->first+1){bool validParts=true;
   for(unsigned i=0;i<2;++i){const int index=group->first+static_cast<int>(i);const auto object=at(episodeData+4*index);char partMap[260]{};
    if(!object||!copyString(object+0x18,partMap,sizeof(partMap))||!equalIgnoreCase(partMap,group->maps[i])){validParts=false;break;}
    s->parts[i].episode=index;std::snprintf(s->parts[i].label,sizeof(s->parts[i].label),"Part %c",'A'+i);std::memcpy(s->partMaps[i],partMap,sizeof(partMap));s->partObjects[i]=object;
   }if(validParts)s->partCount=2;
  }
  s->current=integer(e+0x2a4);s->savedInfo=at(m+0x25c);
  if(s->savedInfo&&integer(s->savedInfo)==12){s->savedData=at(s->savedInfo+0x14);const int n=integer(s->savedInfo+4);if(n>0&&static_cast<std::size_t>(n)<=MaxCheckpointCompressed)s->savedSize=static_cast<unsigned>(n);}
  s->archivesReady=readArchive(at(e+0x29c),&s->initial)&&readArchive(at(e+0x2a0),&s->last);
  if(at(base+0x101f8c0)!=connection||at(connection+0x284)!=block||at(m+0xa8+4*s->profileId)!=s->profile)return Inspection::Unavailable;
  return Inspection::Ready;
 } __except(EXCEPTION_EXECUTE_HANDLER){*why="Waiting for stable campaign data";return Inspection::Unavailable;}
}
bool sameTravelProfile(const Sample& a,const Sample& b) noexcept {
 return a.engine==b.engine&&a.manager==b.manager&&a.profileId==b.profileId&&std::strcmp(a.profileName,b.profileName)==0&&std::strcmp(a.account,b.account)==0;
}
void refreshParts(const Sample& sample) noexcept {
 status.partCount=sample.partCount;for(unsigned i=0;i<sample.partCount;++i)status.parts[i]=sample.parts[i];
 status.partsAvailable=status.installed&&partBindingsReady&&!faulted&&sample.partCount==2&&!armed&&!inRestore&&!partTraveling&&!inPartDispatch&&(!request.pending()||partQueued);
}
CheckpointRequestContext partContext() noexcept {
 return {status.generation,now(),status.partsAvailable&&haveObservation&&fresh(lastObservation)&&fresh(input.time)&&input.connected!=0&&(!request.pending()||input.connected==queuedMask),focused(),input.menuOpen,input.neutral,input.protectedSave&&protectedSave()};
}
bool freshPartCheckpoint(const Sample& sample) noexcept;
bool observePartArrival(const Sample& sample) noexcept {
 if(!partTraveling)return true;
 if(sameTravelProfile(activePart.source,sample)&&sample.act==0&&sample.episode==activePart.target&&equalIgnoreCase(sample.map,activePart.map)) {
  if(sample.difficulty!=activePart.source.difficulty||sample.controllerPreference!=activePart.source.controllerPreference||!protectedSave()) {
   partTraveling=false;request.complete();faulted=true;status.available=status.partsAvailable=false;status.reason="Part start could not be verified; restart the game";return false;
  }
  if(!freshPartCheckpoint(sample)){
   if(!partTargetSeen)partTargetSeen=now();
   if(now()<partTargetSeen||now()-partTargetSeen>10000){partTraveling=false;request.complete();faulted=true;status.available=status.partsAvailable=false;status.reason="Target part checkpoint could not be verified; restart the game";return false;}
   status.available=status.partsAvailable=false;status.reason="Verifying the selected part's starting checkpoint...";return false;
  }
  partTraveling=false;request.complete();haveObservation=false;generation();status.targetPartEpisode=-1;status.reason="Selected part loaded; original saves remain protected";return true;
 }
 if(!sameTravelProfile(activePart.source,sample)||!sameView(activePart.source,sample)||now()<partStarted||now()-partStarted>90000) {
  partTraveling=false;request.complete();faulted=true;status.available=status.partsAvailable=false;status.reason="Unexpected mission context after part travel; restart the game";return false;
 }
 status.available=status.partsAvailable=false;status.reason="Loading selected part...";return false;
}
bool takePartJob(const Sample& source,PartJob* job) noexcept {
 if(!partQueued)return false;if(!request.pending()){partQueued=false;status.targetPartEpisode=-1;return false;}int target=-1;
 if(!request.take(partContext(),&target)){if(!request.pending()){partQueued=false;status.targetPartEpisode=-1;status.reason="Part switch cancelled; current part preserved";}return false;}
 int index=-1;for(unsigned i=0;i<source.partCount;++i)if(source.parts[i].episode==target)index=static_cast<int>(i);
 if(index<0||target==source.episode||!protectedSave()){request.complete();partQueued=false;status.reason="Part choice is no longer available";return false;}
 job->source=source;job->target=target;job->mask=queuedMask;std::memcpy(job->map,source.partMaps[index],sizeof(job->map));activePart=*job;
 partQueued=false;inPartDispatch=true;partStarted=now();partTargetSeen=0;status.available=status.partsAvailable=false;status.reason="Starting protected part travel...";return true;
}
enum class PartResult {Cancelled,Queued,Failed};
#ifndef CE_TEST_HOST
PartResult nativeDispatchPart(const PartJob& job) noexcept {
 bool started=false;
 __try {
  Sample freshSample{};const char* why="Part source changed";
  if(!partBindings()||inspect(reinterpret_cast<void*>(job.source.controller),&freshSample,&why)!=Inspection::Ready||!sameView(freshSample,job.source)||freshSample.difficulty>3||freshSample.difficulty!=job.source.difficulty||freshSample.controllerPreference!=job.source.controllerPreference)return PartResult::Cancelled;
  int target=-1;for(unsigned i=0;i<freshSample.partCount;++i)if(freshSample.parts[i].episode==job.target&&equalIgnoreCase(freshSample.partMaps[i],job.map))target=static_cast<int>(i);
  if(target<0)return PartResult::Cancelled;
  auto at=[](std::uintptr_t p){return *reinterpret_cast<const std::uintptr_t*>(p);};
  const auto engine=freshSample.engine,table=at(engine),flow=at(base+0x102a6a4);
  const int queuedSize=*reinterpret_cast<const int*>(freshSample.viewport+0x22c);
  if(!flow||at(flow+0x1c)!=engine||at(at(flow)+0x1c)!=base+0x36fee||at(table+0x1bc)!=base+0x396fbc||at(table+0x90)!=base+0x39906c
   ||(*reinterpret_cast<const unsigned*>(engine+0x1fc)&0x1c0000)||queuedSize<0||queuedSize>1
   ||(queuedSize==1&&(!at(freshSample.viewport+0x228)||*reinterpret_cast<const unsigned char*>(at(freshSample.viewport+0x228))!=0)))return PartResult::Cancelled;
  const auto levelInfo=at(at(freshSample.level+0x28));if(!levelInfo||at(at(levelInfo)+0x560)!=base+0x19366e)return PartResult::Cancelled;
  // The official replay path uses BLACK_BOX=-1. Verify its native validator
  // accepts that reset before allowing the dispatcher to mutate progress.
  if(*reinterpret_cast<const unsigned*>(levelInfo+0x70c)&0x8000){const int count=*reinterpret_cast<const int*>(levelInfo+0x153c);const auto data=at(levelInfo+0x1538);bool accepts=count==0;
   if(count<0||count>32||(count&&!data))return PartResult::Cancelled;
   for(int i=0;i<count;++i)if(*reinterpret_cast<const int*>(data+0x28*i)== -1)accepts=true;if(!accepts)return PartResult::Cancelled;}
  if(!protectedSave())return PartResult::Cancelled;const bool replayWasSet=(*reinterpret_cast<const unsigned*>(engine+0x1fc)&0x400000)!=0;
  InterlockedOr(reinterpret_cast<volatile LONG*>(engine+0x1fc),0x400000);started=true;
  using Replay=void(__stdcall*)(int,int,int,int);reinterpret_cast<Replay>(base+0x983822)(0,job.target,0,-1);
  char queuedMap[260]{};const auto manager=at(engine+0x264);
  const bool queued=manager==freshSample.manager&&*reinterpret_cast<const int*>(manager+12)==0&&*reinterpret_cast<const int*>(manager+16)==job.target
   &&at(manager+0xa8+4*freshSample.profileId)==freshSample.profile&&*reinterpret_cast<const unsigned char*>(freshSample.profile+0x154)==freshSample.difficulty
   &&*reinterpret_cast<const int*>(freshSample.profile+0x24)==freshSample.controllerPreference&&*reinterpret_cast<const int*>(engine+0x26c)== -1
   &&copyString(freshSample.viewport+0x228,queuedMap,sizeof(queuedMap))&&equalIgnoreCase(queuedMap,job.map);
  if(!queued&&!replayWasSet)InterlockedAnd(reinterpret_cast<volatile LONG*>(engine+0x1fc),static_cast<LONG>(~0x400000u));
  return queued?PartResult::Queued:PartResult::Failed;
 }__except(EXCEPTION_EXECUTE_HANDLER){return started?PartResult::Failed:PartResult::Cancelled;}
}
#endif
PartResult dispatchPart(const PartJob& job) noexcept {
 Input latest{};{Lock lock;latest=input;}
 if(!fresh(latest.time)||!focusedWindow(latest.window)||latest.menuOpen||!latest.neutral||latest.connected!=job.mask||!latest.connected||!latest.protectedSave||!protectedSave())return PartResult::Cancelled;
#ifdef CE_TEST_HOST
 if(testPartFailures&1)return PartResult::Cancelled;
 ++testPartCalls;testPartLast=job.target;testPartReentry=checkpointStatus().partLoading;
 return (testPartFailures&14)?PartResult::Failed:PartResult::Queued;
#else
 return nativeDispatchPart(job);
#endif
}
bool groupHash(const Sample& s,std::string* result) {
 BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;unsigned char digest[32]{};DWORD objectSize=0,done=0;
 if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)!=0)return false;
 bool ok=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&done,0)==0;
 std::vector<unsigned char> object(ok?objectSize:0);if(ok)ok=BCryptCreateHash(algorithm,&hash,object.data(),objectSize,nullptr,0,0)==0;unsigned char separator=0;
 if(ok)ok=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(s.account)),64,0)==0&&BCryptHashData(hash,&separator,1,0)==0&&BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(s.profileName)),static_cast<ULONG>(std::strlen(s.profileName)),0)==0&&BCryptFinishHash(hash,digest,sizeof(digest),0)==0;
 if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
 if(ok){const char* hex="0123456789abcdef";result->clear();for(unsigned char c:digest){result->push_back(hex[c>>4]);result->push_back(hex[c&15]);}}return ok;
}
bool copyArchive(const ArchiveRef& ref,unsigned char* destination) noexcept {
 __try {if(*reinterpret_cast<const std::uintptr_t*>(ref.object+0x3c)!=ref.data||*reinterpret_cast<const unsigned*>(ref.object+0x40)!=ref.size)return false;
  std::memcpy(destination,reinterpret_cast<const void*>(ref.data),ref.size);
  return *reinterpret_cast<const std::uintptr_t*>(ref.object+0x3c)==ref.data&&*reinterpret_cast<const unsigned*>(ref.object+0x40)==ref.size&&std::memcmp(destination,reinterpret_cast<const void*>(ref.data),ref.size)==0;
 } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void refreshChoices() noexcept {
 status.sectionCount=static_cast<unsigned>(snapshots.size());for(unsigned i=0;i<status.sectionCount;++i){const auto& r=*snapshots[i];auto& item=status.sections[i];item.id=r.token;item.checkpoint=r.info.checkpoint;item.initial=r.record.initial;
  if(r.record.initial)std::snprintf(item.label,sizeof(item.label),"Starting checkpoint %d",r.info.checkpoint);else std::snprintf(item.label,sizeof(item.label),"Checkpoint %d (recorded)",r.info.checkpoint);}
}
bool prepareOwner(const Sample& s) {
 if(haveOwner&&sameOwner(owner,s))return !libraryFault;owner=s;haveOwner=true;snapshots.clear();profileKey.clear();status.currentSection=-1;status.sectionCount=0;libraryFault=false;lastCapture=0;generation();
 if(!libraryReady||!groupHash(s,&profileKey)){libraryFault=true;status.reason="Checkpoint library account identity is unavailable";return false;}
 std::vector<CheckpointLibraryRecord> records;const auto loaded=loadCheckpointRecords(profileKey,s.map,s.act,s.episode,records);
 if(!loaded.ok){libraryFault=true;status.reason="Checkpoint library could not be read; existing files were preserved";return false;}
 for(auto& r:records){CheckpointArchiveInfo info{};if(!validateCheckpointArchive(r.bytes.data(),r.bytes.size(),&info)||info.checkpoint!=r.checkpoint){libraryFault=true;status.reason="Saved checkpoint metadata is invalid";return false;}
  int duplicate=-1;for(unsigned i=0;i<snapshots.size();++i)if(snapshots[i]->info.fingerprint==info.fingerprint&&snapshots[i]->record.bytes==r.bytes){duplicate=static_cast<int>(i);break;}
  auto entry=duplicate<0?std::make_shared<Snapshot>():std::make_shared<Snapshot>(*snapshots[duplicate]);
  entry->token=duplicate<0?static_cast<int>(snapshots.size()):snapshots[duplicate]->token;entry->info=info;
  if(r.initial)entry->initialSaved=true;else entry->normalSaved=true;
  if(duplicate<0||r.initial)entry->record=std::move(r);
  if(duplicate<0)snapshots.push_back(std::move(entry));else snapshots[duplicate]=std::move(entry);
 }
 refreshChoices();return true;
}
bool sourceMatchesSaved(const Sample& s,const std::vector<unsigned char>& data) noexcept {
 __try {if(!s.savedInfo||!s.savedData||s.savedSize!=data.size()||*reinterpret_cast<const int*>(s.savedInfo)!=12
  ||*reinterpret_cast<const unsigned char*>(s.savedInfo+0x18)!=s.act||*reinterpret_cast<const unsigned char*>(s.savedInfo+0x19)!=s.episode
  ||*reinterpret_cast<const std::uintptr_t*>(s.savedInfo+0x14)!=s.savedData||*reinterpret_cast<const unsigned*>(s.savedInfo+4)!=s.savedSize)return false;
  return std::memcmp(data.data(),reinterpret_cast<const void*>(s.savedData),data.size())==0;
 }__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool freshPartCheckpoint(const Sample& sample) noexcept {
 if(sample.current== -1)return true;if(sample.current<0||!sample.archivesReady)return false;
 try {std::vector<unsigned char> data(sample.last.size);CheckpointArchiveInfo info{};
  return copyArchive(sample.last,data.data())&&sourceMatchesSaved(sample,data)
   &&validateCheckpointArchive(data.data(),data.size(),&info)&&info.checkpoint==sample.current;
 }catch(...){return false;}
}
int capture(const Sample& s,const ArchiveRef& ref,bool initial) {
 std::vector<unsigned char> data(ref.size);if(!copyArchive(ref,data.data()))return -1;const auto fingerprint=checkpointFingerprint(data.data(),data.size());
 for(unsigned i=0;i<snapshots.size();++i)if(snapshots[i]->info.fingerprint==fingerprint&&snapshots[i]->record.bytes==data){
  const auto& old=*snapshots[i];if(old.normalSaved&&(!initial||old.initialSaved))return old.token;
  auto alias=std::make_shared<Snapshot>(old);auto record=old.record;
  if(!old.normalSaved){record.initial=false;if(!saveCheckpointRecord(profileKey,record).ok){libraryFault=true;return -1;}alias->normalSaved=true;}
  if(initial&&!old.initialSaved){record.initial=true;if(!saveCheckpointRecord(profileKey,record).ok){libraryFault=true;return -1;}alias->initialSaved=true;alias->record.initial=true;}
  snapshots[i]=std::move(alias);generation();refreshChoices();return snapshots[i]->token;
 }
 // A native buffer has no embedded map name. Only a matching current-profile
 // CheckpointInfo version/act/episode and exact BlockA bytes establish provenance.
 // An already-verified same-map record can be reused above; never relabel a new
 // old-map payload during Scene Select or a partially completed map load.
 if(!sourceMatchesSaved(s,data))return -1;
 CheckpointArchiveInfo info{};if(!validateCheckpointArchive(data.data(),data.size(),&info))return -1;int replace=-1;
 for(unsigned i=0;i<snapshots.size();++i)if(snapshots[i]->record.initial==initial&&snapshots[i]->info.checkpoint==info.checkpoint){replace=static_cast<int>(i);break;}
 if(replace<0&&snapshots.size()>=32){status.reason="Checkpoint library is full for this mission";return -1;}
 auto entry=std::make_shared<Snapshot>();entry->token=replace<0?static_cast<int>(snapshots.size()):snapshots[replace]->token;entry->info=info;entry->record={s.act,s.episode,info.checkpoint,initial,s.map,std::move(data)};
 auto record=entry->record;record.initial=false;
 if(!saveCheckpointRecord(profileKey,record).ok){libraryFault=true;status.reason="Checkpoint could not be saved; existing library files were preserved";return -1;}entry->normalSaved=true;
 if(initial){record.initial=true;if(!saveCheckpointRecord(profileKey,record).ok){libraryFault=true;status.reason="Starting checkpoint could not be saved";return -1;}entry->initialSaved=true;}
 if(replace<0)snapshots.push_back(std::move(entry));else snapshots[replace]=std::move(entry);generation();refreshChoices();return replace<0?snapshots.back()->token:snapshots[replace]->token;
}
bool queueFlag(const Sample& s) noexcept {
 __try {auto flags=reinterpret_cast<volatile LONG*>(s.engine+0x1fc);const LONG old=*flags;if((old&0xc0000)!=0)return false;return InterlockedCompareExchange(flags,old|0x80000,old)==old;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
void clearOwnedFlag() noexcept {
#ifndef CE_TEST_HOST
 __try {if(armed&&armedIdentity.engine&&*reinterpret_cast<const std::uintptr_t*>(armedIdentity.engine)==base+0xcb7fec)InterlockedAnd(reinterpret_cast<volatile LONG*>(armedIdentity.engine+0x1fc),static_cast<LONG>(~0x80000u));}__except(EXCEPTION_EXECUTE_HANDLER){}
#endif
}
void cancelArm() noexcept {clearOwnedFlag();armed=false;armedSnapshot.reset();request.complete();queuedMask=0;}
void unavailable(const char* why) noexcept {
 if(inRestore||inPartDispatch)return;if(haveObservation){generation();haveObservation=false;}lastObservation=0;status.available=status.partsAvailable=false;status.sectionCount=0;status.partCount=0;status.mission[0]=0;status.act=status.episode=status.currentHardCheckpoint=-1;
 if(partTraveling){status.reason="Loading selected part...";if(now()<partStarted||now()-partStarted>90000){partTraveling=false;request.complete();faulted=true;status.reason="Part travel timed out; restart the game";}return;}
 if(partQueued){partQueued=false;status.targetPartEpisode=-1;request.cancel();}
 if(armed)cancelArm();else request.cancel();status.reason=why;
}
void update(const Sample& s) {
 if(inRestore||inPartDispatch)return;if(!observePartArrival(s))return;if(haveObservation&&!sameView(observed,s)){generation();if(armed)cancelArm();else request.cancel();}
 observed=s;haveObservation=true;lastObservation=now();status.act=s.act;status.episode=s.episode;status.currentHardCheckpoint=s.current;std::memcpy(status.mission,s.mission,sizeof(status.mission));
 refreshParts(s);
 if(!prepareOwner(s)){status.available=false;return;}refreshChoices();
 if(s.archivesReady&&!armed&&(!lastCapture||now()<lastCapture||now()-lastCapture>=250)){lastCapture=now();capture(s,s.initial,true);const int current=capture(s,s.last,false);if(current>=0)status.currentSection=current;}
 status.available=!faulted&&!libraryFault&&!armed&&!partQueued&&!partTraveling&&!inPartDispatch&&s.archivesReady&&!snapshots.empty();status.persistent=libraryReady&&!libraryFault;
 if(armed){status.reason="Loading recorded checkpoint...";if(now()<loadStarted||now()-loadStarted>5000){cancelArm();faulted=true;status.available=false;status.reason="Checkpoint request timed out; restart the game";}return;}
 if(partQueued){status.reason="Release all controls to load the selected part";return;}
 if(!status.available){if(snapshots.empty()&&!libraryFault&&!faulted)status.reason="Waiting for the game's verified checkpoint data";return;}if(!request.pending()){status.reason="Verified saved checkpoints for the current campaign mission";return;}
 int token=-1;const bool wasPending=request.pending();if(!request.take(context(),&token)){if(wasPending&&!request.pending())status.reason="Checkpoint request cancelled; current state preserved";return;}
 std::shared_ptr<const Snapshot> chosen;for(const auto& r:snapshots)if(r->token==token)chosen=r;
 if(!chosen||!protectedSave()){request.complete();status.reason="Checkpoint request cancelled: save protection changed";return;}
 armedIdentity=s;armedSnapshot=chosen;loadStarted=now();
#ifdef CE_TEST_HOST
 armed=true;++dispatches;lastDispatch=token;
#else
 if(!bindings()||!queueFlag(s)){request.complete();armedSnapshot.reset();status.reason="Another game transition is already pending";return;}armed=true;
#endif
 status.available=false;status.reason="Loading recorded checkpoint...";
}
bool restoreMatches(const Sample& expected,int checkpoint) noexcept {
#ifdef CE_TEST_HOST
 (void)expected;return testPostMap&&testPostId==checkpoint;
#else
 __try {auto at=[](std::uintptr_t p){return *reinterpret_cast<const std::uintptr_t*>(p);};char map[260]{},name[128]{};const auto level=at(expected.engine+0x19c),manager=at(expected.engine+0x264);
  return level&&manager==expected.manager&&*reinterpret_cast<const int*>(manager+0x230)==expected.profileId
   &&at(manager+0xa8+4*expected.profileId)==expected.profile&&copyString(expected.profile,name,sizeof(name))&&std::strcmp(name,expected.profileName)==0
   &&*reinterpret_cast<const int*>(manager+12)==expected.act&&*reinterpret_cast<const int*>(manager+16)==expected.episode
   &&copyString(level+0x6c,map,sizeof(map))&&equalIgnoreCase(map,expected.map)&&*reinterpret_cast<const int*>(expected.engine+0x2a4)==checkpoint;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}
#endif
}
bool safeRestore(void* engine,const void* data,int size,int* result) noexcept {__try {*result=originalRestore(engine,data,size);return true;}__except(EXCEPTION_EXECUTE_HANDLER){return false;}}
bool copyRetryAnchor(void* engine,const Snapshot& selected) noexcept {
#ifdef CE_TEST_HOST
 (void)engine;(void)selected;++testAnchorCopies;return testAnchorOk;
#else
 __try {
  const auto address=reinterpret_cast<std::uintptr_t>(engine);const bool wasDisk=(*reinterpret_cast<const unsigned*>(address+0x1fc)&0x100000)!=0;
  using Copy=int(__thiscall*)(void*,const void*,int);
  const int copied=reinterpret_cast<Copy>(base+0x39a15c)(engine,selected.record.bytes.data(),static_cast<int>(selected.record.bytes.size()));
  // Native setter also marks disk loading; retain the original restore mode.
  if(!wasDisk)InterlockedAnd(reinterpret_cast<volatile LONG*>(address+0x1fc),static_cast<LONG>(~0x100000u));
  ArchiveRef current{};
  return copied!=0&&readArchive(*reinterpret_cast<const std::uintptr_t*>(address+0x2a0),&current)
   &&current.size==selected.record.bytes.size()&&std::memcmp(reinterpret_cast<const void*>(current.data),selected.record.bytes.data(),current.size)==0;
 } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
#endif
}
int __fastcall restoreHook(void* engine,void*,const void* data,int size) {
 
#ifdef CE_TEST_HOST
 const auto caller=testCaller;
#else
 const auto caller=reinterpret_cast<std::uintptr_t>(_ReturnAddress());
#endif
 std::shared_ptr<const Snapshot> selected;Sample expected{};bool cancel=false;
 {Lock lock;if(armed){if(caller!=base+0x39f808||reinterpret_cast<std::uintptr_t>(engine)!=armedIdentity.engine)cancelArm();else{
   Sample current{};const char* why="Checkpoint request context changed";
   
#ifdef CE_TEST_HOST
   current=observed;const bool inspected=haveObservation;(void)why;
#else
   const bool inspected=inspect(reinterpret_cast<void*>(armedIdentity.controller),&current,&why)==Inspection::Ready;
#endif
   CheckpointArchiveInfo verified{};
   const bool validPayload=armedSnapshot&&validateCheckpointArchive(armedSnapshot->record.bytes.data(),armedSnapshot->record.bytes.size(),&verified)
     &&verified.checkpoint==armedSnapshot->info.checkpoint&&verified.fingerprint==armedSnapshot->info.fingerprint;
   const bool ok=inspected&&validPayload&&sameView(current,armedIdentity)&&fresh(input.time)&&input.connected==queuedMask&&input.connected!=0&&focused()&&!input.menuOpen&&input.neutral&&input.protectedSave&&protectedSave()&&now()>=loadStarted&&now()-loadStarted<=5000
#ifndef CE_TEST_HOST
     &&bindings()
#endif
     &&data==reinterpret_cast<const void*>(current.last.data)&&size==static_cast<int>(current.last.size);
   if(!ok){cancelArm();status.reason="Checkpoint load cancelled because its context changed";cancel=true;}else{selected=armedSnapshot;expected=armedIdentity;armed=false;inRestore=true;status.available=false;}
  }}}
 if(cancel)return 0;if(!selected)return originalRestore(engine,data,size);
 // Own immutable bytes across native world teardown; no checkpoint lock held.
 int result=0;const bool completed=safeRestore(engine,selected->record.bytes.data(),static_cast<int>(selected->record.bytes.size()),&result);
 const bool restored=completed&&result!=0&&restoreMatches(expected,selected->info.checkpoint);
 const bool anchored=restored&&protectedSave()&&copyRetryAnchor(engine,*selected);
 {Lock lock;inRestore=false;armedSnapshot.reset();request.complete();haveObservation=false;lastObservation=0;generation();
  if(!restored||!anchored){faulted=true;status.available=false;status.reason="Checkpoint restoration could not be verified; restart the game";}
  else{status.currentHardCheckpoint=selected->info.checkpoint;status.currentSection=selected->token;status.reason="Checkpoint restored; original saves remain protected";}}
 return result;
}
}
bool initializeCheckpointControl(const std::filesystem::path& stateRoot) noexcept {
 try {Lock lock;
#ifdef CE_TEST_HOST
  (void)stateRoot;status.installed=true;libraryReady=true;partBindingsReady=true;status.persistent=false;originalRestore=reinterpret_cast<RestoreFn>(testOriginal);
#else
  const auto library=initializeCheckpointLibrary(stateRoot);libraryReady=library.ok;base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
  const unsigned char signature[]={0x55,0x8d,0x6c,0x24,0x90,0x81,0xec,0,2,0,0,0x56,0x8b,0xf1,0xe8};auto target=reinterpret_cast<void*>(base+0x3a1f02);
  status.installed=libraryReady&&bindings()&&bytes(reinterpret_cast<std::uintptr_t>(target),signature,sizeof(signature))&&MH_CreateHook(target,restoreHook,reinterpret_cast<void**>(&originalRestore))==MH_OK;
  if(status.installed&&MH_EnableHook(target)!=MH_OK){MH_RemoveHook(target);status.installed=false;originalRestore=nullptr;}status.persistent=libraryReady;partBindingsReady=partBindings();
#endif
  status.reason=status.installed?"Load a solo campaign checkpoint to access recorded checkpoints":"Checkpoint library or restore hook is unavailable";return status.installed;
 }catch(...){status.installed=false;status.reason="Checkpoint initialization failed";return false;}
}
CheckpointStatus checkpointStatus() noexcept {Lock lock;auto result=status;result.pending=request.pending();result.loading=request.loading()||inRestore||armed||inPartDispatch||partTraveling;result.partPending=partQueued&&request.pending();result.partLoading=inPartDispatch||partTraveling;if((result.available||result.partsAvailable)&&!fresh(lastObservation)){result.available=result.partsAvailable=false;if(!result.partLoading)result.reason="Waiting for a current campaign view";}return result;}
bool requestSection(int section,std::uint64_t selectedGeneration) noexcept {
 Lock lock;if(partQueued||partTraveling||inPartDispatch){status.reason="A part switch is already in progress";return false;}const auto c=context();
 if(selectedGeneration!=status.generation){status.reason="Checkpoint list changed; choose the checkpoint again";return false;}
 if(!c.focused){status.reason="Return focus to the game before loading a checkpoint";return false;}
 if(!fresh(input.time)||!input.connected){status.reason="Connect the controller and release its controls";return false;}
 if(!c.saveProtected){status.reason="Original save protection must be active and healthy";return false;}
 if(!c.ready||!c.menuOpen){status.reason="Current checkpoint data is not ready";return false;}
 int ids[32]{};for(unsigned i=0;i<snapshots.size();++i)ids[i]=snapshots[i]->token;
 const bool queued=request.queue(section,selectedGeneration,ids,snapshots.size(),c);if(queued)queuedMask=input.connected;status.reason=queued?"Release all controls to load the checkpoint":"Checkpoint choice is no longer available";return queued;
}
bool requestPart(int target,std::uint64_t selectedGeneration) noexcept {
 Lock lock;if(armed||inRestore||partTraveling||inPartDispatch||request.pending()||request.loading()){status.reason="Another checkpoint or part load is already in progress";return false;}
 const auto c=partContext();if(selectedGeneration!=status.generation){status.reason="Part list changed; choose the part again";return false;}
 if(!c.focused){status.reason="Return focus to the game before switching parts";return false;}
 if(!fresh(input.time)||!input.connected){status.reason="Connect the controller and release its controls";return false;}
 if(!c.saveProtected){status.reason="Original save protection must be active and healthy";return false;}
 if(!c.ready||!c.menuOpen){status.reason="Current mission has no verified A/B part choices";return false;}
 if(target==observed.episode){status.reason="Choose the other part of this mission";return false;}
 int ids[2]{};for(unsigned i=0;i<observed.partCount;++i)ids[i]=observed.parts[i].episode;
 if(!request.queue(target,selectedGeneration,ids,observed.partCount,c)){status.reason="Choose a verified part of the current mission";return false;}
 partQueued=true;queuedMask=input.connected;status.targetPartEpisode=target;status.available=false;status.reason="Release all controls to load the selected part";return true;
}
void cancelCheckpointRequest() noexcept {Lock lock;if(inPartDispatch||partTraveling)return;if(partQueued){partQueued=false;status.targetPartEpisode=-1;}if(armed)cancelArm();else request.cancel();}
void pollCheckpointInput(HWND window,unsigned connectedMask,bool menuOpen,bool neutral,bool practiceActiveHealthy) noexcept {Lock lock;input={window,connectedMask,menuOpen,neutral,practiceActiveHealthy,now()};}
void checkpointNativeView(void* controller) noexcept {
 PartJob job{};bool run=false;
 try {
  {Lock lock;if(!status.installed||faulted||inRestore||inPartDispatch)return;
#ifndef CE_TEST_HOST
   Sample sample{};const char* why="Load a solo campaign checkpoint to access recorded checkpoints";const auto result=inspect(controller,&sample,&why);if(result==Inspection::OtherViewport)return;if(result!=Inspection::Ready){unavailable(why);return;}update(sample);run=takePartJob(sample,&job);
#else
   (void)controller;
#endif
  }
  if(run){const auto result=dispatchPart(job);Lock lock;inPartDispatch=false;if(result==PartResult::Queued){partTraveling=true;status.reason="Loading selected part...";}
   else{request.complete();status.targetPartEpisode=-1;if(result==PartResult::Failed){faulted=true;status.available=status.partsAvailable=false;status.reason="Part travel could not be verified; restart the game";}else status.reason="Part switch cancelled; current part preserved";}}
 }catch(...){Lock lock;inPartDispatch=false;partQueued=false;request.complete();unavailable("Checkpoint or part operation could not complete");}
}
#ifdef CE_TEST_HOST
std::vector<unsigned char> testBlob(int checkpoint) {
 std::vector<unsigned char> raw{0x3a,0x6b,0x92,0xc1,1};
 auto index=[&](int n){unsigned v=static_cast<unsigned>(n);unsigned char b=static_cast<unsigned char>(v&63);v>>=6;if(v)b|=64;raw.push_back(b);while(v){b=static_cast<unsigned char>(v&127);v>>=7;if(v)b|=128;raw.push_back(b);}};
 index(checkpoint);raw.push_back(0);raw.push_back(0);std::vector<unsigned char> zipped{0x78,1,1,static_cast<unsigned char>(raw.size()),0,static_cast<unsigned char>(~raw.size()),255};
 zipped.insert(zipped.end(),raw.begin(),raw.end());unsigned a=1,b=0;for(unsigned char c:raw){a=(a+c)%65521;b=(b+a)%65521;}unsigned adler=(b<<16)|a;
 for(int i=3;i>=0;--i)zipped.push_back(static_cast<unsigned char>(adler>>(8*i)));
 std::vector<unsigned char> all{static_cast<unsigned char>(raw.size()),static_cast<unsigned char>(zipped.size())};all.insert(all.end(),zipped.begin(),zipped.end());return all;
}
void testCheckpointClock(std::uint64_t time) noexcept {Lock lock;testTime=time;}
void testCheckpointFocused(bool value) noexcept {Lock lock;testFocus=value;}
void testCheckpointStorage(bool value) noexcept {Lock lock;testStorage=value;}
void testCheckpointView(std::uint32_t identity,int current,const int* ids,unsigned count,bool valid) noexcept {
 try {Lock lock;if(!valid||!identity||(count&&!ids)||count>32){unavailable("Test campaign unavailable");return;}
  Sample sample{};sample.engine=1;sample.manager=2;sample.profile=testProfile;sample.viewport=sample.controller=sample.level=sample.episodeObject=identity;sample.profileId=sample.act=0;sample.episode=testEpisode;sample.current=current;sample.archivesReady=testArchives;sample.difficulty=static_cast<unsigned char>(testDifficulty);sample.last={1,0x11223344,3};std::snprintf(sample.profileName,sizeof(sample.profileName),"Fixture %u",testProfile);std::memcpy(sample.mission,"01_Market",10);std::memcpy(sample.map,"_Single\\01_Market.unr",22);
  if(testPartMaps)fixturePartMetadata(&sample);if(sample.partCount){const unsigned index=sample.episode==sample.parts[0].episode?0:1;std::snprintf(sample.map,sizeof(sample.map),"%s",sample.partMaps[index]);std::snprintf(sample.mission,sizeof(sample.mission),"Part %c",'A'+index);}
  if(partTraveling&&current>=0&&testPartArrivalFlags){
   testArrivalBytes=testBlob(current+((testPartArrivalFlags&8)?1:0));if(testPartArrivalFlags&4)testArrivalBytes.back()^=1;
   std::memset(testArrivalInfo,0,sizeof(testArrivalInfo));std::memset(testArrivalArchive,0,sizeof(testArrivalArchive));
   *reinterpret_cast<int*>(testArrivalInfo)=12;*reinterpret_cast<unsigned*>(testArrivalInfo+4)=static_cast<unsigned>(testArrivalBytes.size());*reinterpret_cast<std::uintptr_t*>(testArrivalInfo+0x14)=reinterpret_cast<std::uintptr_t>(testArrivalBytes.data());testArrivalInfo[0x18]=0;testArrivalInfo[0x19]=static_cast<unsigned char>(sample.episode-((testPartArrivalFlags&2)?1:0));
   *reinterpret_cast<std::uintptr_t*>(testArrivalArchive+0x3c)=reinterpret_cast<std::uintptr_t>(testArrivalBytes.data());*reinterpret_cast<unsigned*>(testArrivalArchive+0x40)=static_cast<unsigned>(testArrivalBytes.size());
   sample.savedInfo=reinterpret_cast<std::uintptr_t>(testArrivalInfo);sample.savedData=reinterpret_cast<std::uintptr_t>(testArrivalBytes.data());sample.savedSize=static_cast<unsigned>(testArrivalBytes.size());sample.last={reinterpret_cast<std::uintptr_t>(testArrivalArchive),sample.savedData,sample.savedSize};
  }
  if(!observePartArrival(sample))return;
  if(armed){if(current==armedSnapshot->info.checkpoint){armed=false;armedSnapshot.reset();request.complete();generation();}else if(!sameView(observed,sample)){cancelArm();faulted=true;status.reason="Wrong checkpoint restored";return;}}
  const bool changed=!haveObservation||!sameView(observed,sample);if(changed){generation();request.cancel();}observed=sample;haveObservation=true;lastObservation=now();status.act=0;status.episode=sample.episode;status.currentHardCheckpoint=current;std::memcpy(status.mission,sample.mission,sizeof(status.mission));
  status.currentSection=-1;snapshots.clear();for(unsigned i=0;i<count;++i){if(ids[i]==current)status.currentSection=ids[i];if(ids[i]<0){unavailable("Invalid test checkpoint");return;}auto item=std::make_shared<Snapshot>();item->token=ids[i];item->info.checkpoint=ids[i];item->record.initial=i==0;item->record.bytes=testBlob(ids[i]);validateCheckpointArchive(item->record.bytes.data(),item->record.bytes.size(),&item->info);snapshots.push_back(std::move(item));}
  refreshChoices();refreshParts(sample);status.available=!faulted&&!armed&&!partQueued&&!partTraveling&&sample.archivesReady&&!snapshots.empty();status.persistent=false;if(armed)return;if(partQueued){PartJob job{};if(takePartJob(sample,&job)){
   // The fixture uses the same unlocked protocol path as production.
   ReleaseSRWLockExclusive(&checkpointLock);const auto result=dispatchPart(job);AcquireSRWLockExclusive(&checkpointLock);inPartDispatch=false;
   if(result==PartResult::Queued){partTraveling=true;status.reason="Loading selected part...";}else{request.complete();if(result==PartResult::Failed){faulted=true;status.available=status.partsAvailable=false;status.reason="Part travel could not be verified; restart the game";}else status.reason="Part switch cancelled; current part preserved";}
  }return;}if(!request.pending()){status.reason="Recorded test checkpoint states";return;}
  int token=-1;if(!request.take(context(),&token))return;for(const auto& r:snapshots)if(r->token==token)armedSnapshot=r;if(!armedSnapshot){request.complete();return;}armedIdentity=sample;armed=true;loadStarted=now();status.available=false;++dispatches;lastDispatch=token;
 }catch(...){Lock lock;unavailable("Test checkpoint allocation failed");}
}
void testCheckpointReset() noexcept {Lock lock;status={};status.installed=true;request={};input={};observed={};owner={};armedIdentity={};snapshots.clear();armedSnapshot.reset();profileKey.clear();haveObservation=haveOwner=faulted=libraryFault=armed=inRestore=false;libraryReady=true;lastObservation=lastCapture=loadStarted=0;testTime=1000;testFocus=testStorage=true;dispatches=testOriginalCalls=0;lastDispatch=-1;testUsedOwned=testReentered=false;testPostMap=true;testOriginalResult=1;testPostId=-999;queuedMask=0;testAnchorCopies=0;testAnchorOk=true;testMaskInOriginal=false;originalRestore=reinterpret_cast<RestoreFn>(testOriginal);partBindingsReady=true;partQueued=partTraveling=inPartDispatch=false;activePart={};partStarted=partTargetSeen=0;testEpisode=0;testPartMaps=testArchives=true;testPartArrivalFlags=0;testArrivalBytes.clear();testDifficulty=1;testProfile=1;testPartFailures=testPartCalls=0;testPartLast=-1;testPartReentry=false;}
int testCheckpointTickRestore(bool expectedCaller,bool sourceMatches,int result,int restoredCheckpoint,bool mapMatches) noexcept {
 testCaller=expectedCaller?base+0x39f808:base+0x1234;testOriginalResult=result;testPostMap=mapMatches;
 {Lock lock;testPostId=restoredCheckpoint== -999&&armedSnapshot?armedSnapshot->info.checkpoint:restoredCheckpoint;}
 return restoreHook(reinterpret_cast<void*>(observed.engine),nullptr,reinterpret_cast<const void*>(sourceMatches?0x11223344:0x11223345),3);
}
void testCheckpointCorruptPending() noexcept {Lock lock;if(armedSnapshot){auto value=std::const_pointer_cast<Snapshot>(armedSnapshot);value->record.bytes.back()^=1;}}
bool testCheckpointProvenance(int sourceAct,int sourceEpisode,int currentAct,int currentEpisode,bool samePayload) noexcept {
 unsigned char payload[3]={1,2,3};unsigned char record[32]{};
 *reinterpret_cast<int*>(record)=12;*reinterpret_cast<unsigned*>(record+4)=3;*reinterpret_cast<std::uintptr_t*>(record+0x14)=reinterpret_cast<std::uintptr_t>(payload);record[0x18]=static_cast<unsigned char>(sourceAct);record[0x19]=static_cast<unsigned char>(sourceEpisode);
 Sample sample{};sample.act=currentAct;sample.episode=currentEpisode;sample.savedInfo=reinterpret_cast<std::uintptr_t>(record);sample.savedData=reinterpret_cast<std::uintptr_t>(payload);sample.savedSize=3;
 std::vector<unsigned char> data{1,2,static_cast<unsigned char>(samePayload?3:4)};return sourceMatchesSaved(sample,data);
}
void testCheckpointChangeMaskDuringOriginal(bool change) noexcept {Lock lock;testMaskInOriginal=change;}
void testCheckpointAnchorFailure(bool failure) noexcept {Lock lock;testAnchorOk=!failure;}
unsigned testCheckpointAnchorCopies() noexcept {Lock lock;return testAnchorCopies;}
unsigned testCheckpointOriginalCalls() noexcept {Lock lock;return testOriginalCalls;}
bool testCheckpointUsedOwned() noexcept {Lock lock;return testUsedOwned;}
bool testCheckpointReentered() noexcept {Lock lock;return testReentered;}
void testCheckpointMissionPart(int episode,bool mapsValid,unsigned difficulty,unsigned profileIdentity) noexcept {Lock lock;testEpisode=episode;testPartMaps=mapsValid;testDifficulty=difficulty;testProfile=profileIdentity;}
unsigned testCheckpointPartDispatches() noexcept {Lock lock;return testPartCalls;}
int testCheckpointLastPartDispatch() noexcept {Lock lock;return testPartLast;}
bool testCheckpointPartReentered() noexcept {Lock lock;return testPartReentry;}
void testCheckpointPartFailure(unsigned flags) noexcept {Lock lock;testPartFailures=flags;}
void testCheckpointPartArchives(bool ready) noexcept {Lock lock;testArchives=ready;}
void testCheckpointPartArrivalProvenance(unsigned flags) noexcept {Lock lock;testPartArrivalFlags=flags;}
unsigned testCheckpointDispatches() noexcept {Lock lock;return dispatches;}
int testCheckpointLastDispatch() noexcept {Lock lock;return lastDispatch;}
#endif
}
