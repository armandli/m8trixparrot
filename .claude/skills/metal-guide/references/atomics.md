# MSL Atomics & Memory Model

Spec section 6.16 and `<metal_atomic>`. Atomic objects must live in `device` or
`threadgroup` memory and be accessed **only** through the atomic functions — never a plain
load/store.

## Atomic types

`atomic_int` `atomic_uint` (always) · `atomic_bool` `atomic_ulong` (Metal 2.4+) ·
`atomic_float` (Metal 3+) · generic `atomic<T>`.

```metal
kernel void k(device atomic_uint *counter [[buffer(0)]]) {
    atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
}
```

## Memory order (section 6.16.1)

`memory_order` controls how surrounding non-atomic memory operations are ordered relative to
the atomic:

| Value | Meaning |
|-------|---------|
| `memory_order_relaxed` | atomicity only, no ordering guarantees (most common on GPU) |
| `memory_order_acquire` | later reads/writes cannot move before this load |
| `memory_order_release` | earlier reads/writes cannot move after this store |
| `memory_order_acq_rel` | both, for read-modify-write ops |
| `memory_order_seq_cst` | single total order across all threads (strongest, default for barriers) |

Acquire/release/acq_rel/seq_cst on atomics require **Metal 4.1+**; earlier versions support
only `memory_order_relaxed`.

## Thread scope (section 6.16.2)

`thread_scope` selects which threads observe the operation's ordering:
`thread_scope_thread` < `thread_scope_simdgroup` < `thread_scope_threadgroup` <
`thread_scope_device`. Passed to fences and the Metal 4.1+ atomic/barrier overloads.

## Fences

```metal
atomic_thread_fence(mem_flags flags,
                    memory_order order,
                    thread_scope scope = thread_scope_device);
```

Orders memory without an associated atomic object; `flags` uses the `mem_flags` set from
[standard-library.md](standard-library.md) (`mem_device`, `mem_threadgroup`, …).

## Atomic functions

All take a pointer to the atomic object and an explicit `memory_order` (and, Metal 4.1+, an
optional `thread_scope`). `A` is `device` or `threadgroup` qualified.

```metal
// Store / load
void  atomic_store_explicit (A atomic<T>*, T val,  memory_order);
T     atomic_load_explicit  (A atomic<T>*,          memory_order);

// Exchange
T     atomic_exchange_explicit(A atomic<T>*, T desired, memory_order);

// Compare-and-swap (expected updated on failure). weak may fail spuriously; strong won't.
bool  atomic_compare_exchange_weak_explicit  (A atomic<T>*, thread T *expected, T desired,
                                              memory_order success, memory_order failure);
bool  atomic_compare_exchange_strong_explicit(A atomic<T>*, thread T *expected, T desired,
                                              memory_order success, memory_order failure);

// Fetch-and-modify (return the PREVIOUS value)
T     atomic_fetch_add_explicit(A atomic<T>*, T, memory_order);   // sub, and, or, xor,
T     atomic_fetch_sub_explicit(A atomic<T>*, T, memory_order);   // min, max analogous:
T     atomic_fetch_min_explicit(A atomic<T>*, T, memory_order);
T     atomic_fetch_max_explicit(A atomic<T>*, T, memory_order);
T     atomic_fetch_and_explicit(A atomic<T>*, T, memory_order);
T     atomic_fetch_or_explicit (A atomic<T>*, T, memory_order);
T     atomic_fetch_xor_explicit(A atomic<T>*, T, memory_order);
```

- `atomic_float` supports `store`, `load`, `exchange`, `fetch_add`, `fetch_sub`,
  `fetch_min`, `fetch_max` (Metal 3+); bitwise ops are integer-only.
- 64-bit `atomic_ulong` modify operations are available on capable hardware (Metal 2.4+).
- Non-explicit shorthand names (`atomic_store`, `atomic_load`, `atomic_fetch_add`, …) exist
  and default to `memory_order_seq_cst`; prefer the `_explicit` forms with
  `memory_order_relaxed` on the GPU for performance.

## Idioms

**Global counter / append index:**
```metal
uint slot = atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
out[slot] = value;
```

**Spin CAS to update a max in device memory:**
```metal
uint prev = atomic_load_explicit(pmax, memory_order_relaxed);
while (candidate > prev &&
       !atomic_compare_exchange_weak_explicit(pmax, &prev, candidate,
                                              memory_order_relaxed, memory_order_relaxed)) {}
```

**Reduce-then-atomic** (combine SIMD/threadgroup reduction with one atomic per group) — see
the reduction example in [examples.md](examples.md).

## See also

- [address-spaces-and-operators.md](address-spaces-and-operators.md) — coherency, address spaces
- [standard-library.md](standard-library.md) — barriers and `mem_flags`
