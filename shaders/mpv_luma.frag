#version 450
// FSRCNNX enlarges brightness only (mpv's LUMA): the picture's brightness
// (BT.709 weights, of the output's signal as pass 1 drew it) into an image
// of the same size. mpv_merge.frag puts the colour back.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D src;

void main()
{
    vec3 c = texelFetch(src, ivec2(gl_FragCoord.xy), 0).rgb;
    color = vec4(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0, 0.0, 1.0);
}
