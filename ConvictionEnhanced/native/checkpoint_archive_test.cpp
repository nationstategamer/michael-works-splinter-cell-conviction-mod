// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include "checkpoint_archive.h"
#include <iostream>
#include <stdexcept>
#include <vector>
namespace {
unsigned checks=0;
void require(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
const unsigned char minus1[]={33,41,120,156,179,202,158,116,144,177,145,33,191,40,51,61,51,47,49,71,161,184,50,175,36,35,181,36,51,89,33,45,179,162,164,180,40,21,0,216,135,12,242};
const unsigned char zero[]={33,41,120,156,179,202,158,116,144,145,129,33,191,40,51,61,51,47,49,71,161,184,50,175,36,35,181,36,51,89,33,45,179,162,164,180,40,21,0,202,107,12,113};
const unsigned char five[]={33,41,120,156,179,202,158,116,144,145,149,33,191,40,51,61,51,47,49,71,161,184,50,175,36,35,181,36,51,89,33,45,179,162,164,180,40,21,0,202,247,12,118};
const unsigned char sixtyfive[]={34,42,120,156,179,202,158,116,144,209,145,145,33,191,40,51,61,51,47,49,71,161,184,50,175,36,35,181,36,51,89,33,45,179,162,164,180,40,21,0,211,222,12,179};
const unsigned char maximum[]={35,43,120,156,179,202,158,116,144,177,254,63,59,67,126,81,102,122,102,94,98,142,66,113,101,94,73,70,106,73,102,178,66,90,102,69,73,105,81,42,0,250,232,13,246};
const unsigned char actors[]={33,41,120,156,179,202,158,116,144,145,153,49,191,40,51,61,51,47,49,71,161,184,50,175,36,35,181,36,51,89,33,45,179,162,164,180,40,21,0,202,218,12,117};
const unsigned char badid[]={33,41,120,156,179,202,158,116,144,177,137,33,191,40,51,61,51,47,49,71,161,184,50,175,36,35,181,36,51,89,33,45,179,162,164,180,40,21,0,216,163,12,243};
const unsigned char highid[]={35,43,120,156,179,202,158,116,144,209,161,129,131,33,191,40,51,61,51,47,49,71,161,184,50,175,36,35,181,36,51,89,33,45,179,162,164,180,40,21,0,229,63,13,57};
const unsigned char version[]={33,41,120,156,179,202,158,116,144,137,129,33,191,40,51,61,51,47,49,71,161,184,50,175,36,35,181,36,51,89,33,45,179,162,164,180,40,21,0,202,136,12,114};
const unsigned char badactors[]={33,41,120,156,179,202,158,116,144,145,129,41,191,40,51,61,51,47,49,71,161,184,50,175,36,35,181,36,51,89,33,45,179,162,164,180,40,21,0,202,161,12,115};
const unsigned char magic[]={33,41,120,156,75,74,76,81,100,100,96,200,47,202,76,207,204,75,204,81,40,174,204,43,201,72,45,201,76,86,72,203,172,40,41,45,74,5,0,182,1,11,193};
void check(const unsigned char* bytes,std::size_t size,int id,bool valid){
 ce::CheckpointArchiveInfo info{};const char* reason=nullptr;
 require(ce::validateCheckpointArchive(bytes,size,&info,&reason)==valid,"Archive acceptance differs");
 require(reason!=nullptr,"Archive result omitted reason");
 if(valid){require(info.checkpoint==id,"Archive ID was misdecoded");require(info.expandedSize>0&&info.compressedSize>0,"Archive sizes omitted");require(info.fingerprint==ce::checkpointFingerprint(bytes,size),"Fingerprint mismatch");}
}
}
int main(){try{
 check(minus1,sizeof(minus1),-1,true);
 check(zero,sizeof(zero),0,true);
 check(five,sizeof(five),5,true);
 check(sixtyfive,sizeof(sixtyfive),65,true);
 check(maximum,sizeof(maximum),65535,true);
 check(actors,sizeof(actors),3,true);
 check(badid,sizeof(badid),-2,false);
 check(highid,sizeof(highid),65536,false);
 check(version,sizeof(version),0,false);
 check(badactors,sizeof(badactors),0,false);
 check(magic,sizeof(magic),0,false);
 ce::CheckpointArchiveInfo info{};
 require(!ce::validateCheckpointArchive(nullptr,sizeof(zero),&info),"Null archive accepted");
 require(!ce::validateCheckpointArchive(zero,sizeof(zero),nullptr),"Null destination accepted");
 for(std::size_t n=0;n<sizeof(zero);++n)require(!ce::validateCheckpointArchive(zero,n,&info),"Truncated archive accepted");
 auto changed=std::vector<unsigned char>(zero,zero+sizeof(zero));changed.back()^=1;
 require(!ce::validateCheckpointArchive(changed.data(),changed.size(),&info),"Bad zlib Adler32 accepted");
 changed.assign(zero,zero+sizeof(zero));changed.push_back(0);
 require(!ce::validateCheckpointArchive(changed.data(),changed.size(),&info),"Trailing archive bytes accepted");
 changed.assign(zero,zero+sizeof(zero));changed[0]|=0x80;
 require(!ce::validateCheckpointArchive(changed.data(),changed.size(),&info),"Negative expanded length accepted");
 const unsigned char noncanonical[]={0x40,0,6,0,0,0,0,0,0};
 require(!ce::validateCheckpointArchive(noncanonical,sizeof(noncanonical),&info),"Noncanonical compact length accepted");
 require(!ce::validateCheckpointArchive(zero,ce::MaxCheckpointCompressed+1,&info),"Oversize input accepted");
 require(ce::validateCheckpointArchive(actors,sizeof(actors),&info)&&info.containsActors,"Actor metadata flag lost");
 std::cout<<"PASS: "<<checks<<" checkpoint archive codec checks; synthetic fixtures, compact IDs, format/version, truncation and checksum refusal.\n";
 return 0;
}catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}}
