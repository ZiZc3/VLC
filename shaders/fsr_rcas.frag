#version 450
#extension GL_GOOGLE_include_directive : require
// AMD FidelityFX FSR 1, its second pass (RCAS: contrast-adaptive sharpening),
// from the upscaled image onto the screen pixel for pixel, turned and flipped
// here. The filter is AMD's (third_party/fsr1, MIT licence); this file only
// feeds it.
#define A_GPU 1
#define A_GLSL 1
#include "../third_party/fsr1/ffx_a.h"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D mid;

layout(push_constant) uniform Push {
    vec4 size;   // the upscaled image: width, height, 1 / width, 1 / height
    vec4 xform;  // quarter turns clockwise, mirror, upside down, sharpness (stops: 0 strongest)
    vec4 outp;   // -
} pc;

#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p)
{
    p = clamp(p, ASU2(0), ASU2(pc.size.xy) - 1);
    return texelFetch(mid, p, 0);
}
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "../third_party/fsr1/ffx_fsr1.h"

/* the point of the image this pixel shows (as video.frag's turned_uv) */
vec2 turned()
{
    vec2 n = uv;
    if (pc.xform.y > 0.5)
        n.x = 1.0 - n.x;
    if (pc.xform.z > 0.5)
        n.y = 1.0 - n.y;
    int q = int(pc.xform.x + 0.5);
    if (q == 1)
        n = vec2(n.y, 1.0 - n.x);
    else if (q == 2)
        n = vec2(1.0 - n.x, 1.0 - n.y);
    else if (q == 3)
        n = vec2(1.0 - n.y, n.x);
    return n;
}

void main()
{
    AU2 ip = AU2(clamp(turned() * pc.size.xy, vec2(0.0), pc.size.xy - 1.0));
    AU4 con;
    FsrRcasCon(con, pc.xform.w);
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, ip, con);
    color = vec4(c, 1.0);
}
