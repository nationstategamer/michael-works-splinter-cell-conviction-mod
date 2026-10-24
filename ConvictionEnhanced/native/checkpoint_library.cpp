#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#include <utility>
#include "checkpoint_library.h"
#include "checkpoint_archive.h"

namespace ce { namespace {
namespace fs=std::filesystem;
using Digest=std::array<unsigned char,32>;
constexpr unsigned char Magic[8]={'M','W','C','K','P','T','L','1'};
constexpr std::uint32_t Schema=1;
constexpr std::size_t HeaderSize=140,MaximumFile=MaxCheckpointCompressed+512;
constexpr std::uint64_t MaximumMissionBytes=64ull*1024*1024;
constexpr const char* ExeHash="beebb33559e064ec2f78065a4e9bc8dfe8844fa8711ea4afbcb880616170936f";
constexpr const char* FusionHash="41291418dd5b5e69c6758f81b06dff6ced8f54344661efc148a6146c5c084ce3";
constexpr const char* Maps[]={"01_Market","02_Museum","03_Airfield","04_FB-Coste","05_WashingtonMonument",
    "06_WindowlessBuilding","07_LincolnMemorial","08_ThirdEchelon_A","08_ThirdEchelon_B",
    "09_AbandonedReservoir_A","09_AbandonedReservoir_B","10_StreetsAndGarden_A","10_StreetsAndGarden_B",
    "11_WhiteHouse_A","11_WhiteHouse_B"};
std::mutex libraryMutex;
fs::path stateFolder,libraryFolder;
bool initialized=false;
std::atomic<unsigned long> serial{0};
#ifdef CE_TEST_HOST
CheckpointLibraryFailure injectedFailure=CheckpointLibraryFailure::None;
std::uint64_t missionBudget=MaximumMissionBytes;
#else
constexpr std::uint64_t missionBudget=MaximumMissionBytes;
#endif
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v=INVALID_HANDLE_VALUE):value(v){}
    Handle(Handle&& other)noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
    Handle& operator=(Handle&& other)noexcept{if(this!=&other){close();value=std::exchange(other.value,INVALID_HANDLE_VALUE);}return *this;}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    ~Handle(){close();}
    void close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
    explicit operator bool()const{return value!=INVALID_HANDLE_VALUE;}
};
struct DirectoryPin {Handle handle;fs::path path;BY_HANDLE_FILE_INFORMATION identity{};};
std::vector<DirectoryPin> pins;
CheckpointLibraryResult good(const char* text){return {true,text};}
CheckpointLibraryResult bad(const std::string& text){return {false,text};}
std::wstring extended(const fs::path& path){return L"\\\\?\\"+path.native();}
std::string io(const char* operation,DWORD error=GetLastError()){return std::string(operation)+" (Windows error "+std::to_string(error)+").";}
bool component(const std::wstring& value){
    if(value.empty()||value==L"."||value==L".."||value.back()==L'.'||value.back()==L' ')return false;
    for(wchar_t ch:value)if(ch<32||std::wstring(L"<>:\"/\\|?*").find(ch)!=std::wstring::npos)return false;
    return true;
}
bool pathAllowed(const fs::path& path){
    const auto text=path.native();
    if(!path.is_absolute()||text.size()<4||text.size()>30000||text[1]!=L':'||
        !((text[0]>=L'A'&&text[0]<=L'Z')||(text[0]>=L'a'&&text[0]<=L'z'))||path.lexically_normal()!=path)return false;
    if(path.root_name().native().size()!=2||GetDriveTypeW(path.root_path().c_str())==DRIVE_REMOTE)return false;
    for(const auto& part:path.relative_path())if(!component(part.native()))return false;
    return true;
}
bool identityEqual(const BY_HANDLE_FILE_INFORMATION& a,const BY_HANDLE_FILE_INFORMATION& b){
    return a.dwVolumeSerialNumber==b.dwVolumeSerialNumber&&a.nFileIndexHigh==b.nFileIndexHigh&&a.nFileIndexLow==b.nFileIndexLow;
}
bool pinDirectory(const fs::path& path,std::vector<DirectoryPin>& result,std::string& why){
    Handle handle(CreateFileW(extended(path).c_str(),FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!handle){why=io("Could not open checkpoint library directory");return false;}
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(handle.value,&info)){why=io("Could not inspect checkpoint library directory");return false;}
    if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){
        why="Checkpoint library directories cannot be redirected.";return false;
    }
    result.push_back({std::move(handle),path,info});return true;
}
bool checkPins(std::string& why){
    if(!initialized||pins.empty()){why="Checkpoint library is not initialized.";return false;}
    for(const auto& pin:pins){
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(pin.handle.value,&info)||!identityEqual(info,pin.identity)
            ||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){
            why="Checkpoint library directory identity changed.";return false;
        }
        const auto attributes=GetFileAttributesW(extended(pin.path).c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)){
            why="Checkpoint library directory is unavailable or redirected.";return false;
        }
    }
    return true;
}
bool unhex(const std::string& text,Digest& out){
    if(text.size()!=64)return false;
    auto digit=[](char ch)->int{if(ch>='0'&&ch<='9')return ch-'0';if(ch>='a'&&ch<='f')return ch-'a'+10;if(ch>='A'&&ch<='F')return ch-'A'+10;return -1;};
    for(unsigned i=0;i<32;++i){const int a=digit(text[i*2]),b=digit(text[i*2+1]);if(a<0||b<0)return false;out[i]=static_cast<unsigned char>((a<<4)|b);}
    return true;
}
std::string hex(const Digest& value){
    constexpr char digits[]="0123456789abcdef";std::string result;result.reserve(64);
    for(unsigned char ch:value){result+=digits[ch>>4];result+=digits[ch&15];}return result;
}
struct Algorithm {BCRYPT_ALG_HANDLE value=nullptr;~Algorithm(){if(value)BCryptCloseAlgorithmProvider(value,0);}};
struct Hash {BCRYPT_HASH_HANDLE value=nullptr;~Hash(){if(value)BCryptDestroyHash(value);}};
bool sha256(const unsigned char* bytes,std::size_t size,Digest& out){
    if(size>0xffffffffu)return false;
    Algorithm algorithm;if(BCryptOpenAlgorithmProvider(&algorithm.value,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    DWORD objectSize=0,received=0;
    if(BCryptGetProperty(algorithm.value,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&received,0)<0||objectSize>1024*1024)return false;
    std::vector<unsigned char> object(objectSize);Hash hash;
    if(BCryptCreateHash(algorithm.value,&hash.value,object.data(),objectSize,nullptr,0,0)<0)return false;
    if(size&&BCryptHashData(hash.value,const_cast<PUCHAR>(bytes),static_cast<ULONG>(size),0)<0)return false;
    return BCryptFinishHash(hash.value,out.data(),static_cast<ULONG>(out.size()),0)>=0;
}
void put(std::vector<unsigned char>& bytes,std::uint32_t value){for(unsigned i=0;i<4;++i)bytes.push_back(static_cast<unsigned char>(value>>(8*i)));}
std::uint32_t get(const std::vector<unsigned char>& bytes,std::size_t offset){
    return std::uint32_t(bytes[offset])|(std::uint32_t(bytes[offset+1])<<8)|(std::uint32_t(bytes[offset+2])<<16)|(std::uint32_t(bytes[offset+3])<<24);
}
bool equalFold(const std::string& a,const std::string& b){
    if(a.size()!=b.size())return false;
    for(std::size_t i=0;i<a.size();++i){char x=a[i],y=b[i];if(x>='A'&&x<='Z')x+=32;if(y>='A'&&y<='Z')y+=32;if(x!=y)return false;}return true;
}
bool mission(const std::string& input,int act,int episode,std::string& canonical){
    if(act!=0||episode<0||episode>=static_cast<int>(std::size(Maps)))return false;
    const std::string bare=Maps[episode];canonical="_Single\\"+bare+".unr";
    return equalFold(input,canonical)||equalFold(input,bare)||equalFold(input,bare+".unr");
}
struct Scope {Digest profile{};std::string map,group;int act=0,episode=-1;};
bool makeScope(const std::string& profile,const std::string& map,int act,int episode,Scope& scope,std::string& why){
    if(!unhex(profile,scope.profile)){why="Checkpoint profile identity is invalid.";return false;}
    if(!mission(map,act,episode,scope.map)){why="Checkpoint mission metadata is unsupported.";return false;}
    scope.act=act;scope.episode=episode;
    const char domain[]="MWCHECKPOINT-GROUP1";
    std::vector<unsigned char> bytes(domain,domain+sizeof(domain)-1);bytes.insert(bytes.end(),scope.profile.begin(),scope.profile.end());
    put(bytes,static_cast<unsigned>(act));put(bytes,static_cast<unsigned>(episode));bytes.insert(bytes.end(),scope.map.begin(),scope.map.end());
    Digest digest{};if(!sha256(bytes.data(),bytes.size(),digest)){why="Could not hash checkpoint mission identity.";return false;}
    scope.group=hex(digest);return true;
}
std::string recordName(const Scope& scope,bool initial,int checkpoint){
    const char domain[]="MWCHECKPOINT-RECORD1";std::vector<unsigned char> bytes(domain,domain+sizeof(domain)-1);
    bytes.insert(bytes.end(),scope.group.begin(),scope.group.end());put(bytes,initial?1:0);put(bytes,static_cast<unsigned>(checkpoint));
    Digest digest{};if(!sha256(bytes.data(),bytes.size(),digest))return {};
    return scope.group+"-"+hex(digest)+".mwcp";
}
struct Contents {bool exists=false;BY_HANDLE_FILE_INFORMATION identity{};std::vector<unsigned char> bytes;};
bool readFile(const fs::path& file,Contents& out,std::string& why){
    Handle handle(CreateFileW(extended(file).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_SEQUENTIAL_SCAN,nullptr));
    if(!handle){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND){out={};return true;}why=io("Could not read checkpoint record",error);return false;}
    if(!GetFileInformationByHandle(handle.value,&out.identity)){why=io("Could not inspect checkpoint record");return false;}
    if(GetFileType(handle.value)!=FILE_TYPE_DISK||out.identity.nNumberOfLinks!=1
        ||(out.identity.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){why="Checkpoint records must be regular files without redirects or hard links.";return false;}
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(handle.value,&size)||size.QuadPart<0||size.QuadPart>MaximumFile){why="Checkpoint record file size is invalid.";return false;}
    out.exists=true;out.bytes.resize(static_cast<std::size_t>(size.QuadPart));DWORD read=0;
    if(size.QuadPart&&(!ReadFile(handle.value,out.bytes.data(),static_cast<DWORD>(out.bytes.size()),&read,nullptr)||read!=out.bytes.size())){
        why=io("Could not read a complete checkpoint record");return false;
    }
    return true;
}
bool decode(const Scope& scope,const std::string& filename,const std::vector<unsigned char>& bytes,CheckpointLibraryRecord& record,std::string& why){
    if(bytes.size()<HeaderSize+32||std::memcmp(bytes.data(),Magic,8)!=0){why="Checkpoint library record header is invalid.";return false;}
    const unsigned schema=get(bytes,8),flags=get(bytes,12),act=get(bytes,16),episode=get(bytes,20),
        checkpoint=get(bytes,24),mapSize=get(bytes,28),payloadSize=get(bytes,32),expanded=get(bytes,36),reserved=get(bytes,40);
    if(schema!=Schema||(flags&~3u)||reserved||act!=static_cast<unsigned>(scope.act)||episode!=static_cast<unsigned>(scope.episode)
        ||mapSize!=scope.map.size()||payloadSize>MaxCheckpointCompressed||expanded>MaxCheckpointExpanded
        ||bytes.size()!=HeaderSize+32ull+mapSize+payloadSize){why="Checkpoint library metadata or schema is invalid.";return false;}
    Digest exe{},fusion{};unhex(ExeHash,exe);unhex(FusionHash,fusion);
    if(std::memcmp(bytes.data()+44,exe.data(),32)||std::memcmp(bytes.data()+76,fusion.data(),32)
        ||std::memcmp(bytes.data()+108,scope.profile.data(),32)||std::memcmp(bytes.data()+HeaderSize,scope.map.data(),mapSize)){
        why="Checkpoint record belongs to another profile, mission or game build.";return false;
    }
    Digest digest{};
    if(!sha256(bytes.data(),bytes.size()-32,digest)||std::memcmp(bytes.data()+bytes.size()-32,digest.data(),32)){
        why="Checkpoint record failed its SHA-256 integrity check.";return false;
    }
    const int signedCheckpoint=static_cast<std::int32_t>(checkpoint);const bool initial=(flags&1)!=0;
    if(filename!=recordName(scope,initial,signedCheckpoint)){why="Checkpoint record filename does not match its identity.";return false;}
    CheckpointArchiveInfo info{};const char* archiveReason=nullptr;
    const auto payload=bytes.data()+HeaderSize+mapSize;
    if(!validateCheckpointArchive(payload,payloadSize,&info,&archiveReason)){
        why=archiveReason?std::string("Checkpoint library archive: ")+archiveReason:"Checkpoint archive validation failed.";return false;
    }
    if(info.checkpoint!=signedCheckpoint||info.expandedSize!=expanded||info.containsActors!=((flags&2)!=0)){
        why="Checkpoint archive metadata disagrees with its record.";return false;
    }
    record={scope.act,scope.episode,signedCheckpoint,initial,scope.map,std::vector<unsigned char>(payload,payload+payloadSize)};return true;
}
bool collect(const Scope& scope,std::vector<CheckpointLibraryRecord>* records,unsigned& count,std::uint64_t& total,std::string& why){
    count=0;total=0;
    const auto pattern=libraryFolder/(scope.group+"-*.mwcp");WIN32_FIND_DATAW data{};
    HANDLE find=FindFirstFileW(extended(pattern).c_str(),&data);
    if(find==INVALID_HANDLE_VALUE){const auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND)return true;why=io("Could not list checkpoint library",error);return false;}
    struct Find {HANDLE handle;~Find(){FindClose(handle);}} owner{find};
    do {
        if(++count>MaxCheckpointLibraryRecords){why="This mission has more than 32 checkpoint records.";return false;}
        const std::wstring wide=data.cFileName;
        if(wide.size()!=134||(data.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))){why="Checkpoint library contains an unsafe mission record.";return false;}
        std::string name;name.reserve(wide.size());
        for(wchar_t ch:wide){if(ch>127){why="Checkpoint library record filename is invalid.";return false;}name+=static_cast<char>(ch);}
        if(name.compare(0,65,scope.group+"-")||name.compare(129,5,".mwcp")
            ||name.substr(65,64).find_first_not_of("0123456789abcdef")!=std::string::npos){why="Checkpoint library record filename is invalid.";return false;}
        Contents contents;if(!readFile(libraryFolder/wide,contents,why)||!contents.exists){if(why.empty())why="Checkpoint library changed while reading.";return false;}
        CheckpointLibraryRecord record;if(!decode(scope,name,contents.bytes,record,why))return false;
        total+=record.bytes.size();if(total>missionBudget){why="This mission's checkpoint library exceeds the 64 MiB limit.";return false;}
        if(records)records->push_back(std::move(record));
    } while(FindNextFileW(find,&data));
    const auto error=GetLastError();if(error!=ERROR_NO_MORE_FILES){why=io("Checkpoint library enumeration failed",error);return false;}return true;
}
bool same(const Contents& a,const Contents& b){
    return a.exists==b.exists&&(!a.exists||(identityEqual(a.identity,b.identity)
        &&a.identity.dwFileAttributes==b.identity.dwFileAttributes
        &&CompareFileTime(&a.identity.ftLastWriteTime,&b.identity.ftLastWriteTime)==0&&a.bytes==b.bytes));
}
struct Temporary {fs::path path;Handle handle;~Temporary(){handle.close();if(!path.empty())DeleteFileW(extended(path).c_str());}};
bool commit(const fs::path& file,const Contents& previous,const std::vector<unsigned char>& bytes,std::string& why){
    if(previous.exists&&(previous.identity.dwFileAttributes&FILE_ATTRIBUTE_READONLY)){why="Checkpoint record is read-only; the existing record was preserved.";return false;}
    Temporary temporary;
    for(unsigned attempt=0;attempt<16;++attempt){
        temporary.path=libraryFolder/(file.filename().native()+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++serial)+L".tmp");
        temporary.handle=Handle(CreateFileW(extended(temporary.path).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(temporary.handle)break;
        const auto error=GetLastError();temporary.path.clear();if(error!=ERROR_FILE_EXISTS&&error!=ERROR_ALREADY_EXISTS){why=io("Could not create checkpoint temporary file",error);return false;}
    }
    if(!temporary.handle){why="Could not reserve a checkpoint temporary file.";return false;}
    DWORD written=0;
#ifdef CE_TEST_HOST
    if(injectedFailure==CheckpointLibraryFailure::Write){WriteFile(temporary.handle.value,bytes.data(),static_cast<DWORD>(bytes.size()/2),&written,nullptr);why="Injected checkpoint write failure.";return false;}
#endif
    if(!WriteFile(temporary.handle.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size()){
        why=io("Could not write a complete checkpoint record");return false;
    }
#ifdef CE_TEST_HOST
    if(injectedFailure==CheckpointLibraryFailure::Flush){why="Injected checkpoint flush failure.";return false;}
#endif
    if(!FlushFileBuffers(temporary.handle.value)){why=io("Could not flush checkpoint record");return false;}
    temporary.handle.close();
    Contents latest;if(!readFile(file,latest,why))return false;
    if(!same(previous,latest)){why="Checkpoint record changed while saving; the existing file was preserved.";return false;}
    if(!checkPins(why))return false;
#ifdef CE_TEST_HOST
    if(injectedFailure==CheckpointLibraryFailure::Replace){why="Injected checkpoint replacement failure.";return false;}
#endif
    const DWORD flags=MOVEFILE_WRITE_THROUGH|(previous.exists?MOVEFILE_REPLACE_EXISTING:0);
    if(!MoveFileExW(extended(temporary.path).c_str(),extended(file).c_str(),flags)){why=io("Could not commit checkpoint record; the existing file was preserved");return false;}
    temporary.path.clear();return true;
}
}
CheckpointLibraryResult initializeCheckpointLibrary(const fs::path& root) noexcept {
    try {
        std::lock_guard<std::mutex> lock(libraryMutex);std::string why;
        if(!pathAllowed(root))return bad("Checkpoint library needs an existing normal directory on a local drive.");
        if(initialized){if(root!=stateFolder)return bad("Checkpoint library is already attached to another state directory.");return checkPins(why)?good("Checkpoint library ready."):bad(why);}
        std::vector<DirectoryPin> checked;fs::path current=root.root_path();
        if(!pinDirectory(current,checked,why))return bad(why);
        for(const auto& part:root.relative_path()){current/=part;if(!pinDirectory(current,checked,why))return bad(why);}
        const auto folder=root/L"checkpoint-library";
        if(!CreateDirectoryW(extended(folder).c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return bad(io("Could not create checkpoint library"));
        if(!pinDirectory(folder,checked,why))return bad(why);
        stateFolder=root;libraryFolder=folder;pins=std::move(checked);initialized=true;return good("Checkpoint library ready.");
    } catch(...){return bad("Library error");}
}
CheckpointLibraryResult saveCheckpointRecord(const std::string& profileHash64,const CheckpointLibraryRecord& record) noexcept {
    try {
        std::lock_guard<std::mutex> lock(libraryMutex);std::string why;Scope scope;
        if(!checkPins(why)||!makeScope(profileHash64,record.map,record.act,record.episode,scope,why))return bad(why);
        CheckpointArchiveInfo archive{};const char* archiveReason=nullptr;
        if(!validateCheckpointArchive(record.bytes.data(),record.bytes.size(),&archive,&archiveReason))
            return bad(archiveReason?std::string("Checkpoint was not stored: ")+archiveReason:"Checkpoint ID does not match archive.");
        if(archive.checkpoint!=record.checkpoint)return bad("Checkpoint ID does not match archive.");
        unsigned count=0;std::uint64_t total=0;if(!collect(scope,nullptr,count,total,why))return bad(why);
        const auto filename=recordName(scope,record.initial,record.checkpoint);if(filename.empty())return bad("Could not hash checkpoint record identity.");
        const auto target=libraryFolder/filename;Contents previous;if(!readFile(target,previous,why))return bad(why);
        if(previous.exists){CheckpointLibraryRecord old;if(!decode(scope,filename,previous.bytes,old,why))return bad(why);total-=old.bytes.size();}
        else if(count>=MaxCheckpointLibraryRecords)return bad("This mission already has 32 stored checkpoints.");
        if(total+record.bytes.size()>missionBudget)return bad("This mission's checkpoint library would exceed 64 MiB.");
        Digest exe{},fusion{};unhex(ExeHash,exe);unhex(FusionHash,fusion);
        std::vector<unsigned char> bytes(std::begin(Magic),std::end(Magic));
        put(bytes,Schema);put(bytes,(record.initial?1u:0u)|(archive.containsActors?2u:0u));
        put(bytes,static_cast<unsigned>(scope.act));put(bytes,static_cast<unsigned>(scope.episode));put(bytes,static_cast<unsigned>(record.checkpoint));
        put(bytes,static_cast<unsigned>(scope.map.size()));put(bytes,static_cast<unsigned>(record.bytes.size()));put(bytes,archive.expandedSize);put(bytes,0);
        bytes.insert(bytes.end(),exe.begin(),exe.end());bytes.insert(bytes.end(),fusion.begin(),fusion.end());bytes.insert(bytes.end(),scope.profile.begin(),scope.profile.end());
        bytes.insert(bytes.end(),scope.map.begin(),scope.map.end());bytes.insert(bytes.end(),record.bytes.begin(),record.bytes.end());
        Digest digest{};if(!sha256(bytes.data(),bytes.size(),digest))return bad("Could not hash checkpoint metadata and payload.");bytes.insert(bytes.end(),digest.begin(),digest.end());
        if(previous.exists&&previous.bytes==bytes)return good("Checkpoint already stored.");
        if(!commit(target,previous,bytes,why))return bad(why);
        return good("Checkpoint stored in the mod library.");
    } catch(...){return bad("Library error");}
}
CheckpointLibraryResult loadCheckpointRecords(const std::string& profileHash64,const std::string& map,int act,int episode,std::vector<CheckpointLibraryRecord>& out) noexcept {
    out.clear();
    try {
        std::lock_guard<std::mutex> lock(libraryMutex);std::string why;Scope scope;
        if(!checkPins(why)||!makeScope(profileHash64,map,act,episode,scope,why))return bad(why);
        std::vector<CheckpointLibraryRecord> loaded;unsigned count=0;std::uint64_t total=0;
        if(!collect(scope,&loaded,count,total,why))return bad(why);
        std::sort(loaded.begin(),loaded.end(),[](const auto& a,const auto& b){return a.initial!=b.initial?a.initial>b.initial:a.checkpoint<b.checkpoint;});
        out=std::move(loaded);return good(count?"Saved checkpoint library loaded.":"No saved checkpoints for this mission yet.");
    } catch(...){out.clear();return bad("Library error");}
}
#ifdef CE_TEST_HOST
void testCheckpointLibraryReset() noexcept {try{std::lock_guard<std::mutex> lock(libraryMutex);pins.clear();initialized=false;stateFolder.clear();libraryFolder.clear();injectedFailure=CheckpointLibraryFailure::None;missionBudget=MaximumMissionBytes;}catch(...){}}
void testCheckpointLibraryFailure(CheckpointLibraryFailure value) noexcept {try{std::lock_guard<std::mutex> lock(libraryMutex);injectedFailure=value;}catch(...){}}
void testCheckpointLibraryBudget(std::uint64_t bytes) noexcept {try{std::lock_guard<std::mutex> lock(libraryMutex);missionBudget=bytes;}catch(...){}}
fs::path testCheckpointLibraryPath(){std::lock_guard<std::mutex> lock(libraryMutex);return libraryFolder;}
#endif
}
