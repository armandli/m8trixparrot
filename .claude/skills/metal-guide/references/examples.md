# MSL Complete Examples

Compilable, self-contained shaders demonstrating common patterns. Each starts from
`<metal_stdlib>`; assume `using namespace metal;`.

## 1. Textured, lit vertex + fragment

```metal
#include <metal_stdlib>
using namespace metal;

struct VertexIn  { float3 position [[attribute(0)]]; float3 normal [[attribute(1)]];
                   float2 uv [[attribute(2)]]; };
struct VertexOut { float4 position [[position]]; float3 normal; float2 uv; };
struct Uniforms  { float4x4 mvp; float4x4 normalMatrix; };

vertex VertexOut v_main(VertexIn in [[stage_in]], constant Uniforms &u [[buffer(1)]]) {
    VertexOut o;
    o.position = u.mvp * float4(in.position, 1.0f);
    o.normal   = (u.normalMatrix * float4(in.normal, 0.0f)).xyz;
    o.uv       = in.uv;
    return o;
}

fragment float4 f_main(VertexOut in [[stage_in]],
                       texture2d<float> albedo [[texture(0)]], sampler s [[sampler(0)]]) {
    float3 N = normalize(in.normal);
    float3 L = normalize(float3(0.5f, 1.0f, 0.5f));
    float  d = saturate(dot(N, L));
    float4 c = albedo.sample(s, in.uv);
    return float4(c.rgb * (0.1f + 0.9f * d), c.a);
}
```

## 2. Compute: SAXPY with bounds guard

```metal
kernel void saxpy(device const float *x [[buffer(0)]],
                  device       float *y [[buffer(1)]],
                  constant     float &a [[buffer(2)]],
                  constant     uint  &n [[buffer(3)]],
                  uint gid [[thread_position_in_grid]]) {
    if (gid >= n) return;
    y[gid] = a * x[gid] + y[gid];
}
```

## 3. Texture blur kernel (read/write)

```metal
kernel void blur(texture2d<float>                src [[texture(0)]],
                 texture2d<float, access::write> dst [[texture(1)]],
                 uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    constexpr sampler s(address::clamp_to_edge, filter::linear, coord::pixel);
    float2 c = float2(gid) + 0.5f;
    float4 sum = src.sample(s, c + float2(-1,  0)) * 0.25f
               + src.sample(s, c + float2( 1,  0)) * 0.25f
               + src.sample(s, c + float2( 0, -1)) * 0.25f
               + src.sample(s, c + float2( 0,  1)) * 0.25f;
    dst.write(sum, gid);
}
```

## 4. Parallel reduction: SIMD + threadgroup + atomic

```metal
kernel void reduce_sum(const device int  *input  [[buffer(0)]],
                       device atomic_int *output [[buffer(1)]],
                       threadgroup int   *ldata  [[threadgroup(0)]],
                       uint gid       [[thread_position_in_grid]],
                       uint lid       [[thread_position_in_threadgroup]],
                       uint lsize     [[threads_per_threadgroup]],
                       uint simd_size [[threads_per_simdgroup]],
                       uint simd_lane [[thread_index_in_simdgroup]],
                       uint simd_id   [[simdgroup_index_in_threadgroup]]) {
    int val = input[gid] + input[gid + lsize];

    for (uint off = simd_size / 2; off > 0; off /= 2)   // reduce within SIMD-group
        val += simd_shuffle_down(val, off);

    if (simd_lane == 0) ldata[simd_id] = val;           // one partial per SIMD-group
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (lid < lsize / simd_size) {                      // final reduce of partials
        val = ldata[lid];
        for (uint off = (lsize / simd_size) / 2; off > 0; off /= 2)
            val += simd_shuffle_down(val, off);
    }
    if (lid == 0)
        atomic_fetch_add_explicit(output, val, memory_order_relaxed);
}
```

## 5. Deferred G-buffer (multiple render targets)

```metal
struct GBufferOut {
    float4 albedo   [[color(0)]];
    float4 normal   [[color(1)]];
    float4 position [[color(2)]];
    float  depth    [[depth(any)]];
};

fragment GBufferOut gbuffer(VertexOut in [[stage_in]],
                            texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]]) {
    GBufferOut o;
    o.albedo   = tex.sample(s, in.uv);
    o.normal   = float4(normalize(in.normal) * 0.5f + 0.5f, 1.0f);
    o.position = in.position;
    o.depth    = in.position.z;
    return o;
}
```

## 6. Shadow mapping (depth compare sampler)

```metal
fragment float4 shadowed(VertexOut in [[stage_in]],
                         depth2d<float> shadowMap [[texture(0)]],
                         sampler cmp [[sampler(0)]],       // compare_func::less
                         constant float4x4 &lightVP [[buffer(0)]]) {
    float4 lp = lightVP * in.position;
    float3 ndc = lp.xyz / lp.w;
    float2 uv  = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
    float  lit = shadowMap.sample_compare(cmp, uv, ndc.z - 1e-3f);
    return float4(float3(lit), 1.0f);
}
```

## 7. Vertex amplification (stereo / multi-view)

```metal
struct AmpOut {
    float4 position [[position]];
    float2 uv       [[shared]];                 // identical across amplifications
    ushort viewport [[viewport_array_index]];
};

vertex AmpOut amp_vertex(VertexIn in [[stage_in]],
                         constant float4x4 *viewProj [[buffer(1)]],   // one per view
                         ushort amp_id    [[amplification_id]],
                         ushort amp_count [[amplification_count]]) {
    AmpOut o;
    o.position = viewProj[amp_id] * float4(in.position, 1.0f);
    o.uv       = in.uv;
    o.viewport = amp_id;
    return o;
}
```

## 8. Bindless material via argument buffer

```metal
struct Material {
    texture2d<float> albedo    [[id(0)]];
    texture2d<float> normalMap [[id(1)]];
    sampler          smp       [[id(2)]];
    device float4   *params    [[id(3)]];
};

fragment float4 pbr(VertexOut in [[stage_in]], constant Material &m [[buffer(0)]]) {
    float4 base = m.albedo.sample(m.smp, in.uv);
    float  rough = m.params[0].x;
    return float4(base.rgb * (1.0f - rough), base.a);
}
```

## 9. Prefix sum (scan) within a SIMD-group

```metal
kernel void inclusive_scan(device const int *in  [[buffer(0)]],
                           device       int *out [[buffer(1)]],
                           uint gid [[thread_position_in_grid]]) {
    int v = in[gid];
    out[gid] = simd_prefix_inclusive_sum(v);   // per-SIMD-group inclusive scan
}
```

## 10. Tiled 3×3 convolution with threadgroup cache

```metal
constant int TILE = 16;

kernel void conv3x3(texture2d<float>                src [[texture(0)]],
                    texture2d<float, access::write> dst [[texture(1)]],
                    constant float                 *k   [[buffer(0)]],   // 9 weights
                    uint2 gid [[thread_position_in_grid]],
                    uint2 lid [[thread_position_in_threadgroup]]) {
    threadgroup float4 tile[TILE + 2][TILE + 2];
    int2 base = int2(gid) - 1;
    tile[lid.y + 1][lid.x + 1] = src.read(uint2(clamp(base + 1, int2(0),
                                    int2(src.get_width() - 1, src.get_height() - 1))));
    threadgroup_barrier(mem_flags::mem_threadgroup);

    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float4 acc = 0.0f;
    for (int dy = 0; dy < 3; ++dy)
        for (int dx = 0; dx < 3; ++dx)
            acc += tile[lid.y + dy][lid.x + dx] * k[dy * 3 + dx];
    dst.write(acc, gid);
}
```

## See also

- [functions-and-attributes.md](functions-and-attributes.md) — attribute reference used above
- [standard-library.md](standard-library.md) · [textures-and-samplers.md](textures-and-samplers.md) · [atomics.md](atomics.md)
- [advanced-features.md](advanced-features.md) — ray tracing / mesh / tensor examples
