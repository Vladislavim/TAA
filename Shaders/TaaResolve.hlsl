// ======================= TaaResolve.hlsl ==========================
// t0: current color (backbuffer copy, R16G16B16A16_FLOAT)
// t1: history color (R16G16B16A16_FLOAT)
// t2: depth SRV (линейная глубина 0..1 или depth-экспорт из SSAO)
Texture2D gCurrColor : register(t0);
Texture2D gHistory   : register(t1);
Texture2D gDepth     : register(t2);

// В твоём рут-сигнатуре s3 — linearClamp (см. GetStaticSamplers).
SamplerState gLinearClamp : register(s3);

cbuffer PassCB : register(b1)
{
    // ── то, что уже есть в твоём PassConstants (порядок важен) ──
    float4x4 gView;
    float4x4 gInvView;
    float4x4 gProj;
    float4x4 gInvProj;
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4x4 gViewProjTex;
    float4x4 gShadowTransform;

    float3   gEyePosW;     float _pad0;

    float2   gRT;          // width,height (не используем)
    float2   gInvRT;       // 1/width, 1/height

    float    gNearZ;       // (не используем)
    float    gFarZ;        // (не используем)

    // ВАЖНО: сюда клади именно UV-джиттер (в пикселях, делённых на RT):
    // см. патч в UpdateMainPassCB ниже
    float2   gJitter;      // текущий jitter в UV
    float2   gPrevJitter;  // предыдущий jitter в UV

    float    gTaaFeedback; // 0..1, вес истории
    int      gTaaEnabled;  // 0/1
    int      gTaaMode;     // 0=final 1=curr 2=hist 3=motion
    int      _pad1;
};

struct VSOut {
    float4 posH : SV_Position;
    float2 uv   : TEXCOORD0;
};

VSOut VS_FullscreenTriangle(uint vid : SV_VertexID)
{
    VSOut o;
    float2 verts[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };
    o.posH = float4(verts[vid], 0.0, 1.0);
    o.uv   = 0.5 * (o.posH.xy + 1.0);
    return o;
}

// Базовый neighborhood clamp 3x3
float3 NeighborhoodClamp(Texture2D currTex, float2 uv, float3 hist, float2 texel)
{
    float3 cmin = float3(  1e9,  1e9,  1e9);
    float3 cmax = float3( -1e9, -1e9, -1e9);

    [unroll]
    for (int j=-1;j<=1;++j)
    {
        [unroll]
        for (int i=-1;i<=1;++i)
        {
            float2 duv = uv + float2(i,j)*texel;
            float3 c = currTex.SampleLevel(gLinearClamp, duv, 0).rgb;
            cmin = min(cmin, c);
            cmax = max(cmax, c);
        }
    }
    return clamp(hist, cmin, cmax);
}

float4 PS_TAA(VSOut pin) : SV_Target
{
    float2 uv = pin.uv;

    float3 curr = gCurrColor.Sample(gLinearClamp, uv).rgb;
    if (gTaaMode == 1) return float4(curr,1);

    if (gTaaEnabled == 0)
        return float4(curr,1);

    // репроекция: только компенсация джиттера (без матриц),
    // потому что мы кладём в PassCB уже UV-смещения на кадр.
    float2 jitterDeltaUV = gPrevJitter - gJitter;
    float2 prevUV = uv + jitterDeltaUV;

    bool outside = any(prevUV < 0.0) || any(prevUV > 1.0);
    float3 hist = outside ? curr : gHistory.Sample(gLinearClamp, prevUV).rgb;

    if (gTaaMode == 2) return float4(hist,1);

    // depth-based history weight (снижаем вклад истории на контрасте по глубине)
    float dC = gDepth.Sample(gLinearClamp, uv).r;
    float dH = gDepth.Sample(gLinearClamp, prevUV).r;        // ок, пусть будет та же текстура
    float dz = abs(dC - dH);
    float edgeFactor = saturate(1.0 - dz * 8.0); // чем больше разница — тем меньше история
    float kHist = saturate(gTaaFeedback * edgeFactor);

    float3 histClamped = NeighborhoodClamp(gCurrColor, uv, hist, gInvRT);
    float3 outCol = lerp(curr, histClamped, kHist);

    if (gTaaMode == 3)
    {
        float mag = length(jitterDeltaUV) / max(gInvRT.x, gInvRT.y); // «псевдо motion»
        return float4(mag.xxx, 1);
    }

    return float4(outCol, 1);
}
