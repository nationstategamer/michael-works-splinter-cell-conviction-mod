#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#include "checkpoint_library.h"
#include "checkpoint_archive.h"

namespace fs=std::filesystem;
std::atomic<unsigned> checks{0};unsigned skipped=0;
const std::string ProfileA(64,'a'),ProfileB(64,'b'),ProfileC(64,'c');
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
std::wstring ext(const fs::path& path){return L"\\\\?\\"+path.native();}
void compact(std::vector<unsigned char>& out,int value){
    const bool negative=value<0;unsigned remaining=static_cast<unsigned>(negative?-value:value);
    unsigned first=remaining&63;remaining>>=6;if(negative)first|=128;if(remaining)first|=64;out.push_back(static_cast<unsigned char>(first));
    while(remaining){unsigned byte=remaining&127;remaining>>=7;if(remaining)byte|=128;out.push_back(static_cast<unsigned char>(byte));}
}
std::vector<unsigned char> archive(int checkpoint,const std::string& marker,bool actors=false){
    std::vector<unsigned char> body={0x3a,0x6b,0x92,0xc1};compact(body,1);compact(body,checkpoint);compact(body,actors?1:0);
    body.insert(body.end(),marker.begin(),marker.end());body.push_back(0);
    check(body.size()<65536,"Fixture stored block is too large");
    std::vector<unsigned char> zlib={0x78,0x01,0x01};const auto count=static_cast<unsigned>(body.size());
    zlib.push_back(static_cast<unsigned char>(count));zlib.push_back(static_cast<unsigned char>(count>>8));
    zlib.push_back(static_cast<unsigned char>(~count));zlib.push_back(static_cast<unsigned char>((~count)>>8));
    zlib.insert(zlib.end(),body.begin(),body.end());unsigned a=1,b=0;for(unsigned char value:body){a=(a+value)%65521;b=(b+a)%65521;}
    const unsigned adler=(b<<16)|a;for(int shift=24;shift>=0;shift-=8)zlib.push_back(static_cast<unsigned char>(adler>>shift));
    std::vector<unsigned char> out;compact(out,static_cast<int>(body.size()));compact(out,static_cast<int>(zlib.size()));out.insert(out.end(),zlib.begin(),zlib.end());
    ce::CheckpointArchiveInfo info{};check(ce::validateCheckpointArchive(out.data(),out.size(),&info)&&info.checkpoint==checkpoint,"Fixture archive is not canonical");return out;
}
ce::CheckpointLibraryRecord record(int checkpoint,const std::string& marker,bool initial=false,int episode=0,const std::string& map="_Single\\01_Market.unr"){
    return {0,episode,checkpoint,initial,map,archive(checkpoint,marker,initial)};
}
std::vector<unsigned char> read(const fs::path& path){std::ifstream in(ext(path).c_str(),std::ios::binary);check(static_cast<bool>(in),"Open owned library fixture");return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
void write(const fs::path& path,const std::vector<unsigned char>& bytes){std::ofstream out(ext(path).c_str(),std::ios::binary|std::ios::trunc);check(static_cast<bool>(out),"Open owned library fixture for mutation");out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());out.flush();check(static_cast<bool>(out),"Write complete owned library fixture");}
std::vector<fs::path> files(){std::vector<fs::path> out;for(const auto& entry:fs::directory_iterator(ce::testCheckpointLibraryPath()))if(entry.path().extension()==L".mwcp")out.push_back(entry.path());return out;}
void setup(const fs::path& root,const char* name){ce::testCheckpointLibraryReset();fs::create_directories(root/name);check(ce::initializeCheckpointLibrary(root/name).ok,"Initialize owned checkpoint library");}
void digest(std::vector<unsigned char>& bytes){
    check(bytes.size()>=32,"Digest fixture length");BCRYPT_ALG_HANDLE algorithm=nullptr;
    check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0,"Open fixture SHA256");
    const auto status=BCryptHash(algorithm,nullptr,0,bytes.data(),static_cast<ULONG>(bytes.size()-32),bytes.data()+bytes.size()-32,32);
    BCryptCloseAlgorithmProvider(algorithm,0);check(status>=0,"Rehash semantic mutation fixture");
}
void put(std::vector<unsigned char>& bytes,std::size_t offset,unsigned value){for(unsigned i=0;i<4;++i)bytes[offset+i]=static_cast<unsigned char>(value>>(8*i));}
bool child(const fs::path& folder){
    wchar_t executable[32768]{};check(GetModuleFileNameW(nullptr,executable,32768)>0,"Resolve owned test executable");
    std::wstring command=L"\""+std::wstring(executable)+L"\" --verify \""+folder.native()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;PROCESS_INFORMATION process{};
    check(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"Launch isolated persistence verifier");
    const auto wait=WaitForSingleObject(process.hProcess,10000);if(wait!=WAIT_OBJECT_0)TerminateProcess(process.hProcess,1);
    DWORD code=1;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hThread);CloseHandle(process.hProcess);return wait==WAIT_OBJECT_0&&code==0;
}
int wmain(int argc,wchar_t** argv){
    try {
        if(argc==3&&std::wstring(argv[1])==L"--verify"){
            check(ce::initializeCheckpointLibrary(fs::path(argv[2])).ok,"Child initialize durable library");std::vector<ce::CheckpointLibraryRecord> out;
            check(ce::loadCheckpointRecords(ProfileC,"03_Airfield",0,2,out).ok&&out.size()==1,"Child reload durable checkpoint");
            check(out[0].checkpoint==3&&out[0].initial&&out[0].bytes==archive(3,"PERSIST",true),"Child did not load exact checkpoint bytes");return 0;
        }
        const auto parent=argc>1?fs::absolute(argv[1]):fs::current_path();fs::create_directories(parent);
        const auto root=parent/(L"checkpoint-library-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        check(fs::create_directory(root),"Create unique owned fixture root");setup(root,"ordinary");
        std::vector<ce::CheckpointLibraryRecord> loaded;
        check(ce::loadCheckpointRecords(ProfileA,"01_Market",0,0,loaded).ok&&loaded.empty(),"Missing mission was not an empty result");
        auto first=record(0,"INITIAL",true),current=record(0,"CURRENT"),later=record(2,"LATER");
        check(ce::saveCheckpointRecord(ProfileA,first).ok,"Save initial record");check(ce::saveCheckpointRecord(ProfileA,current).ok,"Save current same-ID record separately");
        check(ce::saveCheckpointRecord(ProfileA,later).ok,"Save later record");
        check(ce::loadCheckpointRecords(ProfileA,"_single\\01_market.unr",0,0,loaded).ok&&loaded.size()==3,"Load canonical map aliases");
        check(loaded[0].initial&&loaded[0].checkpoint==0&&!loaded[1].initial&&loaded[1].checkpoint==0&&loaded[2].checkpoint==2,"Record order or kind identity changed");
        check(loaded[0].map=="_Single\\01_Market.unr"&&loaded[0].bytes==first.bytes,"Canonical metadata/initial payload changed");
        auto replacement=record(2,"LATEST");check(ce::saveCheckpointRecord(ProfileA,replacement).ok,"Upsert latest checkpoint bytes");
        const auto storedCount=files().size();check(ce::saveCheckpointRecord(ProfileA,replacement).ok&&files().size()==storedCount,"Idempotent save created duplicate records");
        check(ce::loadCheckpointRecords(ProfileA,"01_Market.unr",0,0,loaded).ok&&loaded.back().bytes==replacement.bytes,"Latest upsert did not persist");
        auto other=record(2,"OTHER PROFILE");check(ce::saveCheckpointRecord(ProfileB,other).ok,"Save separate profile");
        check(ce::loadCheckpointRecords(ProfileB,"01_Market",0,0,loaded).ok&&loaded.size()==1&&loaded[0].bytes==other.bytes,"Profile records mixed");
        check(ce::loadCheckpointRecords(ProfileA,"02_Museum",0,1,loaded).ok&&loaded.empty(),"Mission records mixed");
        for(const auto& file:files())check(file.filename().native().size()==134&&file.filename().string().find("Market")==std::string::npos,"Record filename exposed map/profile identity");
        check(!MoveFileExW(ext(ce::testCheckpointLibraryPath()).c_str(),ext(root/L"moved-owned-library").c_str(),0),"Pinned library directory could be moved");
        for(const std::string badHash:{std::string(),std::string(63,'a'),std::string(64,'g'),std::string(65,'a')})
            check(!ce::saveCheckpointRecord(badHash,current).ok,"Malformed profile hash accepted");
        auto mismatch=current;mismatch.episode=1;check(!ce::saveCheckpointRecord(ProfileA,mismatch).ok,"Map/episode disagreement accepted");
        mismatch=current;mismatch.act=1;check(!ce::saveCheckpointRecord(ProfileA,mismatch).ok,"Noncampaign act accepted");
        mismatch=current;mismatch.map="..\\01_Market.unr";check(!ce::saveCheckpointRecord(ProfileA,mismatch).ok,"Map traversal accepted");
        mismatch=current;mismatch.checkpoint=3;check(!ce::saveCheckpointRecord(ProfileA,mismatch).ok,"Metadata/packed checkpoint mismatch accepted");
        mismatch=current;mismatch.bytes.back()^=1;check(!ce::saveCheckpointRecord(ProfileA,mismatch).ok,"Bad packed checksum accepted");
        mismatch=current;mismatch.bytes.resize(ce::MaxCheckpointCompressed+1);check(!ce::saveCheckpointRecord(ProfileA,mismatch).ok,"Oversized packed archive accepted");

        unsigned index=0;
        for(auto failure:{ce::CheckpointLibraryFailure::Write,ce::CheckpointLibraryFailure::Flush,ce::CheckpointLibraryFailure::Replace}){
            const auto name="failure-"+std::to_string(index++);setup(root,name.c_str());check(ce::saveCheckpointRecord(ProfileA,current).ok,"Save atomic baseline");
            const auto target=files().front();const auto before=read(target);const auto stale=ce::testCheckpointLibraryPath()/L"unrelated.tmp";write(stale,{1,2,3});
            ce::testCheckpointLibraryFailure(failure);check(!ce::saveCheckpointRecord(ProfileA,record(0,"REPLACEMENT")).ok,"Injected atomic failure succeeded");
            check(read(target)==before,"Atomic failure changed previous record");check(read(stale)==std::vector<unsigned char>({1,2,3}),"Atomic cleanup removed unrelated temp file");
            check(!ce::saveCheckpointRecord(ProfileA,record(1,"NEW FAILURE")).ok&&files().size()==1,"Failure created a new final record");
            unsigned temporary=0;for(const auto& file:fs::directory_iterator(ce::testCheckpointLibraryPath()))if(file.path().extension()==L".tmp")++temporary;
            check(temporary==1,"Failed save left its owned temporary file");ce::testCheckpointLibraryFailure(ce::CheckpointLibraryFailure::None);
            check(ce::loadCheckpointRecords(ProfileA,"01_Market",0,0,loaded).ok&&loaded.size()==1&&loaded[0].bytes==current.bytes,"Atomic failure broke prior load");
            check(ce::saveCheckpointRecord(ProfileA,record(1,"AFTER FAILURE")).ok,"Failure fixture reset failed");
        }
        setup(root,"corruption");check(ce::saveCheckpointRecord(ProfileA,current).ok&&ce::saveCheckpointRecord(ProfileA,later).ok,"Create corruption group");
        const auto corruptTarget=files().front();const auto baseline=read(corruptTarget);
        for(unsigned mutation=0;mutation<11;++mutation){
            auto changed=baseline;
            if(mutation==0)changed[0]^=1;
            if(mutation==1)put(changed,8,2);
            if(mutation==2)put(changed,12,128);
            if(mutation==3)changed[44]^=1;
            if(mutation==4)changed[108]^=1;
            if(mutation==5)put(changed,24,99);
            if(mutation==6)changed[changed.size()-33]^=1;
            if(mutation==7)changed[changed.size()-1]^=1;
            if(mutation==8)changed[36]^=1;
            if(mutation==9)changed[12]^=2;
            if(mutation==10)changed[140]^=1;
            if(mutation>0&&mutation!=7)digest(changed);
            write(corruptTarget,changed);loaded.push_back(first);
            const auto result=ce::loadCheckpointRecords(ProfileA,"01_Market",0,0,loaded);
            check(!result.ok&&!result.reason.empty()&&loaded.empty(),"Corrupt/foreign record allowed partial or stale group load");
            check(!ce::saveCheckpointRecord(ProfileA,record(5,"DO NOT OVERWRITE")).ok&&read(corruptTarget)==changed,"Save overwrote a corrupt existing group");
            write(corruptTarget,baseline);
        }
        check(ce::loadCheckpointRecords(ProfileA,"01_Market",0,0,loaded).ok&&loaded.size()==2,"Restored complete group unavailable");
        check(SetFileAttributesW(ext(corruptTarget).c_str(),FILE_ATTRIBUTE_READONLY)!=FALSE,"Set owned fixture read-only");
        // Both IDs are tested because directory enumeration order is unspecified.
        const auto beforeReadonly=read(corruptTarget);const bool update0=ce::saveCheckpointRecord(ProfileA,record(0,"RO0")).ok;
        const bool update2=ce::saveCheckpointRecord(ProfileA,record(2,"RO2")).ok;
        check(!update0||!update2,"Read-only target was replaced");check(read(corruptTarget)==beforeReadonly,"Read-only bytes changed");
        check(SetFileAttributesW(ext(corruptTarget).c_str(),FILE_ATTRIBUTE_NORMAL)!=FALSE,"Restore owned fixture attributes");
        const auto hardLink=root/L"owned-hardlink.mwcp";
        if(CreateHardLinkW(ext(hardLink).c_str(),ext(corruptTarget).c_str(),nullptr)){
            loaded.push_back(first);check(!ce::loadCheckpointRecords(ProfileA,"01_Market",0,0,loaded).ok&&loaded.empty(),"Hardlinked record accepted");
            check(!ce::saveCheckpointRecord(ProfileA,record(0,"LINKED")).ok,"Hardlinked group allowed replacement");
            check(DeleteFileW(ext(hardLink).c_str())!=FALSE,"Remove owned hardlink fixture");
        }else{++skipped;std::cout<<"SKIP: hardlink fixture unavailable ("<<GetLastError()<<").\n";}
        const auto renamed=corruptTarget.parent_path()/(corruptTarget.stem().native().substr(0,65)+std::wstring(64,L'0')+L".mwcp");
        check(MoveFileExW(ext(corruptTarget).c_str(),ext(renamed).c_str(),0)!=FALSE,"Rename owned identity fixture");
        check(!ce::loadCheckpointRecords(ProfileA,"01_Market",0,0,loaded).ok&&loaded.empty(),"Filename identity mismatch was accepted");
        check(MoveFileExW(ext(renamed).c_str(),ext(corruptTarget).c_str(),0)!=FALSE,"Restore owned identity fixture");
        setup(root,"file-reparse");check(ce::saveCheckpointRecord(ProfileA,current).ok,"Create file-reparse fixture");
        const auto link=files().front(),linkTarget=root/L"owned-link-target.mwcp";
        check(MoveFileExW(ext(link).c_str(),ext(linkTarget).c_str(),0)!=FALSE,"Move owned symlink target fixture");const auto linkBytes=read(linkTarget);
        if(CreateSymbolicLinkW(ext(link).c_str(),ext(linkTarget).c_str(),2)){
            check(!ce::loadCheckpointRecords(ProfileA,"01_Market",0,0,loaded).ok&&loaded.empty(),"Reparse checkpoint file was followed");
            check(!ce::saveCheckpointRecord(ProfileA,current).ok&&read(linkTarget)==linkBytes,"Reparse target was replaced or changed");
            check(DeleteFileW(ext(link).c_str())!=FALSE,"Remove owned file symlink");
        }else{++skipped;std::cout<<"SKIP: file-reparse fixture unavailable ("<<GetLastError()<<").\n";}
        check(MoveFileExW(ext(linkTarget).c_str(),ext(link).c_str(),0)!=FALSE,"Restore owned symlink fixture");
        ce::testCheckpointLibraryReset();const auto redirectRoot=root/L"directory-reparse",redirectTarget=root/L"owned-directory-target";
        fs::create_directory(redirectRoot);fs::create_directory(redirectTarget);write(redirectTarget/L"sentinel",{9,8,7});
        if(CreateSymbolicLinkW(ext(redirectRoot/L"checkpoint-library").c_str(),ext(redirectTarget).c_str(),3)){
            check(!ce::initializeCheckpointLibrary(redirectRoot).ok,"Reparse library directory accepted");
            check(read(redirectTarget/L"sentinel")==std::vector<unsigned char>({9,8,7}),"Redirected directory target changed");
            check(RemoveDirectoryW(ext(redirectRoot/L"checkpoint-library").c_str())!=FALSE,"Remove owned directory symlink");
        }else{++skipped;std::cout<<"SKIP: directory-reparse fixture unavailable ("<<GetLastError()<<").\n";}
        setup(root,"capacity");for(unsigned i=0;i<32;++i)check(ce::saveCheckpointRecord(ProfileA,record(static_cast<int>(i),"CAPACITY")).ok,"Save within32record cap");
        check(!ce::saveCheckpointRecord(ProfileA,record(32,"OVERFLOW")).ok&&files().size()==32,"33rd checkpoint record accepted");
        check(ce::saveCheckpointRecord(ProfileA,record(31,"UPSERT AT CAP")).ok,"Upsert at record cap rejected");
        check(ce::loadCheckpointRecords(ProfileA,"01_Market",0,0,loaded).ok&&loaded.size()==32,"Complete bounded group failed load");
        setup(root,"budget");const auto small=record(0,"BUDGET"),second=record(1,"BUDGET");
        ce::testCheckpointLibraryBudget(small.bytes.size()+second.bytes.size()-1);
        check(ce::saveCheckpointRecord(ProfileA,small).ok&&!ce::saveCheckpointRecord(ProfileA,second).ok,"Mission byte budget not enforced");
        ce::testCheckpointLibraryBudget(small.bytes.size()-1);loaded.push_back(first);
        check(!ce::loadCheckpointRecords(ProfileA,"01_Market",0,0,loaded).ok&&loaded.empty(),"Oversized total mission load accepted");
        setup(root,"concurrent");std::atomic<unsigned> faults{0};std::vector<std::thread> workers;
        for(int i=0;i<4;++i)workers.emplace_back([&,i]{for(int round=0;round<4;++round){auto r=record(i,"ROUND"+std::to_string(round));if(!ce::saveCheckpointRecord(ProfileA,r).ok)++faults;}});
        for(auto& worker:workers)worker.join();check(faults==0,"Concurrent library writes failed");
        check(ce::loadCheckpointRecords(ProfileA,"01_Market",0,0,loaded).ok&&loaded.size()==4,"Concurrent records lost or mixed");
        setup(root,"persist");check(ce::saveCheckpointRecord(ProfileC,record(3,"PERSIST",true,2,"03_Airfield")).ok,"Create separate-process fixture");
        const auto persistRoot=root/L"persist";ce::testCheckpointLibraryReset();check(child(persistRoot),"New process did not load exact durable checkpoint");
        check(!ce::initializeCheckpointLibrary(root/L"missing").ok,"Missing state root accepted");
        check(!ce::initializeCheckpointLibrary(root/L"persist/../alias").ok,"Parent traversal alias accepted");
        const auto blocked=root/L"blocked";fs::create_directory(blocked);write(blocked/L"checkpoint-library",{1,2,3});
        check(!ce::initializeCheckpointLibrary(blocked).ok,"File used as library directory");
        std::cout<<"PASS: "<<checks<<" durable checkpoint library checks; profile/mission/kind isolation, exact bytes, atomic failures, semantic corruption, caps, concurrent writes and new-process reload. Optional skips="<<skipped<<".\n";return 0;
    } catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}
}
