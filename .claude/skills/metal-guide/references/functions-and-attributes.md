# MSL Functions & Attributes

Spec chapter 5. Function qualifiers, every built-in `[[attribute]]`, resource binding,
interpolation, and function constants.

## Function qualifiers (entry points)

A qualifier before the return type marks a shader entry point. `qualifier`
and `[[qualifier]]` are equivalent (`vertex` == `[[vertex]]`).

| Qualifier | Stage | Return type rule |
|-----------|-------|------------------|
| `vertex` | vertex processing | struct containing `float4 [[position]]`, or `void` |
| `fragment` | fragment processing | color output(s) struct / scalar-vector, or `void` |
| `kernel` | compute (data-parallel) | must be `void` |
| `[[object]]` | mesh pipeline object stage (Metal 3+) | `void` |
| `[[mesh]]` | mesh pipeline mesh stage (Metal 3+) | `void` |
| `[[intersection(tags...)]]` | ray-tracing custom intersection (Metal 2.3+) | bool / result struct |
| `[[visible]]` | callable from other shaders (function tables) | any |
| `[[stitchable]]` | linkable/stitched function | any |
| tile function | tile/imageblock compute in a render pass | `void` |

Non-entry (helper) functions are ordinary C++ functions; they may be called from entry
points. Recursion and function pointers to entry points are not allowed.

Prefixed execution-limit attributes:

```metal
[[max_total_threads_per_threadgroup(256)]] kernel void k(...) { }
[[early_fragment_tests]] fragment float4 f(...) { }   // depth/stencil before shading
```

## Argument (resource) binding

Every buffer/texture/sampler/threadgroup argument gets an explicit index that matches the
host encoder:

```metal
[[buffer(index)]]        // device or constant buffer
[[texture(index)]]       // texture
[[sampler(index)]]       // sampler
[[threadgroup(index)]]   // threadgroup memory allocated by the host
```

`[[raster_order_group(n)]]` orders overlapping fragment accesses to the same pixel.
`[[user(name)]]` attaches a semantic name. Argument-buffer members use `[[id(n)]]`.

## Per-vertex input attributes — `[[stage_in]]`

The vertex `[[stage_in]]` struct's members carry `[[attribute(n)]]` matching the vertex
descriptor:

```metal
struct VIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];
};
```

## Built-in variable attributes by stage

### Vertex function inputs

| Attribute | Type | Meaning |
|-----------|------|---------|
| `[[vertex_id]]` | ushort/uint | index of the vertex |
| `[[instance_id]]` | ushort/uint | index of the instance |
| `[[base_vertex]]` | ushort/uint | base vertex of the draw |
| `[[base_instance]]` | ushort/uint | base instance of the draw |
| `[[amplification_id]]` | ushort/uint | vertex-amplification output index |
| `[[amplification_count]]` | ushort/uint | number of amplification outputs |

### Vertex function outputs (members of the returned struct)

| Attribute | Type | Meaning |
|-----------|------|---------|
| `[[position]]` | float4 | clip-space position (**required** if non-void) |
| `[[point_size]]` | float | point sprite size |
| `[[clip_distance]]` | float or float[n] | user clip distances |
| `[[render_target_array_index]]` | uchar/ushort/uint | layered rendering target slice |
| `[[viewport_array_index]]` | uchar/ushort/uint | viewport selection |
| `[[invariant]]` | on `[[position]]` | bit-exact position across passes (needs `-fpreserve-invariance`) |
| `[[shared]]` | any | same value for all amplification outputs |

### Fragment function inputs

| Attribute | Type | Meaning |
|-----------|------|---------|
| `[[stage_in]]` | struct | interpolated vertex/mesh outputs |
| `[[position]]` | float4 | window-relative (x, y, z, 1/w) |
| `[[front_facing]]` | bool | true if front-facing primitive |
| `[[point_coord]]` | float2 | coordinate within a point sprite |
| `[[color(m)]]` `[[color(m), index(i)]]` | floatN/halfN/intN/… | current color-attachment value (programmable blending) |
| `[[sample_id]]` | uint | current sample index |
| `[[sample_mask]]` | uint | coverage mask (also an output) |
| `[[barycentric_coord]]` | float3/float2 | barycentric weights (Metal 2.2+) |
| `[[primitive_id]]` | uint | primitive index |
| `[[render_target_array_index]]` / `[[viewport_array_index]]` | uint | passthrough |
| `[[thread_index_in_simdgroup]]` / `[[threads_per_simdgroup]]` | ushort/uint | SIMD info |
| `[[thread_index_in_quadgroup]]` | ushort/uint | quad lane |

### Fragment function outputs (members of the returned struct)

| Attribute | Type | Meaning |
|-----------|------|---------|
| `[[color(m)]]` (opt `, index(i)`) | floatN/halfN/intN/… | color attachment m (dual-source blending via index) |
| `[[depth(qualifier)]]` | float | fragment depth; qualifier ∈ `any`, `greater`, `less` |
| `[[stencil]]` | uint | stencil reference (Metal 2.1+) |
| `[[sample_mask]]` | uint | output coverage mask |

### Kernel (compute) function inputs

| Attribute | Type | Meaning |
|-----------|------|---------|
| `[[thread_position_in_grid]]` | uint/uint2/uint3 | global thread id |
| `[[thread_position_in_threadgroup]]` | uint/uint2/uint3 | local thread id |
| `[[threadgroup_position_in_grid]]` | uint/uint2/uint3 | threadgroup id |
| `[[threads_per_grid]]` | uint/uint2/uint3 | total grid size |
| `[[threads_per_threadgroup]]` | uint/uint2/uint3 | threadgroup dimensions |
| `[[threadgroups_per_grid]]` | uint/uint2/uint3 | number of threadgroups |
| `[[thread_index_in_threadgroup]]` | uint | flattened local index |
| `[[thread_index_in_simdgroup]]` / `[[simdgroup_index_in_threadgroup]]` | ushort/uint | SIMD lane / SIMD-group index |
| `[[threads_per_simdgroup]]` | ushort/uint | SIMD width (replaces deprecated `[[thread_execution_width]]`) |
| `[[quadgroup_index_in_threadgroup]]` / `[[thread_index_in_quadgroup]]` | ushort/uint | quad indices |
| `[[simdgroups_per_threadgroup]]` / `[[quadgroups_per_threadgroup]]` | ushort/uint | counts |
| `[[dispatch_threads_per_threadgroup]]` | uint/uint2/uint3 | threadgroup size at dispatch |
| `[[dispatch_simdgroups_per_threadgroup]]` / `[[dispatch_quadgroups_per_threadgroup]]` | uint | dispatch-time counts |
| `[[stage_in]]` | struct | per-thread `[[attribute(n)]]` inputs (compute stage-in) |

**Grid math** (2D, threadgroups of Sx×Sy over Wx×Wy groups):
```
thread_position_in_grid     = (gx*Sx + lx, gy*Sy + ly)
threads_per_grid            = (Wx*Sx, Wy*Sy)
thread_index_in_threadgroup = ly*Sx + lx
```

### Object / mesh function attributes (Metal 3+)

Object stage inputs mirror the kernel grid attributes plus `[[payload]]`
(`object_data` output to the mesh stage) and a `mesh_grid_properties` argument.
Mesh stage uses a `mesh<V, P, NV, NP, topology>` output object; inputs include
`[[payload]]`, `[[thread_position_in_grid]]`, etc. See
[advanced-features.md](advanced-features.md).

### Tile function attributes

`[[pixel_position_in_tile]]`, `[[pixels_per_tile]]`, `[[tile_index]]`,
`[[dispatch_threads_per_threadgroup]]`, plus `[[imageblock_data]]`. Used for imageblock
tile compute inside a render pass.

### Intersection function attributes (Metal 2.3+)

Inputs: `[[origin]]`, `[[direction]]`, `[[min_distance]]`, `[[max_distance]]`,
`[[distance]]`, `[[primitive_id]]`, `[[payload]]` (`ray_data`), plus geometry data.
Outputs: return `bool`/struct tagged `[[accept_intersection]]`, `[[continue_search]]`.
See [advanced-features.md](advanced-features.md).

## Sampling & interpolation qualifiers

On `[[stage_in]]` members of a fragment function, choose how vertex outputs interpolate:

| Qualifier | Meaning |
|-----------|---------|
| `[[center_perspective]]` | perspective-correct at pixel center (**default** for floats) |
| `[[center_no_perspective]]` | linear at pixel center (required for `[[position]]`) |
| `[[centroid_perspective]]` / `[[centroid_no_perspective]]` | sampled at covered-centroid |
| `[[sample_perspective]]` / `[[sample_no_perspective]]` | per-sample (forces per-sample shading) |
| `[[flat]]` | no interpolation (**required** for integer members) |

```metal
struct FIn {
    float4 pos   [[center_no_perspective]];
    float4 color [[center_perspective]];   // default anyway
    float2 uv;                             // defaults to center_perspective
    int    id    [[flat]];                 // integers must be flat
    float  s     [[sample_perspective]];   // triggers per-sample execution
};
```

Pull-model interpolation: declare a member as `interpolant<float4, interpolation::perspective>`
and call `.interpolate_at_center()`, `.interpolate_at_centroid()`,
`.interpolate_at_sample(i)`, `.interpolate_at_offset(o)` in the shader.

## Function constants (specialization)

Program-scope constants selected at pipeline-build time, without recompiling source:

```metal
constant bool  useNormalMap [[function_constant(0)]];
constant int   sampleCount  [[function_constant(1)]];
constant float bias = is_function_constant_defined(sampleCount) ? 0.5f : 0.0f;

fragment float4 f(VertexOut in [[stage_in]],
                  texture2d<float> nmap [[texture(0), function_constant(useNormalMap)]]) {
    if (useNormalMap) { /* nmap is only bound when useNormalMap is true */ }
    return float4(0);
}
```

`[[function_constant(name)]]` can gate: code paths (`if (name)`), whether a resource binding
exists, elements of a stage-in struct, and color/raster-order-group indices. Query with
`is_function_constant_defined(name)`.

## Raster order groups

`[[raster_order_group(n)]]` on device buffers/textures/imageblock members guarantees ordered,
race-free access when fragments overlap the same pixel (programmable blending, OIT).

## See also

- [data-types.md](data-types.md) · [address-spaces-and-operators.md](address-spaces-and-operators.md)
- [textures-and-samplers.md](textures-and-samplers.md) · [examples.md](examples.md)
- [advanced-features.md](advanced-features.md) — mesh/object/intersection/tile specifics
