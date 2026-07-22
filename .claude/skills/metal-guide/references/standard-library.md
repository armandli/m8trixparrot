# MSL Standard Library Functions

Spec chapter 6. All functions are in the `metal` namespace. Below, `T` is a scalar or vector
of the noted category; vector forms operate component-wise. `Ti` = integer, `Tu` = unsigned.

Precise vs fast variants: for single-precision, many math/common/geometric functions have
`metal::precise::` and `metal::fast::` variants (fast leaves NaN behavior undefined). The
`-ffast-math` flag picks the default; the namespaces force a choice: `fast::sin(x)`,
`precise::cos(x)`.

## Common functions — `<metal_common>` (T = half/float scalar or vector)

| Function | Result |
|----------|--------|
| `clamp(x, lo, hi)` | `fmin(fmax(x, lo), hi)` (undefined if lo>hi) |
| `mix(x, y, a)` | linear blend `x + (y-x)*a`, a∈[0,1] |
| `saturate(x)` | clamp to [0,1] |
| `sign(x)` | 1 if x>0, -1 if x<0, ±0 for ±0, 0 for NaN |
| `smoothstep(e0, e1, x)` | Hermite 0→1 between edges |
| `step(edge, x)` | 0 if x<edge else 1 |

## Integer functions — `<metal_integer>` (T = integer scalar/vector)

`abs` `absdiff(x,y)` `addsat(x,y)` `subsat` `clamp` `clz` (leading zeros) `ctz` (trailing
zeros) `extract_bits(x,off,bits)` `insert_bits(base,ins,off,bits)` `hadd((x+y)>>1)`
`rhadd((x+y+1)>>1)` `mad24` `madhi` `madsat` `mul24` `mulhi` `max` `min` `max3` `min3`
`median3` `popcount` `reverse_bits` `rotate(v,i)` (Metal 2.1+ for many).
`interleave`/`deinterleave` (Metal 4.1+) pack/unpack even/odd bits between `uchar↔ushort`,
`ushort↔uint`, `uint↔ulong`.

## Relational functions — `<metal_relational>`

`all(bV)` `any(bV)` `isfinite` `isinf` `isnan` `isnormal` `isordered(x,y)` `isunordered(x,y)`
`not(bV)` `signbit(x)` `select(a, b, c)` (per-lane `c ? b : a`).

## Math functions — `<metal_math>` (T = half/float scalar or vector)

Trig: `sin cos tan asin acos atan atan2(y,x) sinh cosh tanh asinh acosh atanh`
`sinpi cospi tanpi` (compute `fn(πx)`).
Exp/log: `exp exp2 exp10 log log2 log10 pow(x,y) powr(x,y) sqrt rsqrt`.
Rounding: `floor ceil round rint trunc fract fmod(x,y) fdim(x,y)`.
Misc: `fabs`/`abs` `fmax`/`max` `fmin`/`min` `fmax3 fmin3 fmedian3` `copysign(x,y)`
`fma(a,b,c)` `divide(x,y)` `ldexp(x,k)` `frexp(x,&e)` `ilogb` `modf(x,&i)` `sincos(x,&c)`
`nextafter(x,y)` (Metal 3.1+).

**Constants** (float suffix `_F`, half `_H`, bfloat `_BF`):
`M_PI_F M_PI_2_F M_PI_4_F M_1_PI_F M_2_PI_F M_2_SQRTPI_F M_E_F M_LOG2E_F M_LOG10E_F M_LN2_F
M_LN10_F M_SQRT2_F M_SQRT1_2_F` and `MAXFLOAT HUGE_VALF INFINITY NAN` (`MAXHALF/HUGE_VALH`,
`MAXBFLOAT/HUGE_VALBF`).

## Matrix functions — `<metal_matrix>` (T = float/half)

`determinant(mNxN)` (square only) · `transpose(mNxM) -> mMxN`.

## Geometric functions — `<metal_geometric>` (T = floatN/halfN, Ts = its scalar)

`cross(x,y)` (3-comp only) · `dot(x,y)` · `length(x)` · `length_squared(x)` ·
`distance(x,y)` · `distance_squared(x,y)` · `normalize(x)` · `reflect(I,N)` ·
`refract(I,N,eta)` · `faceforward(N,I,Nref)`. Precise/fast variants for
`distance`/`length`/`normalize`.

## Synchronization — `<metal_compute>`

Usable in `kernel`, `fragment`, `mesh`, `object`, and `[[visible]]` functions.

```metal
void threadgroup_barrier(mem_flags flags);
void simdgroup_barrier(mem_flags flags);
// Metal 4.1+ overloads add memory_order + thread_scope:
void threadgroup_barrier(mem_flags, memory_order, thread_scope);
```

`mem_flags` (bitwise-OR combinable): `mem_none` (execution only), `mem_device`,
`mem_threadgroup`, `mem_texture`, `mem_threadgroup_imageblock`, `mem_object_data`.

**Rule:** every thread in the group must reach the barrier. If a barrier is inside a
conditional or loop, all threads in the group must take the same path to it.

```metal
threadgroup_barrier(mem_flags::mem_threadgroup);              // typical shared-memory sync
threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
```

## SIMD-group functions — `<metal_simdgroup>`

Exchange/reduce across a SIMD-group with no shared memory or barrier. `T` = scalar/vector
integer or float (not bool/bfloat/long/ulong). Bitwise ops need integer `Ti`.

**Broadcast / query:** `simd_broadcast(data, laneId)` · `simd_broadcast_first(data)` ·
`simd_is_first()` · `simd_is_helper_thread()` (fragment only).

**Shuffle:** `simd_shuffle(data, laneId)` · `simd_shuffle_up(data, delta)` /
`simd_shuffle_down(data, delta)` (no wrap) · `simd_shuffle_rotate_up/down` (wrap) ·
`simd_shuffle_xor(data, mask)` · `simd_shuffle_and_fill_up/down(data, fill, delta[, modulo])`
(Metal 2.4+).

**Reductions (broadcast result to all lanes):** `simd_sum` `simd_product` `simd_min`
`simd_max` `simd_and` `simd_or` `simd_xor` (bitwise → integer only).

**Prefix scans:** `simd_prefix_inclusive_sum` `simd_prefix_exclusive_sum` (first lane→T(0))
`simd_prefix_inclusive_product` `simd_prefix_exclusive_product` (first lane→T(1)).

**Voting:** `simd_all(expr)` · `simd_any(expr)` · `simd_ballot(expr)` →`simd_vote` ·
`simd_active_threads_mask()`.

> `simd_all(expr)` (all *active* lanes true) ≠ `simd_ballot(expr).all()` (checks the whole
> mask including inactive lanes). `simd_vote` casts to `vote_t` (`uint64_t`).

## Quad-group functions

SIMD-group functions with execution width 4 — same names prefixed `quad_`:
`quad_broadcast(data, laneId)` `quad_broadcast_first` `quad_shuffle` `quad_shuffle_up/down`
`quad_shuffle_xor` `quad_sum/product/min/max/and/or/xor` `quad_prefix_*`
`quad_all/any/ballot`, `quad_vote`. Fragment helper threads compute gradients then go
inactive.

## Pack / unpack functions — `<metal_pack>`

Convert between packed normalized/float formats and float vectors:
`unpack_unorm2x16_to_float`, `unpack_snorm4x8_to_half`, `pack_float_to_unorm4x8`,
`pack_half_to_snorm2x16`, etc. Naming pattern:
`unpack_{u,s}norm{2x16,4x8}_to_{float,half}` and `pack_{float,half}_to_{u,s}norm{...}`.

## Common-graphics (fragment-only) functions

`dfdx(p)` `dfdy(p)` (screen-space partial derivatives) · `fwidth(p)` = `abs(dfdx)+abs(dfdy)` ·
`get_num_samples()` · `get_sample_position(i)` · `discard_fragment()`. Derivatives are
undefined under non-uniform control flow.

## See also

- [textures-and-samplers.md](textures-and-samplers.md) — texture member functions
- [atomics.md](atomics.md) — atomic functions & memory model
- [advanced-features.md](advanced-features.md) — SIMD-group matrix, tensor, ray-tracing funcs
