#ifndef WATER_TRANSPARENCY_HLSLI
#define WATER_TRANSPARENCY_HLSLI

Texture2D<float> TX_WaterDepth : register( t6 );

void ClipBehindWater( float4 screenPosition )
{
    float waterDepth = TX_WaterDepth.Load( int3( int2( screenPosition.xy ), 0 ) );

    // The mask contains the nearest opaque or water depth. Reverse-Z keeps
    // fragments behind it; the scene depth test also rejects any behind opaque
    // geometry. Equality belongs to the later pass.
    if ( waterDepth <= 0.0f || waterDepth <= screenPosition.z )
        discard;
}

#endif
