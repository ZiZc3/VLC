#version 450
// FSRCNNX's result onto the screen: its brightness image (twice the
// picture's size) brought to the rectangle by Lanczos-3, the colour from
// the picture by Catmull-Rom, and the two put together. Changing only the
// brightness of an RGB colour adds the same amount to R, G and B (each has
// a weight of 1 for Y in YCbCr), so: colour + (new Y - the colour's Y).
// Turning and flipping as upscale.frag.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D src;    // the picture (pass 1)
layout(set = 0, binding = 1) uniform sampler2D luma;   // FSRCNNX's brightness

layout(push_constant) uniform Push {
    vec4 size;   // the picture: width, height, 1 / width, 1 / height
    vec4 xform;  // quarter turns clockwise, mirror, upside down
    vec4 lsize;  // the brightness image: width, height
} pc;

const vec3 K = vec3(0.2126, 0.7152, 0.0722);

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

vec3 rgb_at(ivec2 p)
{
    return texelFetch(src, clamp(p, ivec2(0), ivec2(pc.size.xy) - 1), 0).rgb;
}

float y_at(ivec2 p)
{
    return texelFetch(luma, clamp(p, ivec2(0), ivec2(pc.lsize.xy) - 1), 0).x;
}

float cubic(float x)
{
    x = abs(x);
    if (x < 1.0)
        return (1.5 * x - 2.5) * x * x + 1.0;
    if (x < 2.0)
        return ((-0.5 * x + 2.5) * x - 4.0) * x + 2.0;
    return 0.0;
}

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

void main()
{
    vec2 n = turned();

    vec2 p = n * pc.size.xy - 0.5;
    vec2 pb = floor(p), pf = p - pb;
    ivec2 pi = ivec2(pb);
    vec3 rgb = vec3(0.0);
    float rw = 0.0;
    for (int y = -1; y <= 2; y++) {
        float wy = cubic(float(y) - pf.y);
        for (int x = -1; x <= 2; x++) {
            float w = wy * cubic(float(x) - pf.x);
            rgb += rgb_at(pi + ivec2(x, y)) * w;
            rw += w;
        }
    }
    rgb /= rw;

    vec2 q = n * pc.lsize.xy - 0.5;
    vec2 qb = floor(q), qf = q - qb;
    ivec2 qi = ivec2(qb);
    float yv = 0.0, yw = 0.0;
    for (int y = -2; y <= 3; y++) {
        float wy = lanczos3(float(y) - qf.y);
        for (int x = -2; x <= 3; x++) {
            float w = wy * lanczos3(float(x) - qf.x);
            yv += y_at(qi + ivec2(x, y)) * w;
            yw += w;
        }
    }
    yv /= yw;
    /* halos kept small, as upscale.frag: halfway towards the 2x2's range */
    float a = y_at(qi), b = y_at(qi + ivec2(1, 0)), c = y_at(qi + ivec2(0, 1)), d = y_at(qi + ivec2(1, 1));
    yv = mix(yv, clamp(yv, min(min(a, b), min(c, d)), max(max(a, b), max(c, d))), 0.5);

    color = vec4(max(rgb + (yv - dot(rgb, K)), vec3(0.0)), 1.0);
}
