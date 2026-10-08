#version 450
// One triangle over the viewport (the video's rectangle on screen).
layout(location = 0) out vec2 uv;

layout(push_constant) uniform Push {
    vec4 row0;      // YCbCr -> RGB matrix rows
    vec4 row1;
    vec4 row2;
    vec4 params;    // x: sample scale (1 for 8-bit, 65535/1023 for 10-bit in 16), y: limited range
    vec2 uv_scale;  // visible / buffer size: the decoder's padding stays out
} pc;

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = p * pc.uv_scale;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
