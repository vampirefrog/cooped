$input v_normal

#include <bgfx_shader.sh>

uniform vec4 u_albedo;

void main()
{
	vec3 n = normalize(v_normal);
	// Fixed key light direction (toward the light), Z-up world.
	vec3 L = normalize(vec3(0.35, 0.25, 0.9));
	float ndl = max(dot(n, L), 0.0);
	float ambient = 0.28;
	vec3 col = u_albedo.xyz * (ambient + 0.85 * ndl);
	gl_FragColor = vec4(col, 1.0);
}
