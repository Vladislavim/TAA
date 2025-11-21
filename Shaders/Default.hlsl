//***************************************************************************************
// Default.hlsl by Frank Luna (C) 2015 All Rights Reserved.
//***************************************************************************************

// Defaults for number of lights.
#ifndef NUM_DIR_LIGHTS
    #define NUM_DIR_LIGHTS 3
#endif

#ifndef NUM_POINT_LIGHTS
    #define NUM_POINT_LIGHTS 0
#endif

#ifndef NUM_SPOT_LIGHTS
    #define NUM_SPOT_LIGHTS 0
#endif

// Include common HLSL code.
#include "Common.hlsl"

struct VertexIn
{
	float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
	float2 TexC    : TEXCOORD;
	float3 TangentU : TANGENT;
};

struct VertexOut
{
	float4 PosH    : SV_POSITION;
    float4 ShadowPosH : POSITION0;
    float4 SsaoPosH   : POSITION1;
    float3 PosW    : POSITION2;
    float3 NormalW : NORMAL;
	float3 TangentW : TANGENT;
	float2 TexC    : TEXCOORD;
};

VertexOut VS(VertexIn vin)
{
	VertexOut vout = (VertexOut)0.0f;

	// Fetch the material data.
	MaterialData matData = gMaterialData[gMaterialIndex];
	
    // Transform to world space.
    float4 posW = mul(float4(vin.PosL, 1.0f), gWorld);
    vout.PosW = posW.xyz;

    // Assumes nonuniform scaling; otherwise, need to use inverse-transpose of world matrix.
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
	
	vout.TangentW = mul(vin.TangentU, (float3x3)gWorld);

    // Transform to homogeneous clip space.
    vout.PosH = mul(posW, gViewProj);

    // Generate projective tex-coords to project SSAO map onto scene.
    vout.SsaoPosH = mul(posW, gViewProjTex);
	
	// Output vertex attributes for interpolation across triangle.
	float4 texC = mul(float4(vin.TexC, 0.0f, 1.0f), gTexTransform);
	vout.TexC = mul(texC, matData.MatTransform).xy;

    // Generate projective tex-coords to project shadow map onto scene.
    vout.ShadowPosH = mul(posW, gShadowTransform);
	
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    // Fetch the material data.
    MaterialData matData = gMaterialData[gMaterialIndex];
    float4 diffuseAlbedo = matData.DiffuseAlbedo;
    float3 fresnelR0 = matData.FresnelR0;
    float  roughness = matData.Roughness;
    uint diffuseMapIndex = matData.DiffuseMapIndex;
    uint normalMapIndex = matData.NormalMapIndex;
	
    // Dynamically look up the texture in the array.
    diffuseAlbedo *= gTextureMaps[diffuseMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);

#ifdef ALPHA_TEST
    clip(diffuseAlbedo.a - 0.1f);
#endif

    // Interpolating normal can unnormalize it, so renormalize it.
    pin.NormalW = normalize(pin.NormalW);
	
    float4 normalMapSample = gTextureMaps[normalMapIndex].Sample(gsamAnisotropicWrap, pin.TexC);
    float3 bumpedNormalW = NormalSampleToWorldSpace(normalMapSample.rgb, pin.NormalW, pin.TangentW);

    //bumpedNormalW = pin.NormalW;

    float3 toEyeW = normalize(gEyePosW - pin.PosW);

    // SSAO
    pin.SsaoPosH /= pin.SsaoPosH.w;
    float ambientAccess = gSsaoMap.Sample(gsamLinearClamp, pin.SsaoPosH.xy, 0.0f).r;

    // Light terms.
    float4 ambient = ambientAccess * gAmbientLight * diffuseAlbedo;

    // Only the first light casts a shadow.
    float3 shadowFactor = float3(1.0f, 1.0f, 1.0f);
    shadowFactor[0] = CalcShadowFactor(pin.ShadowPosH);

    const float shininess = (1.0f - roughness) * normalMapSample.a;
    Material mat = { diffuseAlbedo, fresnelR0, shininess };
    float4 directLight = ComputeLighting(gLights, mat, pin.PosW,
        bumpedNormalW, toEyeW, shadowFactor);

    float4 litColor = ambient + directLight;

    // Add in specular reflections.
    float3 r = reflect(-toEyeW, bumpedNormalW);
    float4 reflectionColor = gCubeMap.Sample(gsamLinearWrap, r);
    float3 fresnelFactor = SchlickFresnel(fresnelR0, bumpedNormalW, r);
    litColor.rgb += shininess * fresnelFactor * reflectionColor.rgb;

         // === Атмосфера: высотный туман + солнце + "god rays" ===
    {
        float3 worldPos  = pin.PosW;
        float3 cameraPos = gEyePosW;
        float  dist      = length(worldPos - cameraPos);
        float  height    = worldPos.y;

        // 1) Плотность по высоте (Exponential Height Fog)
        float baseDensity =
            gAtmosphereGlobalDensity * exp(-gAtmosphereHeightFalloff * max(height, 0.0f));

        // 2) "Чистота" 0..1 (X/Z с клавиатуры)
        float cleanliness = saturate(gAtmosphereCleanliness);

        // Грязный воздух = очень плотный, чистый = почти прозрачный
        float density = lerp(baseDensity * 4.0f, baseDensity * 0.12f, cleanliness);

        // 3) Дальний туман: ближний план почти чистый, даль — в дымке
        float farStart = 5.0f;   // откуда начинаем туманить
        float farEnd   = 80.0f;  // где туман почти максимален
        float distNorm = saturate((dist - farStart) / (farEnd - farStart));

        // 4) Экстинкция по Бугеру–Ламберту–Беру
        float fogAmount = 1.0f - exp(-density * dist);
        fogAmount = pow(saturate(fogAmount), 0.7f); // усиливаем средние значения

        // 5) Цвет тумана по высоте + "грязности"
        float heightFactor = saturate((height - 0.0f) / 30.0f); // 0 у земли, 1 вверху

        float3 fogBaseDirty = float3(0.72f, 0.55f, 0.48f);   // смог/пыль
        float3 fogBaseClean = float3(0.40f, 0.65f, 0.96f);   // чистое голубое небо
        float3 fogBaseColor = lerp(fogBaseDirty, fogBaseClean, cleanliness);

        float3 horizonTintDirty = float3(0.90f, 0.65f, 0.50f);
        float3 horizonTintClean = float3(0.55f, 0.72f, 0.98f);
        float3 horizonTint = lerp(horizonTintDirty, horizonTintClean, cleanliness);

        float3 skyTintDirty = float3(0.65f, 0.62f, 0.70f);
        float3 skyTintClean = float3(0.35f, 0.55f, 0.90f);
        float3 skyTint = lerp(skyTintDirty, skyTintClean, cleanliness);

        float3 fogHeightColor = lerp(horizonTint, skyTint, heightFactor);

        // базовый цвет тумана
        float3 fogColor = lerp(fogBaseColor, fogHeightColor, 0.6f);

          // 6) “God rays” вокруг солнца (упрощённый и заметный вариант)
        {
            // направление на солнце (от точки к солнцу)
            float3 sunDir = normalize(-gLights[0].Direction);

            // направление от точки к камере
            float3 toCamera = normalize(cameraPos - worldPos);

            // угол между солнцем и взглядом: 1 — смотрим почти на солнце
            float cosSunView = dot(sunDir, toCamera);

            // узкий "конус" вдоль направления на солнце
            float sunLobe = pow(saturate(cosSunView), 12.0f);

            // усиливаем эффект в тумане и вдали
            float godRaysFactor =
                sunLobe * fogAmount * distNorm;

            // цвет ореола вокруг солнца
            float3 sunGlowDirty = float3(1.3f, 0.90f, 0.65f);
            float3 sunGlowClean = float3(1.05f, 0.95f, 0.85f);
            float3 sunGlowColor = lerp(sunGlowDirty, sunGlowClean, cleanliness);

            // добавляем “лучи” в цвет тумана
            fogColor += sunGlowColor * godRaysFactor * 3.0f;
        }

        // 7) Финальный вес тумана:
        //    - сильнее при грязной атмосфере
        //    - только вдали
        float cleanlinessScale = lerp(1.6f, 0.9f, cleanliness);
        float finalFogAmount =
            saturate(fogAmount * gAtmosphereIntensity * distNorm * cleanlinessScale);

        // 8) Применяем
        litColor.rgb = lerp(litColor.rgb, fogColor, finalFogAmount);
    }

    // Alpha как раньше — из диффузной текстуры
    litColor.a = diffuseAlbedo.a;
    return litColor;
}
