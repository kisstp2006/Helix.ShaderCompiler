# Helix.ShaderCompiler

Shader compiler of the Helix engine. Shaders are written in C# with Helix.Shaders and compiled by the C# compiler; this
library reads the resulting assembly and turns the shader methods into GPU code. It contains no third party code.

Current state: vertex and fragment shaders compile to GLSL 3.30, GLSL 4.50 and GLSL ES 3.00.

## Layout

| Path | Content |
|---|---|
| `Source/Helix/ShaderCompiler/Assembly/` | `Assembly` (PE file, CLI metadata, method bodies, custom attributes), signatures, IL instruction decoder |
| `Source/Helix/ShaderCompiler/IR/` | The module a shader is translated to: types, structs, expressions, structured statements, functions, resources |
| `Source/Helix/ShaderCompiler/Frontend/` | `TranslateEntryPoint`: IL of an entry point and everything it calls to a module |
| `Source/Helix/ShaderCompiler/Backends/` | `EmitGlsl`: a module to GLSL source with its binding table |
| `Tests/Shaders/` | C# test shaders, built with the .NET SDK during the CMake build and embedded into the test program |
| `Tests/Oracle/` | C# program that runs the test shaders on the CPU and writes the images the GPU must reproduce |
| `Tests/Execution/` | Compiles every case, draws it through Helix.RHI and compares it with the CPU image |
| `Tools/EmbedFile.cmake` | Turns a binary file into a C++ byte array |

## Compiling a shader

```cpp
using namespace helix::shadercompiler;
Assembly assembly;
assembly.Load(bytes);
u32 type = assembly.FindType("Game.Shaders", "Unlit");

Module module;
GlslShader shader;
String error;
if (!TranslateEntryPoint(assembly, assembly.FindMethod(type, "Fragment"), Stage::Fragment, module, error) ||
    !EmitGlsl(module, GlslTarget::Glsl330, shader, error))
    Report(error);
// shader.source is the GLSL text, shader.bindings maps the names in it to the slots given in C#.
```

The C# method

```csharp
[FragmentShader]
[return: Target(0)]
public static float4 Fragment(Varyings input)
{
    float4 texel = albedo.Sample(albedoSampler, input.uv);
    float3 color = texel.xyz * draw.tint.xyz;
    if (texel.w < 0.5f)
        Discard();
    return new float4(color, texel.w);
}
```

becomes

```glsl
vec4 f_Fragment_0(in S_Varyings_0 p_input)
{
    vec4 l0;
    vec3 l1;
    l0 = texture(g_albedo, p_input.m_uv);
    l1 = (l0.xyz * g_draw.m_tint.xyz);
    if ((l0.w < 0.5))
    {
        discard;
    }
    return vec4(l1, l0.w);
}
```

## How it works

1. The assembly reader finds the method, its signature and its IL.
2. The frontend runs the IL on a symbolic stack: every instruction builds an expression, stores become statements. Calls into
   the shader library turn into operations (`ShaderMath.Dot` is `dot`, `float3.xyz` is a swizzle, `float4 * float4` is a
   multiplication); other static methods are translated the same way and become functions, generic ones once per type.
3. The branches of the IL form a control flow graph. Short circuit conditions are merged back into `&&` and `||`, loops are
   found from back edges, and the graph is written as `if`, `while`, `break`, `continue` and `return`.
4. The backend prints the module as GLSL and checks that every uniform struct has the same layout in C# and in std140.

## What shader code may use

- `float`, `int`, `uint`, `bool`, the vector and matrix types of Helix.Shaders, structs of those (also nested).
- Static methods, generic methods, `ref`, `out` and `in` parameters, methods of user structs.
- `if`, `for`, `while`, `do`, `switch`, `break`, `continue`, early `return`, `?:`, `&&`, `||`.
- The functions of `ShaderMath`, `Texture2D.Sample`, `SampleLevel`, `Load`, `Size`, and uniform structs, textures and samplers
  declared as static fields.
- `double` is computed as `float`.

Not available, each reported with the method name and the reason: classes and anything allocated, recursion, exceptions,
`goto`, 64 bit integers, storage buffers and compute shaders (not implemented yet), calls into other libraries.
Keywords are always off for now; variants are not selected yet.

## Conventions of the generated GLSL

| C# | GLSL |
|---|---|
| vertex input `[Location(n)]` | `layout(location = n) in ... hx_in<n>` |
| value from vertex to fragment `[Location(n)]` | `hx_v<n>`, `flat` for integers |
| fragment output `[Target(n)]` | `layout(location = n) out ... hx_target<n>` |
| `[Position]` | `gl_Position` in the vertex stage, `gl_FragCoord` in the fragment stage |
| `[Uniform(slot)] static T name` | `layout(std140) uniform hx_block_name { T g_name; }` |
| `[Texture(slot)] static Texture2D name` | `uniform sampler2D g_name`; the sampler state travels with the slot |

## Tests

- Assembly reader: tables, signatures, instructions and attributes of real assemblies, and 400 randomly damaged files.
- Execution: 14 C# shaders (arithmetic, branches, loops, switch, functions with `ref`/`out` and generics, library functions,
  integers, matrices, uniforms, textures, discard, bool vectors, nested structs) run on the CPU as C# and on the GPU as
  generated GLSL 3.30 and 4.50; the images must agree within 2/255. They run on every OpenGL device the machine offers.
- GLSL ES 3.00 output is checked as text only; it is not executed yet.
- Rejected shaders: recursion, a uniform struct whose layout differs between C# and std140, a missing stage attribute.

`HELIX_PRINT_SHADERS=<case name>` prints the generated fragment shader of one execution case.

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
