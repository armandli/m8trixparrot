# MSL Advanced Features

Ray tracing, mesh/object shaders, tensors + Metal Performance Primitives, imageblocks,
SIMD-group matrices, indirect command buffers, and logging. Spec sections 2.17–2.22, 5, 6.14,
6.16.3, 6.17, 6.19–6.20, and chapter 7. Note per-feature version gates.

---

## Ray tracing (Metal 2.3+, `<metal_raytracing>`)

Types are in `namespace metal::raytracing`.

### Types

```metal
using namespace metal::raytracing;

ray r(float3 origin, float3 direction,
      float min_distance = 0.0f, float max_distance = INFINITY);
// fields: r.origin, r.direction, r.min_distance, r.max_distance

// Acceleration structures, parameterized by intersection tags:
acceleration_structure<>                              // primitive AS (no tags)
acceleration_structure<instancing>                    // instance AS
acceleration_structure<instancing, primitive_motion>  // + motion blur
// aliases: primitive_acceleration_structure, instance_acceleration_structure

intersector<intersection_tags...>            // configures & runs traversal
intersection_result<intersection_tags...>    // result of intersect()
intersection_query<intersection_tags...>     // inline, step-by-step traversal
intersection_function_table<intersection_tags...>
```

**Intersection tags** compose the behavior: `triangle_data`, `bounding_box_intersection`,
`instancing`, `world_space_data`, `primitive_motion`, `instance_motion`, `curve_data`,
`max_levels<N>`.

### Traversal with `intersector`

```metal
kernel void trace(acceleration_structure<> accel [[buffer(0)]],
                  device Ray *rays               [[buffer(1)]],
                  device Hit *hits               [[buffer(2)]],
                  uint tid [[thread_position_in_grid]]) {
    ray r(rays[tid].origin, rays[tid].dir, 0.001f, INFINITY);

    intersector<triangle_data> isect;
    isect.assume_geometry_type(geometry_type::triangle);
    isect.force_opacity(forced_opacity::opaque);
    isect.accept_any_intersection(false);              // false = closest hit

    intersection_result<triangle_data> res = isect.intersect(r, accel);

    if (res.type == intersection_type::triangle) {
        hits[tid].t          = res.distance;
        hits[tid].primitive  = res.primitive_id;
        hits[tid].geometry   = res.geometry_id;
        hits[tid].bary       = res.triangle_barycentric_coord;   // float2
    } else {
        hits[tid].t = -1.0f;                            // intersection_type::none
    }
}
```

`intersection_result` members (by tag): `type`, `distance`, `primitive_id`, `geometry_id`,
`instance_id` (with `instancing`), `triangle_barycentric_coord`, `world_space_origin`/
`world_space_direction` (with `world_space_data`), `function_id` (Metal 4.1+).

`intersect(...)` overloads take `(ray, accel)`, plus optional `intersection_function_table`,
`uint mask` (instance AS), `thread T &payload`, and motion `time` (with `*_motion` tags).

Intersector controls: `accept_any_intersection(bool)`, `assume_geometry_type(...)`,
`force_opacity(...)`, `set_triangle_cull_mode(...)`, `set_geometry_cull_mode(...)`,
`set_opacity_cull_mode(...)`.

### Inline ray queries — `intersection_query`

```metal
intersection_query<triangle_data> q;
q.reset(r, accel);
while (q.next()) {
    if (q.get_candidate_intersection_type() == intersection_type::triangle) {
        // optional custom test, then:
        q.commit_triangle_intersection();
    }
}
if (q.get_committed_intersection_type() != intersection_type::none) {
    float t = q.get_committed_distance();
}
```

### Custom intersection functions

```metal
[[intersection(bounding_box, triangle_data)]]
bool sphere_intersect(float3 origin      [[origin]],
                      float3 direction   [[direction]],
                      float  min_distance[[min_distance]],
                      float  max_distance[[max_distance]],
                      unsigned int prim  [[primitive_id]],
                      ray_data Payload &p[[payload]]) {
    // return true and set outputs to accept; see [[accept_intersection]]/[[continue_search]]
    return true;
}
```

---

## Mesh shaders (Metal 3+, `<metal_mesh>`)

Two-stage geometry pipeline: an **object** stage dispatches a grid of **mesh** stage
threadgroups.

### Object stage

```metal
struct Payload { float4x4 mvp; uint lodLevel; };

[[object, max_total_threads_per_threadgroup(1)]]
void object_main(object_data Payload &payload   [[payload]],
                 mesh_grid_properties mgp,
                 uint tid [[thread_position_in_grid]]) {
    payload.mvp = /* ... */;
    mgp.set_threadgroups_per_grid(uint3(meshletCount, 1, 1));  // launch mesh grid
}
```

### Mesh stage — `mesh<V, P, NV, NP, topology>`

`V` = per-vertex struct, `P` = per-primitive struct, `NV`/`NP` = max vertices/primitives,
`topology` ∈ `topology::triangle | line | point`.

```metal
struct VOut { float4 position [[position]]; float3 normal; };
struct POut { float3 color [[flat]]; };
using Mesh = mesh<VOut, POut, 64, 126, topology::triangle>;

[[mesh, max_total_threads_per_threadgroup(64)]]
void mesh_main(Mesh out,
               object_data Payload &payload [[payload]],
               uint tid [[thread_position_in_threadgroup]]) {
    if (tid == 0) out.set_primitive_count(meshletPrimCount);
    out.set_vertex(tid, makeVertex(tid, payload));
    out.set_index(tid * 3 + 0, i0);   // per index
    out.set_primitive(tid, makePrimitive(tid));   // per-primitive data
}
```

Methods: `set_primitive_count(uint)`, `set_vertex(i, v)`, `set_index(i, idx)`,
`set_primitive(i, p)`.

---

## Tensors & Metal Performance Primitives (Metal 4+, chapter 7)

Preferred over SIMD-group matrices for general matmul/convolution — portable across extents.

### Tensor types (section 2.22)

```metal
// dextents<T, N> / extents<...> describe rank & sizes
tensor<T, extents_type, addr_space>       // e.g. tensor<half, dextents<int,2>, device>
```

Tensors expose `.rank()`, `.extents()`, element access, slicing, and blockwise loads. A
`cooperative_tensor` (with a `layout`) is held across an execution scope for MPP ops.

### Matrix multiply / convolution (section 7)

```metal
#include <MetalPerformancePrimitives/MetalPerformancePrimitives.h>
using namespace mpp;
// tensor_ops::matmul2d over cooperative tensors within a threadgroup/SIMD execution scope;
// supports convolution as well. Configure with descriptors (dimensions, data types).
```

Use MPP `tensor_ops` for high-performance GEMM/conv rather than hand-rolled loops.

---

## Imageblocks (tile memory)

Structured per-fragment threadgroup storage for tile-based shading (programmable blending,
deferred/OIT, custom MSAA resolve).

```metal
struct GBuffer {
    half4 albedo;
    half4 normal;
    float depth;
};
// Explicit-layout imageblock in a fragment/tile function:
fragment void gbuffer_write(VertexOut in [[stage_in]],
                            imageblock<GBuffer, imageblock_layout_explicit> ib) {
    threadgroup_imageblock GBuffer *px = ib.data(in.position.xy);  // per-fragment slot
    px->albedo = ...;
}

// Tile function resolving imageblock to a texture:
kernel void resolve(imageblock<GBuffer> ib,
                    texture2d<half, access::write> dst [[texture(0)]],
                    ushort2 tilePos [[thread_position_in_threadgroup]]) {
    GBuffer g = ib.read(tilePos);
    dst.write(g.albedo, /* ... */);
}
```

Attributes: `[[imageblock_data]]`, `[[alias_implicit_imageblock]]`; layouts
`imageblock_layout_implicit` / `imageblock_layout_explicit`. Related tile attributes:
`[[pixel_position_in_tile]]`, `[[pixels_per_tile]]`, `[[tile_index]]`.

---

## SIMD-group matrices (`<metal_simdgroup_matrix>`)

8×8 tiles cooperatively stored across a SIMD-group (`simdgroup_float8x8` etc., Metal 3.1+ for
half/bfloat). Prefer Tensors/MPP for new code.

```metal
simdgroup_matrix<T,Cols,Rows> make_filled_simdgroup_matrix(T value);
void simdgroup_load (thread simdgroup_matrix<T,C,R>& d, const device T *src,
                     ulong elements_per_row = C, ulong2 origin = 0, bool transpose = false);
void simdgroup_store(thread simdgroup_matrix<T,C,R>  a,       device T *dst,
                     ulong elements_per_row = C, ulong2 origin = 0, bool transpose = false);
void simdgroup_multiply(thread simdgroup_matrix<T,C,R>& d,
                        thread simdgroup_matrix<T,K,R>& a, thread simdgroup_matrix<T,C,K>& b);
void simdgroup_multiply_accumulate(thread simdgroup_matrix<T,C,R>& d,   // d = a*b + c
                        thread simdgroup_matrix<T,K,R>& a, thread simdgroup_matrix<T,C,K>& b,
                        thread simdgroup_matrix<T,C,R>& c);
```

```metal
kernel void matmad(device float *A, device float *B, device float *C, device float *R) {
    simdgroup_float8x8 a, b, c, r;
    simdgroup_load(a, A); simdgroup_load(b, B); simdgroup_load(c, C);
    simdgroup_multiply_accumulate(r, a, b, c);
    simdgroup_store(r, R);
}
```

---

## Indirect command buffers (section 6.17)

GPU-encoded draw/dispatch commands. In a kernel, receive an
`command_buffer` / `render_command`/`compute_command` and encode:

```metal
kernel void encode(command_buffer cmds [[buffer(0)]],
                   device Draw *draws  [[buffer(1)]],
                   uint i [[thread_position_in_grid]]) {
    render_command cmd(cmds, i);
    cmd.set_vertex_buffer(draws[i].vbuf, 0);
    cmd.draw_primitives(primitive_type::triangle, 0, draws[i].count, 1, 0);
}
```

`compute_command` supports `set_kernel_buffer`, `concurrent_dispatch_threadgroups`, etc.

---

## Logging (Metal 3.2+, `<metal_logging>`)

```metal
os_log_default.log_info("gid=%u value=%f", gid, value);
// levels: log_debug / log_info / log_default / log_error / log_fault
```

Requires enabling logging at compile/pipeline creation; output appears in the host log stream.

## See also

- [functions-and-attributes.md](functions-and-attributes.md) — object/mesh/intersection/tile attributes
- [data-types.md](data-types.md) — tensor/atomic/simdgroup type declarations
- [examples.md](examples.md) — end-to-end shaders
