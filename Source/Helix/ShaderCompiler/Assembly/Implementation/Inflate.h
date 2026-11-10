#pragma once

#include <Helix/Core/Basics/Span.h>
#include <Helix/Core/Containers/Array.h>

namespace helix::shadercompiler
{
    // Decompresses raw deflate data (RFC 1951), as embedded portable PDBs store it. Fails on damaged data and when the
    // result would be larger than `limit`.
    bool Inflate(Span<const u8> in, usize limit, Array<u8>& out);
}
