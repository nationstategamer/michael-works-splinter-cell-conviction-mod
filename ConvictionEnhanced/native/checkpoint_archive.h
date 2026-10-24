#pragma once
// Copyright (c) 2026 Michael Works. All rights reserved. Original mod work.
#include <cstddef>
#include <cstdint>
namespace ce {
struct CheckpointArchiveInfo {
    int checkpoint=-1;
    std::uint32_t expandedSize=0,compressedSize=0;
    bool containsActors=false;
    std::uint64_t fingerprint=0;
};
inline constexpr std::size_t MaxCheckpointCompressed=16u*1024u*1024u;
inline constexpr std::size_t MaxCheckpointExpanded=64u*1024u*1024u;
std::uint64_t checkpointFingerprint(const unsigned char* data,std::size_t size) noexcept;
// Verifies both bounded UE compact lengths, the entire zlib stream and Adler32,
// exact archive magic/version and the native compact header. No game calls.
bool validateCheckpointArchive(const unsigned char* data,std::size_t size,
    CheckpointArchiveInfo* out,const char** reason=nullptr) noexcept;
}
