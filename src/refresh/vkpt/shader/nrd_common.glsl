// Adapted from NVIDIA NRD 4.18 Shaders/NRD.hlsli.
// Copyright NVIDIA Corporation. See extern/NRD/LICENSE.txt.
// Keep the equations synchronized with the pinned SDK; matrices below explicitly
// transpose HLSL's row-major constructors to GLSL's column-major constructors.
#ifndef VKPT_NRD_COMMON_GLSL
#define VKPT_NRD_COMMON_GLSL
#define NRD_PI 3.14159265358979323846
#define NRD_EPS 1e-6
#define NRD_REJITTER_AMPLITUDE 2.0
#define NRD_REJITTER_VIEWZ_THRESHOLD 0.01
#define NRD_MATERIAL_FACTOR_MIN_SCALE 0.02
#define NRD_ROUGHNESS_FACTOR_MIN_SCALE 0.1
float nrd_saturate(float v) { return clamp(v, 0.0, 1.0); }
vec3 nrd_saturate(vec3 v) { return clamp(v, 0.0, 1.0); }
float nrd_rcp(float v) { return 1.0 / v; }
struct NRD_SG { float c0; vec2 chroma; float normHitDist; vec3 c1; float sharpness; };
vec3 _NRD_SafeNormalize( vec3 v )
{
    return v * inversesqrt( dot( v, v ) + 1e-9 );
}

float _NRD_Luminance( vec3 linearColor )
{
    // IMPORTANT: must be in sync with ML_LUMINANCE_DEFAULT
    return dot( linearColor, vec3( 0.2126, 0.7152, 0.0722 ) );
}

vec3 _NRD_LinearToYCoCg( vec3 color )
{
    float Y = dot( color, vec3( 0.25, 0.5, 0.25 ) );
    float Co = dot( color, vec3( 0.5, 0.0, -0.5 ) );
    float Cg = dot( color, vec3( -0.25, 0.5, -0.25 ) );

    return vec3( Y, Co, Cg );
}

vec3 _NRD_YCoCgToLinear( vec3 color )
{
    float t = color.x - color.z;

    vec3 r;
    r.y = color.x + color.z;
    r.x = t + color.y;
    r.z = t - color.y;

    return max( r, 0.0 );
}

vec3 _NRD_YCoCgToLinear_Corrected( float Y, float Y0, vec2 CoCg )
{
    Y = max( Y, 0.0 );
    CoCg *= ( Y + NRD_EPS ) / ( Y0 + NRD_EPS );

    return _NRD_YCoCgToLinear( vec3( Y, CoCg ) );
}

float _NRD_GetSpecularDominantFactor( float NoV, float roughness )
{
    float a = 0.298475 * log( 39.4115 - 39.0029 * roughness );
    float dominantFactor = pow( nrd_saturate( 1.0 - NoV ), 10.8649 ) * ( 1.0 - a ) + a;

    return nrd_saturate( dominantFactor );
}

vec3 _NRD_GetSpecularDominantDirection( vec3 N, vec3 V, float dominantFactor )
{
    vec3 R = reflect( -V, N );
    vec3 D = mix( N, R, dominantFactor );

    return _NRD_SafeNormalize( D );
}

float _NRD_Pow5( float x )
{
    return pow( nrd_saturate( 1.0 - x ), 5.0 );
}

float _NRD_DistributionTerm( float roughness, float NoH )
{
    // Trowbridge-Reitz ( GGX )
    float m = roughness * roughness;
    float m2 = m * m;

    float t = ( NoH * m2 - NoH ) * NoH + 1.0;
    float a = m / t;
    float d = a * a;

    return d / NRD_PI;
}

float _NRD_GeometryTerm( float roughness, float NoL, float NoV )
{
    // Height-correlated
    float m = roughness * roughness;
    float m2 = m * m;

    float a = NoV * sqrt( ( NoL - m2 * NoL ) * NoL + m2 );
    float b = NoL * sqrt( ( NoV - m2 * NoV ) * NoV + m2 );

    return 0.5 / ( a + b );
}

float _NRD_DiffuseTerm( float roughness, float NoL, float NoV, float VoH )
{
    // Burley
    float f = 2.0 * VoH * VoH * roughness - 0.5; // yes, linear roughness
    float FdV = f * _NRD_Pow5( NoV ) + 1.0;
    float FdL = f * _NRD_Pow5( NoL ) + 1.0;
    float d = FdV * FdL;

    return d / NRD_PI;
}

vec2 _NRD_ComputeBrdfs( vec3 Ld, vec3 Ls, vec3 N, vec3 V, float roughness )
{
    vec2 result;
    float NoV = abs( dot( N, V ) );

    { // Diffuse
        vec3 H = _NRD_SafeNormalize( Ld + V );

        float NoL = nrd_saturate( dot( N, Ld ) );
        float VoH = abs( dot( V, H ) );

        float Kdiff = _NRD_DiffuseTerm( roughness, NoL, NoV, VoH );

        result.x = Kdiff * NoL;
    }

    { // Specular
        vec3 H = _NRD_SafeNormalize( Ls + V );

        float NoL = nrd_saturate( dot( N, Ls ) );
        float NoH = nrd_saturate( dot( N, H ) );

        float D = _NRD_DistributionTerm( roughness, NoH );
        float Gmod = _NRD_GeometryTerm( roughness, NoL, NoV );
        float Kspec = D * Gmod;

        result.y = Kspec * NoL;
    }

    return result; // no F, because it's already demodulated
}

vec3 _NRD_EnvironmentTerm_Rtg( vec3 Rf0, float NoV, float roughness )
{
    // "Ray Tracing Gems", Chapter 32, Equation 4 - the approximation assumes GGX VNDF and Schlick's approximation
    float m = nrd_saturate( roughness * roughness );

    vec4 X;
    X.x = 1.0;
    X.y = NoV;
    X.z = NoV * NoV;
    X.w = NoV * X.z;

    vec4 Y;
    Y.x = 1.0;
    Y.y = m;
    Y.z = m * m;
    Y.w = m * Y.z;

    const mat2 M1 = transpose(mat2( 0.99044, -1.28514, 1.29678, -0.755907 ));
    const mat3 M2 = transpose(mat3( 1.0, 2.92338, 59.4188, 20.3225, -27.0302, 222.592, 121.563, 626.13, 316.627 ));

    const mat2 M3 = transpose(mat2( 0.0365463, 3.32707, 9.0632, -9.04756 ));
    const mat3 M4 = transpose(mat3( 1.0, 3.59685, -1.36772, 9.04401, -16.3174, 9.22949, 5.56589, 19.7886, -20.2123 ));

    float bias = dot( (M1 * X.xy), Y.xy ) * nrd_rcp( max( dot( (M2 * X.xyw), Y.xyw ), NRD_EPS ) );
    float scale = dot( (M3 * X.xy), Y.xy ) * nrd_rcp( max( dot( (M4 * X.xzw), Y.xyw ), NRD_EPS ) );

    return nrd_saturate( Rf0 * scale + bias );
}

float _NRD_GetSpecMagicCurve( float roughness, float power )
{
    // https://www.desmos.com/calculator/fb1h5kiouj
    float f = 1.0 - exp2( -200.0 * roughness * roughness );
    f *= pow( nrd_saturate( roughness ), power );

    return f;
}

void NRD_MaterialFactors( vec3 N, vec3 V, vec3 albedo, vec3 Rf0, float roughness, out vec3 diffFactor, out vec3 specFactor )
{
    float NoV = abs( dot( N, V ) );
    vec3 Fenv = _NRD_EnvironmentTerm_Rtg( Rf0, NoV, roughness );

    diffFactor = ( 1.0 - Fenv ) * albedo;
    diffFactor = mix( vec3(NRD_MATERIAL_FACTOR_MIN_SCALE), vec3( 1.0, 1.0, 1.0 ), diffFactor );

    specFactor = Fenv;
    specFactor *= mix( vec3(NRD_ROUGHNESS_FACTOR_MIN_SCALE), vec3( 1.0, 1.0, 1.0 ), roughness ); // don't be greedy, it's a biased solution
    specFactor = mix( vec3(NRD_MATERIAL_FACTOR_MIN_SCALE), vec3( 1.0, 1.0, 1.0 ), specFactor );
}

NRD_SG _NRD_SG_Create( vec3 radiance, vec3 direction, float normHitDist )
{
    vec3 YCoCg = _NRD_LinearToYCoCg( radiance );

    NRD_SG sg;
    sg.c0 = YCoCg.x;
    sg.chroma = YCoCg.yz;
    sg.c1 = direction * YCoCg.x;
    sg.normHitDist = normHitDist;
    sg.sharpness = 0.0; // computed in resolve

    return sg;
}

float _NRD_SG_InnerProduct( NRD_SG a, NRD_SG b )
{
    // Integral of the product of two SGs
    precise vec3 dir = a.sharpness * a.c1 + b.sharpness * b.c1;
    precise float d = length( dir );

    precise float c = exp( d - a.sharpness - b.sharpness );
    c *= 1.0 - exp( -2.0 * d );
    c /= max( d, NRD_EPS );

    return 2.0 * NRD_PI * c * a.c0 * b.c0;
}

NRD_SG REBLUR_BackEnd_UnpackSh( vec4 sh0, vec3 sh1 )
{
    NRD_SG sg;
    sg.c0 = sh0.x;
    sg.chroma = sh0.yz;
    sg.normHitDist = sh0.w;
    sg.c1 = sh1;
    sg.sharpness = 0.0; // computed in resolve

    return sg;
}

NRD_SG RELAX_BackEnd_UnpackSh( vec4 sh0, vec3 sh1 )
{
    NRD_SG sg;
    sg.c0 = sh0.x;
    sg.chroma = sh0.yz;
    sg.normHitDist = sh0.w;
    sg.c1 = sh1;
    sg.sharpness = 0.0;

    return sg;
}

vec3 NRD_SG_ExtractColor( NRD_SG sg )
{
    return _NRD_YCoCgToLinear( vec3( sg.c0, sg.chroma ) );
}

vec3 NRD_SG_ExtractDirection( NRD_SG sg )
{
    return sg.c1 / max( length( sg.c1 ), NRD_EPS );
}

vec3 NRD_SG_ResolveSpecular( NRD_SG sg, vec3 N, vec3 V, float roughness )
{
    // Clamp roughness to avoid numerical imprecisions
    roughness = max( roughness, 0.05 );

    float m = roughness * roughness;
    float m2 = m * m;

    vec3 L = NRD_SG_ExtractDirection( sg );
    float NoL = nrd_saturate( dot( N, L ) );

    vec3 H = _NRD_SafeNormalize( L + V );
    //H = _NRD_SafeNormalize( mix( N, H, roughness ) ); // this helps in re-jittering but not here

    // Use "abs" because normal mapping is a lie
    float NoV = abs( dot( N, V ) );
    float VoH = abs( dot( V, H ) );

    NoV = mix( 0.02, 1.0, NoV ); // fix energy increase on the horizon / silhouette

    // SG light
    NRD_SG light;
    light.sharpness = 2.0 / m2; // extract directionality? but no IQ gains, only instabilities for low roughness
    light.c0 = sg.c0 * light.sharpness; // with normalization
    light.c1 = L;

    // Warped NDF
    float ndfSharpness = 0.5 / max( m2 * VoH, 1e-8 ); // "1e-8" needed to not add energy for very low roughness ( "1e-6" is not enough )

    NRD_SG warpedNdf;
    warpedNdf.c0 = 1.0;
    warpedNdf.c1 = L; // same as "reflect( -V, H )"
    warpedNdf.sharpness = ndfSharpness; // = ( 2 / m2 ) / ( 4 * VoH )

    // Multiply two SGs and integrate the result
    float Y = _NRD_SG_InnerProduct( warpedNdf, light );

    // Apply BRDF terms
    float Gmod = _NRD_GeometryTerm( roughness, NoL, NoV );
    Y *= Gmod * NoL; // F applied in demodulation

    // Fitting the unfittable
    Y *= mix( mix( 0.1, 0.4, m2 ), 0.8, NoV );

    // Fix the bare minimum
    Y = max( Y, sg.c0 / NRD_PI );

    return _NRD_YCoCgToLinear_Corrected( Y, sg.c0, sg.chroma );
}

vec2 NRD_SG_ReJitter(
    NRD_SG diffSg, NRD_SG specSg,
    vec3 V, float roughness,
    float Z, float Ze, float Zw, float Zn, float Zs,
    vec3 N, vec3 Ne, vec3 Nw, vec3 Nn, vec3 Ns
)
{
    // Extract dominant light directions
    vec3 Ld = NRD_SG_ExtractDirection( diffSg );
    vec3 Ls = NRD_SG_ExtractDirection( specSg );

    // Fix instabilities
    // TODO: compared with linear roughness, "smc" keeps near-mirror directions closer to "V" but moves toward "Ls" faster otherwise,
    // reducing bias at the cost of greater sensitivity to potentially unstable "Ls"
    float smc = _NRD_GetSpecMagicCurve( roughness, 0.5 );
    Ls = _NRD_SafeNormalize( mix( V, Ls, smc ) );

    // BRDF at center
    vec2 brdfCenter = _NRD_ComputeBrdfs( Ld, Ls, N, V, roughness );

    // BRDFs at neighbors
    vec2 brdfAverage = _NRD_ComputeBrdfs( Ld, Ls, Ne, V, roughness );
    brdfAverage += _NRD_ComputeBrdfs( Ld, Ls, Nn, V, roughness );
    brdfAverage += _NRD_ComputeBrdfs( Ld, Ls, Nw, V, roughness );
    brdfAverage += _NRD_ComputeBrdfs( Ld, Ls, Ns, V, roughness );
    brdfAverage *= 0.25;

    // Jacobian
    vec2 j = ( brdfCenter + NRD_EPS ) / ( brdfAverage + NRD_EPS );
    j = clamp( j, 1.0 / NRD_REJITTER_AMPLITUDE, NRD_REJITTER_AMPLITUDE );

    // Z weights to avoid ringing on geometry edges
    float NoV = abs( dot( N, V ) );
    float zThreshold = NRD_REJITTER_VIEWZ_THRESHOLD * abs( Z ) / ( NoV * 0.95 + 0.05 );
    vec4 w = step( abs( vec4( Ze, Zw, Zn, Zs ) - Z ), vec4(zThreshold) );

    // Normal weights to avoid ringing on hard normal edges
    vec4 NoN;
    NoN.x = dot( Ne, N );
    NoN.y = dot( Nw, N );
    NoN.z = dot( Nn, N );
    NoN.w = dot( Ns, N );

    w *= step( 0.01, NoN );

    // Result
    bool isSymmetrical = dot( w, vec4( 1.0, 1.0, 1.0, 1.0 ) ) > 3.5;

    return isSymmetrical ? j : vec2( 1.0, 1.0 );
}

// Material factor shared by tracing, NRD prepare and composition. V points
// from the surface toward the viewer. The input roughness is the material value,
// never NRD's filtered roughness.
vec3 nrd_specular_factor(vec3 N, vec3 V, vec3 Rf0, float roughness)
{
    vec3 diffuseFactor, specularFactor;
    NRD_MaterialFactors(N, V, vec3(0.0), Rf0, clamp(roughness, 0.0, 1.0), diffuseFactor, specularFactor);
    return specularFactor;
}

// Cumulative directional moment. Each contribution has its own real incoming
// direction. The two NRD families use different luminance weights on input.
vec4 nrd_specular_moment(vec3 radiance, vec3 direction, bool relax)
{
    if (any(isnan(radiance)) || any(isinf(radiance))) return vec4(0.0);
    float weight = dot(max(radiance, vec3(0.0)), relax ? vec3(0.2126, 0.7152, 0.0722) : vec3(0.25, 0.5, 0.25));
    return vec4(_NRD_SafeNormalize(direction) * weight, weight);
}
#endif
