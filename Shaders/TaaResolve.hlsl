// ================= TaaResolve.hlsl (простой/базовый) =================
Texture2D gCurrColor : register(t0);
Texture2D gHistory   : register(t1);
SamplerState gLinClamp : register(s0);

// Должно совпадать с PassCB из C++
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

    float2   gRT;      // width, height (необязательно использовать)
    float2   gInvRT;   // 1/width, 1/height

    float    gNearZ;
    float    gFarZ;

    float2   gJitter;
    float2   gPrevJitter;

    float    gTaaFeedback;   // 0..1 — вес истории
    int      gTaaEnabled;    // 0/1
    int      gTaaMode;       // игнорируем тут
    int      _pad1;
};

struct VSOut {
    float4 posH : SV_Position;
    float2 uv   : TEXCOORD0;
};

VSOut VS_FullscreenTriangle(uint vid : SV_VertexID)
{
    VSOut o;
    // Большой треугольник
    float2 verts[3] = {
        float2(-1.0, -1.0),
        float2( 3.0, -1.0),
        float2(-1.0,  3.0)
    };
    o.posH = float4(verts[vid], 0.0, 1.0);
    // Перегоняем в UV
    o.uv = 0.5 * (o.posH.xy + 1.0);
    return o;
}

float4 PS_TAA(VSOut pin) : SV_Target
{
    float2 uv = pin.uv;

    float3 curr = gCurrColor.Sample(gLinClamp, uv).rgb;

    // Если TAA выключен — показываем текущий кадр
    if (gTaaEnabled == 0)
        return float4(curr, 1.0);

    // Простое смешивание с историей (без репроекции и ограничений)
    float3 hist = gHistory.Sample(gLinClamp, uv).rgb;

    // gTaaFeedback — вес истории (0..1). Чем больше — тем стабильнее и "мыльнее".
    float kHist = saturate(gTaaFeedback);

    float3 outCol = lerp(curr, hist, kHist);

    return float4(outCol, 1.0);
}
