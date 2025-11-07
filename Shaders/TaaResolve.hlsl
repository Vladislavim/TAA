// Shaders/TaaResolve.hlsl
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

    float3   gEyePosW; float _pad0;

    float2   gRT;            // RenderTargetSize
    float2   gInvRT;         // InvRenderTargetSize
    float    gNearZ;
    float    gFarZ;
    float    gTotalTime;
    float    gDeltaTime;

    float4   gAmbientLight;

    // --- TAA ---
    float4x4 gPrevViewProj;
    float2   gJitter;
    float2   gPrevJitter;
    float2   gInvRT_dup;     // дублируем (C++ тоже пишет InvRT сюда)
    float    gTaaFeedback;
    float    gTaaDepthThresh;
    int      gTaaMode;
    int      gTaaEnabledInt;

    float2   _taaPad;

    // Debug skull (не обязателен для компиляции PS, но держим layout)
    float3   gSkullCenterWS; float gSkullRadius;
    float4x4 gInvSkullWorld;
    float3   gSkullExtentsLS; float _skullPad;
};

Texture2D gCurr    : register(t0);
Texture2D gHistory : register(t1);
Texture2D gDepth   : register(t2);

// Статические сэмплеры из root-signature:
// s0 pointWrap, s1 pointClamp, s2 linearWrap, s3 linearClamp, s4 anisoWrap, s5 anisoClamp, s6 shadow
SamplerState gsamLinearClamp : register(s3);

// === Полноэкранный треугольник ===
struct VSOut {
    float4 PosH : SV_Position;
    float2 Tex  : TEXCOORD0;
};

VSOut VS_FullscreenTriangle(uint vid : SV_VertexID)
{
    VSOut o;
    // 3 вершины: (-1,-1), (3,-1), (-1,3)
    float2 pos =
        (vid == 0) ? float2(-1.0, -1.0) :
        (vid == 1) ? float2( 3.0, -1.0) :
                     float2(-1.0,  3.0);

    o.PosH = float4(pos, 0.0, 1.0);
    o.Tex  = 0.5 * (pos + 1.0); // NDC->UV
    return o;
}

// Простейшая TAA: смешиваем текущий и историю, историю подвыравниваем на разницу джиттера
float4 PS_TAA(VSOut i) : SV_Target
{
    float2 uv = i.Tex;

    // текущий кадр
    float4 curr = gCurr.SampleLevel(gsamLinearClamp, uv, 0);

    // смещение истории на дельту джиттера (в UV)
    float2 deltaJ = gJitter - gPrevJitter;
    float4 hist = gHistory.SampleLevel(gsamLinearClamp, uv + deltaJ, 0);

    // простая глубинная защита от «призраков» (если доступна глубина)
    float dCurr = gDepth.SampleLevel(gsamLinearClamp, uv, 0).r;
    float dHist = gDepth.SampleLevel(gsamLinearClamp, uv + deltaJ, 0).r;

    float reject = step(gTaaDepthThresh, abs(dCurr - dHist)); // 1 = различаются сильно
    float feedback = gTaaEnabledInt ? gTaaFeedback : 0.0;
    float useHist = (1.0 - reject) * feedback;

    float4 taa = lerp(curr, hist, useHist);

    // режимы просмотра (как в C++: 0..4)
    if (gTaaMode == 1) return curr;               // NoTAA
    if (gTaaMode == 2) return hist;               // ShowHistory
    if (gTaaMode == 3) return abs(curr - hist);   // ShowDiff
    // gTaaMode==4 можно потом подсветить череп, сейчас возвращаем TAA
    return taa;
}
