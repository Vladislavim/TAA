// ======================= TaaResolve.hlsl ==========================
// t0: current color (backbuffer copy, same fmt as backbuffer)
// t1: history color (same fmt as backbuffer)
// t2: depth SRV (линейная глубина 0..1 из SSAO Normal+Depth)

Texture2D gCurrColor : register(t0);
Texture2D gHistory   : register(t1);
Texture2D gDepth     : register(t2);

// В рут-сигнатуре s3 — linearClamp (как у тебя в GetStaticSamplers).
SamplerState gLinearClamp : register(s3);

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

    float3   gEyePosW;     float _pad0;

    float2   gRT;          // (не используется здесь)
    float2   gInvRT;       // 1/width, 1/height

    float    gNearZ;
    float    gFarZ;

    // ВАЖНО: сюда кладём именно UV-джиттер (в пикселях/RT), см. UpdateMainPassCB
    float2   gJitter;      // текущий jitter в UV
    float2   gPrevJitter;  // предыдущий jitter в UV

    // Параметры TAA
    float    gTaaFeedback; // 0..1, базовый вес истории
    int      gTaaEnabled;  // 0/1
    int      gTaaMode;     // 0=final 1=curr 2=hist 3=motion 4=debugSkull
    int      _pad1;

    // ==== ДАННЫЕ ДЛЯ ПОДСВЕТКИ SKULL ====
    float3   gSkullCenterWS; float gSkullRadius;
    float4x4 gInvSkullWorld;   // используется только для тестов в отладке
    float3   gSkullExtentsLS;  float _pad2;
};

// ---------------------------------------------------------------
// Полноэкранный треугольник
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

// ---------------------------------------------------------------
// YCoCg <-> RGB (лучше удерживает хрому для вариационного клэмпа)
float3 RGBtoYCoCg(float3 c)
{
    float Y  = 0.25 * c.r + 0.5 * c.g + 0.25 * c.b;
    float Co = 0.5  * c.r - 0.5 * c.b;
    float Cg = -0.25 * c.r + 0.5 * c.g - 0.25 * c.b;
    return float3(Y, Co, Cg);
}

float3 YCoCgToRGB(float3 c)
{
    float Y=c.x, Co=c.y, Cg=c.z;
    float r = Y + Co - Cg;
    float g = Y + Cg;
    float b = Y - Co - Cg;
    return float3(r,g,b);
}

// ---------------------------------------------------------------
// Catmull-Rom upscale (упрощённый, LOD0 + билинейный)
float3 SampleCatmullRom(Texture2D tex, float2 uv)
{
    return tex.SampleLevel(gLinearClamp, uv, 0).rgb;
}

// ---------------------------------------------------------------
// Neighborhood Variance Clamp (3x3), в YCoCg
float3 NeighborhoodClampYCoCg(Texture2D currTex, float2 uv, float3 histYCoCg, float2 texel)
{
    float3 mean = 0, m2 = 0;
    float3 cmin = float3( 1e9,  1e9,  1e9);
    float3 cmax = float3(-1e9, -1e9, -1e9);

    int idx = 0;
    [unroll]
    for (int j=-1;j<=1;++j)
    {
        [unroll]
        for (int i=-1;i<=1;++i)
        {
            float2 duv = uv + float2(i,j)*texel;
            float3 c = currTex.SampleLevel(gLinearClamp, duv, 0).rgb;
            float3 ycc = RGBtoYCoCg(c);

            cmin = min(cmin, ycc);
            cmax = max(cmax, ycc);

            float3 d  = ycc - mean;
            idx++;
            mean += d / idx;
            float3 d2 = ycc - mean;
            m2 += d * d2;
        }
    }

    float3 var = m2 / max(idx - 1, 1);
    float3 sigma = sqrt(max(var, 0.0));

    const float k = 1.5;
    float3 lo = max(mean - k*sigma, cmin);
    float3 hi = min(mean + k*sigma, cmax);

    return clamp(histYCoCg, lo, hi);
}

// ---------------------------------------------------------------
// Перевод линейной глубины 0..1 в viewZ (LH)
float Depth01ToViewZ(float depth01, float2 uv)
{
    float2 ndc = uv * 2.0 - 1.0;
    float4 H = float4(ndc, depth01, 1.0);
    float4 p = mul(gInvProj, H);
    return p.z / max(p.w, 1e-6); // LH
}

// ---------------------------------------------------------------

float4 PS_TAA(VSOut pin) : SV_Target
{
    float2 uv = pin.uv;
    float2 texel = gInvRT;

    float3 curr = SampleCatmullRom(gCurrColor, uv);
    if (gTaaMode == 1) return float4(curr,1);  // «только текущий»

    if (gTaaEnabled == 0)
        return float4(curr,1);

    // Репроекция (только дельта джиттера)
    float2 jitterDeltaUV = gPrevJitter - gJitter;
    float2 prevUV = uv + jitterDeltaUV;

    bool outside = any(prevUV < 0.0) || any(prevUV > 1.0);

    float3 hist = outside
        ? curr
        : gHistory.Sample(gLinearClamp, prevUV).rgb;

    if (gTaaMode == 2) return float4(hist,1);  // «только история»

    // Edge fade по глубине — сравниваем в пространстве вида
    float dC01 = gDepth.Sample(gLinearClamp, uv).r;
    float dH01 = gDepth.Sample(gLinearClamp, prevUV).r;

    float zC = Depth01ToViewZ(dC01, uv);
    float zH = Depth01ToViewZ(dH01, prevUV);
    float dz = abs(zC - zH);

    // Подстрой коэффициент под сцену (0.02—0.1)
    float edgeFactor = saturate(1.0 - dz * 0.03);

    // Variance clamp в YCoCg
    float3 currYCoCg = RGBtoYCoCg(curr);
    float3 histYCoCg = RGBtoYCoCg(hist);
    float3 histClampedYCoCg = NeighborhoodClampYCoCg(gCurrColor, uv, histYCoCg, texel);
    float3 histClamped = YCoCgToRGB(histClampedYCoCg);

    // Базовый вес истории
    float kHist = saturate(gTaaFeedback * edgeFactor);

    float3 outCol = lerp(curr, histClamped, kHist);

    // Лёгкая "резкость"
    const float sharpen = 0.05;
    outCol += sharpen * (curr - histClamped);

    // === Визуальная подсветка skull: сам проход ограничен stencil==1 в PSO ===
    {
        const float3 tint = float3(1.0, 0.45, 0.1);
        const float  intensity = 0.35;
        outCol = lerp(outCol, tint, intensity);
    }

    // Экранное кольцо проекции сферы (гарантированно видно)
    {
        float4 cH = mul(gViewProj, float4(gSkullCenterWS, 1));
        float2 centerUV = cH.xy / cH.w * 0.5 + 0.5;

        float projScaleX = 0.5 * gProj[0][0];
        float z = max(abs(cH.w), 1e-4);
        float rUV = (gSkullRadius / z) * projScaleX;

        float dist = length(uv - centerUV);
        float ring = smoothstep(rUV, rUV - 2.0 * max(gInvRT.x, gInvRT.y), dist);
        outCol = lerp(outCol, float3(1,0,0), 0.65 * (1.0 - ring));
    }

    if (gTaaMode == 3)
    {
        // «Псевдо motion»: величина сдвига джиттера в пикселях
        float mag = length(jitterDeltaUV) / max(texel.x, texel.y);
        return float4(mag.xxx, 1);
    }

    if (gTaaMode == 4)
    {
        // Режим проверки: красная маска проекции сферы
        float4 cH = mul(gViewProj, float4(gSkullCenterWS, 1));
        float2 centerUV = cH.xy / cH.w * 0.5 + 0.5;
        float projScaleX = 0.5 * gProj[0][0];
        float z = max(abs(cH.w), 1e-4);
        float rUV = (gSkullRadius / z) * projScaleX;
        float m = step(length(uv - centerUV), rUV);
        return float4(m, 0, 0, 1);
    }

    // чуточку текущего для снижения "залипания" на очень резких краях
    outCol = lerp(outCol, curr, 0.05);

    return float4(outCol, 1);
}
