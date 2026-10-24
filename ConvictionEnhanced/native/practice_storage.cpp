#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "practice_storage.h"
#ifndef CE_TEST_HOST
#include "MinHook.h"
#endif

namespace ce { namespace {
namespace fs=std::filesystem;
constexpr unsigned MaxName=255,MaxPayload=64*1024*1024;
constexpr unsigned char Magic[8]={'M','W','P','R','A','C','T','1'};
std::recursive_mutex mutex;
PracticeStorageStatus state;
fs::path storageRoot,sessionRoot,sourceDirectory;
std::string accountDigest;
HANDLE directoryHandle=INVALID_HANDLE_VALUE;
std::vector<HANDLE> parentHandles;
std::vector<HANDLE> originalFileHandles;
std::unordered_map<std::string,bool> entries; // true means a deleted local entry
std::unordered_map<std::string,std::vector<unsigned char>> bootstrap;
std::unordered_map<std::string,bool> bootstrapExists;
std::size_t bootstrapBytes=0;
std::vector<std::string> deletedPrefixes;
std::atomic<unsigned> tempId{0};
unsigned originalCallsInFlight=0;
std::uint64_t rawDeletesBlocked=0;
std::uintptr_t base=0;
using WriteFn=unsigned(__thiscall*)(void*,const char*,const void*,unsigned);
using ReadFn=unsigned(__thiscall*)(void*,const char*,void*,unsigned);
using NameFn=bool(__thiscall*)(void*,const char*);
WriteFn originalWrite=nullptr;
ReadFn originalRead=nullptr;
NameFn originalExists=nullptr,originalDelete=nullptr,originalBulk=nullptr;
void* originalRawDelete=nullptr;
#ifdef CE_TEST_HOST
std::unordered_map<std::string,std::vector<unsigned char>> source;
std::unordered_map<unsigned,std::string> originalIdKeys;
PracticeOriginalCounts originalCounts;
PracticeStorageFailure failure=PracticeStorageFailure::None;
std::atomic<bool> pauseOriginalWrite{false},originalWritePaused{false},releaseOriginalWrite{false};
fs::path originalDirectoryOverride;
#endif

struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE h=INVALID_HANDLE_VALUE):value(h){}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    bool valid()const{return value!=INVALID_HANDLE_VALUE;}
    void close(){if(valid())CloseHandle(value);value=INVALID_HANDLE_VALUE;}
};
// Do not hold the storage mutex while the Ubisoft API waits for callbacks.
// Activation refuses an in-flight original call instead of waiting from a UI
// thread that an API callback might need.
struct OriginalCall {
    OriginalCall(){++originalCallsInFlight;}
    ~OriginalCall(){std::lock_guard<std::recursive_mutex> lock(mutex);--originalCallsInFlight;}
};
bool rawIdName(void* context,unsigned saveId,char out[MaxName+1]) noexcept {
#ifdef CE_TEST_HOST
    (void)context;const auto found=originalIdKeys.find(saveId);
    if(found==originalIdKeys.end()||found->second.size()>MaxName)return false;
    std::memcpy(out,found->second.c_str(),found->second.size()+1);return true;
#else
    __try {
        const auto owner=reinterpret_cast<std::uintptr_t>(context);
        if(owner!=base+0x10a2f18)return false;
        const auto records=*reinterpret_cast<const std::uintptr_t*>(owner+0x2c);
        const auto count=*reinterpret_cast<const unsigned*>(owner+0x30);
        if(!records||!count||count>1024)return false;
        for(unsigned index=0;index<count;++index) {
            const auto record=records+index*0x18;
            if(*reinterpret_cast<const unsigned*>(record)!=saveId)continue;
            const auto text=*reinterpret_cast<const wchar_t* const*>(record+4);
            const auto length=*reinterpret_cast<const unsigned*>(record+8),capacity=*reinterpret_cast<const unsigned*>(record+12);
            if(!text||!length||length>MaxName+1||capacity<length||capacity>4096)return false;
            for(unsigned i=0;i<length;++i) {
                const auto ch=text[i];
                if(!ch){out[i]=0;return i!=0&&i+1==length;}
                // Unknown encoding/case mappings invalidate all bootstrap data.
                if(ch<32||ch>126)return false;
                out[i]=static_cast<char>(ch>=L'A'&&ch<=L'Z'?ch+(L'a'-L'A'):ch);
            }
            return false;
        }
        return false;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
#endif
}
int rawDeleteEnter(void* context,unsigned saveId) noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    if(state.active){++rawDeletesBlocked;return 0;}
    try {
        char name[MaxName+1]{};
        if(rawIdName(context,saveId,name)) {
            const auto found=bootstrap.find(name);if(found!=bootstrap.end()){bootstrapBytes-=found->second.size();bootstrap.erase(found);}
            bootstrapExists.erase(name); // deletion may still fail, so absence is not assumed
        } else {bootstrap.clear();bootstrapExists.clear();bootstrapBytes=0;}
    } catch(...) {bootstrap.clear();bootstrapExists.clear();bootstrapBytes=0;}
    ++originalCallsInFlight;return 1;
}
void rawDeleteLeave() noexcept {
    std::lock_guard<std::recursive_mutex> lock(mutex);--originalCallsInFlight;
}
// The native helper's context is in EAX rather than ECX. Preserve all caller
// registers/flags while deciding, and preserve the original AL result on return.
__declspec(naked) void rawDeleteHook() {
    __asm {
        pushfd
        pushad
        push dword ptr [esp+40]
        push dword ptr [esp+32]
        call rawDeleteEnter
        add esp,8
        test eax,eax
        popad
        jnz passthrough
        popfd
        mov al,1
        ret 4
    passthrough:
        popfd
        push dword ptr [esp+4]
        call dword ptr [originalRawDelete]
        pushfd
        pushad
        call rawDeleteLeave
        popad
        popfd
        ret 4
    }
}
bool safeCopy(void* target,const void* from,unsigned size) noexcept {
    if(!size)return true;
    if(!target||!from)return false;
    __try {std::memcpy(target,from,size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool rawName(const char* name,char* out) noexcept {
    if(!name)return false;
    __try {
        for(unsigned i=0;i<=MaxName;++i) {
            const unsigned char ch=static_cast<unsigned char>(name[i]);out[i]=static_cast<char>(ch);
            if(!ch)return i!=0;
            // Native saves on this supported profile use bounded ANSI names.
            // No path can reach disk: all accepted names become SHA-256 keys.
            if(ch<32||ch==127||ch=='/'||ch=='\\'||ch==':'||ch=='?'||ch=='<'||ch=='>'||ch=='|'||ch=='"')return false;
        }
        return false;
    } __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool nameKey(const char* name,std::string& out,bool prefix=false) {
    char buffer[MaxName+1]{};if(!rawName(name,buffer))return false;
    out=buffer;
    if(out.find('*')!=std::string::npos&&!(prefix&&out=="*"))return false;
    // Byte-preserving except ASCII case, matching the game's generated names.
    for(char& ch:out)if(ch>='A'&&ch<='Z')ch=static_cast<char>(ch+('a'-'A'));
    return true;
}
bool prefixMatches(const std::string& name,const std::string& prefix) {
    return prefix=="*"||(name.size()>prefix.size()&&name.compare(0,prefix.size(),prefix)==0&&name[prefix.size()]=='@');
}
bool hiddenByPrefix(const std::string& key) {
    for(const auto& prefix:deletedPrefixes)if(prefixMatches(key,prefix))return true;
    return false;
}
void fail(const char* reason) {
    state.healthy=false;state.reason=reason;++state.blocked;
}
bool hashKey(const std::string& key,std::string& result) {
    BCRYPT_ALG_HANDLE algorithm=nullptr;BCRYPT_HASH_HANDLE hash=nullptr;
    DWORD bytes=0,objectBytes=0;unsigned char digest[32]{};
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0)return false;
    bool ok=BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectBytes),sizeof(objectBytes),&bytes,0)>=0;
    std::vector<unsigned char> object(ok?objectBytes:0);
    if(ok)ok=BCryptCreateHash(algorithm,&hash,object.data(),objectBytes,nullptr,0,0)>=0;
    if(ok)ok=BCryptHashData(hash,reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),static_cast<ULONG>(key.size()),0)>=0;
    if(ok)ok=BCryptFinishHash(hash,digest,sizeof(digest),0)>=0;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);
    if(!ok)return false;
    constexpr char hex[]="0123456789abcdef";result.clear();result.reserve(64);
    for(unsigned char value:digest){result+=hex[value>>4];result+=hex[value&15];}
    return true;
}
std::uint32_t crc32(const unsigned char* data,std::size_t size) {
    std::uint32_t crc=0xffffffffu;
    for(std::size_t i=0;i<size;++i){crc^=data[i];for(unsigned bit=0;bit<8;++bit)crc=(crc>>1)^(0xedb88320u&(0u-(crc&1u)));}
    return ~crc;
}
void put32(std::vector<unsigned char>& bytes,std::uint32_t value) {
    for(unsigned n=0;n<4;++n)bytes.push_back(static_cast<unsigned char>(value>>(8*n)));
}
std::uint32_t get32(const std::vector<unsigned char>& bytes,std::size_t at) {
    return static_cast<std::uint32_t>(bytes[at])|(static_cast<std::uint32_t>(bytes[at+1])<<8)
        |(static_cast<std::uint32_t>(bytes[at+2])<<16)|(static_cast<std::uint32_t>(bytes[at+3])<<24);
}
std::wstring winPath(const fs::path& path) {auto name=path.native();for(auto& ch:name)if(ch==L'/')ch=L'\\';return L"\\\\?\\"+name;}
bool plainFile(const fs::path& path,bool missingAllowed) {
    const DWORD attr=GetFileAttributesW(winPath(path).c_str());
    if(attr==INVALID_FILE_ATTRIBUTES)return missingAllowed&&(GetLastError()==ERROR_FILE_NOT_FOUND||GetLastError()==ERROR_PATH_NOT_FOUND);
    return !(attr&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY));
}
bool safeRootPath(const fs::path& path) {
    if(!path.is_absolute()||path.native().size()>30000||path.root_name().native().size()!=2||path.root_name().native()[1]!=L':')return false;
    if(path.lexically_normal()!=path)return false;
    const auto relative=path.relative_path().native();if(relative.find(L':')!=std::wstring::npos)return false;
    fs::path current=path.root_path();
    for(const auto& part:path.relative_path()) {
        if(part==L"."||part==L".."||part.empty())return false;
        current/=part;const DWORD attr=GetFileAttributesW(winPath(current).c_str());
        if(attr==INVALID_FILE_ATTRIBUTES||!(attr&FILE_ATTRIBUTE_DIRECTORY)||(attr&FILE_ATTRIBUTE_REPARSE_POINT))return false;
    }
    return true;
}
void closeParents() {
    for(HANDLE handle:originalFileHandles)CloseHandle(handle);originalFileHandles.clear();
    if(directoryHandle!=INVALID_HANDLE_VALUE){CloseHandle(directoryHandle);directoryHandle=INVALID_HANDLE_VALUE;}
    for(HANDLE handle:parentHandles)CloseHandle(handle);parentHandles.clear();
}
bool pinParents(const fs::path& path);
bool equalPath(const fs::path& a,const fs::path& b) {
    const auto left=a.native(),right=b.native();
    return CompareStringOrdinal(left.c_str(),static_cast<int>(left.size()),right.c_str(),static_cast<int>(right.size()),TRUE)==CSTR_EQUAL;
}
bool canonicalDirectory(const fs::path& path,fs::path& result) {
    if(!safeRootPath(path)){state.reason="Original save path is not a plain absolute directory";return false;}
    Handle handle(CreateFileW(winPath(path).c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!handle.valid()){state.reason="Original save directory could not be inspected";return false;}
    wchar_t finalName[32768]{};const auto count=GetFinalPathNameByHandleW(handle.value,finalName,32768,FILE_NAME_NORMALIZED);
    if(count<7||count>=32768||std::wmemcmp(finalName,L"\\\\?\\",4)!=0){state.reason="Original save directory identity is unavailable";return false;}
    result=fs::path(finalName+4);if(!safeRootPath(result)){state.reason="Canonical save directory is not supported";return false;}return true;
}
bool prepareOriginalDirectory() {
    fs::path candidate;
#ifdef CE_TEST_HOST
    if(originalDirectoryOverride.empty()) {
        std::string digest;if(!hashKey(storageRoot.parent_path().u8string(),digest))return false;
        if(!accountDigest.empty()&&accountDigest!=digest)return false;
        accountDigest=digest;sourceDirectory.clear();return true;
    }
    candidate=originalDirectoryOverride;
#else
    const auto marker=storageRoot.parent_path()/L"practice-originals.ini";
    if(!plainFile(marker,false))return false;
    wchar_t folder[32768]{};
    if(GetPrivateProfileIntW(L"Source",L"Schema",0,marker.c_str())!=1
        ||!GetPrivateProfileStringW(L"Source",L"Directory",L"",folder,32768,marker.c_str()))return false;
    candidate=fs::path(folder);
    wchar_t module[32768]{};const auto orbit=GetModuleHandleW(L"ubiorbitapi_r2.dll");
    if(!orbit||!GetModuleFileNameW(orbit,module,32768))return false;
    fs::path expected,actualParent;
    if(candidate.filename()!=L"2"||!canonicalDirectory(fs::path(module).parent_path()/L"savegames",expected)
        ||!canonicalDirectory(candidate.parent_path().parent_path(),actualParent)||!equalPath(actualParent,expected))return false;
#endif
    fs::path canonical;if(!canonicalDirectory(candidate,canonical))return false;
    std::string digest;if(!hashKey(canonical.u8string(),digest))return false;
    if(!accountDigest.empty()&&accountDigest!=digest)return false;
    sourceDirectory=canonical;accountDigest=digest;return true;
}
bool sourceInventory(std::vector<std::wstring>& result) {
    WIN32_FIND_DATAW entry{};HANDLE find=FindFirstFileW(winPath(sourceDirectory/L"*").c_str(),&entry);
    if(find==INVALID_HANDLE_VALUE)return false;
    bool ok=true;
    do {
        const std::wstring name=entry.cFileName;if(name==L"."||name==L"..")continue;
        if(entry.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) {ok=false;break;}
        const auto size=(static_cast<std::uint64_t>(entry.nFileSizeHigh)<<32)|entry.nFileSizeLow;
        if(size>MaxPayload||result.size()>=128){ok=false;break;}
        result.push_back(name);
    } while(FindNextFileW(find,&entry));
    if(ok&&GetLastError()!=ERROR_NO_MORE_FILES)ok=false;FindClose(find);
    std::sort(result.begin(),result.end());return ok&&!result.empty();
}
bool pinOriginalFiles() {
#ifdef CE_TEST_HOST
    if(sourceDirectory.empty())return true;
#endif
    if(!prepareOriginalDirectory()||!safeRootPath(sourceDirectory)||!pinParents(sourceDirectory))return false;
    std::vector<std::wstring> before,after;if(!sourceInventory(before))return false;
    std::uint64_t total=0;
    for(const auto& name:before) {
        HANDLE handle=CreateFileW(winPath(sourceDirectory/name).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return false;
        originalFileHandles.push_back(handle);BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(handle,&info)||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)))return false;
        const auto size=(static_cast<std::uint64_t>(info.nFileSizeHigh)<<32)|info.nFileSizeLow;total+=size;
        if(size>MaxPayload||total>256ull*1024*1024)return false;
    }
    return sourceInventory(after)&&before==after;
}
void cachePayload(const std::string& key,const void* data,unsigned size) {
    if(size>MaxPayload)return;
    const auto found=bootstrap.find(key);const auto previous=found==bootstrap.end()?0:found->second.size();
    if((found==bootstrap.end()&&bootstrap.size()>=128)||bootstrapBytes-previous+size>256ull*1024*1024)return;
    std::vector<unsigned char> payload(size);if(!safeCopy(payload.data(),data,size))return;
    bootstrapBytes=bootstrapBytes-previous+size;bootstrap[key]=std::move(payload);bootstrapExists[key]=true;
}
void discardPayload(const std::string& key) {
    const auto found=bootstrap.find(key);if(found!=bootstrap.end()){bootstrapBytes-=found->second.size();bootstrap.erase(found);}
}
bool pinParents(const fs::path& path) {
    fs::path current=path.root_path();
    for(const auto& part:path.relative_path()) {
        current/=part;
        HANDLE handle=CreateFileW(winPath(current).c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        if(handle==INVALID_HANDLE_VALUE)return false;
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(handle,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)
            ||!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)){CloseHandle(handle);return false;}
        parentHandles.push_back(handle);
    }
    return true;
}
bool newSession() {
    if(!safeRootPath(storageRoot.parent_path())||!pinParents(storageRoot.parent_path()))return false;
    if(!CreateDirectoryW(winPath(storageRoot).c_str(),nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS)return false;
    if(!safeRootPath(storageRoot))return false;
    if(!pinParents(storageRoot))return false;
    std::array<unsigned char,16> random{};
    if(BCryptGenRandom(nullptr,random.data(),static_cast<ULONG>(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)return false;
    constexpr wchar_t hex[]=L"0123456789abcdef";std::wstring suffix;
    for(unsigned char value:random){suffix+=hex[value>>4];suffix+=hex[value&15];}
    sessionRoot=storageRoot/(L"session-"+suffix);
    if(!CreateDirectoryW(winPath(sessionRoot).c_str(),nullptr))return false;
    directoryHandle=CreateFileW(winPath(sessionRoot).c_str(),FILE_LIST_DIRECTORY|FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
    return directoryHandle!=INVALID_HANDLE_VALUE&&safeRootPath(sessionRoot);
}
bool atomicRecord(const std::string& key,unsigned kind,const void* data,unsigned size) {
    if(size>MaxPayload)return false;
    std::string digest;if(!hashKey(key,digest))return false;
    const fs::path file=sessionRoot/(digest+(kind==2?".prefix":".save"));
    if(!plainFile(file,true))return false;
    std::vector<unsigned char> bytes(std::begin(Magic),std::end(Magic));
    put32(bytes,kind);put32(bytes,static_cast<unsigned>(key.size()));put32(bytes,size);
    bytes.insert(bytes.end(),key.begin(),key.end());const auto old=bytes.size();bytes.resize(old+size);
    if(!safeCopy(bytes.data()+old,data,size))return false;
    put32(bytes,crc32(bytes.data(),bytes.size()));
    const fs::path temporary=sessionRoot/(digest+"."+std::to_string(++tempId)+".tmp");
    Handle out(CreateFileW(winPath(temporary).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!out.valid())return false;
    DWORD written=0;bool ok=true;
#ifdef CE_TEST_HOST
    if(failure==PracticeStorageFailure::Write){WriteFile(out.value,bytes.data(),static_cast<DWORD>(bytes.size()/2),&written,nullptr);ok=false;}
    else
#endif
    ok=WriteFile(out.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)&&written==bytes.size();
#ifdef CE_TEST_HOST
    if(failure==PracticeStorageFailure::Flush)ok=false;
#endif
    if(ok)ok=FlushFileBuffers(out.value)!=FALSE;
    out.close();
#ifdef CE_TEST_HOST
    if(failure==PracticeStorageFailure::Replace)ok=false;
#endif
    if(ok)ok=MoveFileExW(winPath(temporary).c_str(),winPath(file).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!ok)DeleteFileW(winPath(temporary).c_str()); // only this exclusively-created temporary
    return ok;
}
bool readRecord(const std::string& key,std::vector<unsigned char>& payload,bool& deleted) {
    std::string digest;if(!hashKey(key,digest))return false;
    const auto file=sessionRoot/(digest+".save");if(!plainFile(file,false))return false;
    Handle in(CreateFileW(winPath(file).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!in.valid())return false;
    LARGE_INTEGER length{};
    if(!GetFileSizeEx(in.value,&length)||length.QuadPart<24||length.QuadPart>MaxPayload+MaxName+24)return false;
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length.QuadPart));DWORD read=0;
    if(!ReadFile(in.value,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=bytes.size())return false;
    if(std::memcmp(bytes.data(),Magic,8)!=0)return false;
    const unsigned kind=get32(bytes,8),nameSize=get32(bytes,12),size=get32(bytes,16);
    if(kind>1||nameSize!=key.size()||size>MaxPayload||bytes.size()!=24ull+nameSize+size
        ||std::memcmp(bytes.data()+20,key.data(),nameSize)!=0||get32(bytes,bytes.size()-4)!=crc32(bytes.data(),bytes.size()-4)
        ||(kind==1&&size!=0))return false;
    deleted=kind==1;payload.assign(bytes.begin()+20+nameSize,bytes.end()-4);return true;
}
bool validContext(void* self) {
#ifdef CE_TEST_HOST
    return self==reinterpret_cast<void*>(1);
#else
    return reinterpret_cast<std::uintptr_t>(self)==base+0x10a2f18;
#endif
}
unsigned __fastcall writeHook(void* self,void*,const char* name,const void* data,unsigned size) noexcept {
    try {
        std::unique_lock<std::recursive_mutex> lock(mutex);
        if(!state.active) {
            OriginalCall call;lock.unlock();const auto result=originalWrite(self,name,data,size);lock.lock();
            try {std::string key;if(nameKey(name,key)){discardPayload(key);if(size&&result==size)cachePayload(key,data,size);}}
            catch(...) {} // optional read cache must not change normal saving
            return result;
        }
        if(!state.healthy){++state.blocked;return 0;}
        std::string key;
        if(!validContext(self)||!nameKey(name,key)||size>MaxPayload){fail("Practice write refused an unsupported save request");return 0;}
        if(!atomicRecord(key,0,data,size)){fail("Practice save could not be written; original saves remain protected");return 0;}
        entries[key]=false;++state.writes;state.reason="Practice saves are isolated until the game exits";return size;
    } catch(...) {std::lock_guard<std::recursive_mutex> lock(mutex);fail("Practice save allocation failed; original saves remain protected");return 0;}
}
unsigned __fastcall readHook(void* self,void*,const char* name,void* data,unsigned size) noexcept {
    try {
        std::unique_lock<std::recursive_mutex> lock(mutex);
        if(!state.active) {
            OriginalCall call;lock.unlock();const auto result=originalRead(self,name,data,size);lock.lock();
            try {std::string key;if(size&&nameKey(name,key)){discardPayload(key);if(result&&result<size)cachePayload(key,data,result);}}
            catch(...) {} // a complete original read is optional practice bootstrap
            return result;
        }
        if(!state.healthy){++state.blocked;return 0;}
        std::string key;
        if(!validContext(self)||!nameKey(name,key)||size>MaxPayload){fail("Practice read refused an unsupported save request");return 0;}
        ++state.reads;const auto found=entries.find(key);
        if(found!=entries.end()) {
            bool deleted=false;std::vector<unsigned char> payload;
            if(!readRecord(key,payload,deleted)||deleted!=found->second){fail("Practice save failed its integrity check; original saves remain protected");return 0;}
            if(deleted)return 0;
            const auto amount=std::min(size,static_cast<unsigned>(payload.size()));
            if(!safeCopy(data,payload.data(),amount)){fail("Practice read destination was unavailable");return 0;}
            return amount;
        }
        if(hiddenByPrefix(key))return 0;
        const auto cached=bootstrap.find(key);
        if(cached!=bootstrap.end()) {
            const auto amount=std::min(size,static_cast<unsigned>(cached->second.size()));
            if(!safeCopy(data,cached->second.data(),amount)){fail("Practice read destination was unavailable");return 0;}
            return amount;
        }
        const auto known=bootstrapExists.find(key);if(known!=bootstrapExists.end()&&!known->second)return 0;
        fail("Original save data was not cached; restart to leave protected practice mode");return 0;
    } catch(...) {std::lock_guard<std::recursive_mutex> lock(mutex);fail("Practice read allocation failed; original saves remain protected");return 0;}
}
bool __fastcall existsHook(void* self,void*,const char* name) noexcept {
    try {
        std::unique_lock<std::recursive_mutex> lock(mutex);
        if(!state.active) {
            OriginalCall call;lock.unlock();const bool result=originalExists(self,name);lock.lock();
            try {std::string key;if(nameKey(name,key)&&(bootstrapExists.count(key)||bootstrapExists.size()<256))bootstrapExists[key]=result;}
            catch(...) {}
            return result;
        }
        if(!state.healthy){++state.blocked;return false;}
        std::string key;if(!validContext(self)||!nameKey(name,key)){fail("Practice lookup refused an unsupported save request");return false;}
        const auto found=entries.find(key);
        if(found!=entries.end()) {
            bool deleted=false;std::vector<unsigned char> payload;
            if(!readRecord(key,payload,deleted)||deleted!=found->second){fail("Practice save failed its integrity check; original saves remain protected");return false;}
            return !deleted;
        }
        if(hiddenByPrefix(key))return false;
        const auto cached=bootstrapExists.find(key);return cached!=bootstrapExists.end()&&cached->second;
    } catch(...) {std::lock_guard<std::recursive_mutex> lock(mutex);fail("Practice lookup failed; original saves remain protected");return false;}
}
bool __fastcall deleteHook(void* self,void*,const char* name) noexcept {
    try {
        std::unique_lock<std::recursive_mutex> lock(mutex);
        if(!state.active) {
            OriginalCall call;lock.unlock();const bool result=originalDelete(self,name);lock.lock();
            try {std::string key;if(result&&nameKey(name,key)){discardPayload(key);bootstrapExists[key]=false;}}catch(...) {}
            return result;
        }
        if(!state.healthy){++state.blocked;return false;}
        std::string key;if(!validContext(self)||!nameKey(name,key)){fail("Practice delete refused an unsupported save request");return false;}
        if(!atomicRecord(key,1,nullptr,0)){fail("Practice deletion could not be recorded; original saves remain protected");return false;}
        entries[key]=true;++state.deletes;return true; // idempotent logical deletion in this practice namespace
    } catch(...) {std::lock_guard<std::recursive_mutex> lock(mutex);fail("Practice deletion failed; original saves remain protected");return false;}
}
bool __fastcall bulkHook(void* self,void*,const char* name) noexcept {
    try {
        std::unique_lock<std::recursive_mutex> lock(mutex);
        if(!state.active) {
            OriginalCall call;lock.unlock();const bool result=originalBulk(self,name);lock.lock();
            try {std::string prefix;if(result&&nameKey(name,prefix,true)) {
                for(auto it=bootstrap.begin();it!=bootstrap.end();)if(prefixMatches(it->first,prefix)){bootstrapBytes-=it->second.size();it=bootstrap.erase(it);}else++it;
                for(auto& entry:bootstrapExists)if(prefixMatches(entry.first,prefix))entry.second=false;
            }}catch(...) {}
            return result;
        }
        if(!state.healthy){++state.blocked;return false;}
        std::string prefix;if(!validContext(self)||!nameKey(name,prefix,true)){fail("Practice profile deletion refused an unsupported request");return false;}
        if(!atomicRecord(prefix,2,nullptr,0)){fail("Practice profile deletion could not be recorded; original saves remain protected");return false;}
        deletedPrefixes.push_back(prefix);
        for(auto& entry:entries)if(prefixMatches(entry.first,prefix)) {
            if(!atomicRecord(entry.first,1,nullptr,0)){fail("Practice profile deletion was incomplete; original saves remain protected");return false;}
            entry.second=true;
        }
        ++state.deletes;return true; // original Orbit remove functions are never called
    } catch(...) {std::lock_guard<std::recursive_mutex> lock(mutex);fail("Practice profile deletion failed; original saves remain protected");return false;}
}
bool bytesMatch(std::uintptr_t address,const unsigned char* bytes,unsigned size) noexcept {
    __try{return std::memcmp(reinterpret_cast<const void*>(address),bytes,size)==0;}
    __except(EXCEPTION_EXECUTE_HANDLER){return false;}
}
bool signatures() noexcept {
    const unsigned char write[]={0x55,0x8b,0xec,0x83,0xec,0x50,0x53,0x33,0xc0,0x8b,0xd9,0x39,0x03,0x56,0x57};
    const unsigned char read[]={0x55,0x8b,0xec,0x83,0xec,0x24,0x53,0x33,0xc0,0x8b,0xd9,0x39,0x03,0x56,0x57};
    const unsigned char exists[]={0x55,0x8b,0xec,0x53,0x8d,0x45,0x08,0x50,0xff,0x75,0x08,0x8b,0xd9,0xe8,0x05,0,0,0};
    const unsigned char remove[]={0x55,0x8b,0xec,0x51,0x51,0x33,0xc0,0x53,0x8b,0xd9,0x39,0x03,0x89,0x45,0xf8,0x88,0x45,0xff};
    const unsigned char bulk[]={0x55,0x8b,0xec,0x83,0xec,0x34,0x83,0x65,0xf4,0,0x53,0x8b,0xd9,0x56};
    const unsigned char raw[]={0x53,0x56,0x57,0x8b,0xf8,0x8b,0x37,0x32,0xdb,0x85,0xf6,0x74,0x35};
    const unsigned char map[]={0x8b,0x51,0x24,0x85,0xd2,0x74,0x0b,0x3b,0x41,0x28,0x73,0x06,0x6b,0xc0,0x18,0x03,0xc2,0xc3};
    return bytesMatch(base+0xad8c,write,sizeof(write))&&bytesMatch(base+0xaf75,read,sizeof(read))
        &&bytesMatch(base+0xaa9e,exists,sizeof(exists))&&bytesMatch(base+0xabee,remove,sizeof(remove))
        &&bytesMatch(base+0xac44,bulk,sizeof(bulk))&&bytesMatch(base+0xab93,raw,sizeof(raw))&&bytesMatch(base+0x9f83,map,sizeof(map));
}
#ifdef CE_TEST_HOST
unsigned __fastcall fakeWrite(void*,void*,const char* name,const void* data,unsigned size) {
    if(pauseOriginalWrite.exchange(false)) {
        originalWritePaused=true;while(!releaseOriginalWrite.load())Sleep(1);originalWritePaused=false;
    }
    std::lock_guard<std::recursive_mutex> lock(mutex);
    ++originalCounts.writes;std::string key;if(!nameKey(name,key))return 0;
    std::vector<unsigned char> bytes(size);if(!safeCopy(bytes.data(),data,size))return 0;source[key]=bytes;return size;
}
unsigned __fastcall fakeRead(void*,void*,const char* name,void* data,unsigned size) {
    std::lock_guard<std::recursive_mutex> lock(mutex);
    ++originalCounts.reads;std::string key;if(!nameKey(name,key))return 0;const auto found=source.find(key);if(found==source.end())return 0;
    const unsigned amount=std::min(size,static_cast<unsigned>(found->second.size()));return safeCopy(data,found->second.data(),amount)?amount:0;
}
bool __fastcall fakeExists(void*,void*,const char* name){std::lock_guard<std::recursive_mutex> lock(mutex);++originalCounts.exists;std::string key;return nameKey(name,key)&&source.count(key)!=0;}
bool __fastcall fakeDelete(void*,void*,const char* name){std::lock_guard<std::recursive_mutex> lock(mutex);++originalCounts.deletes;std::string key;return nameKey(name,key)&&source.erase(key)!=0;}
bool __fastcall fakeBulk(void*,void*,const char* name){std::lock_guard<std::recursive_mutex> lock(mutex);++originalCounts.bulkDeletes;std::string prefix;if(!nameKey(name,prefix,true))return false;for(auto it=source.begin();it!=source.end();)if(prefixMatches(it->first,prefix))it=source.erase(it);else++it;return true;}
void fakeRawDeleteCount(){std::lock_guard<std::recursive_mutex> lock(mutex);++originalCounts.rawDeletes;}
__declspec(naked) void fakeRawDelete(){
    __asm {
        pushad
        call fakeRawDeleteCount
        popad
        mov al,0
        ret 4
    }
}
#endif
}

bool initializePracticeStorage(const std::filesystem::path& modStateRoot) noexcept {
    try {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if(state.installed)return true;
        if(!safeRootPath(modStateRoot)){state.reason="Practice save folder is unavailable or unsafe";return false;}
        storageRoot=modStateRoot/L"practice-saves";
        // The module can load before the Ubisoft API. Retry identity discovery
        // when the library asks for its binding or practice is activated.
        prepareOriginalDirectory();
#ifdef CE_TEST_HOST
        originalWrite=reinterpret_cast<WriteFn>(fakeWrite);originalRead=reinterpret_cast<ReadFn>(fakeRead);
        originalExists=reinterpret_cast<NameFn>(fakeExists);originalDelete=reinterpret_cast<NameFn>(fakeDelete);originalBulk=reinterpret_cast<NameFn>(fakeBulk);
        originalRawDelete=reinterpret_cast<void*>(fakeRawDelete);
#else
        base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        if(!signatures()){state.reason="Save functions differ from the supported build";return false;}
        struct Hook {std::uintptr_t offset;void* replacement;void** original;};
        const Hook hooks[]={
            {0xad8c,reinterpret_cast<void*>(writeHook),reinterpret_cast<void**>(&originalWrite)},
            {0xaf75,reinterpret_cast<void*>(readHook),reinterpret_cast<void**>(&originalRead)},
            {0xaa9e,reinterpret_cast<void*>(existsHook),reinterpret_cast<void**>(&originalExists)},
            {0xabee,reinterpret_cast<void*>(deleteHook),reinterpret_cast<void**>(&originalDelete)},
            {0xac44,reinterpret_cast<void*>(bulkHook),reinterpret_cast<void**>(&originalBulk)},
            {0xab93,reinterpret_cast<void*>(rawDeleteHook),&originalRawDelete}};
        unsigned created=0;bool ok=true;
        for(const auto& hook:hooks){if(MH_CreateHook(reinterpret_cast<void*>(base+hook.offset),hook.replacement,hook.original)!=MH_OK){ok=false;break;}++created;}
        if(ok)for(const auto& hook:hooks)if(MH_QueueEnableHook(reinterpret_cast<void*>(base+hook.offset))!=MH_OK){ok=false;break;}
        if(ok)ok=MH_ApplyQueued()==MH_OK;
        if(!ok) {
            for(unsigned i=0;i<created;++i)MH_QueueDisableHook(reinterpret_cast<void*>(base+hooks[i].offset));
            MH_ApplyQueued();for(unsigned i=0;i<created;++i)MH_RemoveHook(reinterpret_cast<void*>(base+hooks[i].offset));
            state.reason="Save isolation hooks could not all be installed";return false;
        }
#endif
        state.installed=true;state.healthy=true;state.reason="Save protection ready; normal saves are active";return true;
    } catch(...) {std::lock_guard<std::recursive_mutex> lock(mutex);state.reason="Save protection initialization failed";return false;}
}
bool activatePracticeStorage() noexcept {
    try {
        std::lock_guard<std::recursive_mutex> lock(mutex);
        if(state.active)return state.healthy;
        if(!state.installed||!state.healthy)return false;
        if(originalCallsInFlight){state.reason="Wait for the current save operation to finish, then retry checkpoint loading";return false;}
        if(!prepareOriginalDirectory()){state.reason="The original save location is not verified; checkpoint loading is disabled";return false;}
        if(!newSession()){closeParents();state.reason="Practice save folder could not be created; checkpoint loading is disabled";return false;}
        if(!pinOriginalFiles()){closeParents();state.reason="Original saves are busy or changed; wait for saving to finish and retry";return false;}
        state.active=true;state.reason="Practice saves are isolated until the game exits";return true;
    } catch(...) {std::lock_guard<std::recursive_mutex> lock(mutex);closeParents();state.reason="Practice save activation failed; checkpoint loading is disabled";return false;}
}
PracticeStorageStatus practiceStorageStatus() noexcept {std::lock_guard<std::recursive_mutex> lock(mutex);auto result=state;result.deletes+=rawDeletesBlocked;return result;}
bool practiceAccountKey(char out[65]) noexcept {
    try {std::lock_guard<std::recursive_mutex> lock(mutex);if(!state.installed)return false;if(accountDigest.empty()&&!prepareOriginalDirectory())return false;
        return accountDigest.size()==64&&safeCopy(out,accountDigest.c_str(),65);}
    catch(...){return false;}
}

#ifdef CE_TEST_HOST
void testPracticeReset() noexcept {std::lock_guard<std::recursive_mutex> lock(mutex);closeParents();state={};entries.clear();deletedPrefixes.clear();bootstrap.clear();bootstrapExists.clear();bootstrapBytes=0;storageRoot.clear();sessionRoot.clear();sourceDirectory.clear();accountDigest.clear();originalDirectoryOverride.clear();source.clear();originalIdKeys.clear();originalCounts={};rawDeletesBlocked=0;failure=PracticeStorageFailure::None;pauseOriginalWrite=false;originalWritePaused=false;releaseOriginalWrite=false;}
void testPracticeSource(const char* name,const void* data,unsigned size){std::lock_guard<std::recursive_mutex> lock(mutex);std::string key;if(!nameKey(name,key))return;std::vector<unsigned char> bytes(size);if(!safeCopy(bytes.data(),data,size))return;source[key]=bytes;}
unsigned testPracticeWrite(const char* name,const void* data,unsigned size) noexcept{return writeHook(reinterpret_cast<void*>(1),nullptr,name,data,size);}
unsigned testPracticeRead(const char* name,void* data,unsigned size) noexcept{return readHook(reinterpret_cast<void*>(1),nullptr,name,data,size);}
bool testPracticeExists(const char* name) noexcept{return existsHook(reinterpret_cast<void*>(1),nullptr,name);}
bool testPracticeDelete(const char* name) noexcept{return deleteHook(reinterpret_cast<void*>(1),nullptr,name);}
bool testPracticeBulkDelete(const char* name) noexcept{return bulkHook(reinterpret_cast<void*>(1),nullptr,name);}
bool testPracticeRawIdDelete(unsigned saveId) noexcept {
    unsigned result=0;
    __asm {
        mov eax,1
        push saveId
        call rawDeleteHook
        movzx eax,al
        mov result,eax
    }
    return result!=0;
}
PracticeOriginalCounts testPracticeOriginalCounts() noexcept {std::lock_guard<std::recursive_mutex> lock(mutex);return originalCounts;}
void testPracticeFailure(PracticeStorageFailure value) noexcept {std::lock_guard<std::recursive_mutex> lock(mutex);failure=value;}
std::filesystem::path testPracticeSessionPath(){std::lock_guard<std::recursive_mutex> lock(mutex);return sessionRoot;}
void testPracticePauseNextOriginalWrite() noexcept {releaseOriginalWrite=false;originalWritePaused=false;pauseOriginalWrite=true;}
bool testPracticeOriginalWritePaused() noexcept {return originalWritePaused.load();}
void testPracticeReleaseOriginalWrite() noexcept {releaseOriginalWrite=true;}
void testPracticeOriginalDirectory(const std::filesystem::path& directory){std::lock_guard<std::recursive_mutex> lock(mutex);if(state.active)return;originalDirectoryOverride=directory;sourceDirectory=directory;accountDigest.clear();}
void testPracticeSourceId(unsigned saveId,const char* logicalName){std::lock_guard<std::recursive_mutex> lock(mutex);std::string key;if(nameKey(logicalName,key))originalIdKeys[saveId]=key;}
#endif
}
