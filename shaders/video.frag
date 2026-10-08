#version 450
// Planar YCbCr (VLC's I420 / J420 / I0AL) to RGB. Same constants as
// host/vlc_probe.c, which checked them against real frames in World 1.
// Then the picture controls: brightness and contrast on luma, hue and
// saturation on chroma, gamma on the result.
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
} pc;

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
    float u = texture(tex_u, at).r * s - 0.5;
    float v = texture(tex_v, at).r * s - 0.5;
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
    color = vec4(pow(rgb, vec3(pc.adjust.w)), 1.0);
}
