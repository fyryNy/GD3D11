#ifndef HDR_COLOR_HLSLI
#define HDR_COLOR_HLSLI

// Gothic textures and HUD colors use the legacy, encoded sRGB convention.
// Convert at the HDR boundary without changing the existing SDR renderer.
float3 HDRFiniteColor(float3 color)
{
    // Corrupt pixels must not poison the presentation conversion.
    return float3(isfinite(color.r) ? max(color.r, 0.0f) : 0.0f,
                  isfinite(color.g) ? max(color.g, 0.0f) : 0.0f,
                  isfinite(color.b) ? max(color.b, 0.0f) : 0.0f);
}

float3 HDRSRGBToLinear(float3 color)
{
    // Encoded scene textures use FP16. Bound the power's input to their range,
    // while keeping the resulting linear light unbounded until the shoulder.
    color = min(HDRFiniteColor(color), 65504.0f);
    float3 low = color / 12.92f;
    float3 high = pow((color + 0.055f) / 1.055f, 2.4f);
    return float3(color.r <= 0.04045f ? low.r : high.r,
                  color.g <= 0.04045f ? low.g : high.g,
                  color.b <= 0.04045f ? low.b : high.b);
}

float3 HDRLinearToSRGB(float3 color)
{
    color = HDRFiniteColor(color);
    float3 low = color * 12.92f;
    float3 high = 1.055f * pow(color, 1.0f / 2.4f) - 0.055f;
    return float3(color.r <= 0.0031308f ? low.r : high.r,
                  color.g <= 0.0031308f ? low.g : high.g,
                  color.b <= 0.0031308f ? low.b : high.b);
}

// Keep diffuse white at 1 and roll brighter colors toward the panel peak.
// Scaling all three channels together preserves the highlight's hue.
float3 HDRShoulder(float3 color, float peakRelativeToWhite)
{
    color = HDRFiniteColor(color);
    float brightest = max(color.r, max(color.g, color.b));
    if (brightest <= 1.0f)
        return color;

    float headroom = max(peakRelativeToWhite - 1.0f, 0.0f);
    float excess = brightest - 1.0f;
    float mapped = 1.0f + headroom * (excess / max(headroom + excess, 0.0001f));
    return color * (mapped / brightest);
}

#endif
