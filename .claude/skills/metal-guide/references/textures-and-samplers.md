# MSL Textures & Samplers

Spec sections 2.9–2.10 (types) and 6.13 (functions).

## Samplers

A sampler describes how a texture is filtered and addressed. Declare it `constexpr` in the
shader, or bind one from the host with `[[sampler(n)]]`.

```metal
constexpr sampler s(coord::normalized,
                    address::clamp_to_edge,
                    filter::linear,
                    mip_filter::linear);

constexpr sampler shadow(coord::normalized,
                         address::clamp_to_edge,
                         compare_func::less);          // for sample_compare

fragment float4 f(..., sampler bound [[sampler(0)]]) { ... }
```

**Sampler state options (Table 2.7):**

| Option | Values |
|--------|--------|
| `coord` | `normalized` (default), `pixel` |
| `address` (per axis: `s_address`,`t_address`,`r_address` too) | `clamp_to_edge` (default), `clamp_to_zero`, `clamp_to_border`, `repeat`, `mirrored_repeat` |
| `filter` (sets both min & mag) | `nearest` (default), `linear` |
| `mag_filter` / `min_filter` | `nearest`, `linear` |
| `mip_filter` | `none` (default — no mipmapping), `nearest`, `linear` |
| `compare_func` | `never`,`less`,`less_equal`,`greater`,`greater_equal`,`equal`,`not_equal`,`always` |
| `border_color` | `transparent_black` (default), `opaque_black`, `opaque_white` |
| `max_anisotropy(n)` | 1–16 |
| `lod_clamp(min, max)` | float range clamp on level-of-detail |

## Texture types

`textureTYPE<T, access a = access::sample>`; `T` ∈ `float, half, int, uint, short, ushort`
(depth textures use `float`). Access: `access::sample` (default), `read`, `write`,
`read_write`.

```
texture1d  texture1d_array
texture2d  texture2d_array  texture2d_ms  texture2d_ms_array
texture3d  texturecube  texturecube_array
texture_buffer
depth2d  depth2d_array  depth2d_ms  depthcube  depthcube_array
```

## Member functions

### sample — requires `access::sample`

```metal
texture2d<float> tex; sampler s;
float4 c = tex.sample(s, float2(u, v));
float4 c = tex.sample(s, uv, level(mip));          // explicit LOD
float4 c = tex.sample(s, uv, bias(0.5f));          // LOD bias
float4 c = tex.sample(s, uv, gradient2d(dPdx, dPdy));
float4 c = tex.sample(s, uv, min_lod_clamp(minLod));
float4 c = tex.sample(s, uv, int2(dx, dy));        // integer texel offset (const)
```

### read — sampler-less, requires `access::read` or `read_write`

```metal
float4 c = tex.read(uint2(x, y));
float4 c = tex.read(uint2(x, y), lod);             // mip level
```

### write — requires `access::write` or `read_write`

```metal
tex.write(float4(r, g, b, a), uint2(x, y));
tex.write(color, uint2(x, y), lod);
```

### gather — 2×2 footprint (returns one channel from 4 texels)

```metal
float4 g = tex.gather(s, uv);                       // red by default
float4 g = tex.gather(s, uv, int2(0), component::x);// component: x/y/z/w
```

### Depth textures — `sample_compare` / `gather_compare`

```metal
depth2d<float> shadowMap;
float lit = shadowMap.sample_compare(shadow, uv, refZ);   // PCF-style compare
float4 g  = shadowMap.gather_compare(shadow, uv, refZ);
```

### Queries

```metal
uint w  = tex.get_width();
uint h  = tex.get_height();
uint d  = tex.get_depth();          // 3D
uint n  = tex.get_num_mip_levels();
uint sz = tex.get_array_size();     // *_array textures
uint ns = tex.get_num_samples();    // multisampled
```

### LOD calculation (fragment functions)

```metal
float lod = tex.calculate_clamped_lod(s, uv);
float lod = tex.calculate_unclamped_lod(s, uv);
```

## Per-type coordinate cheatsheet

| Texture | sample coord | read coord | extra args |
|---------|--------------|-----------|-----------|
| `texture1d` | `float` | `uint` | |
| `texture1d_array` | `float`, `slice` | `uint`, `slice` | |
| `texture2d` | `float2` | `uint2` | `lod`/`offset` |
| `texture2d_array` | `float2`, `slice` | `uint2`, `slice`, `lod` | |
| `texture3d` | `float3` | `uint3`, `lod` | |
| `texturecube` | `float3` (direction) | `uint2`, `face`, `lod` | face 0–5 |
| `texturecube_array` | `float3`, `slice` | `uint2`, `face`, `slice` | |
| `texture2d_ms` | — (read only) | `uint2`, `sampleIdx` | |
| `depth2d` | `float2` (returns `float`) | `uint2` | `sample_compare(s, uv, ref)` |
| `texture_buffer` | — | `uint` | linear, no filtering |

## Arrays of textures / samplers

```metal
kernel void k(const array<texture2d<float>, 8> src [[texture(0)]],
              texture2d<float, access::write>   dst [[texture(8)]],
              const array<sampler, 2>           smp [[sampler(0)]],
              uint2 gid [[thread_position_in_grid]]) {
    float4 c = src[gid.x % 8].sample(smp[0], float2(0.5f));
    dst.write(c, gid);
}
```

## Coordinate systems

| Space | Origin | +X | +Y | Notes |
|-------|--------|----|----|-------|
| NDC | center | right | up | z: 0 (near) → 1 (far), left-handed |
| Viewport (pixel) | top-left | right | down | fragment `[[position]]` units |
| Texture (normalized) | top-left | right | down | 0.0–1.0; `coord::pixel` for texels |

## See also

- [data-types.md](data-types.md) — texture/sampler type declarations, pixel formats
- [functions-and-attributes.md](functions-and-attributes.md) — `[[texture(n)]]`/`[[sampler(n)]]` binding
- [examples.md](examples.md) — texture copy/blur kernels, shadow sampling
