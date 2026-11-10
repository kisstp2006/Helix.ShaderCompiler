# Helix.ShaderCompiler

Shader compiler of the Helix engine. Shaders are written in C# with Helix.Shaders and compiled by the C# compiler; this
library reads the resulting assembly and turns the shader methods into GPU code. It contains no third party code.

Current state: the assembly reader. The translation of IL to GPU code builds on it.

## Layout

| Path | Content |
|---|---|
| `Source/Helix/ShaderCompiler/Assembly/Assembly.h` | `Assembly`: PE file, CLI metadata tables and heaps, types, fields, methods, parameters, method bodies, custom attributes |
| `Source/Helix/ShaderCompiler/Assembly/Signature.h` | Method, field, local variable, type and generic instantiation signatures; decoding of custom attribute arguments |
| `Source/Helix/ShaderCompiler/Assembly/IL.h` | IL opcodes and the instruction decoder |
| `Tests/Assembly/` | Tests against real assemblies |
| `Tests/Shaders/` | C# test shaders, built with the .NET SDK during the CMake build and embedded into the test program |
| `Tools/EmbedFile.cmake` | Turns a binary file into a C++ byte array |

## Assembly reader

```cpp
using namespace helix::shadercompiler;
Assembly assembly;
if (!assembly.Load(bytes))
    Report(assembly.Error());
u32 type = assembly.FindType("Game.Shaders", "Unlit");
u32 method = assembly.FindMethod(type, "Fragment");

MethodSig signature;
ParseMethodSignature(assembly.Method(method).signature, signature);

MethodBody body;
assembly.Body(method, body);
Instruction instruction;
for (u32 at = 0; at < body.code.Size();)
    if (!DecodeInstruction(body.code, at, instruction))
        break;
```

- Implements the parts of ECMA-335 the compiler needs: PE32 and PE32+ containers, all 45 metadata tables with their
  variable index sizes, the string, blob, user string and GUID heaps, tiny and fat method headers.
- Rows are 1 based and tokens carry the table in the top byte, as in the file format.
- Every read is bounds checked. A damaged file fails to load or yields empty values; the tests walk 400 randomly damaged
  copies of an assembly.
- The image passed to `Load` must stay alive while the `Assembly` is used; names and blobs point into it.

## Build

```
git clone https://github.com/kisstp2006/Helix.ShaderCompiler
git -C Helix.ShaderCompiler submodule update --init
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

The tests need the .NET 11 SDK (`dotnet` on the path, or `HELIX_DOTNET_ROOT` or `DOTNET_ROOT` pointing to it).

## License

GPL-3.0, see `LICENSE`.
