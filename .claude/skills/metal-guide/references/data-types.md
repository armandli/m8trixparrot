# MSL Data Types

Complete type reference for Metal Shading Language 4.1 (spec chapter 2). Types live in the
`metal` namespace (pulled in by `<metal_stdlib>` + `using namespace metal;`).

## Scalar types

| Type | Alias | Size | Notes |
|------|-------|------|-------|
| `bool` | | 1 B | `true`/`false` |
| `char` | `int8_t` | 1 B | signed |
| `uchar` | `uint8_t` | 1 B | |
| `short` | `int16_t` | 2 B | |
| `ushort` | `uint16_t` | 2 B | |
| `int` | `int32_t` | 4 B | |
| `uint` | `uint32_t` | 4 B | |
| `long` | `int64_t` | 8 B | Metal 2.2+ |
| `ulong` | `uint64_t` | 8 B | Metal 2.2+ |
| `half` | | 2 B | IEEE 754 binary16 |
| `bfloat` | | 2 B | brain float (8-bit exponent, 7-bit mantissa), Metal 3.1+ |
| `float` | | 4 B | IEEE 754 binary32 |
| `size_t` / `ptrdiff_t` | | 8 B | |
| `void` | | | |

**Not supported:** `double`, `long long`, `unsigned long long`, `long double`.

**Literals:** `1.0f` (float), `1.0h` (half), `1.0bf` (bfloat). Integer suffixes `u`, `l`, `ul`.

## Vector types

Form `<scalar><N>` where N ∈ {2, 3, 4}: `float2 float3 float4`, `half2..4`, `int2..4`,
`uint2..4`, `short/ushort/char/uchar/long/ulong/bool 2..4`, `bfloat2..4` (Metal 3.1+).
Generic template form: `vec<T, N>`.

```metal
float4 p = float4(1, 2, 3, 4);
float4 s = float4(1.0f);              // splat: all lanes = 1.0
float4 c = float4(float2(1,2), 3, 4); // mix scalars and smaller vectors
```

**Component access & swizzling** — `.xyzw` or `.rgba` (never mix the two in one access);
also `[i]`:

```metal
float x  = p.x;      // == p[0] == p.r
float3 v = p.xyz;    // swizzle read
float4 r = p.wzyx;   // reverse
float4 d = p.xxyy;   // duplicates allowed on the read side
p.xw     = float2(5, 6);   // swizzle write: components must be unique on the write side
```

- `bool` vector comparisons: relational operators produce a `boolN`; use `all()`/`any()`.
- Size/alignment (Table 2.3): a `floatN` aligns to the next power-of-two ≥ its size, so
  `float3` occupies **16 B** (aligned like `float4`), `float2` 8 B, `half3` 8 B, etc.

### Packed vector types

1-element alignment for tightly packed buffer data. Read/write, arithmetic works, but they
are meant for storage — convert to aligned vectors for heavy math.

```metal
packed_float2/3/4  packed_half2/3/4  packed_int2/3/4  packed_uint2/3/4
packed_short/ushort/char/uchar 2/3/4  packed_long/ulong 2/3/4
```

`packed_float3` is exactly 12 B (vs 16 B for `float3`). Conversions are implicit both ways.

## Matrix types

`floatCxR` and `halfCxR` where C = columns, R = rows, each ∈ {2,3,4}: `float2x2 … float4x4`,
`half2x2 … half4x4`. Generic form `matrix<T, C, R>`. **Column-major** storage.

```metal
float4x4 m;
m[1];        // second column, a float4
m[0][0];     // column 0, row 0
float4x4 I = float4x4(1.0f);   // scalar ctor -> diagonal (identity)
float4x4 c = a * b;            // matrix * matrix
float4   v = m * float4(...);  // matrix * column-vector
```

Standard library: `determinant(mNxN)` (square only), `transpose(mNxM)` — see
[standard-library.md](standard-library.md).

## SIMD-group matrix types

8×8 tiles cooperatively held across a SIMD-group, for tensor/matmul acceleration.
For new code prefer **Tensors + Metal Performance Primitives** (see
[advanced-features.md](advanced-features.md)); these remain for compatibility.

```metal
simdgroup_float8x8   simdgroup_half8x8   simdgroup_bfloat8x8   // Metal 3.1+
// generic: simdgroup_matrix<T, Cols, Rows>
```

Operations (`<metal_simdgroup_matrix>`): `make_filled_simdgroup_matrix`, `simdgroup_load`,
`simdgroup_store`, `simdgroup_multiply`, `simdgroup_multiply_accumulate`. Full signatures in
[advanced-features.md](advanced-features.md).

## Atomic types

Lock-free atomics operated on with C11-style functions (see [atomics.md](atomics.md)):

```metal
atomic_int    atomic_uint            // always
atomic_bool   atomic_ulong           // Metal 2.4+
atomic_float                         // Metal 3+
atomic<T>                            // generic form
```

Atomic objects must live in `device` or `threadgroup` memory; access only via the
`atomic_*_explicit` functions, never by direct load/store.

## Pixel data types

Templated storage types that read/write a compact pixel format while exposing a wider
in-register type (e.g. store as 8-bit unorm, compute in `half4`):

```metal
r8unorm<T>   rg8unorm<T>   rgba8unorm<T>   srgba8unorm<T>
r8snorm<T>   rg8snorm<T>   rgba8snorm<T>
r16unorm<T>  rg16unorm<T>  rgba16unorm<T>   r16snorm<T> rg16snorm<T> rgba16snorm<T>
rgb10a2<T>   rg11b10f<T>   rgb9e5<T>
```

```metal
kernel void k(device rgba8unorm<half4> *p [[buffer(0)]],
              uint gid [[thread_position_in_grid]]) {
    half4 v = p[gid];        // decoded to half4
    p[gid]  = saturate(v);   // encoded back to rgba8
}
```

## Buffers

A buffer is a pointer or reference in the `device` or `constant` address space:

```metal
device float4 *out       // read/write array in device memory
constant Uniforms &u     // read-only struct/UBO
const device Vertex *v   // read-only device array
```

Argument buffers, arrays, and structs of buffers/textures/samplers are aggregate types
(below).

## Textures

`textureTYPE<T, access a = access::sample>` where `T` ∈ `float, half, int, uint, short,
ushort` (and `ulong` for some, Metal 3.1+). See
[textures-and-samplers.md](textures-and-samplers.md) for member functions.

```metal
texture1d<T,a>        texture1d_array<T,a>
texture2d<T,a>        texture2d_array<T,a>
texture2d_ms<T,a>     texture2d_ms_array<T,a>       // multisampled
texture3d<T,a>        texturecube<T,a>   texturecube_array<T,a>
texture_buffer<T,a>                                  // 1D linear, no filtering
depth2d<T,a>          depth2d_array<T,a>             // T is float; sample/read only
depthcube<T,a>        depthcube_array<T,a>   depth2d_ms<T,a>
```

**Access modes:** `access::sample` (default; sampled reads + mips) · `access::read`
(sampler-less reads) · `access::write` · `access::read_write` (restricted formats).

## Samplers

See [textures-and-samplers.md](textures-and-samplers.md) for the full option table.

```metal
constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::linear);
// or bound from host:  sampler s [[sampler(0)]]
```

## Aggregate types

### Arrays of textures/texture-buffers/samplers — `metal::array<T, N>`

```metal
kernel void k(const array<texture2d<float>, 8> tex [[texture(0)]],
              const array<sampler, 2>          smp [[sampler(0)]]) {
    float4 c = tex[0].sample(smp[0], float2(0.5f));   // .size(), [i] access
}
```

### Structures of buffers/textures/samplers

Plain structs may contain resources when passed by reference; useful for grouping bindings.

## Argument buffers (bindless)

A struct of resources placed in `device`/`constant` memory, each member tagged with
`[[id(n)]]`. Enables large, indirectly indexed resource tables.

```metal
struct Material {
    texture2d<float>              albedo    [[id(0)]];
    texture2d<float>              normalMap [[id(1)]];
    sampler                       smp       [[id(2)]];
    device float4                *params    [[id(3)]];
};

fragment float4 f(VertexOut in [[stage_in]],
                  constant Material &m [[buffer(0)]]) {
    return m.albedo.sample(m.smp, in.uv);
}
```

Tier-2 hardware allows arrays of argument buffers and dynamic indexing of resources within
them.

## Alignment

- A type's alignment equals the alignment of its largest member (scalars: their size;
  vectors: rounded up to the next power of two of the total size).
- `float3`/`int3` etc. are aligned and sized as 16 B; use `packed_float3` for 12 B storage.
- Host (Swift/C++) struct layouts must match; keep field order and add explicit padding to
  agree with these rules. Use `alignas(N)` where needed.

## Type conversion & reinterpretation

- Implicit conversions follow C++ usual arithmetic conversions (narrowing to `half` is
  allowed). Vector conversions are component-wise; no implicit scalar↔vector.
- `static_cast<T>(x)` / functional casts convert values.
- `as_type<DstT>(x)` reinterprets the bit pattern (must be same size), e.g.
  `as_type<uint>(1.0f)`, `as_type<float2>(someUint2)`.

## See also

- [address-spaces-and-operators.md](address-spaces-and-operators.md) — where these types live
- [textures-and-samplers.md](textures-and-samplers.md) — texture/sampler member functions
- [atomics.md](atomics.md) — atomic operations and memory model
- [advanced-features.md](advanced-features.md) — ray-tracing, mesh, tensor, imageblock types
