# MSL Address Spaces & Operators

Spec chapters 3 (Operators) and 4 (Address Spaces).

## Address spaces

Every pointer and reference argument in MSL **must** name an address space. Local variables
default to `thread`. The `-Wmissing-metal-address-space` warning flags omissions.

| Space | Access | Where it lives / used for |
|-------|--------|---------------------------|
| `device` | read/write | Main GPU buffer/texture memory. Buffers, UAVs, atomics. |
| `constant` | read-only | Uniform data shared by all threads; must be initialized at program scope. Faster for broadcast reads. |
| `thread` | read/write | Per-thread private memory (registers/stack). Default for locals. |
| `threadgroup` | read/write | Shared by all threads in a threadgroup. Needs barriers for correctness. |
| `threadgroup_imageblock` | read/write | Imageblock storage in threadgroup memory (tile shading). |
| `ray_data` | read/write | Payload passed to intersection functions (Metal 2.3+). |
| `object_data` | read/write | Object-shader → mesh-shader payload (Metal 3+). |

```metal
kernel void k(device float        *g   [[buffer(0)]],   // device
              constant Params     &p   [[buffer(1)]],   // constant (by ref)
              threadgroup float   *tg  [[threadgroup(0)]]) {
    thread float local = g[0];    // 'thread' is implicit here
    threadgroup float tile[256];  // threadgroup array declared in-shader
}
```

**Program-scope constants** must be initialized and are immutable:

```metal
constant float kWeights[] = { 0.25f, 0.5f, 0.25f };
constant float3 kUp       = float3(0, 1, 0);
```

**Storage class specifiers** at program scope: `static` (internal linkage) and `extern`.
`static constant` tables and `extern constant` declarations are permitted.

### SIMD-groups and quad-groups

A threadgroup is partitioned into SIMD-groups (execution width = `threads_per_simdgroup`,
often 32) and quad-groups (width 4). SIMD/quad functions exchange data within a group without
threadgroup memory or barriers — see [standard-library.md](standard-library.md).

### Memory coherency (Metal 3.2+)

`coherent` marks `device` memory whose accesses are ordered/visible across threadgroups:

```metal
coherent device float  *a;
coherent(device) device float4 *b;   // explicit scope form
```

Ordering strength increases: `thread` < `threadgroup` < `device`. Use barriers/fences and
atomics with the right `memory_order`/`thread_scope` (see [atomics.md](atomics.md)) to make
cross-thread writes visible.

## Operators

### Scalar and vector operators

All operate **component-wise** on vectors (operands must have equal length, or one is a
scalar that broadcasts):

- Arithmetic: `+  -  *  /  %` (`%` integer only), unary `+ - ++ --`.
- Relational: `<  >  <=  >=  ==  !=` → `bool` (scalar) or `boolN` (vector). Reduce a
  `boolN` with `all(v)` / `any(v)`.
- Logical: `&&  ||  !` (scalar operands; for vectors use bitwise + `all/any`).
- Bitwise (integers): `&  |  ^  ~  <<  >>`.
- Assignment: `=  +=  -=  *=  /=  %=  &=  |=  ^=  <<=  >>=`.
- Ternary `?:`; comma `,`; `sizeof`, `alignof`.
- Address/deref `&`/`*` (subject to address-space rules), member `.`/`->`, subscript `[]`.

```metal
float4 a = float4(1,2,3,4), b = float4(4,3,2,1);
float4 s = a + b;          // component-wise
bool4  m = a < b;          // (true,true,false,false)
if (any(m)) { /* ... */ }
float4 t = m ? a : b;      // vector select via ?: is NOT allowed; use select()/mix
```

Vector `?:` requires a scalar condition; for per-lane selection use `select(a, b, cond)`
(see relational functions in [standard-library.md](standard-library.md)).

### Matrix operators

- `m * m` (matrix product), `m * v` and `v * m` (matrix-vector), `m * scalar`,
  `m + m`, `m - m` (component-wise), unary `-`.
- Subscript `m[col]` yields a column vector; `m[col][row]` a scalar (column-major).
- No `/` between matrices; invert via your own routine or math on components.

## See also

- [data-types.md](data-types.md) — the types these spaces hold
- [atomics.md](atomics.md) — memory_order, thread_scope, fences, barriers
- [functions-and-attributes.md](functions-and-attributes.md) — where address spaces appear in
  function signatures
