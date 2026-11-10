// Fuzzes the assembly reader, the PDB reader, the definition readers, the translation and the backends with mutated
// copies of the test assemblies. Every round is reproducible from its number.
//
// usage: helix_shader_fuzz <seconds> [first round] [progress file]
// The progress file holds the round being run, so a crash names the round to run again with `<seconds> <round>`.

#include <Support/Fuzz.h>

#include <Helix/Core/Basics/Assert.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#include <windows.h>

#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
#endif

using namespace helix;
using namespace helix::shadercompiler;
using namespace helix::shadercompiler::testing;

namespace
{
    // A failed assertion prints where it came from, so a round that breaks a check can be found without a debugger.
    AssertAction PrintStack(const char* expression, const char* message, const char* file, int line)
    {
        std::fprintf(stderr, "%s(%d): assertion failed: %s %s\n", file, line, expression, message ? message : "");
#ifdef _WIN32
        HANDLE process = GetCurrentProcess();
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(process, nullptr, TRUE);
        void* frames[48];
        USHORT count = CaptureStackBackTrace(1, 48, frames, nullptr);
        alignas(SYMBOL_INFO) char storage[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
        for (USHORT i = 0; i < count; i++)
        {
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 address = DWORD64(frames[i]), offset = 0;
            IMAGEHLP_LINE64 where{};
            where.SizeOfStruct = sizeof(where);
            DWORD column = 0;
            bool named = SymFromAddr(process, address, &offset, symbol);
            bool placed = SymGetLineFromAddr64(process, address, &column, &where);
            std::fprintf(stderr, "    %s  %s:%lu\n", named ? symbol->Name : "?", placed ? where.FileName : "?", placed ? where.LineNumber : 0ul);
        }
#endif
        std::fflush(stderr);
        return AssertAction::Abort;
    }
}

int main(int argc, char** argv)
{
    SetAssertHandler(&PrintStack);
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: helix_shader_fuzz <seconds> [first round] [progress file]\n");
        return 2;
    }
    double seconds = std::atof(argv[1]);
    u64 round = argc > 2 ? u64(std::strtoull(argv[2], nullptr, 10)) : 0;
    const char* progress = argc > 3 ? argv[3] : "shader_fuzz_progress.txt";

    // A round that takes a minute is a hang.
    std::atomic<u64> current{round};
    std::atomic<bool> done{false};
    std::thread watchdog([&] {
        u64 seen = current.load();
        auto since = std::chrono::steady_clock::now();
        while (!done.load())
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            u64 now = current.load();
            if (now != seen)
            {
                seen = now;
                since = std::chrono::steady_clock::now();
            }
            else if (std::chrono::steady_clock::now() - since > std::chrono::seconds(60))
            {
                std::fprintf(stderr, "round %llu hangs\n", static_cast<unsigned long long>(now));
                std::fflush(stderr);
                std::abort();
            }
        }
    });

    auto start = std::chrono::steady_clock::now();
    u64 rounds = 0, translated = 0;
    FuzzRound fuzz{};
    for (;; round++)
    {
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= seconds)
            break;
        current.store(round);
        if (std::FILE* f = std::fopen(progress, "wb"))
        {
            std::fprintf(f, "%llu\n", static_cast<unsigned long long>(round));
            std::fclose(f);
        }
        translated += fuzz.Run(round);
        rounds++;
        if (rounds % 1000 == 0)
        {
            std::printf("%llu rounds, %.0f s, %llu entry points translated\n", static_cast<unsigned long long>(rounds), elapsed,
                        static_cast<unsigned long long>(translated));
            std::fflush(stdout);
        }
    }
    done.store(true);
    watchdog.join();
    std::printf("done: %llu rounds without a crash or a hang, %llu entry points translated\n", static_cast<unsigned long long>(rounds),
                static_cast<unsigned long long>(translated));
    return 0;
}
