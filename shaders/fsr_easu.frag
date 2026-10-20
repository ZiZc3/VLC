#version 450
#extension GL_GOOGLE_include_directive : require
// AMD FidelityFX FSR 1, its first pass (EASU: edge-adaptive upscale). The
// filter is AMD's (third_party/fsr1, MIT licence); this file only feeds it:
// the picture at its own size (pass 1) in, an image at the rectangle's size
// out (upright; the second pass turns it).
#define A_GPU 1
#define A_GLSL 1
#include "../third_party/fsr1/ffx_a.h"

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Push {
    vec4 size;   // the picture: width, height, 1 / width, 1 / height
    vec4 xform;  // (second pass only)
    vec4 outp;   // this image: width, height
} pc;

#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return textureGather(src, p, 0); }
AF4 FsrEasuGF(AF2 p) { return textureGather(src, p, 1); }
AF4 FsrEasuBF(AF2 p) { return textureGather(src, p, 2); }
#include "../third_party/fsr1/ffx_fsr1.h"

layout(location = 0) out vec4 color;

void main()
{
    AU4 con0, con1, con2, con3;
    FsrEasuCon(con0, con1, con2, con3, pc.size.x, pc.size.y, pc.size.x, pc.size.y, pc.outp.x, pc.outp.y);
    AF3 c;
    FsrEasuF(c, AU2(gl_FragCoord.xy), con0, con1, con2, con3);
    color = vec4(c, 1.0);
}
