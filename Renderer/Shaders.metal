// Shaders.metal — Ragdoll Sandbox 2D batch renderer.
// Original code. Metal 3 baseline; no Metal 4-only language features used,
// so this compiles and runs on every Apple Silicon iPad (the Metal 4 driver
// executes this path natively).
//
// Batches (all instanced triangle strips, no vertex buffers):
//   boxes      vs_box      + fs_box      (alpha blend)
//   circles    vs_circle   + fs_circle   (alpha blend, SDF edge)
//   segments   vs_segment  + fs_segment  (alpha blend, round caps)
//   particles  vs_particle + fs_particle (alpha blend pass, then additive pass)
//   grid       vs_grid     + fs_grid     (opaque background pass)
//
// Instance layout must match Renderer.swift RSInstance (48 bytes).
// Uniform layout must match Renderer.swift RSUniforms (112 bytes).

#include <metal_stdlib>
using namespace metal;

struct RSInstance {
    float4 posSize; // xy: center (box/circle/particle) or p0 (segment)
                    // zw: size wh (diameter for circles) or p1 (segment)
    float4 color;   // rgba, 0..1
    float4 misc;    // x: angle radians (boxes), y: unused, z: tint (0 normal,
                    //    1 dead, 2 burning), w: particle life01 (particles)
};

struct RSUniforms {
    float3x3 viewMatrix;    // world -> centered y-up points
    float2   ndcScale;      // 2 / drawableSizeInPoints
    float    time;          // seconds, for burn flicker
    float    pad0;
    float3x3 invViewMatrix; // points -> world (grid pass)
};

struct VSOut {
    float4 pos   [[position]];
    float4 color;
    float2 uv;              // 0..1 across the quad
    float4 misc;
    float2 world;
};

// ---------------------------------------------------------------- tint

float3 rs_apply_tint(float3 col, float tint, float time, float2 world) {
    if (tint > 0.5 && tint < 1.5) {
        // dead: desaturate + darken
        float lum = dot(col, float3(0.299, 0.587, 0.114));
        col = mix(col, float3(lum), 0.65) * 0.42;
    } else if (tint > 1.5) {
        // burning: lick of orange, flickers with time
        float flick = 0.7 + 0.3 * sin(time * 21.0 + world.x * 0.35 + world.y * 0.27);
        col = mix(col, float3(1.0, 0.42, 0.08), 0.6 * flick);
    }
    return col;
}

// ---------------------------------------------------------------- boxes

vertex VSOut vs_box(uint vid [[vertex_id]],
                    uint iid [[instance_id]],
                    constant RSInstance *instances [[buffer(1)]],
                    constant RSUniforms &u [[buffer(0)]])
{
    RSInstance ins = instances[iid];
    // vid: 0=(0,0) 1=(1,0) 2=(0,1) 3=(1,1) -> triangle strip quad
    float2 c = float2(float(vid & 1u), float((vid >> 1u) & 1u));
    float2 local = (c - 0.5) * ins.posSize.zw;
    float ca = cos(ins.misc.x);
    float sa = sin(ins.misc.x);
    float2 world = ins.posSize.xy + float2(local.x * ca - local.y * sa,
                                           local.x * sa + local.y * ca);
    float3 vp = u.viewMatrix * float3(world, 1.0);
    VSOut out;
    out.pos   = float4(vp.xy * u.ndcScale, 0.0, 1.0);
    out.color = ins.color;
    out.uv    = c;
    out.misc  = ins.misc;
    out.world = world;
    return out;
}

fragment float4 fs_box(VSOut in [[stage_in]],
                       constant RSUniforms &u [[buffer(0)]])
{
    float2 q = abs(in.uv - 0.5) * 2.0;
    float edge = max(q.x, q.y);
    float aa = fwidth(edge) * 1.4142 + 1e-4;
    float3 col = rs_apply_tint(in.color.rgb, in.misc.z, u.time, in.world);
    float outline = smoothstep(0.82, 0.98, edge);
    col *= 1.0 - outline * 0.45;
    float alpha = in.color.a * (1.0 - smoothstep(1.0 - aa, 1.0, edge));
    return float4(col, alpha);
}

// ---------------------------------------------------------------- circles (SDF)

vertex VSOut vs_circle(uint vid [[vertex_id]],
                       uint iid [[instance_id]],
                       constant RSInstance *instances [[buffer(1)]],
                       constant RSUniforms &u [[buffer(0)]])
{
    RSInstance ins = instances[iid];
    float2 c = float2(float(vid & 1u), float((vid >> 1u) & 1u));
    float2 world = ins.posSize.xy + (c - 0.5) * ins.posSize.zw;
    float3 vp = u.viewMatrix * float3(world, 1.0);
    VSOut out;
    out.pos   = float4(vp.xy * u.ndcScale, 0.0, 1.0);
    out.color = ins.color;
    out.uv    = c;
    out.misc  = ins.misc;
    out.world = world;
    return out;
}

fragment float4 fs_circle(VSOut in [[stage_in]],
                          constant RSUniforms &u [[buffer(0)]])
{
    float d = length(in.uv - 0.5) * 2.0;
    float aa = fwidth(d) * 1.4142 + 1e-4;
    float3 col = rs_apply_tint(in.color.rgb, in.misc.z, u.time, in.world);
    float outline = smoothstep(0.78, 0.96, d);
    col *= 1.0 - outline * 0.45;
    float alpha = in.color.a * (1.0 - smoothstep(1.0 - aa, 1.0, d));
    return float4(col, alpha);
}

// ---------------------------------------------------------------- segments (round caps)

constant float RS_SEGMENT_WIDTH = 0.6; // world units; see README

vertex VSOut vs_segment(uint vid [[vertex_id]],
                        uint iid [[instance_id]],
                        constant RSInstance *instances [[buffer(1)]],
                        constant RSUniforms &u [[buffer(0)]])
{
    RSInstance ins = instances[iid];
    float2 p0 = ins.posSize.xy;
    float2 p1 = ins.posSize.zw;
    float2 dir = p1 - p0;
    float len = max(length(dir), 1e-5);
    float2 dn = dir / len;
    float2 n = float2(-dn.y, dn.x);
    // Round caps: extend the quad half a width past each endpoint so the
    // fragment cap-SDF has room to work. (Bug fix 2026-10-04: previously the
    // quad ended exactly at p0/p1, making the cap code dead and caps flat.)
    float hw = RS_SEGMENT_WIDTH * 0.5;
    float2 e0 = p0 - dn * hw;
    float2 e1 = p1 + dn * hw;
    float2 c = float2(float(vid & 1u), float((vid >> 1u) & 1u));
    float2 world = mix(e0, e1, c.x) + n * (c.y - 0.5) * RS_SEGMENT_WIDTH;
    float3 vp = u.viewMatrix * float3(world, 1.0);
    VSOut out;
    out.pos   = float4(vp.xy * u.ndcScale, 0.0, 1.0);
    out.color = ins.color;
    // uv.x: 0 at p0, 1 at p1 (cap SDF measures past [0,1]); uv.y across.
    out.uv    = float2((c.x * (len + 2.0 * hw) - hw) / len, c.y);
    out.misc  = ins.misc;
    out.world = world;
    return out;
}

fragment float4 fs_segment(VSOut in [[stage_in]],
                           constant RSUniforms &u [[buffer(0)]])
{
    // uv.x along the segment (0..1), uv.y across (-0.5..0.5 after shift)
    float2 p = float2(in.uv.x, in.uv.y - 0.5);
    float dx = max(max(-p.x, p.x - 1.0), 0.0); // 0 inside, >0 past caps
    float d = length(float2(dx, p.y)) / 0.5;   // 1 at the edge
    float aa = fwidth(d) * 1.4142 + 1e-4;
    float3 col = rs_apply_tint(in.color.rgb, in.misc.z, u.time, in.world);
    float alpha = in.color.a * (1.0 - smoothstep(1.0 - aa, 1.0, d));
    return float4(col, alpha);
}

// ---------------------------------------------------------------- particles

vertex VSOut vs_particle(uint vid [[vertex_id]],
                         uint iid [[instance_id]],
                         constant RSInstance *instances [[buffer(1)]],
                         constant RSUniforms &u [[buffer(0)]])
{
    RSInstance ins = instances[iid];
    float2 c = float2(float(vid & 1u), float((vid >> 1u) & 1u));
    float2 world = ins.posSize.xy + (c - 0.5) * ins.posSize.zw;
    float3 vp = u.viewMatrix * float3(world, 1.0);
    VSOut out;
    out.pos   = float4(vp.xy * u.ndcScale, 0.0, 1.0);
    out.color = ins.color;
    out.uv    = c;
    out.misc  = ins.misc;
    out.world = world;
    return out;
}

fragment float4 fs_particle(VSOut in [[stage_in]])
{
    float d = length(in.uv - 0.5) * 2.0;
    float fall = pow(clamp(1.0 - d, 0.0, 1.0), 1.6);
    float lifeFade = clamp(in.misc.w * 1.5, 0.0, 1.0);
    float a = in.color.a * fall * lifeFade;
    return float4(in.color.rgb, a);
}

// ---------------------------------------------------------------- grid background (opaque)

struct GridOut {
    float4 pos [[position]];
    float2 world;
};

vertex GridOut vs_grid(uint vid [[vertex_id]],
                       constant RSUniforms &u [[buffer(0)]])
{
    // fullscreen triangle: (-1,-1), (3,-1), (-1,3)
    float2 ndc = float2(vid == 1u ? 3.0 : -1.0,
                        vid == 2u ? 3.0 : -1.0);
    float2 vp = ndc / u.ndcScale;
    float3 w = u.invViewMatrix * float3(vp, 1.0);
    GridOut out;
    out.pos = float4(ndc, 0.0, 1.0);
    out.world = w.xy;
    return out;
}

fragment float4 fs_grid(GridOut in [[stage_in]])
{
    float3 bg = float3(0.055, 0.060, 0.075);
    // minor cells 50u, major 250u, anti-aliased
    float2 gp = in.world / 50.0;
    float2 gfw = max(fwidth(gp), float2(1e-5));
    float2 gf = abs(fract(gp - 0.5) - 0.5) / gfw;
    float minor = 1.0 - min(min(gf.x, gf.y), 1.0);
    // Fade lines out when cells go sub-pixel (far zoom-out); otherwise the
    // AA saturates and the whole background washes out. (2026-10-04)
    minor *= 1.0 - smoothstep(0.25, 0.5, min(gfw.x, gfw.y));
    float2 Hp = in.world / 250.0;
    float2 Hfw = max(fwidth(Hp), float2(1e-5));
    float2 Hf = abs(fract(Hp - 0.5) - 0.5) / Hfw;
    float major = 1.0 - min(min(Hf.x, Hf.y), 1.0);
    major *= 1.0 - smoothstep(0.25, 0.5, min(Hfw.x, Hfw.y));
    float3 col = bg + float3(0.45, 0.50, 0.60) * minor * 0.10
                    + float3(0.55, 0.60, 0.70) * major * 0.16;
    return float4(col, 1.0);
}
