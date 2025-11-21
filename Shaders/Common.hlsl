//***************************************************************************************
// Common.hlsl by Frank Luna (C) 2015 All Rights Reserved.
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

// Include structures and functions for lighting.
#include "LightingUtil.hlsl"

struct MaterialData
{
	float4   DiffuseAlbedo;
	float3   FresnelR0;
	float    Roughness;
	float4x4 MatTransform;
	uint     DiffuseMapIndex;
	uint     NormalMapIndex;
	uint     MatPad1;
	uint     MatPad2;
};

TextureCube gCubeMap : register(t0);
Texture2D gShadowMap : register(t1);
Texture2D gSsaoMap   : register(t2);

// An array of textures, which is only supported in shader model 5.1+.  Unlike Texture2DArray, the textures
// in this array can be different sizes and formats, making it more flexible than texture arrays.
Texture2D gTextureMaps[10] : register(t3);

// Put in space1, so the texture array does not overlap with these resources.  
// The texture array will occupy registers t0, t1, ..., t3 in space0. 
StructuredBuffer<MaterialData> gMaterialData : register(t0, space1);


SamplerState gsamPointWrap        : register(s0);
SamplerState gsamPointClamp       : register(s1);
SamplerState gsamLinearWrap       : register(s2);
SamplerState gsamLinearClamp      : register(s3);
SamplerState gsamAnisotropicWrap  : register(s4);
SamplerState gsamAnisotropicClamp : register(s5);
SamplerComparisonState gsamShadow : register(s6);

// Constant data that varies per frame.
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
	float4x4 gTexTransform;
	uint gMaterialIndex;
	uint gObjPad0;
	uint gObjPad1;
	uint gObjPad2;
};

cbuffer cbPass : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4x4 gViewProjTex;
    float4x4 gShadowTransform;

    float3   gEyePosW;
    float    cbPerObjectPad1;

    float2   gRenderTargetSize;
    float2   gInvRenderTargetSize;

    float    gNearZ;
    float    gFarZ;
    float    gTotalTime;
    float    gDeltaTime;

    float4   gAmbientLight;
    Light    gLights[MaxLights];

    // === TAA / extra ===
    float4x4 gPrevViewProj;
    float2   gInvRT;
    float2   gInvRT_dup;
    float2   gJitter;
    float2   gPrevJitter;

    int      gTaaMode;
    int      gTaaEnabledInt;
    float    gTaaFeedback;
    float    gTaaDepthThresh;

    // === skull debug ===
    float3   gSkullCenterWS;
    float    gSkullRadius;
    float4x4 gInvSkullWorld;
    float3   gSkullExtentsLS;
    float    gSkullPad;

    // === НОВОЕ: режим теней ===
    int      gShadowMode;   // 0 = Luna, 1 = soft
    float3   gShadowPad;
// === НОВОЕ: атмосфера ===
    float gAtmosphereGlobalDensity;
    float gAtmosphereHeightFalloff;
    float gAtmosphereCleanliness;
    float gAtmosphereIntensity;
};


//---------------------------------------------------------------------------------------
// Transforms a normal map sample to world space.
//---------------------------------------------------------------------------------------
float3 NormalSampleToWorldSpace(float3 normalMapSample, float3 unitNormalW, float3 tangentW)
{
	// Uncompress each component from [0,1] to [-1,1].
	float3 normalT = 2.0f*normalMapSample - 1.0f;

	// Build orthonormal basis.
	float3 N = unitNormalW;
	float3 T = normalize(tangentW - dot(tangentW, N)*N);
	float3 B = cross(N, T);

	float3x3 TBN = float3x3(T, B, N);

	// Transform from tangent space to world space.
	float3 bumpedNormalW = mul(normalT, TBN);

	return bumpedNormalW;
}

//---------------------------------------------------------------------------------------
// PCF for shadow mapping.
//---------------------------------------------------------------------------------------
//#define SMAP_SIZE = (2048.0f)
//#define SMAP_DX = (1.0f / SMAP_SIZE)
//---------------------------------------------------------------------------------------
// PCF for shadow mapping: gShadowMode = 0 -> Luna 3x3, gShadowMode = 1 -> мягкие 5x5
//---------------------------------------------------------------------------------------
float CalcShadowFactor(float4 shadowPosH)
{
    shadowPosH.xyz /= shadowPosH.w;
    float depth = shadowPosH.z;

    if (depth <= 0.0f || depth >= 1.0f)
        return 1.0f;

    uint width, height, numMips;
    gShadowMap.GetDimensions(0, width, height, numMips);

    float dx = 1.0f / (float)width;

    // === Режим 0: стандартный Luna 3x3 PCF ===
    if (gShadowMode == 0)
    {
        float percentLit = 0.0f;

        const float2 offsets[9] =
        {
            float2(-dx,  -dx), float2(0.0f,  -dx), float2(dx,  -dx),
            float2(-dx,   0.0f), float2(0.0f,   0.0f), float2(dx,   0.0f),
            float2(-dx,  +dx), float2(0.0f,  +dx), float2(dx,  +dx)
        };

        [unroll]
        for (int i = 0; i < 9; ++i)
        {
            percentLit += gShadowMap.SampleCmpLevelZero(
                gsamShadow,
                shadowPosH.xy + offsets[i],
                depth).r;
        }

        return percentLit / 9.0f;
    }
    // === Режим 1: мягкие тени 7x7, большой радиус ===
    else
    {
        const int kernelRadius = 3;      // 7x7
        float baseRadius = dx * 4.0f;    // сильно шире, чтобы было видно

        float sum = 0.0f;
        int count = 0;

        [loop]
        for (int y = -kernelRadius; y <= kernelRadius; ++y)
        {
            for (int x = -kernelRadius; x <= kernelRadius; ++x)
            {
                float2 offset = float2(x, y) * baseRadius;

                sum += gShadowMap.SampleCmpLevelZero(
                    gsamShadow,
                    shadowPosH.xy + offset,
                    depth).r;

                count++;
            }
        }

        return sum / count;
    }
}


