#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "practice_storage.h"

namespace fs=std::filesystem;
unsigned checks=0;
void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
std::wstring extended(const fs::path& file){auto name=file.native();for(auto& ch:name)if(ch==L'/')ch=L'\\';return L"\\\\?\\"+name;}
std::vector<char> bytes(const fs::path& file){std::ifstream in(extended(file).c_str(),std::ios::binary);check(static_cast<bool>(in),"Open complete fixture bytes");return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};}
fs::path blob(){for(const auto& p:fs::directory_iterator(ce::testPracticeSessionPath()))if(p.path().extension()==L".save")return p.path();throw std::runtime_error("Fixture blob missing");}
void source(const char* name,const std::string& value){ce::testPracticeSource(name,value.data(),static_cast<unsigned>(value.size()));}
unsigned write(const char* name,const std::string& value){return ce::testPracticeWrite(name,value.data(),static_cast<unsigned>(value.size()));}
std::string read(const char* name,unsigned capacity=128){std::vector<char> out(capacity?capacity:1,'#');const auto size=ce::testPracticeRead(name,out.data(),capacity);check(size<=capacity,"Read exceeded requested length");return {out.data(),out.data()+size};}
void start(const fs::path& root,const char* name){ce::testPracticeReset();const auto folder=root/name;fs::create_directories(folder);check(ce::initializePracticeStorage(folder),"Initialize fixture storage");}
int wmain(int argc,wchar_t** argv) {
    try {
        const auto parent=argc>1?fs::absolute(argv[1]):fs::current_path();
        fs::create_directories(parent);
        const auto root=parent/(L"practice-storage-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
        check(fs::create_directory(root),"Create unique owned fixture root");
        start(root,"ordinary");source("Alice@profile.sav","ORIGINAL");
        check(read("Alice@profile.sav")=="ORIGINAL","Inactive original read changed");
        check(write("Bob@profile.sav","NORMAL")==6,"Inactive original write changed");
        check(ce::testPracticeExists("Bob@profile.sav"),"Inactive original exists changed");
        check(ce::testPracticeDelete("Bob@profile.sav"),"Inactive original delete changed");
        source("Bob@profile.sav","NORMAL");check(ce::testPracticeBulkDelete("Bob"),"Inactive original bulk delete changed");
        source("Alpha@profile.sav","A");source("Alpha@checkpoint.sav","C");source("Alphabet@profile.sav","B");
        check(read("Alpha@profile.sav")=="A"&&read("Alpha@checkpoint.sav")=="C"&&read("Alphabet@profile.sav")=="B","Cache complete original payloads before activation");
        ce::testPracticeSourceId(17,"Bob@profile.sav");
        check(!ce::testPracticeRawIdDelete(17),"Inactive EAX-context helper result changed");
        check(ce::testPracticeOriginalCounts().rawDeletes==1,"Inactive EAX-context helper did not forward");
        const auto before=ce::testPracticeOriginalCounts();
        check(ce::activatePracticeStorage(),"Activate practice storage");
        check(ce::practiceStorageStatus().active&&ce::practiceStorageStatus().healthy,"Activation state unavailable");
        const auto session=ce::testPracticeSessionPath();check(fs::is_directory(session),"Practice session directory missing");
        check(ce::activatePracticeStorage()&&ce::testPracticeSessionPath()==session,"Activation changed an existing practice namespace");
        check(read("Alice@profile.sav")=="ORIGINAL","Practice bootstrap could not read original data");
        const auto firstWrite=write("Alice@profile.sav","PRACTICE");
        if(firstWrite!=8)std::cerr<<"Write diagnostics: "<<ce::practiceStorageStatus().reason<<" Windows error="<<GetLastError()<<"\n";
        check(firstWrite==8,"Practice write byte count changed");
        check(read("alice@PROFILE.SAV")=="PRACTICE","Case-normalized local save did not override original");
        check(ce::testPracticeOriginalCounts().writes==before.writes,"Practice write reached original save writer");
        check(read("Alice@profile.sav",3)=="PRA","Short read did not retain byte-count semantics");
        char destination[12];for(char& ch:destination)ch='!';
        check(ce::testPracticeRead("Alice@profile.sav",destination,4)==4,"Read byte count incorrect");
        check(destination[4]=='!'&&destination[11]=='!',"Read wrote outside requested buffer");
        check(ce::testPracticeRead("Alice@profile.sav",nullptr,0)==0,"Zero-size read should return zero");
        check(ce::testPracticeWrite("Empty@profile.sav",nullptr,0)==0,"Zero-size write should return zero");
        check(ce::testPracticeExists("Empty@profile.sav"),"Empty local save lost existence");
        check(ce::testPracticeDelete("Alice@profile.sav"),"Practice logical delete failed");
        check(!ce::testPracticeExists("Alice@profile.sav")&&read("Alice@profile.sav").empty(),"Deleted save fell back to original");
        check(ce::testPracticeOriginalCounts().deletes==before.deletes,"Practice delete reached original delete");
        check(ce::testPracticeDelete("Missing@profile.sav"),"Practice delete should be idempotent");
        check(write("Alice@profile.sav","AFTER DELETE")==12,"Recreate after deletion failed");
        check(read("Alice@profile.sav")=="AFTER DELETE","Recreated local save unavailable");
        check(ce::testPracticeRawIdDelete(17),"Active EAX-context delete not blocked with successful result");
        check(ce::testPracticeOriginalCounts().rawDeletes==before.rawDeletes,"Active raw-ID cleanup reached original delete");
        check(write("Alpha@profile.sav","LOCAL")==5,"Local bulk fixture write failed");
        check(ce::testPracticeBulkDelete("ALPHA"),"Practice profile delete failed");
        check(read("Alpha@profile.sav").empty()&&read("Alpha@checkpoint.sav").empty(),"Bulk tombstone allowed original fallback");
        check(read("Alphabet@profile.sav")=="B","Profile prefix matched an unrelated profile");
        check(write("Alpha@profile.sav","NEW")==3&&read("Alpha@profile.sav")=="NEW","Write after bulk deletion was hidden");
        check(ce::testPracticeBulkDelete("*"),"Wildcard practice delete failed");
        check(read("Alphabet@profile.sav").empty()&&read("Alpha@profile.sav").empty(),"Wildcard deletion leaked original or old local data");
        check(write("Alpha@profile.sav","AGAIN")==5&&read("Alpha@profile.sav")=="AGAIN","New write after wildcard deletion failed");
        const auto after=ce::testPracticeOriginalCounts();
        check(after.writes==before.writes&&after.deletes==before.deletes&&after.bulkDeletes==before.bulkDeletes&&after.rawDeletes==before.rawDeletes,"Original save mutation occurred in practice mode");
        check(after.reads==before.reads&&after.exists==before.exists,"Active practice called the Ubisoft reader/list API");

        start(root,"in-flight");ce::testPracticePauseNextOriginalWrite();unsigned pendingResult=0;
        std::thread pending([&]{pendingResult=write("Player@profile.sav","BEFORE PRACTICE");});
        const auto deadline=GetTickCount64()+3000;
        while(!ce::testPracticeOriginalWritePaused()&&GetTickCount64()<deadline)Sleep(1);
        const bool paused=ce::testPracticeOriginalWritePaused();
        const bool activatedWhilePending=paused?ce::activatePracticeStorage():false;
        ce::testPracticeReleaseOriginalWrite();pending.join();
        check(paused,"Original-write fixture did not pause");
        check(!activatedWhilePending,"Practice activated before an original write finished");
        check(pendingResult==15&&ce::testPracticeOriginalCounts().writes==1,"Pending original write did not complete normally");
        check(ce::activatePracticeStorage(),"Practice did not activate after original I/O drained");
        check(write("Player@profile.sav","AFTER")==5,"Post-drain practice write failed");
        check(ce::testPracticeOriginalCounts().writes==1,"Post-drain write reached the original writer");
        check(!MoveFileExW(extended(ce::testPracticeSessionPath()).c_str(),extended(ce::testPracticeSessionPath().parent_path()/L"moved-session").c_str(),0),"Active practice folder could be renamed under open protection");

        start(root,"concurrent");check(ce::activatePracticeStorage(),"Activate concurrent fixture");
        std::atomic<unsigned> faults{0};std::vector<std::thread> workers;
        for(unsigned worker=0;worker<4;++worker)workers.emplace_back([&,worker]{
            const auto key="Worker"+std::to_string(worker)+"@checkpoint.sav";
            for(unsigned generation=0;generation<12;++generation) {
                const auto value="generation-"+std::to_string(generation);char out[64]{};
                if(ce::testPracticeWrite(key.c_str(),value.data(),static_cast<unsigned>(value.size()))!=value.size())++faults;
                const auto amount=ce::testPracticeRead(key.c_str(),out,sizeof(out));
                if(std::string(out,out+amount)!=value)++faults;
            }
        });
        for(auto& worker:workers)worker.join();
        check(faults==0,"Concurrent local saves lost or mixed payloads");
        check(ce::testPracticeOriginalCounts().writes==0&&ce::practiceStorageStatus().writes==48,"Concurrent practice writes reached originals or were dropped");

        start(root,"inactive-delete-cache");source("Keep@profile.sav","KEEP");source("Gone@profile.sav","DELETED");
        check(read("Keep@profile.sav")=="KEEP"&&read("Gone@profile.sav")=="DELETED","Seed inactive delete cache");
        check(ce::testPracticeExists("Gone@profile.sav")&&ce::testPracticeDelete("Gone@profile.sav"),"Inactive cached delete failed");
        const auto namedReads=ce::testPracticeOriginalCounts().reads;
        check(ce::activatePracticeStorage(),"Activate after inactive named deletion");
        check(!ce::testPracticeExists("Gone@profile.sav")&&read("Gone@profile.sav").empty(),"Deleted original reappeared from cache");
        check(read("Keep@profile.sav")=="KEEP"&&ce::practiceStorageStatus().healthy,"Named deletion invalidated an unrelated original cache");
        check(ce::testPracticeOriginalCounts().reads==namedReads,"Deleted-cache check called original reader in practice");

        start(root,"inactive-bulk-cache");source("Alpha@profile.sav","PROFILE");source("Alpha@checkpoint.sav","CHECKPOINT");source("Alphabet@profile.sav","KEEP");
        check(read("Alpha@profile.sav")=="PROFILE"&&read("Alpha@checkpoint.sav")=="CHECKPOINT"&&read("Alphabet@profile.sav")=="KEEP","Seed inactive bulk cache");
        check(ce::testPracticeBulkDelete("Alpha"),"Inactive bulk delete failed");
        check(ce::activatePracticeStorage(),"Activate after inactive bulk deletion");
        check(read("Alpha@profile.sav").empty()&&read("Alpha@checkpoint.sav").empty(),"Bulk-deleted data reappeared from cache");
        check(read("Alphabet@profile.sav")=="KEEP"&&ce::practiceStorageStatus().healthy,"Bulk deletion invalidated an unrelated profile prefix");

        start(root,"inactive-raw-cache");source("Keep@profile.sav","KEEP");source("Gone@profile.sav","DELETED");
        check(read("Keep@profile.sav")=="KEEP"&&read("Gone@profile.sav")=="DELETED","Seed raw-ID cache");
        ce::testPracticeSourceId(44,"Gone@profile.sav");ce::testPracticeRawIdDelete(44);
        check(ce::activatePracticeStorage(),"Activate after known raw-ID cleanup");
        check(read("Keep@profile.sav")=="KEEP"&&ce::practiceStorageStatus().healthy,"Known raw-ID cleanup discarded unrelated original data");
        check(!ce::testPracticeExists("Gone@profile.sav")&&read("Gone@profile.sav").empty(),"Raw-ID-deleted data reappeared from cache");

        start(root,"unknown-raw-cache");source("Keep@profile.sav","KEEP");check(read("Keep@profile.sav")=="KEEP","Seed unknown raw-ID fixture");
        ce::testPracticeRawIdDelete(999);check(ce::activatePracticeStorage(),"Activate after unknown raw-ID cleanup");
        check(read("Keep@profile.sav").empty()&&!ce::practiceStorageStatus().healthy,"Unknown raw-ID cleanup retained unverified bootstrap data");

        start(root,"uncached-read");source("Uncached@profile.sav","MUST NOT READ SDK");
        check(ce::activatePracticeStorage(),"Activate uncached fixture");
        check(!ce::testPracticeExists("Uncached@profile.sav"),"Unknown original existence queried the SDK");
        check(read("Uncached@profile.sav").empty(),"Unknown original payload called the SDK");
        check(ce::testPracticeOriginalCounts().reads==0&&ce::testPracticeOriginalCounts().exists==0,"Active uncached lookup reached SDK");
        check(ce::practiceStorageStatus().active&&!ce::practiceStorageStatus().healthy,"Uncached original read did not remain protected");

        start(root,"physical-locks");const auto physical=root/L"physical-originals/2";fs::create_directories(physical);
        for(unsigned i=0;i<3;++i){std::ofstream out(physical/(std::to_string(i)+".save"),std::ios::binary);out<<"ORIGINAL";}
        ce::testPracticeOriginalDirectory(physical);char accountA[65]{},accountB[65]{};
        const bool gotAccountA=ce::practiceAccountKey(accountA),gotAccountB=ce::practiceAccountKey(accountB);
        if(!gotAccountA||!gotAccountB)std::cerr<<"Account diagnostics: "<<ce::practiceStorageStatus().reason<<" Windows error="<<GetLastError()<<"\n";
        check(gotAccountA&&gotAccountB,"Stable account key unavailable before activation");
        check(std::string(accountA)==accountB&&std::string(accountA).size()==64,"Account key is not stable SHA-256 text");
        const auto protectedFile=physical/L"0.save";
        HANDLE busy=CreateFileW(extended(protectedFile).c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        check(busy!=INVALID_HANDLE_VALUE,"Open owned busy-writer fixture");
        const bool activatedBusy=ce::activatePracticeStorage();CloseHandle(busy);
        check(!activatedBusy&&!ce::practiceStorageStatus().active,"Practice activated with a busy physical original save");
        check(ce::activatePracticeStorage(),"Practice did not pin original files after writer closed");
        for(unsigned i=0;i<3;++i) {
            const auto original=physical/(std::to_string(i)+".save");
            HANDLE writer=CreateFileW(extended(original).c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
            if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
            check(writer==INVALID_HANDLE_VALUE,"Original save still allowed writer access");
            check(!DeleteFileW(extended(original).c_str()),"Original save could be deleted during practice");
            check(bytes(original)==std::vector<char>({'O','R','I','G','I','N','A','L'}),"Physical original bytes changed");
        }
        ce::testPracticeReset();
        HANDLE released=CreateFileW(extended(protectedFile).c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
        check(released!=INVALID_HANDLE_VALUE,"Process-lifetime original pins did not release in isolated reset");if(released!=INVALID_HANDLE_VALUE)CloseHandle(released);

        unsigned index=0;
        for(auto failure:{ce::PracticeStorageFailure::Write,ce::PracticeStorageFailure::Flush,ce::PracticeStorageFailure::Replace}) {
            const auto name="failure"+std::to_string(index++);start(root,name.c_str());source("Player@profile.sav","ORIGINAL");
            check(ce::activatePracticeStorage(),"Activate failure fixture");check(write("Player@profile.sav","BEFORE")==6,"Initial atomic write failed");
            const auto file=blob();const auto previous=bytes(file);ce::testPracticeFailure(failure);
            check(write("Player@profile.sav","REPLACEMENT")==0,"Injected save failure succeeded");
            check(bytes(file)==previous,"Interrupted save replaced the previous complete local file");
            auto state=ce::practiceStorageStatus();check(state.active&&!state.healthy,"Failure disabled protection or remained healthy");
            check(!ce::activatePracticeStorage(),"Unhealthy active guard reactivated normal saving");
            check(write("Other@profile.sav","NO FALLBACK")==0&&read("Player@profile.sav").empty(),"Unhealthy guard fell back to originals");
            check(!ce::testPracticeDelete("Player@profile.sav")&&!ce::testPracticeBulkDelete("Player"),"Unhealthy guard allowed removal");
            const auto counts=ce::testPracticeOriginalCounts();check(counts.writes==0&&counts.deletes==0&&counts.bulkDeletes==0,"Failed local I/O mutated an original save");
        }
        start(root,"corruption");source("Player@profile.sav","ORIGINAL");check(ce::activatePracticeStorage(),"Activate corruption fixture");
        check(write("Player@profile.sav","CHECKSUM")==8,"Write corruption fixture");
        {std::fstream io(extended(blob()).c_str(),std::ios::in|std::ios::out|std::ios::binary);check(static_cast<bool>(io),"Open corruption fixture");io.seekg(-1,std::ios::end);char changed=0;io.read(&changed,1);changed^=1;io.seekp(-1,std::ios::end);io.write(&changed,1);io.flush();check(static_cast<bool>(io),"Corrupt owned fixture byte");}
        check(read("Player@profile.sav").empty(),"Corrupted practice data was served");
        check(!ce::practiceStorageStatus().healthy&&ce::practiceStorageStatus().active,"CRC failure did not retain a closed save guard");
        check(ce::testPracticeOriginalCounts().reads==0&&ce::testPracticeOriginalCounts().writes==0,"CRC failure fell back to an original");

        index=0;
        for(const char* bad:{"../profile.sav","A/B.sav","A\\B.sav","C:profile.sav","A*B.sav","A?B.sav",""}) {
            const auto name="name"+std::to_string(index++);start(root,name.c_str());check(ce::activatePracticeStorage(),"Activate name fixture");
            check(write(bad,"X")==0,"Unsafe logical filename accepted");check(ce::testPracticeOriginalCounts().writes==0,"Unsafe logical name reached original writer");
        }
        start(root,"bounds");check(ce::activatePracticeStorage(),"Activate bounds fixture");
        check(ce::testPracticeWrite("Player@profile.sav",reinterpret_cast<void*>(1),64*1024*1024+1)==0,"Oversized payload accepted");
        check(ce::testPracticeOriginalCounts().writes==0,"Oversized payload reached original writer");
        start(root,"null-buffer");check(ce::activatePracticeStorage(),"Activate pointer fixture");
        check(ce::testPracticeWrite("Player@profile.sav",nullptr,5)==0,"Unavailable save buffer accepted");
        check(ce::testPracticeOriginalCounts().writes==0,"Invalid buffer reached original writer");
        ce::testPracticeReset();check(!ce::initializePracticeStorage(root/L"missing"),"Missing mod state directory accepted");
        start(root,"blocked-root");{std::ofstream out(root/L"blocked-root/practice-saves");out<<"occupied";}
        check(!ce::activatePracticeStorage()&&!ce::practiceStorageStatus().active,"Non-directory practice root activated protection incorrectly");
        ce::testPracticeReset();
        std::cout<<"PASS: "<<checks<<" practice storage checks; inactive pass-through, local payload/read-through, all delete paths, atomic failures, corruption and closed-save protection.\n";
        return 0;
    } catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<"\n";return 1;}
}
