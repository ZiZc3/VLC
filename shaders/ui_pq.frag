#version 450
// HDR output: the UI layer (8-bit, premultiplied, the TV's gamma) over the
// PQ picture. Its colours become light at a graphics white of pc.nits
// (BT.2408's 203), BT.709 -> BT.2020, then PQ; premultiplied again so it
// blends with ONE, ONE_MINUS_SRC_ALPHA as on an SDR screen.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D ui;

layout(push_constant) uniform Push {
    float nits;
} pc;

const float PQ_M1 = 0.1593017578125, PQ_M2 = 78.84375;
const float PQ_C1 = 0.8359375, PQ_C2 = 18.8515625, PQ_C3 = 18.6875;

vec3 nits_to_pq(vec3 nits)
{
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(PQ_M1));
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), vec3(PQ_M2));
}

void main()
{
    vec4 c = texture(ui, uv);
    if (c.a <= 0.0) {
        color = vec4(0.0);
        return;
    }
    vec3 straight = clamp(c.rgb / c.a, 0.0, 1.0);
    vec3 lin = pow(straight, vec3(2.2)) * pc.nits;
    /* BT.709 -> BT.2020 primaries (GLSL matrices are by column) */
    lin = mat3(0.6274, 0.0691, 0.0164,
               0.3293, 0.9195, 0.0880,
               0.0433, 0.0114, 0.8956) * lin;
    /* The blend happens on PQ codes: half-transparent black over the picture
     * halved the code, which darkened it far more than on an SDR screen.
     * There the blend is on gamma codes, leaving (1 - a)^2.2 of the light;
     * the coverage that leaves that much of a 100-nit picture is used. */
    const float ref = 100.0;
    float left = pow(1.0 - c.a, 2.2);
    float a = c.a < 1.0 ? 1.0 - nits_to_pq(vec3(ref * left)).x / nits_to_pq(vec3(ref)).x : 1.0;
    color = vec4(nits_to_pq(lin) * a, a);
}
