#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "interface_preferences.h"

namespace {
namespace fs=std::filesystem;
int checks=0,skips=0;
void require(bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);}
bool same(const ce::InterfacePreferences& a,const ce::InterfacePreferences& b){
    return a.hiddenMask==b.hiddenMask&&a.allHidden==b.allHidden&&a.cameraHint==b.cameraHint;
}
std::string readBytes(const fs::path& file){
    std::ifstream input(file,std::ios::binary);
    require(static_cast<bool>(input),"Could not read fixture file");
    return {std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>()};
}
void writeBytes(const fs::path& file,const std::string& bytes){
    std::ofstream output(file,std::ios::binary|std::ios::trunc);
    output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));output.close();
    require(static_cast<bool>(output),"Could not write fixture file");
}
std::size_t tempCount(const fs::path& file){
    std::size_t count=0;const auto prefix=file.filename().native()+L".tmp.";
    for(const auto& item:fs::directory_iterator(file.parent_path()))if(item.path().filename().native().find(prefix)==0)++count;
    return count;
}
std::string canonical(unsigned mask=0,bool all=false,bool hint=true){
    return "[Interface]\r\nSchema=1\r\nHiddenMask="+std::to_string(mask)+"\r\nAllHidden="+(all?"1":"0")+"\r\nCameraHint="+(hint?"1":"0")+"\r\n";
}
struct Fixture {
    fs::path work,root;
    explicit Fixture(const fs::path& requested){
        work=fs::absolute(requested).lexically_normal();fs::create_directories(work);
        root=work/(L"interface-preferences-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        require(fs::create_directory(root),"Fixture directory already exists");
    }
    ~Fixture(){
        std::error_code error;
        const auto resolvedRoot=fs::weakly_canonical(root,error);if(error)return;
        const auto resolvedWork=fs::weakly_canonical(work,error);if(error||resolvedRoot.parent_path()!=resolvedWork)return;
        // Only remove the unique, verified direct child of the test-work root.
        fs::remove_all(resolvedRoot,error);
    }
};
void defaultsAndRoundTrips(const fs::path& root){
    const auto file=root/L"roundtrip.ini";
    auto result=ce::loadInterfacePreferences(file);
    require(result.ok&&result.missing&&same(result.value,{}),"Missing preferences did not return visible defaults");
    require(!fs::exists(file)&&tempCount(file)==0,"Loading missing preferences wrote a file");
    for(unsigned mask=0;mask<128;++mask)for(int all=0;all<2;++all)for(int hint=0;hint<2;++hint){
        const ce::InterfacePreferences expected{mask,all!=0,hint!=0};
        result=ce::saveInterfacePreferences(file,expected);
        require(result.ok&&!result.missing&&same(result.value,expected),"Valid preference combination did not save");
        const auto bytes=readBytes(file);
        require(bytes==canonical(mask,all!=0,hint!=0),"Saved preferences were not canonical schema 1");
        result=ce::loadInterfacePreferences(file);
        require(result.ok&&!result.missing&&same(result.value,expected),"Preference combination did not round-trip");
        require(readBytes(file)==bytes&&tempCount(file)==0,"Loading rewrote preferences or saving left a temporary file");
    }
    writeBytes(file,"[Interface]\nCameraHint=0\nAllHidden=1\nHiddenMask=127\nSchema=1");
    result=ce::loadInterfacePreferences(file);
    require(result.ok&&same(result.value,{127,true,false}),"Valid LF/reordered fields were rejected");
}
void malformedFilesArePreserved(const fs::path& root){
    const auto file=root/L"malformed.ini";
    std::vector<std::string> invalid={
        "","[Other]\nSchema=1\nHiddenMask=0\nAllHidden=0\nCameraHint=1\n",
        "[Interface]\nHiddenMask=0\nAllHidden=0\nCameraHint=1\n",
        "[Interface]\nSchema=1\nHiddenMask=0\nAllHidden=0\n",
        canonical()+"Schema=1\n",canonical()+"Extra=1\n",canonical()+"\n",
        "[Interface]\nSchema=2\nHiddenMask=0\nAllHidden=0\nCameraHint=1\n",
        "[Interface]\nSchema=999\nHiddenMask=0\nAllHidden=0\nCameraHint=1\n",
        "[Interface]\nSchema=1\nHiddenMask=128\nAllHidden=0\nCameraHint=1\n",
        "[Interface]\nSchema=1\nHiddenMask=-1\nAllHidden=0\nCameraHint=1\n",
        "[Interface]\nSchema=1\nHiddenMask=00\nAllHidden=0\nCameraHint=1\n",
        "[Interface]\nSchema=1\nHiddenMask=1x\nAllHidden=0\nCameraHint=1\n",
        "[Interface]\nSchema=1\nHiddenMask=999999999999\nAllHidden=0\nCameraHint=1\n",
        "[Interface]\nSchema=1\nHiddenMask=0\nAllHidden=true\nCameraHint=1\n",
        "[Interface]\nSchema=1\nHiddenMask=0\nAllHidden=2\nCameraHint=1\n",
        "[Interface]\nSchema=1\nHiddenMask=0\nAllHidden=0\nCameraHint=-1\n",
        "[Interface]\nSchema=1\nHiddenMask=0\nAllHidden=0\nCameraHint=\n",
        "[Interface]\nSchema =1\nHiddenMask=0\nAllHidden=0\nCameraHint=1\n",
        std::string("\xef\xbb\xbf")+canonical(),std::string(4097,'X'),
    };
    auto nul=canonical();nul.insert(nul.find("HiddenMask"),1,'\0');invalid.push_back(nul);
    auto partial=canonical(127,true,false);partial.resize(partial.size()-5);invalid.push_back(partial);
    for(const auto& bytes:invalid){
        writeBytes(file,bytes);
        auto result=ce::loadInterfacePreferences(file);
        require(!result.ok&&!result.missing&&!result.reason.empty()&&same(result.value,{}),"Malformed/future preferences did not report failure with defaults");
        require(readBytes(file)==bytes,"Loading malformed preferences altered the file");
        result=ce::saveInterfacePreferences(file,{7,true,false});
        require(!result.ok&&same(result.value,{})&&!result.reason.empty(),"Saving silently replaced malformed/future preferences");
        require(readBytes(file)==bytes&&tempCount(file)==0,"Failed save altered malformed preferences or left a temporary file");
    }
}
void failedWritesPreserveExistingState(const fs::path& root){
    const auto file=root/L"atomic.ini";
    require(ce::saveInterfacePreferences(file,{3,true,false}).ok,"Could not establish atomic-save baseline");
    const auto original=readBytes(file);
    const ce::InterfacePreferencesFailure failures[]={ce::InterfacePreferencesFailure::Write,ce::InterfacePreferencesFailure::Flush,ce::InterfacePreferencesFailure::Replace};
    for(const auto point:failures){
        ce::testInterfacePreferencesFailure(point);
        const auto result=ce::saveInterfacePreferences(file,{127,false,true});
        ce::testInterfacePreferencesFailure(ce::InterfacePreferencesFailure::None);
        require(!result.ok&&!result.reason.empty(),"Injected partial-write/flush/replace failure reported success");
        require(readBytes(file)==original&&tempCount(file)==0,"Interrupted save changed prior preferences or left its temporary file");
        const auto missing=root/(L"missing-"+std::to_wstring(static_cast<int>(point))+L".ini");
        ce::testInterfacePreferencesFailure(point);
        const auto missingResult=ce::saveInterfacePreferences(missing,{127,true,false});
        ce::testInterfacePreferencesFailure(ce::InterfacePreferencesFailure::None);
        require(!missingResult.ok&&!fs::exists(missing)&&tempCount(missing)==0,"Interrupted first save did not preserve absence");
    }
    HANDLE lock=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(lock!=INVALID_HANDLE_VALUE,"Could not lock target for replacement-failure test");
    const auto blocked=ce::saveInterfacePreferences(file,{127,false,true});CloseHandle(lock);
    require(!blocked.ok&&!blocked.reason.empty(),"Locked target replacement reported success");
    require(readBytes(file)==original&&tempCount(file)==0,"Real replacement failure changed the file or leaked a temporary file");
    require(SetFileAttributesW(file.c_str(),FILE_ATTRIBUTE_READONLY)!=FALSE,"Could not make fixture read-only");
    const auto readonlyLoad=ce::loadInterfacePreferences(file);
    const auto readonlySave=ce::saveInterfacePreferences(file,{7,false,true});
    const BOOL restored=SetFileAttributesW(file.c_str(),FILE_ATTRIBUTE_NORMAL);
    require(restored!=FALSE,"Could not restore fixture attributes");
    require(readonlyLoad.ok&&!readonlySave.ok&&readBytes(file)==original,"Read-only preferences were unreadable or overwritten");
    const auto orphan=fs::path(file.native()+L".tmp.stale");writeBytes(orphan,"partial orphaned write");
    require(ce::saveInterfacePreferences(file,{7,false,true}).ok,"Stale temporary file prevented a new unique save");
    require(readBytes(orphan)=="partial orphaned write"&&tempCount(file)==1,"Saving reused or deleted an unrelated temporary file");
    require(same(ce::loadInterfacePreferences(file).value,{7,false,true}),"Save did not recover after a replacement failure");
}
void invalidValuesAndUnsafePaths(const fs::path& root){
    const auto file=root/L"safe.ini";
    require(ce::saveInterfacePreferences(file,{5,false,true}).ok,"Could not establish safe-path baseline");
    const auto bytes=readBytes(file);
    for(unsigned mask:{128u,255u,0xffffffffu}){
        const auto result=ce::saveInterfacePreferences(file,{mask,true,false});
        require(!result.ok&&readBytes(file)==bytes&&tempCount(file)==0,"Unsupported save mask changed valid preferences");
    }
    const auto directory=root/L"directory-target.ini";fs::create_directory(directory);
    const std::vector<fs::path> unsafe={directory,fs::path(file.native()+L":stream"),fs::path(file.native()+L"."),
        root/L"missing-parent"/L"settings.ini",file/L"child.ini",root/L".."/L"escape.ini",fs::path()};
    for(const auto& path:unsafe){
        const auto load=ce::loadInterfacePreferences(path);
        const auto save=ce::saveInterfacePreferences(path,{7,true,false});
        require(!load.ok&&!save.ok&&same(load.value,{})&&same(save.value,{}),"Unsafe path was read or written");
        require(readBytes(file)==bytes,"Unsafe path altered the original file");
    }
    const auto fileLink=root/L"file-link.ini";
    if(CreateSymbolicLinkW(fileLink.c_str(),file.c_str(),0x2)){
        const auto load=ce::loadInterfacePreferences(fileLink),save=ce::saveInterfacePreferences(fileLink,{7,true,false});
        require(!load.ok&&!save.ok&&readBytes(file)==bytes,"Reparse target was followed or overwritten");
        require(DeleteFileW(fileLink.c_str())!=FALSE,"Could not remove fixture file link");
    }else {++skips;std::cout<<"SKIP: file reparse fixture unavailable (Windows error "<<GetLastError()<<").\n";}
    const auto folder=root/L"real-folder";fs::create_directory(folder);writeBytes(folder/L"settings.ini",canonical());
    const auto directoryLink=root/L"folder-link";
    if(CreateSymbolicLinkW(directoryLink.c_str(),folder.c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|0x2)){
        const auto path=directoryLink/L"settings.ini";
        require(!ce::loadInterfacePreferences(path).ok&&!ce::saveInterfacePreferences(path,{7,true,false}).ok,
            "Reparse parent was followed");
        require(readBytes(folder/L"settings.ini")==canonical(),"Reparse parent allowed a target write");
        require(RemoveDirectoryW(directoryLink.c_str())!=FALSE,"Could not remove fixture directory link");
    }else {++skips;std::cout<<"SKIP: directory reparse fixture unavailable (Windows error "<<GetLastError()<<").\n";}
}
}
int wmain(int argc,wchar_t** argv){
    try {
        wchar_t executable[32768]{};
        const DWORD length=GetModuleFileNameW(nullptr,executable,static_cast<DWORD>(std::size(executable)));
        require(length>0&&length<std::size(executable),"Could not find the test executable");
        const fs::path work=argc>1?fs::path(argv[1]):fs::path(executable).parent_path().parent_path()/L"test-work";
        Fixture fixture(work);
        defaultsAndRoundTrips(fixture.root);
        malformedFilesArePreserved(fixture.root);
        failedWritesPreserveExistingState(fixture.root);
        invalidValuesAndUnsafePaths(fixture.root);
        std::cout<<"PASS: "<<checks<<" interface-preference checks; 512 combinations, strict parsing, atomic failure preservation and path validation. Optional reparse skips="<<skips<<".\n";
        return 0;
    }catch(const std::exception& error){
        ce::testInterfacePreferencesFailure(ce::InterfacePreferencesFailure::None);
        std::cerr<<"FAIL after "<<checks<<" interface-preference checks: "<<error.what()<<'\n';return 1;
    }
}
