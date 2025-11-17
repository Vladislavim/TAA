// Shaders/TaaResolve.hlsl

#define MaxLights 16

struct Light
{
    float3 Strength;
    float  FalloffStart; // point/spot light only
    float3 Direction;    // directional/spot light only
    float  FalloffEnd;   // point/spot light only
    float3 Position;     // point/spot light only
    float  SpotPower;    // spot light only
};

cbuffer PassCB : register(b1)
{
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4x4 gViewProjTex;
    float4x4 gShadowTransform;

    float3  gEyePosW; float _pad0;

    float2  gRT;            // RenderTargetSize
    float2  gInvRT;         // InvRenderTargetSize
    float   gNearZ;
    float   gFarZ;
    float   gTotalTime;
    float   gDeltaTime;

    float4  gAmbientLight;
    Light   gLights[MaxLights];

    // === TAA ===
    float4x4 gPrevViewProj;
    float2   gJitter;
    float2   gPrevJitter;

    float2   gInvRT_TAA;    // соответствует полю InvRT в C++
    float2   gInvRT_dup;    // соответствует InvRT_dup

    float    gTaaFeedback;
    float    gTaaDepthThresh;
    int      gTaaMode;
    int      gTaaEnabledInt;

    float2   _taaPad;

    // Debug skull
    float3   gSkullCenterWS; float gSkullRadius;
    float4x4 gInvSkullWorld;
    float3   gSkullExtentsLS; float _skullPad;
};

Texture2D gCurr    : register(t0);
Texture2D gHistory : register(t1);
Texture2D gDepth   : register(t2);

// s3 = linearClamp (см. root-signature)
SamplerState gsamLinearClamp : register(s3);

// ================== Fullscreen triangle ==================
struct VSOut
{
    float4 PosH : SV_Position;
    float2 Tex  : TEXCOORD0;
};

VSOut VS_FullscreenTriangle(uint vid : SV_VertexID)
{
    VSOut o;

    // три вершины: (-1,-1), (3,-1), (-1,3)
    float2 pos =
        (vid == 0) ? float2(-1.0, -1.0) :
        (vid == 1) ? float2( 3.0, -1.0) :
                     float2(-1.0,  3.0);

    o.PosH = float4(pos, 0.0, 1.0);
    o.Tex  = 0.5 * (pos + 1.0); // NDC -> UV
    return o;
}

// ================== TAA resolve ==================
float4 PS_TAA(VSOut i) : SV_Target
{
    float2 uv = i.Tex;

    float4 curr = gCurr.SampleLevel(gsamLinearClamp, uv, 0);

    // если TAA выключен или вес истории 0 — просто текущий кадр
    if (gTaaEnabledInt == 0 || gTaaFeedback <= 0.0f)
        return curr;

    // глубина текущего кадра
    float depth = gDepth.SampleLevel(gsamLinearClamp, uv, 0).r;
    if (depth == 0.0f)
        return curr;

    // 1) screen uv -> NDC
    float4 posNdc = float4(uv * 2.0f - 1.0f, depth, 1.0f);

    // 2) NDC -> world (через инвертированный ViewProj текущего кадра)
    float4 posWorld = mul(posNdc, gInvViewProj);
    posWorld /= posWorld.w;

    // 3) world -> prev clip
    float4 prevClip = mul(posWorld, gPrevViewProj);
    if (prevClip.w <= 0.0f)
        return curr;

    float2 prevNdc = prevClip.xy / prevClip.w;
    float2 prevUv  = prevNdc * 0.5f + 0.5f;

    // если ушли за экран прошлого кадра — история невалидна
    if (prevUv.x < 0.0f || prevUv.x > 1.0f ||
        prevUv.y < 0.0f || prevUv.y > 1.0f)
    {
        return curr;
    }

    // 4) выборка истории
    float4 hist = gHistory.SampleLevel(gsamLinearClamp, prevUv, 0);

    // 5) грубая depth-защита
    float depthCurr = depth;
    float depthPrev = gDepth.SampleLevel(gsamLinearClamp, uv, 0).r; // для простоты используем тот же depth
    float depthDiff = abs(depthCurr - depthPrev);

    float reject = step(gTaaDepthThresh, depthDiff); // 1 = отбрасываем историю

    float feedback = saturate(gTaaFeedback);
    float useHist  = (1.0 - reject) * feedback;

    float4 taa = lerp(curr, hist, useHist);

    // режимы просмотра
    if (gTaaMode == 1) return curr;             // Current
    if (gTaaMode == 2) return hist;             // History
    if (gTaaMode == 3) return abs(curr - hist); // Diff

    // gTaaMode == 4 — debug по stencil, делается в C++
    return taa;
}
