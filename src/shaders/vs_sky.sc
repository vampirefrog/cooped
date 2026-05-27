$input a_position
$output v_ray

#include <bgfx_shader.sh>

void main()
{
	// a_position is a fullscreen triangle in clip space (z=1 = far plane).
	gl_Position = vec4(a_position.xy, 1.0, 1.0);
	// u_invViewProj and u_invView are bgfx built-ins (set from setViewTransform).
	vec4 wf = mul(u_invViewProj, vec4(a_position.xy, 1.0, 1.0));
	vec3 camPos = u_invView[3].xyz;
	v_ray = wf.xyz / wf.w - camPos;  // world-space view ray
}
