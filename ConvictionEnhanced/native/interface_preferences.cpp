#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <array>
#include <utility>
#include <vector>
#include "interface_preferences.h"

namespace ce { namespace {
namespace fs=std::filesystem;
constexpr std::uint32_t allowedMask=127;
constexpr LONGLONG maximumBytes=4096;
std::atomic<unsigned long> tempSerial{0};
#ifdef CE_TEST_HOST
std::atomic<InterfacePreferencesFailure> injectedFailure{InterfacePreferencesFailure::None};
#endif
struct Handle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v=INVALID_HANDLE_VALUE):value(v){}
    Handle(Handle&& other) noexcept:value(std::exchange(other.value,INVALID_HANDLE_VALUE)){}
    Handle& operator=(Handle&& other) noexcept {
        if(this!=&other){close();value=std::exchange(other.value,INVALID_HANDLE_VALUE);}return *this;
    }
    Handle(const Handle&)=delete;
    Handle& operator=(const Handle&)=delete;
    ~Handle(){close();}
    void close(){if(value!=INVALID_HANDLE_VALUE){CloseHandle(value);value=INVALID_HANDLE_VALUE;}}
    explicit operator bool() const{return value!=INVALID_HANDLE_VALUE;}
};
InterfacePreferencesResult failure(const std::string& why) {InterfacePreferencesResult r;r.reason=why;return r;}
std::string ioReason(const char* action,DWORD error=GetLastError()) {
    return std::string(action)+" (Windows error "+std::to_string(error)+").";
}
bool validComponent(const std::wstring& component) {
    if(component.empty()||component==L"."||component==L".."||component.back()==L'.'||component.back()==L' ')return false;
    for(wchar_t c:component)if(c<32||std::wstring(L"<>:\"/\\|?*").find(c)!=std::wstring::npos)return false;
    return true;
}
bool preparePath(const fs::path& requested,fs::path& file,std::vector<Handle>& directories,std::string& why) {
    if(requested.empty()){why="No interface preferences path was supplied.";return false;}
    // Reject aliases before normalization, including alternate data streams.
    for(const auto& part:requested.relative_path())if(!validComponent(part.native())){
        why="The interface preferences path contains an unsafe component.";return false;
    }
    std::error_code error;
    file=fs::absolute(requested,error).lexically_normal();
    if(error){why="Could not resolve the interface preferences path.";return false;}
    const auto native=file.native();
    if(native.size()<4||native[1]!=L':'||(native[2]!=L'\\'&&native[2]!=L'/')||
       !((native[0]>=L'A'&&native[0]<=L'Z')||(native[0]>=L'a'&&native[0]<=L'z'))||file.filename().empty()){
        why="Interface preferences require a file on a local drive.";return false;
    }
    fs::path current=file.root_path();
    const auto holdDirectory=[&](const fs::path& path){
        // Omitting SHARE_DELETE prevents checked parent directories from being
        // renamed/replaced while the target is read or atomically committed.
        Handle h(CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
            OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(!h){why=ioReason("Could not open the preferences folder");return false;}
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(h.value,&info)){why=ioReason("Could not inspect the preferences folder");return false;}
        if(!(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)){
            why="The preferences folder must be a normal directory, without redirects.";return false;
        }
        directories.push_back(std::move(h));return true;
    };
    if(!holdDirectory(current))return false;
    for(const auto& component:file.parent_path().relative_path()){
        if(!validComponent(component.native())){why="The preferences folder contains an unsafe component.";return false;}
        current/=component;if(!holdDirectory(current))return false;
    }
    return true;
}
struct Contents {bool exists=false;DWORD attributes=0;std::string bytes;};
bool readContents(const fs::path& file,Contents& out,std::string& why) {
    Handle h(CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(!h){
        const DWORD error=GetLastError();
        if(error==ERROR_FILE_NOT_FOUND){out={};return true;}
        why=ioReason("Could not read interface preferences",error);return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(h.value,&info)){why=ioReason("Could not inspect interface preferences");return false;}
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||GetFileType(h.value)!=FILE_TYPE_DISK){
        why="Interface preferences must be a normal file, without redirects.";return false;
    }
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(h.value,&size)){why=ioReason("Could not measure interface preferences");return false;}
    if(size.QuadPart<0||size.QuadPart>maximumBytes){why="Interface preferences exceed the 4 KiB limit.";return false;}
    out.exists=true;out.attributes=info.dwFileAttributes;out.bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD received=0;
    if(size.QuadPart&&(!ReadFile(h.value,out.bytes.data(),static_cast<DWORD>(out.bytes.size()),&received,nullptr)||received!=out.bytes.size())){
        why=ioReason("Could not read the complete interface preferences file");return false;
    }
    return true;
}
bool parse(const std::string& bytes,InterfacePreferences& value,std::string& why) {
    std::array<std::string,4> values{};unsigned seen=0;bool section=false;
    std::size_t start=0;
    while(start<bytes.size()){
        auto end=bytes.find('\n',start);if(end==std::string::npos)end=bytes.size();
        auto line=bytes.substr(start,end-start);if(!line.empty()&&line.back()=='\r')line.pop_back();
        start=end==bytes.size()?end:end+1;
        if(!section){if(line!="[Interface]"){why="Interface preferences have an invalid section.";return false;}section=true;continue;}
        const auto equal=line.find('=');
        if(equal==std::string::npos){why="Interface preferences contain an incomplete setting.";return false;}
        const auto name=line.substr(0,equal);const auto entry=line.substr(equal+1);
        const char* names[]={"Schema","HiddenMask","AllHidden","CameraHint"};int index=-1;
        for(int i=0;i<4;++i)if(name==names[i])index=i;
        if(index<0||(seen&(1u<<index))){why="Interface preferences contain an unknown or repeated setting.";return false;}
        values[static_cast<std::size_t>(index)]=entry;seen|=1u<<index;
    }
    if(!section||seen!=15){why="Interface preferences are incomplete.";return false;}
    if(values[0]!="1"){why="Interface preferences use an unsupported schema.";return false;}
    const auto& mask=values[1];
    if(mask.empty()||mask.size()>3||(mask.size()>1&&mask[0]=='0')){why="Interface preferences have an invalid hidden-layer mask.";return false;}
    unsigned parsed=0;
    for(char c:mask){if(c<'0'||c>'9'){why="Interface preferences have an invalid hidden-layer mask.";return false;}parsed=parsed*10+static_cast<unsigned>(c-'0');}
    if(parsed&~allowedMask){why="Interface preferences include unsupported HUD layers.";return false;}
    if((values[2]!="0"&&values[2]!="1")||(values[3]!="0"&&values[3]!="1")){
        why="Interface preference switches must be 0 or 1.";return false;
    }
    value.hiddenMask=parsed;value.allHidden=values[2]=="1";value.cameraHint=values[3]=="1";return true;
}
std::string encode(const InterfacePreferences& value) {
    return "[Interface]\r\nSchema=1\r\nHiddenMask="+std::to_string(value.hiddenMask)+
        "\r\nAllHidden="+(value.allHidden?"1":"0")+"\r\nCameraHint="+(value.cameraHint?"1":"0")+"\r\n";
}
struct TempFile {
    fs::path path;Handle handle;
    ~TempFile(){handle.close();if(!path.empty())DeleteFileW(path.c_str());}
};
bool createTemp(const fs::path& file,TempFile& temporary,std::string& why) {
    for(int attempt=0;attempt<32;++attempt){
        temporary.path=file;temporary.path+=L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+
            std::to_wstring(GetTickCount64())+L"."+std::to_wstring(++tempSerial);
        temporary.handle=Handle(CreateFileW(temporary.path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL|FILE_FLAG_WRITE_THROUGH|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        if(temporary.handle)return true;
        const DWORD error=GetLastError();temporary.path.clear();
        if(error!=ERROR_FILE_EXISTS&&error!=ERROR_ALREADY_EXISTS){why=ioReason("Could not create temporary interface preferences",error);return false;}
    }
    why="Could not allocate a unique temporary preferences file.";return false;
}
}
InterfacePreferencesResult loadInterfacePreferences(const fs::path& requested) {
    fs::path file;std::vector<Handle> directories;std::string why;
    if(!preparePath(requested,file,directories,why))return failure(why);
    Contents contents;if(!readContents(file,contents,why))return failure(why);
    InterfacePreferencesResult result;
    if(!contents.exists){result.ok=true;result.missing=true;result.reason="No saved interface preferences; using visible defaults.";return result;}
    if(!parse(contents.bytes,result.value,why))return failure(why);
    result.ok=true;result.reason="Saved interface preferences loaded.";return result;
}
InterfacePreferencesResult saveInterfacePreferences(const fs::path& requested,const InterfacePreferences& value) {
    if(value.hiddenMask&~allowedMask)return failure("Cannot save unsupported HUD layers.");
    fs::path file;std::vector<Handle> directories;std::string why;
    if(!preparePath(requested,file,directories,why))return failure(why);
    Contents previous;if(!readContents(file,previous,why))return failure(why);
    if(previous.exists){
        InterfacePreferences existing;
        if(!parse(previous.bytes,existing,why))return failure(why+" Existing file was preserved.");
        if(previous.attributes&FILE_ATTRIBUTE_READONLY)return failure("Interface preferences are read-only; the existing file was preserved.");
    }
    TempFile temporary;if(!createTemp(file,temporary,why))return failure(why);
    const auto bytes=encode(value);DWORD written=0;
#ifdef CE_TEST_HOST
    if(injectedFailure==InterfacePreferencesFailure::Write){
        WriteFile(temporary.handle.value,bytes.data(),static_cast<DWORD>(bytes.size()/2),&written,nullptr);
        return failure("Injected partial preferences write failure.");
    }
#endif
    if(!WriteFile(temporary.handle.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr)||written!=bytes.size())
        return failure(ioReason("Could not write complete interface preferences"));
#ifdef CE_TEST_HOST
    if(injectedFailure==InterfacePreferencesFailure::Flush)return failure("Injected preferences flush failure.");
#endif
    if(!FlushFileBuffers(temporary.handle.value))return failure(ioReason("Could not flush interface preferences"));
    temporary.handle.close();
    Contents latest;if(!readContents(file,latest,why))return failure(why);
    if(latest.exists!=previous.exists||latest.bytes!=previous.bytes||latest.attributes!=previous.attributes)
        return failure("Interface preferences changed while saving; reload before trying again.");
#ifdef CE_TEST_HOST
    if(injectedFailure==InterfacePreferencesFailure::Replace)return failure("Injected preferences replacement failure.");
#endif
    const DWORD flags=MOVEFILE_WRITE_THROUGH|(previous.exists?MOVEFILE_REPLACE_EXISTING:0);
    if(!MoveFileExW(temporary.path.c_str(),file.c_str(),flags))return failure(ioReason("Could not replace interface preferences; the previous file was preserved"));
    temporary.path.clear();
    InterfacePreferencesResult result;result.ok=true;result.value=value;result.reason="Interface preferences saved.";return result;
}
#ifdef CE_TEST_HOST
void testInterfacePreferencesFailure(InterfacePreferencesFailure point){injectedFailure=point;}
#endif
}
