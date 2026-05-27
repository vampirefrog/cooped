$input a_position, a_normal, a_texcoord0
$output v_normal, v_texcoord0, v_wpos

#include <bgfx_shader.sh>

void main()
{
	vec4 wpos = mul(u_model[0], vec4(a_position, 1.0));
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	// Geometry is world-space (identity model for brushes, translation for avatars),
	// so the normal passes straight through; world position used for point lighting.
	v_normal = a_normal;
	v_texcoord0 = a_texcoord0;
	v_wpos = wpos.xyz;
}
