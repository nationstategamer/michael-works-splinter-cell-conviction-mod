// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include "checkpoint_archive.h"
#include "vendor/miniz/miniz_tinfl.h"
#include <memory>
#include <vector>
#include <limits>
namespace ce { namespace {
bool compact(const unsigned char* data,std::size_t size,std::size_t* position,int* result) noexcept {
    if(*position>=size)return false;
    const unsigned first=data[(*position)++];
    const bool negative=(first&0x80)!=0;
    std::uint32_t value=first&0x3f;bool more=(first&0x40)!=0;
    unsigned shift=6,bytes=1;
    while(more) {
        if(bytes>=5||*position>=size)return false;
        const unsigned next=data[(*position)++];
        if(bytes==4&&(next&0xe0)!=0)return false;
        value|=static_cast<std::uint32_t>(next&0x7f)<<shift;
        more=(next&0x80)!=0;shift+=7;++bytes;
        if(!more&&next==0)return false; // reject noncanonical extension
    }
    if(negative) {
        if(!value||value>0x80000000u)return false;
        *result=value==0x80000000u?std::numeric_limits<int>::min():-static_cast<int>(value);
    } else {
        if(value>0x7fffffffu)return false;
        *result=static_cast<int>(value);
    }
    return true;
}
bool fail(const char** reason,const char* text) noexcept {if(reason)*reason=text;return false;}
}
std::uint64_t checkpointFingerprint(const unsigned char* data,std::size_t size) noexcept {
    std::uint64_t hash=14695981039346656037ull;
    for(std::size_t i=0;i<size;++i){hash^=data[i];hash*=1099511628211ull;}
    return hash;
}
bool validateCheckpointArchive(const unsigned char* data,std::size_t size,CheckpointArchiveInfo* out,const char** reason) noexcept {
    if(!data||!out||size<9||size>MaxCheckpointCompressed)return fail(reason,"Checkpoint archive size is unsupported");
    try {
        std::size_t offset=0;int expanded=0,compressed=0;
        if(!compact(data,size,&offset,&expanded)||!compact(data,size,&offset,&compressed)
            ||expanded<8||static_cast<std::size_t>(expanded)>MaxCheckpointExpanded||compressed<6
            ||static_cast<std::size_t>(compressed)!=size-offset)return fail(reason,"Checkpoint archive lengths are invalid");
        std::vector<unsigned char> decoded(static_cast<std::size_t>(expanded));
        auto inflater=std::make_unique<tinfl_decompressor>();tinfl_init(inflater.get());
        std::size_t consumed=static_cast<std::size_t>(compressed),written=decoded.size();
        const auto result=tinfl_decompress(inflater.get(),data+offset,&consumed,decoded.data(),decoded.data(),&written,
            TINFL_FLAG_PARSE_ZLIB_HEADER|TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        if(result!=TINFL_STATUS_DONE||consumed!=static_cast<std::size_t>(compressed)||written!=decoded.size())
            return fail(reason,"Checkpoint compression or checksum is invalid");
        if(decoded[0]!=0x3a||decoded[1]!=0x6b||decoded[2]!=0x92||decoded[3]!=0xc1)
            return fail(reason,"Checkpoint format differs from supported build");
        offset=4;int version=0,checkpoint=0,actors=0;
        if(!compact(decoded.data(),decoded.size(),&offset,&version)||version!=1
            ||!compact(decoded.data(),decoded.size(),&offset,&checkpoint)||checkpoint< -1||checkpoint>65535
            ||!compact(decoded.data(),decoded.size(),&offset,&actors)||(actors!=0&&actors!=1)||offset>=decoded.size())
            return fail(reason,"Checkpoint metadata differs from supported build");
        *out={checkpoint,static_cast<std::uint32_t>(expanded),static_cast<std::uint32_t>(compressed),actors==1,checkpointFingerprint(data,size)};
        if(reason)*reason="Verified native checkpoint archive";return true;
    } catch(...) {return fail(reason,"Could not allocate checkpoint validation memory");}
}
}
