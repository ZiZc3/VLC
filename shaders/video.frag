#version 450
// Planar YCbCr (VLC's I420 / J420 / I0AL, or NV12 / P010) to RGB. Same constants as
// host/vlc_probe.c, which checked them against real frames in World 1.
// Then the picture controls: brightness and contrast on luma, hue and
// saturation on chroma, gamma on the result. HDR (PQ, HLG) is tone mapped
// to SDR between the two; with HDR10 output on (outp.x), the picture goes
// out as PQ, BT.2020 instead.
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

layout(set = 0, binding = 0) uniform sampler2D tex_y;
layout(set = 0, binding = 1) uniform sampler2D tex_u;
layout(set = 0, binding = 2) uniform sampler2D tex_v;

layout(push_constant) uniform Push {
    vec4 row0;
    vec4 row1;
    vec4 row2;
    vec4 params; // sample scale, limited range, hue (radians), -
    vec2 uv_scale;
    vec4 adjust; // brightness, contrast, saturation, gamma exponent
    vec4 view;   // 360: yaw, pitch, tan(fov / 2), screen aspect
    vec4 xform;  // quarter turns clockwise, mirror, upside down, sharpen amount
    vec4 hdr;    // transfer (0 SDR, 1 PQ, 2 HLG), peak (nits), BT.2020 primaries (1), NV12/P010 (1)
    vec4 outp;   // HDR10 output (1): PQ, BT.2020; -, -, -
} pc;

/* HDR to SDR, the same sums as hdr_to_sdr() in player.cc (screenshots). */
const float SDR_WHITE = 203.0; // HDR's reference white (BT.2408) = the TV's white
const float PQ_M1 = 0.1593017578125, PQ_M2 = 78.84375;
const float PQ_C1 = 0.8359375, PQ_C2 = 18.8515625, PQ_C3 = 18.6875;

vec3 pq_to_nits(vec3 e)
{
    vec3 p = pow(clamp(e, 0.0, 1.0), vec3(1.0 / PQ_M2));
    return 10000.0 * pow(max(p - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * p), vec3(1.0 / PQ_M1));
}

float nits_to_pq(float nits)
{
    float y = pow(clamp(nits / 10000.0, 0.0, 1.0), PQ_M1);
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), PQ_M2);
}

/* HLG: scene light, then the system gamma of a 1000-nit TV (BT.2100) */
vec3 hlg_to_nits(vec3 e)
{
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    e = clamp(e, 0.0, 1.0);
    vec3 s = mix(e * e / 3.0, (exp((e - c) / a) + b) / 12.0, step(vec3(0.5), e));
    float ys = max(dot(s, vec3(0.2627, 0.6780, 0.0593)), 1e-6);
    return 1000.0 * pow(ys, 0.2) * s;
}

vec3 nits_to_pq3(vec3 nits)
{
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(PQ_M1));
    return pow((PQ_C1 + PQ_C2 * y) / (1.0 + PQ_C3 * y), vec3(PQ_M2));
}

/* HDR10 output: the picture as PQ with BT.2020 primaries. HDR10 is that
 * already and goes through untouched; HLG and SDR become light (SDR white
 * at 203 nits), then BT.2020, then PQ. */
vec3 to_pq_output(vec3 e)
{
    if (pc.hdr.x > 0.5 && pc.hdr.x < 1.5 && pc.hdr.z > 0.5)
        return e;
    vec3 nits;
    if (pc.hdr.x > 0.5)
        nits = pc.hdr.x < 1.5 ? pq_to_nits(e) : hlg_to_nits(e);
    else
        nits = pow(clamp(e, 0.0, 1.0), vec3(2.4)) * SDR_WHITE;
    if (pc.hdr.z < 0.5) {
        /* BT.709 -> BT.2020 primaries */
        nits = mat3(0.6274, 0.0691, 0.0164,
                    0.3293, 0.9195, 0.0880,
                    0.0433, 0.0114, 0.8956) * nits;
    }
    return nits_to_pq3(nits);
}

vec3 hdr_to_sdr(vec3 e)
{
    vec3 nits = pc.hdr.x < 1.5 ? pq_to_nits(e) : hlg_to_nits(e);
    /* BT.2390's roll-off on the brightest channel (keeps the hue): up to
     * about half of SDR white nothing changes, above it the highlights bend
     * down to fit under white. */
    float peak = pc.hdr.y;
    float sig = max(nits.r, max(nits.g, nits.b));
    if (sig > 0.0 && peak > SDR_WHITE) {
        float src = nits_to_pq(peak);
        float e1 = min(nits_to_pq(sig) / src, 1.0);
        float maxl = nits_to_pq(SDR_WHITE) / src;
        float ks = 1.5 * maxl - 0.5;
        float e2 = e1;
        if (e1 > ks) {
            float t = (e1 - ks) / (1.0 - ks), t2 = t * t, t3 = t2 * t;
            e2 = (2.0 * t3 - 3.0 * t2 + 1.0) * ks + (t3 - 2.0 * t2 + t) * (1.0 - ks) +
                 (-2.0 * t3 + 3.0 * t2) * maxl;
        }
        nits *= pq_to_nits(vec3(e2 * src)).r / sig;
    }
    vec3 lin = nits / SDR_WHITE;
    if (pc.hdr.z > 0.5) {
        /* BT.2020 -> BT.709 primaries (linear light; GLSL matrices are by column) */
        lin = mat3(1.6605, -0.1246, -0.0182,
                   -0.5876, 1.1329, -0.1006,
                   -0.0728, -0.0083, 1.1187) * lin;
    }
    /* back to a TV's signal (BT.1886, gamma 2.4) */
    return pow(clamp(lin, 0.0, 1.0), vec3(1.0 / 2.4));
}

/* Rotate and flip: the point of the picture this pixel shows. */
vec2 turned_uv()
{
    vec2 n = uv / pc.uv_scale;
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
    return n * pc.uv_scale;
}

/* A 360° video (equirectangular): the direction this pixel looks in, turned by
 * the view, picks its point on the sphere. */
vec2 spherical_uv()
{
    vec2 ndc = (uv / pc.uv_scale) * 2.0 - 1.0;
    vec3 d = normalize(vec3(ndc.x * pc.view.z * pc.view.w, -ndc.y * pc.view.z, 1.0));
    float cp = cos(pc.view.y), sp = sin(pc.view.y);
    d = vec3(d.x, d.y * cp + d.z * sp, -d.y * sp + d.z * cp);
    float cy = cos(pc.view.x), sy = sin(pc.view.x);
    d = vec3(d.x * cy + d.z * sy, d.y, -d.x * sy + d.z * cy);
    float lon = atan(d.x, d.z), lat = asin(clamp(d.y, -1.0, 1.0));
    return vec2(lon / 6.2831853 + 0.5, 0.5 - lat / 3.14159265) * pc.uv_scale;
}

void main()
{
    float s = pc.params.x;
    vec2 at = pc.params.w > 0.5 ? spherical_uv() : turned_uv();
    float y = texture(tex_y, at).r * s;
    if (pc.xform.w > 0.0) {
        /* sharpen: the luma pushed away from the average of the 3x3 around
         * it (an unsharp mask), the edges' halo kept in range */
        vec2 t = 1.0 / vec2(textureSize(tex_y, 0));
        float around = 0.0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                if (dx != 0 || dy != 0)
                    around += texture(tex_y, at + vec2(float(dx), float(dy)) * t).r;
        y = clamp(y + (y - around * 0.125 * s) * pc.xform.w, 0.0, 1.0);
    }
    /* NV12 / P010: U and V side by side in the second plane */
    vec2 chroma = pc.hdr.w > 0.5 ? texture(tex_u, at).rg
                                 : vec2(texture(tex_u, at).r, texture(tex_v, at).r);
    float u = chroma.x * s - 0.5;
    float v = chroma.y * s - 0.5;
    if (pc.params.y > 0.5) {
        y = (y - 16.0 / 255.0) * (255.0 / 219.0);
        u *= 255.0 / 224.0;
        v *= 255.0 / 224.0;
    }
    y = (y - 0.5) * pc.adjust.y + 0.5 + pc.adjust.x;
    float c = cos(pc.params.z), sn = sin(pc.params.z);
    vec2 uv2 = vec2(u * c - v * sn, u * sn + v * c) * pc.adjust.z;
    vec3 yuv = vec3(y, uv2);
    vec3 rgb = clamp(vec3(dot(pc.row0.xyz, yuv), dot(pc.row1.xyz, yuv), dot(pc.row2.xyz, yuv)),
                     0.0, 1.0);
    if (pc.outp.x > 0.5) {
        color = vec4(to_pq_output(rgb), 1.0);
        return;
    }
    if (pc.hdr.x > 0.5)
        rgb = hdr_to_sdr(rgb);
    color = vec4(pow(rgb, vec3(pc.adjust.w)), 1.0);
}
