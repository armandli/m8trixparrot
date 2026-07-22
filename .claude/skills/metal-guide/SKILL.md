---
name: metal-guide
description: Reference guide for Apple Metal Shading Language (MSL) programming. Use this skill whenever the user asks about Metal shader code, MSL syntax, Metal GPU programming, writing vertex/fragment/kernel functions, Metal data types, address spaces, texture sampling, SIMD-group operations, atomics, ray tracing, mesh shaders, tensors, or any Apple Metal GPU shader development. Always use this skill when the user mentions "Metal shader", "MSL", "Apple GPU", ".metal files", or asks how to write GPU code for Apple platforms.
version: 2.0.0
---

# Apple Metal Shading Language (MSL) Reference Guide

Authoritative reference distilled from the **Metal Shading Language Specification v4.1**
(Apple, June 2026). Use it when writing, reviewing, or explaining `.metal` shader code.

MSL is a **C++17-based** language (C++14 for Metal 3 and earlier) with GPU extensions and
restrictions, compiled by a clang/LLVM-based compiler. Source files use the `.metal`
extension. When a feature has a version gate, this guide notes it as *(Metal X.Y+)*.

## How to use this guide

`SKILL.md` (this file) is the fast index: language basics, the most common types, function
qualifiers, and canonical examples. For anything beyond the basics, **open the matching
reference file** — do not guess signatures or attribute names, look them up:

| Topic | Reference |
|-------|-----------|
| All data types (scalar/vector/matrix/packed/atomic/pixel/buffer/texture/sampler/aggregate/argument buffers/alignment) | [references/data-types.md](references/data-types.md) |
| Address spaces, operators, memory coherency | [references/address-spaces-and-operators.md](references/address-spaces-and-operators.md) |
| Function qualifiers + **every** `[[attribute]]` (vertex/fragment/kernel/mesh/object/intersection/tile I/O), resource binding, function constants, interpolation | [references/functions-and-attributes.md](references/functions-and-attributes.md) |
| Standard library: common/integer/relational/math/matrix/geometric functions, constants, barriers, SIMD-group & quad-group ops, pack/unpack | [references/standard-library.md](references/standard-library.md) |
| Samplers + all texture types and their member functions (sample/read/write/gather/query) | [references/textures-and-samplers.md](references/textures-and-samplers.md) |
| Atomics: memory model, memory_order, thread_scope, fences, all atomic functions | [references/atomics.md](references/atomics.md) |
| Ray tracing, mesh/object shaders, tensors + Metal Performance Primitives, imageblocks, indirect command buffers, SIMD-group matrices, logging | [references/advanced-features.md](references/advanced-features.md) |
| Complete, compilable worked examples | [references/examples.md](references/examples.md) |

## Language basics

Every shader file starts with the standard library and (usually) the `metal` namespace:

```metal
#include <metal_stdlib>
using namespace metal;
```

Individual headers exist (`<metal_math>`, `<metal_integer>`, `<metal_atomic>`,
`<metal_texture>`, `<metal_simdgroup>`, …) but `<metal_stdlib>` pulls in everything.

**Predefined preprocessor macros:**

```metal
__METAL_VERSION__   // integer, e.g. 40100 for Metal 4.1 (Major*10000 + Minor*100 + Patch)
__METAL__           // always defined when compiling MSL
__METAL_MACOS__     // defined when targeting macOS
__METAL_IOS__       // defined when targeting iOS
```

**Three function qualifiers** mark GPU entry points (`kernel`/`[[kernel]]` are equivalent):

| Qualifier | Purpose | Return |
|-----------|---------|--------|
| `vertex` | per-vertex stage | struct with `float4 [[position]]`, or `void` |
| `fragment` | per-fragment stage | color(s), or `void` |
| `kernel` | compute / data-parallel | must be `void` |

Also: `[[mesh]]`, `[[object]]` (mesh pipeline), `[[intersection(...)]]` (ray tracing),
`[[visible]]`, `[[stitchable]]`, tile functions. See the functions reference.

## Canonical examples

### Vertex + fragment (textured, lit)

```metal
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 uv       [[attribute(2)]];
};

struct VertexOut {
    float4 position [[position]];   // required clip-space output
    float3 normal;
    float2 uv;
};

struct Uniforms { float4x4 mvp; float4x4 normalMatrix; };

vertex VertexOut vertex_main(VertexIn in            [[stage_in]],
                             constant Uniforms &u   [[buffer(1)]])
{
    VertexOut out;
    out.position = u.mvp * float4(in.position, 1.0f);
    out.normal   = (u.normalMatrix * float4(in.normal, 0.0f)).xyz;
    out.uv       = in.uv;
    return out;
}

fragment float4 fragment_main(VertexOut in          [[stage_in]],
                              texture2d<float> albedo [[texture(0)]],
                              sampler s               [[sampler(0)]])
{
    float4 base   = albedo.sample(s, in.uv);
    float3 N      = normalize(in.normal);
    float3 L      = normalize(float3(0.5f, 1.0f, 0.5f));
    float  diff   = max(dot(N, L), 0.0f);
    return float4(base.rgb * diff, base.a);
}
```

### Compute kernel

```metal
#include <metal_stdlib>
using namespace metal;

kernel void saxpy(device const float *x [[buffer(0)]],
                  device       float *y [[buffer(1)]],
                  constant     float &a [[buffer(2)]],
                  constant     uint  &n [[buffer(3)]],
                  uint gid [[thread_position_in_grid]])
{
    if (gid >= n) return;                 // always guard the grid bounds
    y[gid] = a * x[gid] + y[gid];
}
```

## Quick type reference

**Scalars:** `bool char uchar short ushort int uint long ulong half bfloat float`
(`half`=fp16, `bfloat`=brain-float *Metal 3.1+*, `long`/`ulong` *Metal 2.2+*).
**Not supported:** `double`, `long long`, `long double`. Literals: `1.0f`/`1.0h`/`1.0bf`.

**Vectors:** `<type><2|3|4>`, e.g. `float4`, `int3`, `half2`; generic `vec<T,N>`.
Swizzle with `.xyzw`/`.rgba` (read allows duplicates: `v.xxy`; write must be unique:
`v.xy = ...`). `packed_floatN` etc. are 1-byte-aligned for tight buffer packing.

**Matrices:** `floatCxR` / `halfCxR` (C columns, R rows), column-major; `m[col][row]`.

See [references/data-types.md](references/data-types.md) for sizes, alignment, atomics,
pixel types, buffers, textures, samplers, arrays/structs of resources, and argument buffers.

## Address spaces (must qualify pointers/references)

`device` (RW GPU memory) · `constant` (read-only, must be initialized at program scope) ·
`thread` (per-thread, default for locals) · `threadgroup` (shared across a threadgroup) ·
`threadgroup_imageblock` · `ray_data` *(2.3+)* · `object_data` *(Metal 3+)*.
Details and memory coherency: [references/address-spaces-and-operators.md](references/address-spaces-and-operators.md).

## C++ features NOT supported in MSL

No `double`; no RTTI (`dynamic_cast`, `typeid`); no exceptions/`noexcept`; no `goto`;
no `virtual` functions or `new`/`delete` (placement `new` is allowed *Metal 4.1+*);
no `thread_local`/`register`; no C++ standard library (`<vector>`, `<string>`, …);
no function pointers to shader entry points; recursion is not allowed.

## Working conventions

- Match the surrounding file's style. Default to `<metal_stdlib>` + `using namespace metal;`.
- Always bounds-check compute kernels against the grid/buffer size before writing.
- Put the address-space qualifier on every `device`/`constant`/`threadgroup` pointer and
  reference argument; omitting it is an error (`-Wmissing-metal-address-space` warns).
- Give each buffer/texture/sampler argument an explicit `[[buffer(n)]]`/`[[texture(n)]]`/
  `[[sampler(n)]]` index; these must match the host-side binding.
- Prefer `half`/`bfloat` for bandwidth-bound math when precision allows.
- Use `simd_*` / `quad_*` reductions and shuffles before falling back to threadgroup memory.
- When unsure of a signature, attribute spelling, or version gate, **read the relevant
  reference file above** rather than guessing.

### Final Step — Record Usage

After answering a Metal/MSL question or producing shader code with this skill, run:

```bash
python3 ${PWD}/.claude/skills/skill-stat/scripts/record-stat.py "metal-guide"
```
