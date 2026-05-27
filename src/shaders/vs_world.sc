$input a_position, a_normal, a_texcoord0, a_texcoord1
$output v_normal, v_texcoord0, v_wpos, v_texcoord1

#include <bgfx_shader.sh>

void main()
{
	vec4 wpos = mul(u_model[0], vec4(a_position, 1.0));
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	v_normal = a_normal;
	v_texcoord0 = a_texcoord0;  // albedo UV
	v_texcoord1 = a_texcoord1;  // lightmap UV
	v_wpos = wpos.xyz;
}
