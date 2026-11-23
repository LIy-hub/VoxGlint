"""Shared lunar relief and scattering for the model and Minecraft sky material."""

LUNAR_LIGHTING = r"""
struct LunarLighting
{
    float3 Radial(float2 uv)
    {
        float lon = (uv.x - 0.5) * 6.28318530718;
        float lat = (0.5 - uv.y) * 3.14159265359;
        return float3(sin(lon) * cos(lat), sin(lat), cos(lon) * cos(lat));
    }

    float3 East(float3 radial)
    {
        float2 xz = radial.xz;
        if (dot(xz, xz) < 1e-8) return float3(1.0, 0.0, 0.0);
        return normalize(float3(radial.z, 0.0, -radial.x));
    }

    float3 GradeAlbedo(float3 color)
    {
        // Keep bright ejecta rays while separating the darker maria and crater floors.
        float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
        return color * pow(saturate(luminance), 0.22);
    }

    float Intensity(float3 radial, float3 normalTS, float3 view, float phase, float latitude, float relief,
                    float earthshine)
    {
        float3 east = East(radial);
        float3 south = cross(east, radial);
        float3 normal = normalize(radial * max(normalTS.z, 0.001) +
                                  relief * (east * normalTS.x + south * normalTS.y));

        // Minecraft indexes from full through waning (left lit), new, then waxing (right lit).
        float angle = phase * 0.78539816339;
        float latitudeRad = radians(latitude);
        float3 light = float3(-sin(angle) * cos(latitudeRad), sin(latitudeRad), cos(angle) * cos(latitudeRad));
        float emissionCos = saturate(dot(radial, view));
        float incidenceCos = saturate(dot(normal, light));
        float terrainHorizon = smoothstep(-0.06, 0.015, dot(radial, light));

        // Mostly Lommel-Seeliger scattering with a small diffuse term for limb falloff.
        float scatter = 2.0 * incidenceCos / max(emissionCos + incidenceCos, 0.015);
        float phaseFade = smoothstep(-1.0, -0.94, cos(angle));
        return terrainHorizon * lerp(scatter, incidenceCos, 0.18) * phaseFade +
               earthshine * smoothstep(0.0, 0.45, emissionCos);
    }
};
LunarLighting lunar;
"""

LUNAR_SKY_RENDER = r"""
    // Intersect each view ray with the apparent lunar sphere. The sky remains camera-relative.
    float3 mDir = normalize(MoonDir.xyz);
    float phaseVisibility = saturate(0.5 + 0.5 * cos(MoonPhase * 0.78539816339));
    if (mDir.z > -0.05 && phaseVisibility > 0.001) {
        float cosTheta = dot(viewDir, mDir);
        float angle = acos(clamp(cosTheta, -1.0, 1.0));
        float halo = exp(-angle * 16.0) * 0.032 + exp(-angle * 5.0) * 0.008;
        sky += float3(0.58, 0.72, 1.0) * halo * sqrt(phaseVisibility) *
               (nightGain * 0.45) * nightFade * (1.0 - overcast * 0.75) * horizonMask;

        float sinRadius = sin(radians(clamp(MoonDiameter, 0.1, 15.0) * 0.5));
        if (cosTheta > 0.0 && angle < asin(sinRadius) + lunarPixelAngle) {
            // The Minecraft orbit lies in YZ. A fixed X reference keeps lunar north continuous at zenith.
            float3 rightRef = abs(mDir.x) < 0.99 ? float3(1, 0, 0) : float3(0, 1, 0);
            float3 mRight = normalize(rightRef - mDir * dot(rightRef, mDir));
            float3 mUp = cross(mDir, mRight);
            float2 disk = float2(dot(viewDir, mRight), dot(viewDir, mUp)) / sinRadius;
            float diskLength = length(disk);
            float coverage = saturate((1.0 - diskLength) / max(lunarPixelAngle / sinRadius, 0.0001) + 0.5);
            disk *= min(1.0, 0.99999 / max(diskLength, 1e-5));
            float3 radial = float3(disk, sqrt(saturate(1.0 - dot(disk, disk))));
            float2 uv = float2(0.5 + atan2(radial.x, radial.z) / 6.28318530718,
                               0.5 - asin(clamp(radial.y, -1.0, 1.0)) / 3.14159265359);

            // Pass explicit gradients to avoid undefined mip choice at the divergent disk edge.
            #if PIXELSHADER
            float2 diskDx = float2(dot(lunarViewDx, mRight), dot(lunarViewDx, mUp)) / sinRadius;
            float2 diskDy = float2(dot(lunarViewDy, mRight), dot(lunarViewDy, mUp)) / sinRadius;
            float3 sphereDx = float3(diskDx, -dot(disk, diskDx) / max(radial.z, 0.035));
            float3 sphereDy = float3(diskDy, -dot(disk, diskDy) / max(radial.z, 0.035));
            float3 east = lunar.East(radial);
            float3 south = cross(east, radial);
            float lonScale = 6.28318530718 * max(length(radial.xz), 0.035);
            float2 uvDx = float2(dot(east, sphereDx) / lonScale, dot(south, sphereDx) / 3.14159265359);
            float2 uvDy = float2(dot(east, sphereDy) / lonScale, dot(south, sphereDy) / 3.14159265359);
            float3 color = Texture2DSampleGrad(LunarAlbedo, LunarAlbedoSampler, uv, uvDx, uvDy).rgb;
            float2 slope = Texture2DSampleGrad(LunarRelief, LunarReliefSampler, uv, uvDx, uvDy).rg * 2.0 - 1.0;
            #else
            float3 color = Texture2DSampleLevel(LunarAlbedo, LunarAlbedoSampler, uv, 1).rgb;
            float2 slope = Texture2DSampleLevel(LunarRelief, LunarReliefSampler, uv, 1).rg * 2.0 - 1.0;
            #endif
            slope *= LunarReliefStrength;
            float3 normalTS = float3(slope, sqrt(saturate(1.0 - dot(slope, slope))));
            float lit = lunar.Intensity(radial, normalTS, float3(0, 0, 1), MoonPhase, 6.0, 1.35, 0.0);
            float phaseBackscatter = lerp(0.68, 1.0, sqrt(phaseVisibility));
            float3 radiance = lunar.GradeAlbedo(color) * lit * nightGain * MoonLuminance * phaseBackscatter *
                              (1.0 - overcast * 0.85);
            sky += coverage * (radiance * nightFade * horizonMask - starsBehindMoon);
        }
    }
"""
