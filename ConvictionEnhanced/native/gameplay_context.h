#pragma once
#include <cstddef>
#include <cstring>

namespace ce {
enum class GameplayKind { Unavailable, Campaign, SoloDeniableOps };

namespace gameplay_detail {
inline char lower(char ch) noexcept {return ch>='A'&&ch<='Z'?static_cast<char>(ch+('a'-'A')):ch;}
inline bool equal(const char* text,std::size_t size,const char* expected) noexcept {
    if(std::strlen(expected)!=size)return false;
    for(std::size_t i=0;i<size;++i)if(lower(text[i])!=lower(expected[i]))return false;
    return true;
}
}

// Pure predicate: callers validate pointer readability before passing bounded
// copies. Map count includes its NUL; the Ubisoft connection string does not.
inline GameplayKind gameplayKind(const char* map,int mapCount,int mapCapacity,
    const char* connection,int connectionLength,bool splitScreen) noexcept {
    if(splitScreen||!map||mapCount<2||mapCount>260||mapCapacity<mapCount||mapCapacity>4096
        ||!connection||connectionLength!=4||std::memcmp(connection,"SOLO",4)!=0)return GameplayKind::Unavailable;
    if(map[mapCount-1]!=0)return GameplayKind::Unavailable;
    const auto size=static_cast<std::size_t>(mapCount-1);
    for(std::size_t i=0;i<size;++i) {
        const unsigned char ch=static_cast<unsigned char>(map[i]);
        if(ch<32||ch>126)return GameplayKind::Unavailable; // includes embedded NULs
    }
    // Preserve the verified campaign namespace, accepting only a single map
    // filename with the expected extension, never traversal or another path.
    if(size>12&&gameplay_detail::equal(map,8,"_single\\")
        &&gameplay_detail::equal(map+size-4,4,".unr")) {
        bool filename=true;
        for(std::size_t i=8;i<size-4;++i) {
            const char ch=gameplay_detail::lower(map[i]);
            if(!((ch>='a'&&ch<='z')||(ch>='0'&&ch<='9')||ch=='_'||ch=='-'))filename=false;
        }
        if(filename)return GameplayKind::Campaign;
    }
    // Gameflow.ini and installed UMDS establish these base maps; the shipped
    // MapConfigurationDLC.xml establishes the four DLC paths. Live solo Hunter
    // uses the bare gameflow name (C01_MafiaClub), without a path or extension.
    // C05_Antonov is the co-op story finale; T02 is absent from gameflow/UMDS.
    struct Map {const char* name;const char* path;};
    constexpr Map deniableMaps[]={
        {"C01_MafiaClub","_COOP\\C01_MafiaClub.unr"},
        {"C02_Embassy","_COOP\\C02_Embassy.unr"},
        {"C03_Bunker","_COOP\\C03_Bunker.unr"},
        {"C04_ResearchBase","_COOP\\C04_ResearchBase.unr"},
        {"T01_LumberMill","_COOP\\T01_LumberMill.unr"},
        {"T03_PrintingPress","_COOP\\T03_PrintingPress.unr"},
        {"T04_ThirdEchelon","_COOP\\T04_ThirdEchelon.unr"},
        {"T05_Docks","_COOP\\_DLC\\T05_Docks.unr"},
        {"T06_Cemetery","_COOP\\_DLC\\T06_Cemetery.unr"},
        {"T07_Jail","_COOP\\_DLC\\T07_Jail.unr"},
        {"T08_SecretLabs","_COOP\\_DLC\\T08_SecretLabs.unr"}
    };
    for(const auto& entry:deniableMaps)
        if(gameplay_detail::equal(map,size,entry.name)||gameplay_detail::equal(map,size,entry.path))return GameplayKind::SoloDeniableOps;
    return GameplayKind::Unavailable;
}
}
