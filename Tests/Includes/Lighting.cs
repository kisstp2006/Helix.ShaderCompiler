using System;
using HelixEngine.Shaders;
using static HelixEngine.Shaders.ShaderMath;

// Shader code in an assembly of its own, used by the test shaders the way shaders use include files.
namespace Helix.ShaderIncludes;

public enum Falloff
{
    Linear,
    Squared,
}

public struct SurfaceData
{
    public float3 albedo;
    public float roughness;

    public readonly float Gloss => 1.0f - roughness;
}

public struct LightData
{
    public float4 direction;
    public float4 color;
}

public static class Lighting
{
    /// <summary>A resource declared by the include, bound by every shader that uses it.</summary>
    [Uniform(7)] public static LightData mainLight;

    [Texture(7)] public static Texture2D<float4> ramp;
    [Sampler(7)] public static SamplerState rampSampler;

    public static float Lambert(float3 normal, float3 direction) => Saturate(Dot(normal, direction));

    public static float Attenuate(float distance, Falloff falloff) => falloff == Falloff.Squared ? 1.0f / (1.0f + distance * distance) : 1.0f / (1.0f + distance);

    public static float3 Shade(SurfaceData surface, float3 normal, LightData light, Falloff falloff)
    {
        float3 lit = surface.albedo * Lambert(normal, light.direction.xyz) * light.color.xyz;
        return lit * Attenuate(light.direction.w, falloff) + surface.Gloss * 0.125f;
    }

    /// <summary>Takes a delegate of the calling shader.</summary>
    public static float Remap(float value, Func<float, float> curve) => curve(value) * 0.5f + 0.25f;

    public static float3 ShadeMain(SurfaceData surface, float3 normal) => Shade(surface, normal, mainLight, Falloff.Linear) * ramp.Sample(rampSampler, new float2(0.5f, 0.5f)).xyz;
}
