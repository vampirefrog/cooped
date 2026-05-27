$input v_ray

#include <bgfx_shader.sh>

SAMPLER2D(s_sky, 0);
uniform vec4 u_skyParams;  // x = exposure

void main()
{
	vec3 d = normalize(v_ray);
	// Equirectangular lookup, Z-up: longitude from atan2(y,x), latitude from z.
	float u = atan2(d.y, d.x) * 0.15915494 + 0.5;          // 1/(2*pi)
	float v = acos(clamp(d.z, -1.0, 1.0)) * 0.31830989;    // 1/pi  (up -> 0, down -> 1)
	vec3 hdr = texture2D(s_sky, vec2(u, v)).rgb;
	vec3 col = vec3_splat(1.0) - exp(-hdr * u_skyParams.x); // exposure tonemap
	gl_FragColor = vec4(col, 1.0);
}
