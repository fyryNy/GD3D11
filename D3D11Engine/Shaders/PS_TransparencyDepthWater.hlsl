#include "WaterTransparency.hlsli"

// Apply the same water clipping to the ghost depth prepass as to its color pass.
// No color output is needed; surviving fragments only update the depth buffer.
void PSMain( float4 screenPosition : SV_POSITION )
{
    ClipBehindWater( screenPosition );
}
