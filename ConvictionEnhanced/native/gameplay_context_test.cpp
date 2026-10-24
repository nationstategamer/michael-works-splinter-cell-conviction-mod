#include "gameplay_context.h"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
int checks=0;
void require(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
ce::GameplayKind classify(const char* map,const char* connection="SOLO",bool split=false) {
    const int count=static_cast<int>(std::strlen(map))+1;
    return ce::gameplayKind(map,count,count,connection,static_cast<int>(std::strlen(connection)),split);
}
}
int main() {
    try {
        using ce::GameplayKind;
        require(classify("_Single\\08_ThirdEchelon_A.unr")==GameplayKind::Campaign,"Campaign checkpoint rejected");
        const char* names[]={"C01_MafiaClub","C02_Embassy","C03_Bunker","C04_ResearchBase",
            "T01_LumberMill","T03_PrintingPress","T04_ThirdEchelon","T05_Docks","T06_Cemetery","T07_Jail","T08_SecretLabs"};
        for(std::size_t i=0;i<sizeof(names)/sizeof(names[0]);++i) {
            require(classify(names[i])==GameplayKind::SoloDeniableOps,"Installed solo Deniable map rejected");
            const std::string path=std::string(i<7?"_COOP\\":"_COOP\\_DLC\\")+names[i]+".unr";
            require(classify(path.c_str())==GameplayKind::SoloDeniableOps,"Configured Deniable map path rejected");
        }
        require(classify("c01_mafiaclub")==GameplayKind::SoloDeniableOps,"Case-insensitive map matching failed");
        const char* modes[]={"","UNKNOWN","solo","SOL","SOLOx","SPLIT","LINK","XBOX","Party","Invited"};
        for(const auto mode:modes) {
            require(classify("C01_MafiaClub",mode)==GameplayKind::Unavailable,"Non-solo Deniable session allowed");
            require(classify("_Single\\08_ThirdEchelon_A.unr",mode)==GameplayKind::Unavailable,"Non-solo campaign session allowed");
        }
        require(classify("C01_MafiaClub","SOLO",true)==GameplayKind::Unavailable,"Solo lobby with split-screen allowed");
        require(classify("_Single\\08_ThirdEchelon_A.unr","SOLO",true)==GameplayKind::Unavailable,"Split campaign allowed");
        const char* unsupported[]={"menu.unr","C05_Antonov","_COOP\\C05_Antonov.unr","T02_AmusementPark","C99_Unknown",
            "_Multi\\C01_MafiaClub.unr","_COOP\\Unknown.unr","_COOP\\C01_MafiaClub.unr?listen","C01_MafiaClubSuffix",
            "_Single\\..\\menu.unr","_Single\\nested\\map.unr","_Single\\.unr","_Single\\08_ThirdEchelon_A.unr?listen"};
        for(const auto map:unsupported)require(classify(map)==GameplayKind::Unavailable,"Unverified map or travel suffix allowed");
        char map[]="C01_MafiaClub";
        const int count=sizeof(map);
        require(ce::gameplayKind(nullptr,count,count,"SOLO",4,false)==GameplayKind::Unavailable,"Null map allowed");
        require(ce::gameplayKind(map,count,count,nullptr,4,false)==GameplayKind::Unavailable,"Null session marker allowed");
        require(ce::gameplayKind(map,count-1,count,"SOLO",4,false)==GameplayKind::Unavailable,"Unterminated map allowed");
        require(ce::gameplayKind(map,count,count-1,"SOLO",4,false)==GameplayKind::Unavailable,"Short map capacity allowed");
        require(ce::gameplayKind(map,count,4097,"SOLO",4,false)==GameplayKind::Unavailable,"Unbounded map capacity allowed");
        require(ce::gameplayKind(map,261,261,"SOLO",4,false)==GameplayKind::Unavailable,"Oversized map count allowed");
        require(ce::gameplayKind(map,-1,count,"SOLO",4,false)==GameplayKind::Unavailable,"Negative map count allowed");
        map[4]=0;
        require(ce::gameplayKind(map,count,count,"SOLO",4,false)==GameplayKind::Unavailable,"Embedded NUL map allowed");
        std::cout<<"Gameplay context: "<<checks<<" checks passed.\n";return 0;
    } catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<" checks: "<<error.what()<<"\n";return 1;}
}
