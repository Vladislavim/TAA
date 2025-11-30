//***************************************************************************************
// Default.hlsl (модифицировано)
// Освещение + SSAO + атмосфера с влиянием солнца + outline для черепа
//***************************************************************************************

#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 0
#endif

#include "Common.hlsl"

// =================== smoke noise helpers ===================

// простейший hash из float3 -> float
float Hash31(float3 p)
{
    p = frac(p * 0.3183099 + 0.1);
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}

// value-noise 3D с трилинейной интерполяцией
float Noise3D(float3 p)
{
    float3 i = floor(p);
    float3 f = frac(p);

    float n000 = Hash31(i + float3(0, 0, 0));
    float n100 = Hash31(i + float3(1, 0, 0));
    float n010 = Hash31(i + float3(0, 1, 0));
    float n110 = Hash31(i + float3(1, 1, 0));
    float n001 = Hash31(i + float3(0, 0, 1));
    float n101 = Hash31(i + float3(1, 0, 1));
    float n011 = Hash31(i + float3(0, 1, 1));
    float n111 = Hash31(i + float3(1, 1, 1));

    float3 u = f * f * (3.0 - 2.0 * f); // smoothstep

    float n00 = lerp(n000, n100, u.x);
    float n10 = lerp(n010, n110, u.x);
    float n01 = lerp(n001, n101, u.x);
    float n11 = lerp(n011, n111, u.x);

    float n0 = lerp(n00, n10, u.y);
    float n1 = lerp(n01, n11, u.y);

    return lerp(n0, n1, u.z);
}

// простое FBM из нескольких октав
float Fbm3D(float3 p)
{
    float sum  = 0.0f;
    float amp  = 0.5f;
    float freq = 1.0f;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        sum  += Noise3D(p * freq) * amp;
        freq *= 2.0f;
        amp  *= 0.5f;
    }

    return sum; // ~[0..1]
}

// =================== основной шейдер ===================

struct VertexIn
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float2 TexC     : TEXCOORD;
    float3 TangentU : TANGENT;
};

struct VertexOut
{
    float4 PosH        : SV_POSITION;
    float4 ShadowPosH  : POSITION0;
    float4 SsaoPosH    : POSITION1;
    float3 PosW        : POSITION2;
    float3 NormalW     : NORMAL;
    float3 TangentW    : TANGENT;
    float2 TexC        : TEXCOORD;
};

// === обычный VS ===
VertexOut VS(VertexIn vin)
{
    VertexOut vout = (VertexOut)0.0f;

    MaterialData matData = gMaterialData[gMaterialIndex];

    // в мир
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    vout.NormalW  = mul(vin.NormalL,  (float3x3)gWorld);
    vout.TangentW = mul(vin.TangentU, (float3x3)gWorld);

    // clip
    vout.PosH = mul(posW, gViewProj);

    // для SSAO
    vout.SsaoPosH = mul(posW, gViewProjTex);

    // UV
    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = mul(texC, matData.MatTransform).xy;

    // для теней
    vout.ShadowPosH = mul(posW, gShadowTransform);

    return vout;
}

// === VS для outline черепа (inverted hull) ===
VertexOut VS_SkullOutline(VertexIn vin)
{
    VertexOut vout = (VertexOut)0.0f;

    MaterialData matData = gMaterialData[gMaterialIndex];

    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    vout.NormalW  = mul(vin.NormalL,  (float3x3)gWorld);
    vout.TangentW = mul(vin.TangentU, (float3x3)gWorld);

    float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
    vout.TexC = mul(texC, matData.MatTransform).xy;

    vout.SsaoPosH   = mul(posW, gViewProjTex);
    vout.ShadowPosH = mul(posW, gShadowTransform);

    // сдвигаем по нормали наружу — толщина контура
    float3 nW = normalize(vout.NormalW);
    float outlineWidth = 0.05f;
    vout.PosW += nW * outlineWidth;

    vout.PosH = mul(float4(vout.PosW, 1.0f), gViewProj);

    return vout;
}

// === основной PS: материал + свет + туман с солнцем ===
float4 PS(VertexOut pin) : SV_Target
{
    // материал
    MaterialData matData = gMaterialData[gMaterialIndex];
    float4 diffuseAlbedo   = matData.DiffuseAlbedo;
    float3 fresnelR0       = matData.FresnelR0;
    float  roughness       = matData.Roughness;
    uint   diffuseMapIndex = matData.DiffuseMapIndex;
    uint   normalMapIndex  = matData.NormalMapIndex;

    diffuseAlbedo *= gTextureMaps[diffuseMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);

#ifdef ALPHA_TEST
    clip(diffuseAlbedo.a - 0.1f);
#endif

    pin.NormalW = normalize(pin.NormalW);

    float4 normalMapSample = gTextureMaps[normalMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);
    float3 bumpedNormalW = NormalSampleToWorldSpace(normalMapSample.rgb, pin.NormalW, pin.TangentW);
    // bumpedNormalW = pin.NormalW;

    float3 toEyeW = normalize(gEyePosW - pin.PosW);

    // SSAO
    pin.SsaoPosH /= pin.SsaoPosH.w;
    float ambientAccess = gSsaoMap.Sample(gsamLinearClamp, pin.SsaoPosH.xy, 0.0f).r;

    // освещение
    float4 ambient = ambientAccess * gAmbientLight * diffuseAlbedo;

    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    shadowFactor[0] = CalcShadowFactor(pin.ShadowPosH);

    const float shininess = (1.0f - roughness) * normalMapSample.a;
    Material mat = { diffuseAlbedo, fresnelR0, shininess };

    float4 directLight = ComputeLighting(
        gLights, mat, pin.PosW, bumpedNormalW, toEyeW, shadowFactor);

    float4 litColor = ambient + directLight;

    // отражения
    float3 r = reflect(-toEyeW, bumpedNormalW);
    float4 reflectionColor = gCubeMap.Sample(gsamLinearWrap, r);
    float3 fresnelFactor = SchlickFresnel(fresnelR0, bumpedNormalW, r);
    litColor.rgb += shininess * fresnelFactor * reflectionColor.rgb;

    // === Атмосфера: туман/дым с влиянием солнца ===
    {
        float3 worldPos  = pin.PosW;
        float3 cameraPos = gEyePosW;
        float3 viewVec   = worldPos - cameraPos;
        float  dist      = length(viewVec);

        if (dist > 0.01f)
        {
            float density       = max(gAtmosphereGlobalDensity, 0.0f);
            float heightFalloff = max(gAtmosphereHeightFalloff, 0.0f);
            float cleanliness   = saturate(gAtmosphereCleanliness);
            float intensity     = max(gAtmosphereIntensity, 0.0f);

            float3 viewDir = viewVec / dist;
            float  height  = worldPos.y;

            // экспоненциальный высотный туман
            float heightTerm   = exp(-heightFalloff * max(height, -5.0f));
            float localDensity = density * heightTerm;

            localDensity *= 4.0f;
            localDensity = clamp(localDensity, 0.0f, 4.0f);

            float fogBase = 1.0f - exp(-localDensity * dist);
            fogBase = pow(saturate(fogBase), 0.7f);

            float densityScale = lerp(3.5f, 0.8f, cleanliness);
            fogBase *= densityScale * intensity;

            float fogStart = 1.5f;
            float fogEnd   = 45.0f;
            float distNorm = saturate((dist - fogStart) / (fogEnd - fogStart));
            fogBase *= distNorm;

            // 3D-шум для клубов дыма
            float3 smokePos = worldPos * float3(0.20f, 0.30f, 0.20f);
            smokePos.y += gTotalTime * 0.15f;
            smokePos.x += gTotalTime * 0.05f;

            float smokeNoise = Fbm3D(smokePos);
            smokeNoise = saturate(smokeNoise * 1.9f - 0.5f);

            float groundBoost = saturate(1.0f - (height + 2.0f) / 10.0f);
            smokeNoise = saturate(smokeNoise + groundBoost * 0.3f);

            float smokeFactor = smokeNoise;

            float fogFactor = saturate(fogBase * smokeFactor * 1.6f);

            // === влияние солнца на туман (как в UE/AtmospherePost) ===

            // направление на солнце (свет идёт по -Direction)
            float3 sunDir   = -normalize(gLights[0].Direction);
            float3 sunColor = gLights[0].Strength;

            // высота солнца
            float sunAlt = saturate(sunDir.y * 0.5f + 0.5f); // 0 — у горизонта, 1 — высоко

            // угол между взглядом и солнцем
            float mu = dot(-viewDir, sunDir);

            // фронтальное рассеяние: яркий "ореол" вокруг солнца
            float halo = pow(saturate(mu), 32.0f) * 1.0f;

            // rayleigh-градиент: горизонт тёплый, зенит холодный
            float3 rayleighHorizon = float3(1.0f, 0.45f, 0.18f);
            float3 rayleighZenith  = float3(0.45f, 0.60f, 0.80f);
            float3 rayleighBase    = lerp(rayleighHorizon, rayleighZenith, sunAlt);

            // базовый дым как раньше
            float3 baseSmokeDirty = float3(0.30f, 0.27f, 0.26f);
            float3 baseSmokeClean = float3(0.45f, 0.52f, 0.60f);
            float3 baseSmokeColor = lerp(baseSmokeDirty, baseSmokeClean, cleanliness);

            // смешиваем дым с небесным градиентом
            float3 skyFogColor = lerp(baseSmokeColor, rayleighBase, 0.6f);

            // вклад солнца в цвет тумана
            float  sunFogStrength = saturate(halo * 0.8f + sunAlt * 0.3f);
            float3 sunFogColor    = sunColor * (0.25f + 2.0f * halo) * (0.5f + cleanliness * 0.5f);

            float3 fogColor = skyFogColor;
            fogColor = lerp(fogColor, sunFogColor, sunFogStrength);

            // затемняем самые плотные клубы
            float darken = lerp(1.0f, 0.7f, smokeFactor);
            fogColor *= darken;

            // смешиваем туман с освещённой сценой
            litColor.rgb = lerp(litColor.rgb, fogColor, fogFactor);
        }
    }

    litColor.a = diffuseAlbedo.a;
    return litColor;
}

// === PS для контура черепа (wallhack-style, ярко-красный) ===
float4 PS_SkullOutline(VertexOut pin) : SV_Target
{
    return float4(1.0f, 0.0f, 0.0f, 1.0f); // красный контур
}
