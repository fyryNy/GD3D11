#include "HDRColor.hlsli"

SamplerState SS_Linear : register(s0);
Texture2D TX_Scene : register(t0);
Texture2D TX_AntBlack : register(t1);
Texture2D TX_AntWhite : register(t2);
Texture2D TX_D2D : register(t3);

cbuffer GammaCorrectConstantBuffer : register(b0)
{
    float G_Gamma;
    float G_Brightness;
    float2 G_TextureSize;
    float G_SharpenStrength;
    float G_HDROutput;
    float G_HDRPaperWhiteNits;
    float G_HDRPeakNits;
};

struct PS_INPUT
{
    float2 vTexcoord : TEXCOORD0;
    float3 vEyeRay : TEXCOORD1;
    float4 vPosition : SV_POSITION;
};

float4 PSMain(PS_INPUT input) : SV_TARGET
{
    float3 scene = TX_Scene.Sample(SS_Linear, input.vTexcoord).rgb;
    float gamma = isfinite(G_Gamma) ? clamp(G_Gamma, 0.01f, 8.0f) : 1.0f;
    float brightness = isfinite(G_Brightness) ? max(G_Brightness, 0.0f) : 1.0f;
    scene = HDRFiniteColor(pow(min(HDRFiniteColor(scene * brightness), 65504.0f), gamma));
    bool hdrOutput = G_HDROutput > 0.5f;
    if (!hdrOutput)
        scene = saturate(scene);

    // The engine supplies the display's reference white explicitly. SDR menus
    // and video in an FP16 surface need the Windows SDR white on HDR desktops
    // even when native HDR scene output is inactive; SDR desktops supply 80.
    float whiteScale = max(G_HDRPaperWhiteNits, 1.0f) / 80.0f;
    float3 color = HDRSRGBToLinear(scene) * whiteScale;

    // AntTweakBar's old backend does not preserve usable destination alpha.
    // Draws over black and white recover its coverage in each color channel.
    float3 black = HDRFiniteColor(TX_AntBlack.Sample(SS_Linear, input.vTexcoord).rgb);
    float3 white = HDRFiniteColor(TX_AntWhite.Sample(SS_Linear, input.vTexcoord).rgb);
    float3 transmittance = saturate(white - black);
    float3 coverage = 1.0f - transmittance;
    float3 antColor = black / max(coverage, 0.0001f);
    color = color * transmittance + HDRSRGBToLinear(antColor) * coverage * whiteScale;

    // Direct2D renders encoded SDR UI into a premultiplied alpha surface.
    float4 d2d = TX_D2D.Sample(SS_Linear, input.vTexcoord);
    float alpha = isfinite(d2d.a) ? saturate(d2d.a) : 0.0f;
    float3 uiColor = d2d.rgb / max(alpha, 0.0001f);
    color = color * (1.0f - alpha) + HDRSRGBToLinear(uiColor) * alpha * whiteScale;

    float peak = max(G_HDRPeakNits, 1.0f) / 80.0f;
    float brightest = max(color.r, max(color.g, color.b));
    color *= min(1.0f, peak / max(brightest, 0.0001f));
    return float4(max(color, 0.0f), 1.0f);
}
