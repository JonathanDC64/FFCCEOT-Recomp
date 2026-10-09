// ffcc patch: texture smoothing (uSmoothTex: 0 off, 1 smooth, 2 sharp), spliced in before FinalColor() in 3DRenderFS.glsl.
//
// The game's textures are integer textures read with nearest sampling, which a GPU cannot filter, so the four texels around
// the point are fetched one by one (each fetch keeps the DS's repeat, flip and clamp) and blended here. Smooth is the
// usual bilinear blend. Sharp keeps every texel crisp and blends only in a band one pixel wide between texels (as wide as
// the texel is small on the screen), so a texture enlarged by the upscaling loses its stair-steps without going soft.
// Texels with alpha 0 keep the palette's colour 0 (often black or pink): the colours are weighted by their alpha so they
// do not bleed into the edge of a cut-out. When every texel is either clear or solid (all but the A3I5 and A5I3 formats) the
// result is clear or solid too: the cut-out is cut along the smooth blend's 0.5 contour (also in Sharp, so its edge is a clean
// diagonal, not texel-sized steps), and the opaque and translucent passes get the same kind of pixels as before.
uniform int uSmoothTex;

vec4 SmoothTexel(vec3 tc)
{
    vec2 size = vec2(textureSize(CurTexture, 0).xy);
    vec2 p = tc.xy * size - 0.5;
    vec2 base = floor(p);
    vec2 f = p - base;
    vec2 fs = f;  // sharpened blend for the colours; the shape of a cut-out always follows the smooth blend
    if (uSmoothTex == 2)
    {
        vec2 w = clamp(fwidth(tc.xy * size), vec2(1.0/256.0), vec2(1.0));
        fs = clamp((f - 0.5) / w + 0.5, 0.0, 1.0);
    }

    vec2 o = (base + 0.5) / size;
    vec2 d = 1.0 / size;
    vec4 t00 = vec4(texture(CurTexture, vec3(o, tc.z)));
    vec4 t10 = vec4(texture(CurTexture, vec3(o + vec2(d.x, 0.0), tc.z)));
    vec4 t01 = vec4(texture(CurTexture, vec3(o + vec2(0.0, d.y), tc.z)));
    vec4 t11 = vec4(texture(CurTexture, vec3(o + d, tc.z)));

    vec4 a4 = vec4(t00.a, t10.a, t01.a, t11.a) / 31.0;
    vec4 w4 = vec4((1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y);
    float cover = dot(w4, a4);
    vec4 ws = vec4((1.0 - fs.x) * (1.0 - fs.y), fs.x * (1.0 - fs.y), (1.0 - fs.x) * fs.y, fs.x * fs.y);
    if (dot(ws, a4) < 1.0/64.0) ws = w4;
    vec4 wa = ws * a4;
    vec3 rgb = (wa.x * t00.rgb + wa.y * t10.rgb + wa.z * t01.rgb + wa.w * t11.rgb) / max(dot(wa, vec4(1.0)), 1.0/1024.0) / 63.0;
    if (cover < 1.0/1024.0) rgb = t00.rgb / 63.0;

    bool solid = all(equal(a4 * (1.0 - a4), vec4(0.0)));  // every texel is 0 or 31 (a4 is 0 or 1)
    float a = solid ? (cover >= 0.5 ? 1.0 : 0.0) : cover;
    return vec4(rgb, a);
}

