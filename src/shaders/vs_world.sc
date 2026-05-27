$input a_position, a_normal
$output v_normal

#include <bgfx_shader.sh>

void main()
{
	gl_Position = mul(u_modelViewProj, vec4(a_position, 1.0));
	// Geometry is submitted in world space with an identity model matrix,
	// so the normal passes straight through.
	v_normal = a_normal;
}
