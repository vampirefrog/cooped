$input v_normal, v_texcoord0, v_wpos, v_texcoord1

#include <bgfx_shader.sh>

#define MAX_LIGHTS 32

SAMPLER2D(s_tex, 0);
SAMPLER2D(s_lightmap, 1);
uniform vec4 u_albedo;
uniform vec4 u_lightParams;                 // x = light count, y = ambient, z = use lightmap
uniform vec4 u_sunDir;                       // xyz = direction toward the sun, w = intensity
uniform vec4 u_lightPosRadius[MAX_LIGHTS];   // xyz = position, w = radius
uniform vec4 u_lightColor[MAX_LIGHTS];       // rgb = color, w = intensity

void main()
{
	vec3 tex = texture2D(s_tex, v_texcoord0).rgb;
	vec3 lit;
	if (u_lightParams.z > 0.5) {
		vec4 lmv = texture2D(s_lightmap, v_texcoord1);   // RGBM-encoded baked HDR lighting
		vec3 hdr = lmv.rgb * (lmv.a * 8.0);              // decode (kRgbmRange = 8)
		lit = vec3_splat(1.0) - exp(-hdr * 1.2);         // exposure tonemap -> LDR
	} else {
		vec3 n = normalize(v_normal);
		lit = vec3_splat(u_lightParams.y);                                       // ambient
		lit += vec3_splat(max(dot(n, normalize(u_sunDir.xyz)), 0.0) * u_sunDir.w);  // sun
		int count = int(u_lightParams.x);
		for (int i = 0; i < MAX_LIGHTS; ++i) {
			if (i >= count) break;
			vec3 d = u_lightPosRadius[i].xyz - v_wpos;
			float dist = length(d);
			float atten = max(0.0, 1.0 - dist / u_lightPosRadius[i].w);
			atten *= atten;
			float ndl = max(dot(n, d / max(dist, 0.0001)), 0.0);
			lit += u_lightColor[i].rgb * (ndl * atten * u_lightColor[i].w);
		}
	}
	gl_FragColor = vec4(u_albedo.xyz * tex * lit, 1.0);
}
