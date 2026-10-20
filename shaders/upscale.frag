#version 450
// The picture, already in RGB at its own size (pass 1, video.frag into an
// image), drawn into the video's rectangle by a better upscaler than the
// GPU's bilinear: Bicubic (Catmull-Rom), Lanczos-3 or EWA Lanczos Sharp
// (the round, jinc-based Lanczos: fewer jaggies and halos). Standard
// filters, written here from their formulas. Turning and flipping happen
// here too (pass 1 draws the picture upright). The colours are the
// output's signal already (gamma, or PQ for HDR10 output), so nothing is
// converted: only resampled.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D src;

layout(push_constant) uniform Push {
    vec4 size;   // the picture: width, height, 1 / width, 1 / height
    vec4 xform;  // quarter turns clockwise, mirror, upside down, filter (0 as it is, 1 Bicubic, 2 Lanczos-3, 4 EWA Lanczos Sharp)
} pc;

/* the point of the picture this pixel shows (as video.frag's turned_uv) */
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

vec3 at(ivec2 p)
{
    p = clamp(p, ivec2(0), ivec2(pc.size.xy) - 1);
    return texelFetch(src, p, 0).rgb;
}

/* Catmull-Rom (cubic, a = -0.5) */
float cubic(float x)
{
    x = abs(x);
    if (x < 1.0)
        return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0)
        return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

/* Lanczos, 3 lobes: sinc(x) sinc(x / 3) */
float lanczos3(float x)
{
    x = abs(x);
    if (x < 1e-5)
        return 1.0;
    if (x >= 3.0)
        return 0.0;
    float px = 3.14159265 * x;
    return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}

/* EWA Lanczos Sharp: a jinc kernel windowed by a jinc stretched to the
 * kernel's third zero (radius), slightly sharpened (blur, the common
 * "lanczossharp" values). The weights against (distance / radius)^2, worked
 * out once (scratchpad ewalut.py: jinc from Bessel J1's series) instead of
 * two Bessel functions per tap: 4K at 60 frames a second couldn't keep up
 * (the user, 2026-10-20). */
const float EWA_RADIUS = 3.2383154841662362;   /* jinc's third zero */
const float EWA_BLUR = 0.9812505644269356;
const int EWA_N = 128;
const float ewa_weights[EWA_N] = float[](
    1.00000000, 0.88856625, 0.78646167, 0.69304924, 0.60772891, 0.52993568, 0.45913785, 0.39483532,
    0.33655797, 0.28386408, 0.23633888, 0.19359313, 0.15526176, 0.12100260, 0.09049514, 0.06343937,
    0.03955469, 0.01857879, 0.00026671, -0.01561017, -0.02926503, -0.04089648, -0.05068938, -0.05881565,
    -0.06543500, -0.07069565, -0.07473502, -0.07768032, -0.07964923, -0.08075044, -0.08108421, -0.08074286,
    -0.07981134, -0.07836759, -0.07648310, -0.07422323, -0.07164766, -0.06881077, -0.06576196, -0.06254603,
    -0.05920345, -0.05577069, -0.05228051, -0.04876221, -0.04524186, -0.04174259, -0.03828478, -0.03488626,
    -0.03156254, -0.02832697, -0.02519092, -0.02216395, -0.01925395, -0.01646730, -0.01380900, -0.01128280,
    -0.00889129, -0.00663604, -0.00451770, -0.00253608, -0.00069026, 0.00102136, 0.00260094, 0.00405117,
    0.00537518, 0.00657647, 0.00765889, 0.00862656, 0.00948383, 0.01023522, 0.01088541, 0.01143918,
    0.01190139, 0.01227696, 0.01257078, 0.01278777, 0.01293282, 0.01301075, 0.01302631, 0.01298418,
    0.01288892, 0.01274502, 0.01255679, 0.01232845, 0.01206406, 0.01176756, 0.01144270, 0.01109311,
    0.01072225, 0.01033340, 0.00992971, 0.00951414, 0.00908952, 0.00865847, 0.00822350, 0.00778693,
    0.00735094, 0.00691754, 0.00648860, 0.00606585, 0.00565087, 0.00524509, 0.00484983, 0.00446625,
    0.00409541, 0.00373824, 0.00339554, 0.00306803, 0.00275628, 0.00246079, 0.00218196, 0.00192009,
    0.00167539, 0.00144801, 0.00123798, 0.00104530, 0.00086989, 0.00071158, 0.00057018, 0.00044542,
    0.00033698, 0.00024452, 0.00016761, 0.00010583, 0.00005870, 0.00002571, 0.00000633, 0.00000000);

float ewa_weight(float d2)   /* d2: (distance / blur)^2 */
{
    float t = d2 / (EWA_RADIUS * EWA_RADIUS) * float(EWA_N - 1);
    int i = int(t);
    if (i >= EWA_N - 1)
        return 0.0;
    return mix(ewa_weights[i], ewa_weights[i + 1], t - float(i));
}

vec3 ewa_lanczos(vec2 p)
{
    /* around the nearest pixel: the circle (3.18 pixels) then fits in 7x7 */
    vec2 base = floor(p + 0.5);
    vec2 f = p - base;
    ivec2 i = ivec2(base);
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (int y = -3; y <= 3; y++) {
        for (int x = -3; x <= 3; x++) {
            vec2 o = (vec2(float(x), float(y)) - f) / EWA_BLUR;
            float d2 = dot(o, o);
            if (d2 >= EWA_RADIUS * EWA_RADIUS)
                continue;
            float w = ewa_weight(d2);
            sum += at(i + ivec2(x, y)) * w;
            weight += w;
        }
    }
    vec3 c = sum / weight;
    /* a light anti-ringing, a quarter of the way to the 2x2 around the point */
    ivec2 q = ivec2(floor(p));
    vec3 a = at(q), b = at(q + ivec2(1, 0)), d = at(q + ivec2(0, 1)), e = at(q + ivec2(1, 1));
    return mix(c, clamp(c, min(min(a, b), min(d, e)), max(max(a, b), max(d, e))), 0.25);
}

void main()
{
    vec2 p = turned() * pc.size.xy - 0.5;
    if (int(pc.xform.w + 0.5) == 0) {
        /* already the rectangle's size (Anime4K's 1080p doubled on a 4K
         * screen): pixel for pixel */
        color = vec4(at(ivec2(floor(p + 0.5))), 1.0);
        return;
    }
    if (int(pc.xform.w + 0.5) == 4) {
        color = vec4(ewa_lanczos(p), 1.0);
        return;
    }
    vec2 base = floor(p);
    vec2 f = p - base;
    ivec2 i = ivec2(base);
    int filter_kind = int(pc.xform.w + 0.5);
    int lo = filter_kind == 2 ? -2 : -1, hi = filter_kind == 2 ? 3 : 2;
    vec3 sum = vec3(0.0);
    float weight = 0.0;
    for (int y = lo; y <= hi; y++) {
        float wy = filter_kind == 2 ? lanczos3(float(y) - f.y) : cubic(float(y) - f.y);
        for (int x = lo; x <= hi; x++) {
            float w = wy * (filter_kind == 2 ? lanczos3(float(x) - f.x) : cubic(float(x) - f.x));
            sum += at(i + ivec2(x, y)) * w;
            weight += w;
        }
    }
    vec3 c = sum / weight;
    /* halos kept small: halfway back towards the four nearest pixels' range
     * (a full clamp took away most of the sharpness these filters add) */
    vec3 a = at(i), b = at(i + ivec2(1, 0)), d = at(i + ivec2(0, 1)), e = at(i + ivec2(1, 1));
    c = mix(c, clamp(c, min(min(a, b), min(d, e)), max(max(a, b), max(d, e))), 0.5);
    color = vec4(c, 1.0);
}
