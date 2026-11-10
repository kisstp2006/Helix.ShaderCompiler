#pragma once

#include <Helix/Core/Containers/Array.h>
#include <Helix/Core/Strings/Format.h>
#include <Helix/Image/Codecs/Png.h>
#include <Helix/Platform/IO/FileSystem.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace helix::shadercompiler::testing
{
    // Compares the RGBA8 image of a case with Tests/Golden/<name>.png within `tolerance` per channel. With
    // HELIX_UPDATE_GOLDEN=1 the first backend that draws a name writes its file; every other backend and GPU is compared
    // with it. A mismatch writes the image to the temp directory.
    inline bool MatchGolden(StringView label, const u8* pixels, u32 width, u32 height, u32 tolerance = 2)
    {
        // Labels hold the keyword values: "Keywords Stripes=1 Quality=2" is Keywords.Stripes1.Quality2.
        String name;
        for (char c : label)
            if (c == ' ')
                name.Append('.');
            else if (c != '=')
                name.Append(c);
        String path = platform::JoinPath(HELIX_SHADER_GOLDEN_DIR, helix::Format("{}.png", name));
        image::Image drawn;
        drawn.Allocate(width, height, image::ImageFormat::RGBA8);
        std::memcpy(drawn.pixels.Data(), pixels, drawn.pixels.Size());

        static Array<String> written;
        const char* update = std::getenv("HELIX_UPDATE_GOLDEN");
        bool first = !written.Contains(name);
        if (update && update[0] == '1' && first)
        {
            Array<u8> file;
            image::EncodePng(drawn, file, 9);
            written.Push(name);
            return platform::WriteFile(path, file);
        }
        Array<u8> file;
        image::Image golden;
        if (!platform::ReadFile(path, file) || !image::DecodePng(file, golden))
        {
            std::printf("    golden %s missing, run with HELIX_UPDATE_GOLDEN=1\n", name.CStr());
            return false;
        }
        if (golden.width != width || golden.height != height)
        {
            std::printf("    golden %s is %ux%u, drawn %ux%u\n", name.CStr(), golden.width, golden.height, width, height);
            return false;
        }
        u32 bad = 0, worst = 0;
        for (usize i = 0; i < golden.pixels.Size(); i++)
        {
            i32 a = golden.pixels[i], b = drawn.pixels[i];
            u32 diff = u32(a > b ? a - b : b - a);
            worst = Max(worst, diff);
            bad += diff > tolerance ? 1 : 0;
        }
        if (!bad)
            return true;
        String actual = platform::JoinPath(platform::TempDirectory(), helix::Format("{}.actual.png", name));
        Array<u8> out;
        image::EncodePng(drawn, out);
        platform::WriteFile(actual, out);
        std::printf("    golden %s: %u channels differ by more than %u (worst %u), drawn image in %s\n", name.CStr(), bad, tolerance, worst, actual.CStr());
        return false;
    }
}
