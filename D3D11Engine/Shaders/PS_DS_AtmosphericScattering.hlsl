//--------------------------------------------------------------------------------------
// World/VOB-Pixelshader for G2D3D11 by Degenerated
//--------------------------------------------------------------------------------------
#include <DS_Defines.h>

#include <AtmosphericScattering.h>

cbuffer DS_ScreenQuadConstantBuffer : register( b0 )
{
	matrix SQ_InvProj; // Optimize out!
	matrix SQ_InvView;
	matrix SQ_View;
	
	matrix SQ_RainViewProj;
	
	float3 SQ_LightDirectionVS;
	float SQ_ShadowmapSize;
	
	float4 SQ_LightColor;
	matrix SQ_ShadowView;
	matrix SQ_ShadowProj;
	
	matrix SQ_RainView;
	matrix SQ_RainProj;
	
	float SQ_ShadowStrength;
	float SQ_ShadowAOStrength;
	float SQ_WorldAOStrength;
	float SQ_Pad;
};

//--------------------------------------------------------------------------------------
// Textures and Samplers
//--------------------------------------------------------------------------------------
SamplerState SS_Linear : register( s0 );
SamplerState SS_samMirror : register( s1 );
SamplerComparisonState SS_Comp : register( s2 );
Texture2D	TX_Diffuse : register( t0 );
Texture2D	TX_Nrm : register( t1 );
Texture2D	TX_Depth : register( t2 );
Texture2D	TX_Shadowmap : register( t3 );
Texture2D	TX_RainShadowmap : register( t4 );
TextureCube	TX_ReflectionCube : register( t5 );
Texture2D	TX_Distortion : register( t6 );
Texture2D	TX_SI_SP : register( t7 );

//--------------------------------------------------------------------------------------
// Input / Output structures
//--------------------------------------------------------------------------------------
struct PS_INPUT
{
	float2 vTexCoord 		: TEXCOORD0;
	float3 vEyeRay			: TEXCOORD1;
	float4 vPosition		: SV_POSITION;
};

float3 VSPositionFromDepth(float depth, float2 vTexCoord)
{
	// Get NDC clip-space position
	float4 vProjectedPos = float4(vTexCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), depth, 1.0f);

	// Transform by the inverse projection matrix
	float4 vPositionVS = mul(vProjectedPos, SQ_InvProj); //invViewProj == invProjection here

	// Divide by w to get the view-space position
	return vPositionVS.xyz / vPositionVS.www;
}

//--------------------------------------------------------------------------------------
// Blinn-Phong Lighting Reflection Model
//--------------------------------------------------------------------------------------
float CalcBlinnPhongLighting(float3 N, float3 H)
{
    return saturate(dot(N, H));
}

float2 TexOffset( int u, int v )
{
    return float2( u * 1.0f/SQ_ShadowmapSize, v * 1.0f/SQ_ShadowmapSize );
}

float IsInShadow(float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState)
{
	float4 vShadowSamplingPos = mul(float4(wsPosition, 1), mul(SQ_ShadowView, SQ_ShadowProj));
	vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
	float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
	return shadowmap.SampleCmpLevelZero(samplerState, projectedTexCoords.xy, vShadowSamplingPos.z);
}

float IsWet(float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, matrix viewProj)
{
	float4 vShadowSamplingPos = mul(float4(wsPosition, 1), mul(SQ_RainView, SQ_RainProj));
	vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
	float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);
	float bias = 0.001f;
	return shadowmap.SampleCmpLevelZero( samplerState, projectedTexCoords.xy, vShadowSamplingPos.z - bias);
}

float ComputeShadowValue(float2 uv, float3 wsPosition, Texture2D shadowmap, SamplerComparisonState samplerState, float distance, float vertLighting, matrix viewProj, float bias = 0.01f, float softnessScale = 1.0f)
{
	// Reconstruct VS World ShadowViewPosition from depth
	float4 vShadowSamplingPos = mul(float4(wsPosition, 1), viewProj);
	vShadowSamplingPos.xyz /= vShadowSamplingPos.www;
	
	float2 projectedTexCoords = vShadowSamplingPos.xy * float2(0.5f, -0.5f) + float2(0.5f, 0.5f);	
	float shadow = 1.0f;
	if( !(projectedTexCoords.x > 1 || projectedTexCoords.y > 1 ||
		projectedTexCoords.x < 0 || projectedTexCoords.y < 0))
	{
#if SHD_FILTER_16TAP_PCF
		//return shadowmap.SampleCmpLevelZero( samplerState, projectedTexCoords.xy, vShadowSamplingPos.z - 0.00001f);
		//return shadowmap.Sample(SS_Linear, projectedTexCoords).r > vShadowSamplingPos.z ? 1 : 0;
		
		float dist = shadowmap.Sample(SS_Linear, projectedTexCoords).r - vShadowSamplingPos.z;
		
		//return dist * 10.0f;
		
		//PCF sampling for shadow map
		float sum = 0;
		float x, y;
		
		float dx = ddx(projectedTexCoords.xy);
		float dy = ddy(projectedTexCoords.xy);
	 
		float minValue = 999999.0f;
		/*for (y = -1.5; y <= 1.5; y += 1.0)
		{
			for (x = -1.5; x <= 1.5; x += 1.0)
			{
				
				minValue = min(minValue, shadowmap.SampleGrad(SS_Linear, projectedTexCoords.xy + TexOffset(x,y), dx, dy)).r;
			}
		}*/
		
		float scale = softnessScale;//1 + (minValue - vShadowSamplingPos.z) * 500.0f;
		
	 
		//perform PCF filtering on a 4 x 4 texel neighborhood
		[unroll] for (y = -1.5; y <= 1.5; y += 1.0)
		{
			[unroll] for (x = -1.5; x <= 1.5; x += 1.0)
			{
				sum += shadowmap.SampleCmpLevelZero( samplerState, projectedTexCoords.xy + TexOffset(x,y) * scale, vShadowSamplingPos.z - bias);
			}
		}
	 
		float shadowFactor = sum / 16.0;
	
		shadow *= shadowFactor;
#else
		shadow = shadowmap.SampleCmpLevelZero( samplerState, projectedTexCoords.xy, vShadowSamplingPos.z - bias);
#endif
	}
	
	float border;
	border = pow(abs(projectedTexCoords.x), 16.0f);
	border += pow(abs(projectedTexCoords.y), 16.0f);
	border += pow(abs(1.0f-projectedTexCoords.x), 16.0f);
	border += pow(abs(1.0f-projectedTexCoords.y), 16.0f);
	shadow = lerp(shadow, vertLighting, saturate(border));
	
	return saturate(shadow);
}

static const float WEIGHT_BIAS = -0.55;
static const float WEIGHT_MUL = 0.7;

float3 NormalizeRainVector(float3 value, float3 fallback)
{
	float lengthSquared = dot(value, value);
	return lengthSquared > 0.000001f ? value * rsqrt(lengthSquared) : fallback;
}

void ComputeRainPositionGradients(float2 screenUV, float depth, float3 vsPosition, float3 vsNormal, out float3 wsPositionDX, out float3 wsPositionDY)
{
	uint width, height;
	TX_Depth.GetDimensions(width, height);
	float4 projectedPosition = float4(screenUV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), depth, 1.0f);
	float positionW = mul(projectedPosition, SQ_InvProj).w;
	float inverseW = 1.0f / (positionW < 0.0f ? min(positionW, -0.000001f) : max(positionW, 0.000001f));

	// Differentiate the inverse projection on the local surface plane. Unlike
	// ddx/ddy, these gradients remain valid inside pixel-dependent branches.
	float3 directionX = SQ_InvProj[0].xyz - vsPosition * SQ_InvProj[0].w;
	float3 directionY = SQ_InvProj[1].xyz - vsPosition * SQ_InvProj[1].w;
	float3 directionDepth = SQ_InvProj[2].xyz - vsPosition * SQ_InvProj[2].w;
	float normalDepth = dot(vsNormal, directionDepth);
	float minimumNormalDepth = max(length(directionDepth) * 0.0001f, 0.000001f);
	float inverseNormalDepth = 1.0f / (normalDepth < 0.0f ? min(normalDepth, -minimumNormalDepth) : max(normalDepth, minimumNormalDepth));
	float3 vsPositionDX = (directionX - directionDepth * dot(vsNormal, directionX) * inverseNormalDepth) * (2.0f * inverseW / max(width, 1u));
	float3 vsPositionDY = (directionY - directionDepth * dot(vsNormal, directionY) * inverseNormalDepth) * (-2.0f * inverseW / max(height, 1u));
	wsPositionDX = mul(vsPositionDX, (float3x3)SQ_InvView);
	wsPositionDY = mul(vsPositionDY, (float3x3)SQ_InvView);
}

/** Applys normal-deformation for the rain */
void ApplyRainNormalDeformation(inout float3 vsNormal, float3 wsPosition, float3 wsPositionDX, float3 wsPositionDY)
{
	// Need worldspace normal for this
	float3 wsNormal = NormalizeRainVector(mul(vsNormal, (float3x3)SQ_InvView).xyz, float3(0, 1, 0));
	float3 originalNormal = wsNormal;
	
	float2 groundDir = normalize(float2(0.1f, 0.1f) + saturate(cross(wsNormal, float3(0.0f,1.0f,0.0f)).xz));
	
	const float scale = 1000.0f;
	float2 uv[4] = {wsPosition.zy / scale, 
					wsPosition.xz / (scale*2),
					wsPosition.xz / (scale*2),					
					wsPosition.xy / scale};
	float2 uvDX[4] = {wsPositionDX.zy / scale,
					wsPositionDX.xz / (scale*2),
					wsPositionDX.xz / (scale*2) * float2(0.8f, 1.2f),
					wsPositionDX.xy / scale};
	float2 uvDY[4] = {wsPositionDY.zy / scale,
					wsPositionDY.xz / (scale*2),
					wsPositionDY.xz / (scale*2) * float2(0.8f, 1.2f),
					wsPositionDY.xy / scale};
	
	float groundSpeed = 0.1f * AC_RainFXWeight;
	float downSpeed = 0.2f * AC_RainFXWeight;
	uv[0] += float2(0, AC_Time * downSpeed);
	uv[1] += float2(AC_Time * groundSpeed, AC_Time * groundSpeed);
	uv[2] = uv[2] * float2(0.8f, 1.2f) + float2(-AC_Time * groundSpeed * 0.7f, AC_Time * groundSpeed * 0.4f);
	uv[3] += float2(0, AC_Time * downSpeed);
	
	// Create weights for all 3 axis
	float3 weights = float3(abs(wsNormal.x),
							abs(wsNormal.y),
							abs(wsNormal.z));
							
	// Tighten up the blending zone:
	weights = (weights + WEIGHT_BIAS) * WEIGHT_MUL;
	weights = max(weights, 0);						
							
	weights /= max(weights.x + weights.y + weights.z, 0.000001f);
				
	weights.xz *= 0.6f;
	weights.y *= 0.7f;
		
	float3 dist[3] =  {NormalizeRainVector(TX_Distortion.SampleGrad(SS_Linear, uv[0], uvDX[0], uvDY[0]).zyx * 2 - 1, originalNormal),
					  NormalizeRainVector(TX_Distortion.SampleGrad(SS_Linear, uv[1], uvDX[1], uvDY[1]).xzy * 2 - 1, originalNormal) * 0.5f +
					  NormalizeRainVector(TX_Distortion.SampleGrad(SS_Linear, uv[2], uvDX[2], uvDY[2]).xzy * 2 - 1, originalNormal) * 0.5f,
					  NormalizeRainVector(TX_Distortion.SampleGrad(SS_Linear, uv[3], uvDX[3], uvDY[3]).xyz * 2 - 1, originalNormal)};
		
	weights = pow(weights, 4.0f);
		
	const float distWeight = 0.9f;
	
	// Sample the distortion-texture for all 3 axis
	for(int i=0;i<3;i++)
	{		
		// Add to normal
		wsNormal = lerp(wsNormal, dist[i], weights[i] * distWeight);//distWeight * weights[i]); 
	}

	wsNormal = NormalizeRainVector(wsNormal, originalNormal);
	//diffuse.xyz = wsNormal;
	
	vsNormal = normalize(mul(wsNormal, (float3x3)SQ_View).xyz);
}

/** Returns new diffusecolor (rgb)*/
void ApplySceneWettness(float3 wsPosition, float3 wsPositionDX, float3 wsPositionDY, float3 vsPosition, float3 vsDir, inout float3 vsNormal, inout float3 diffuse, float specIntensity, float specPower, out float3 specAdd)
{
	specAdd = 0.0f;
	// Ask the rain-shadowmap if we can hit this pixel
	float pixelWettnes = ComputeShadowValue(0.0f, wsPosition, TX_RainShadowmap, SS_Comp, vsPosition.z, 1.0f, mul(SQ_RainView, SQ_RainProj), 0.0001f, 2.5f) * saturate(AC_SceneWettness);
	float3 originalNormal = vsNormal;
	float3 wsNormal = NormalizeRainVector(mul(originalNormal, (float3x3)SQ_InvView).xyz, float3(0, 1, 0));
	float upward = saturate(wsNormal.y);
	// Walls retain a thin wet film; horizontal surfaces retain more water.
	pixelWettnes *= lerp(0.2f, 1.0f, upward * upward) * (1.0f - saturate(-wsNormal.y));
	if (pixelWettnes < 0.001f) return;

	// Keep the material's own highlight shape and color. Wetness mainly darkens
	// the texture; it must not turn grass or a matte FX-map region into a mirror.
	diffuse *= lerp(1.0f, 0.82f, pixelWettnes);
	float materialResponse = saturate(specIntensity * 4.0f) * smoothstep(0.0f, 16.0f, specPower);
	if (materialResponse <= 0.0f) return;

	float rippleWeight = saturate(AC_RainFXWeight) * pixelWettnes * upward * 0.1f;
	if (rippleWeight > 0.0f)
	{
		float3 nrm = originalNormal;
		ApplyRainNormalDeformation(nrm, wsPosition, wsPositionDX, wsPositionDY);
		vsNormal = NormalizeRainVector(lerp(originalNormal, nrm, rippleWeight), originalNormal);
	}

	float fresnel = 0.02f + 0.98f * pow(1.0f - saturate(dot(originalNormal, vsDir)), 5.0f);
	float gloss = materialResponse * saturate(specPower / 150.0f);
	// The sky cubemap is world-oriented and has rough mips for a softer reflection.
	float3 reflect_vec = mul(reflect(-vsDir, vsNormal), (float3x3)SQ_InvView);
	float4 refCube = TX_ReflectionCube.SampleLevel(SS_Linear, reflect_vec, lerp(4.0f, 2.0f, gloss));
	specAdd = max(refCube.rgb, 0.0f) * saturate(refCube.a) * fresnel * materialResponse * pixelWettnes * 0.35f;
}

//--------------------------------------------------------------------------------------
// Pixel Shader
//--------------------------------------------------------------------------------------
float4 PSMain( PS_INPUT Input ) : SV_TARGET
{
	// Get screen UV
	float2 uv = Input.vTexCoord; 
	
	// Look up the diffuse color
    float4 diffuse = TX_Diffuse.Sample(SS_Linear, uv);
	float vertLighting = diffuse.a;
	
	// Get the second GBuffer
	float4 gb2 = TX_Nrm.Sample(SS_Linear, uv);
	
	// If we dont have a normal, just return the diffuse color
	if(gb2.w < 0.001f)
		return float4(diffuse.rgb, 1);
	
	// Decode the view-space normal back
    float3 normal = normalize(gb2.xyz);
	
	// Get specular parameters
	float4 gb3 = TX_SI_SP.Sample(SS_Linear, uv);
	float specIntensity = gb3.x;
	float specPower = gb3.y;
	
	// Reconstruct VS World Position from depth
	float expDepth = TX_Depth.Sample(SS_Linear, uv).r;
	float3 vsPosition = VSPositionFromDepth(expDepth, uv);
	float3 wsPosition = mul(float4(vsPosition, 1), SQ_InvView).xyz;
	float3 V = normalize(-vsPosition);
	
#if SHD_ENABLE
	//return float4(mul(float4(wsPosition, 1), mul(SQ_ShadowView, SQ_ShadowProj)).xyz, 1);
	
	// Get shadowing
	float shadow = 0.0f;
	if(AC_LightPos.y > 0) // only get shadow value if it isn't night-time otherwise report that the whole scene is in shadow
		shadow = ComputeShadowValue(uv, wsPosition, TX_Shadowmap, SS_Comp, vsPosition.z, vertLighting, mul(SQ_ShadowView, SQ_ShadowProj), lerp(0.00005f, 0.0001f, vsPosition.z / 1000));
#else
	float shadow = vertLighting;
#endif
	//shadow = 1.0f;

	// Sunrays
	/*float3 vsDir = normalize(vsPosition);
	const int numSamples = 100;
	float stepSize = 1000.0f / numSamples;
	float shaft = 0.0f;
	for(float r=0;r < 1000.0f;r+=stepSize)
	{
		float3 vsRayPos = vsDir * r;
		float3 wsRayPos = mul(float4(vsRayPos, 1), SQ_InvView).xyz;
		
		float s = IsInShadow(wsRayPos, TX_Shadowmap, SS_Comp);
		
		shaft += s / numSamples;
	}*/
	
	
	
	// Compute wettness
	float3 specWet = 0.0f;
	
#ifdef APPLY_RAIN_EFFECTS
	float3 wsPositionDX, wsPositionDY;
	ComputeRainPositionGradients(uv, expDepth, vsPosition, normal, wsPositionDX, wsPositionDY);
	ApplySceneWettness(wsPosition, wsPositionDX, wsPositionDY, vsPosition, V, normal, diffuse.rgb, specIntensity, specPower, specWet);
#endif
	// Compute specular lighting
	
	float3 H = normalize(SQ_LightDirectionVS + V);
	float spec = CalcBlinnPhongLighting(normal, H);
	float specMod = pow(dot(float3(0.333f,0.333f,0.333f), diffuse.rgb), 2);
	
	
	
	//return float4(diffuse.rgb, 1);
	
	float4 lightColor = SQ_LightColor;
    lightColor.rgb = lerp(lightColor.rgb, lightColor.rgb * 0.8f, AC_SceneWettness);
	
	// Apply sunlight
	float sunStrength = dot(lightColor.rgb, float3(0.333f,0.333f,0.333f));
	
	float vertAO = lerp(pow(saturate(vertLighting * 2), 2), 1.0f, 0.5f);
	float sun = saturate(dot(normalize(SQ_LightDirectionVS), normal) * shadow) * 1.0f;

	spec = pow(spec, specPower) * specIntensity;
	float3 specBare = spec * lightColor.rgb * sun;
	float3 specColored = saturate(lerp(specBare, specBare * diffuse.rgb, specMod));
	
	float shadowAO = lerp(1.0f, vertLighting, SQ_ShadowAOStrength);
	float worldAO = lerp(1.0f, vertLighting, SQ_WorldAOStrength);
	
	float3 litPixel = lerp( diffuse.rgb * SQ_ShadowStrength * sunStrength * shadowAO, 
							diffuse.rgb * lightColor.rgb * lightColor.a * worldAO, sun) 
					  + specColored + specWet;
	
    float fresnel = pow(1.0f - saturate(dot(normal, V)), 10.0f);
    litPixel += lerp(fresnel * litPixel * 0.5f, 0.0f, sun);
	
	// Run scattering
	litPixel = ApplyAtmosphericScatteringGround(wsPosition, litPixel.rgb);
	
	
	// Fix indoor stuff
	//litPixel = lerp(diffuse * vertLighting, litPixel, vertLighting < 0.9f ? 0 : 1);
	//diffuse.rgb = lerp(diffuse.rgb, 1.0f, clamp(shaft, 0.0f, 0.4f));
	
	
	//return float4(sun.rgb, 1);
	//return float4(vertLighting.rrr, 1);
	return float4(litPixel.rgb, 1);
	//return float4(pow(spec, specPower) * specIntensity.xxx * diffuse.rgb * SQ_LightColor.rgb,1);
	
}
