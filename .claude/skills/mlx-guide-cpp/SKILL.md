---
name: mlx-guide-cpp
description: Reference guide for Apple MLX machine learning framework C++ API. Use when writing MLX C++ code, building with the MLX library, implementing ML models in C++, using MLX array operations, autograd/gradients in C++, or extending MLX with custom Metal GPU kernels. Triggers: "mlx c++", "mlx library", "mlx::core", "#include mlx", CMakeLists for MLX, MLX array ops in C++. Do NOT use for Python mlx code or non-MLX C++ ML code.
argument-hint: "[topic: array | ops | autograd | device | stream | cmake | metal]"
---

# MLX C++ Guide

MLX is Apple's array framework for ML on Apple silicon. It uses **unified CPU/GPU memory** and **lazy evaluation** — results are not computed until `eval()` is called. All symbols are in `mlx::core`; use `namespace mx = mlx::core` by convention.

Headers: `/opt/homebrew/include/mlx/mlx.h` (includes everything)  
Refs: [Ops Reference](references/ops-reference.md) | [Examples](references/examples.md) | [Metal Extensions](references/metal-extension.md)

## Quick Start

```cpp
#include "mlx/mlx.h"
namespace mx = mlx::core;

int main() {
    auto x = mx::array({1.0f, 2.0f, 3.0f});
    auto y = mx::array({4.0f, 5.0f, 6.0f});
    auto z = x + y;       // lazy — no computation yet
    mx::eval(z);           // materialize
    std::cout << z << "\n"; // [5, 7, 9]
}
```

## CMake Setup

```cmake
cmake_minimum_required(VERSION 3.27)
project(myproject LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Homebrew-installed MLX:
set(MLX_ROOT "/opt/homebrew")
# Python-installed MLX (alternative):
# execute_process(COMMAND python3 -m mlx --cmake-dir
#   OUTPUT_STRIP_TRAILING_WHITESPACE OUTPUT_VARIABLE MLX_ROOT)

find_package(MLX CONFIG REQUIRED)
add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE mlx)
```

Build: `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`

**CMake variables after `find_package(MLX)`:**

| Variable | Meaning |
|---|---|
| `MLX_FOUND` | MLX located |
| `MLX_INCLUDE_DIRS` | Header paths |
| `MLX_LIBRARIES` | Link targets |
| `MLX_BUILD_METAL` | Metal GPU available |
| `MLX_BUILD_ACCELERATE` | Accelerate framework |

## Array Construction (`mlx/array.h`)

```cpp
// Scalars
mx::array s(3.14f);                                   // float32 scalar
mx::array i(42, mx::int32);                           // explicit dtype

// 1D from initializer list
mx::array v({1.0f, 2.0f, 3.0f});                     // shape {3}, float32
mx::array vi({1, 2, 3}, mx::int32);

// N-D from data + shape
std::vector<float> data = {1,2,3,4,5,6};
mx::array m(data.begin(), {2,3}, mx::float32);        // 2x3 matrix

// N-D from initializer list + shape
mx::array mat({1.f,2.f,3.f,4.f}, {2,2});             // 2x2

// From raw pointer (zero-copy if possible; deleter called when done)
mx::array a((void*)ptr, {n}, mx::float32, [](void* p){ free(p); });
```

**Dtype enum:** `bool_` `uint8` `uint16` `uint32` `uint64` `int8` `int16` `int32` `int64` `float16` `bfloat16` `float32` `float64` `complex64`

## Array Introspection

```cpp
a.ndim()               // size_t — number of dimensions
a.shape()              // const Shape& (SmallVector<int32_t>)
a.shape(i)             // size of dim i; negative indexing supported
a.strides()            // const Strides& (SmallVector<int64_t>)
a.dtype()              // Dtype
a.size()               // size_t — total elements
a.nbytes()             // size_t — total bytes
a.itemsize()           // size_t — bytes per element
a.flags().row_contiguous
a.flags().col_contiguous
a.flags().contiguous

// After eval():
float v = a.item<float>();          // scalar value from size-1 array
float* ptr = a.data<float>();       // raw CPU pointer
```

**Status:** `array::unscheduled` → `array::evaluated` → `array::available`

## Lazy Evaluation

```cpp
auto z = x + y;                            // builds graph, no compute
mx::eval(z);                               // synchronous evaluate
mx::eval(a, b, c);                         // multiple outputs (efficient)
mx::eval(std::vector<mx::array>{a,b,c});

mx::async_eval(z);                         // schedule async
z.wait();                                  // block until available
bool ready = z.is_available();
```

## Operators (Inline, No StreamOrDevice)

```
+  -  *  /  %              arithmetic
==  !=  >  >=  <  <=       comparison (returns bool array)
&&  ||                     logical
&  |  ^  ~  <<  >>         bitwise
-a                         negation
```

Scalars work on either side: `a + 1.0f`, `2.0f * a`, `a == 0`.

## Device and Stream (`mlx/device.h`, `mlx/stream.h`)

```cpp
mx::Device cpu{mx::Device::cpu};
mx::Device gpu{mx::Device::gpu};       // Metal GPU

mx::set_default_device(gpu);           // global default
mx::Device d = mx::default_device();
bool avail = mx::is_available(gpu);
int n = mx::device_count(mx::Device::gpu);

// Streams
mx::Stream s = mx::default_stream(gpu);
mx::Stream s2 = mx::new_stream(cpu);
mx::set_default_stream(s);

// ThreadLocalStream: unique per thread
mx::ThreadLocalStream tls = mx::new_thread_local_stream(gpu);
mx::Stream actual = mx::stream_from_thread_local_stream(tls);

mx::synchronize();           // sync default stream
mx::synchronize(s);          // sync specific stream
mx::clear_streams();         // destroy thread streams

// Pass stream/device as last arg to any op
auto r = mx::matmul(a, b, gpu);     // force GPU
auto r2 = mx::sum(a, cpu);          // force CPU
auto r3 = mx::add(a, b, s);         // use stream s
```

## Transforms / Autograd (`mlx/transforms.h`)

```cpp
// grad: returns function computing gradients
auto gf = mx::grad(
    [](const std::vector<mx::array>& args) -> mx::array {
        return mx::sum(mx::square(args[0]));
    }, /*argnum=*/0);
std::vector<mx::array> grads = gf({x});

// grad of multiple args
auto gf2 = mx::grad(loss_fn, std::vector<int>{0, 1});

// Unary grad shortcut
auto gf3 = mx::grad([](const mx::array& x) { return mx::sum(x * x); });

// value_and_grad: returns {values, gradients} together
auto vgf = mx::value_and_grad(
    [](const std::vector<mx::array>& args) {
        return mx::sum(mx::square(args[0]));
    }, /*argnums=*/std::vector<int>{0});
auto [vals, gs] = vgf({x});

// vjp: vector-Jacobian product (reverse mode AD)
auto [out, vjp_out] = mx::vjp(f, primals, cotangents);
// Unary: auto [o, v] = mx::vjp(f, primal, cotangent);

// jvp: Jacobian-vector product (forward mode AD)
auto [out2, jvp_out] = mx::jvp(f, primals, tangents);

// Stop gradient flow
auto detached = mx::stop_gradient(x);
```

## Key Operations Quick Reference

Full signatures: [references/ops-reference.md](references/ops-reference.md)

**Creation:** `zeros` `ones` `full` `eye` `identity` `arange` `linspace` `zeros_like` `ones_like` `full_like` `tri` `tril` `triu`

**Shape:** `reshape` `flatten` `unflatten` `squeeze` `expand_dims` `transpose` `swapaxes` `moveaxis` `pad` `tile` `repeat` `atleast_1d/2d/3d`

**Combine/split:** `concatenate` `stack` `split` `meshgrid` `broadcast_to` `broadcast_arrays`

**Indexing:** `take` `take_along_axis` `gather` `scatter` `scatter_add` `scatter_prod` `scatter_max` `scatter_min` `put_along_axis` `slice` `slice_update` `slice_update_add` `masked_scatter`

**Elementwise math:** `abs` `negative` `sign` `sqrt` `rsqrt` `square` `reciprocal` `exp` `expm1` `log` `log2` `log10` `log1p` `logaddexp` `sigmoid` `erf` `erfinv` `floor` `ceil` `round` `power`

**Trig:** `sin` `cos` `tan` `arcsin` `arccos` `arctan` `arctan2` `sinh` `cosh` `tanh` `arcsinh` `arccosh` `arctanh` `degrees` `radians`

**Reductions:** `sum` `mean` `median` `var` `std` `prod` `min` `max` `argmin` `argmax` `all` `any` `logsumexp`

**Cumulative:** `cumsum` `cumprod` `cummax` `cummin` `logcumsumexp`

**Linear algebra:** `matmul` `addmm` `inner` `outer` `tensordot` `kron` `diagonal` `diag` `trace` `block_masked_mm` `gather_mm` `segmented_mm`

**Convolutions:** `conv1d` `conv2d` `conv3d` `conv_transpose1d/2d/3d` `conv_general`

**Quantization:** `quantize` `dequantize` `quantized_matmul` `gather_qmm` `qqmm` `from_fp8` `to_fp8`

**Comparison/masking:** `equal` `not_equal` `greater` `less` `greater_equal` `less_equal` `array_equal` `allclose` `isclose` `isnan` `isinf` `isfinite` `isposinf` `isneginf` `where` `nan_to_num` `clip` `maximum` `minimum`

**Sorting:** `sort` `argsort` `partition` `argpartition` `topk`

**Type/view:** `astype` `view` `as_strided` `copy` `contiguous` `real` `imag` `conjugate`

**Other:** `softmax` `hadamard_transform` `roll` `depends` `stop_gradient` `number_of_elements`

**Window functions:** `hanning` `hamming` `bartlett` `blackman`

## Metal / Custom Ops Extension

For custom Metal GPU kernels integrated into MLX, see [references/metal-extension.md](references/metal-extension.md). Also invoke the [[metal-guide]] skill for MSL syntax, Metal data types, address spaces, and kernel programming.

Summary:
- Subclass `Primitive` implementing `eval_cpu()`, `eval_gpu()`, `vjp()`, `jvp()`, `vmap()`
- Metal kernels in `.metal` files compiled via `mlx_build_metallib()` CMake macro
- Output memory must be manually allocated: `out.set_data(mx::allocator::malloc(out.nbytes()))`
- Use `mx::cpu::get_command_encoder(stream)` for CPU dispatch
- Guard Metal code with `#if MLX_BUILD_METAL` / `if(MLX_BUILD_METAL)` in CMake
- Check `MLX_BUILD_METAL` CMake variable before adding `.metal` sources

## References

- [Full Ops Signatures](references/ops-reference.md) — complete API from ops.h
- [Code Examples](references/examples.md) — linear regression, tutorial, AXPBY custom op
- [Metal Extension Guide](references/metal-extension.md) — custom Metal GPU kernels
- MLX C++ usage: https://ml-explore.github.io/mlx/build/html/dev/mlx_in_cpp.html
- MLX C++ ops: https://ml-explore.github.io/mlx/build/html/cpp/ops.html
- MLX extensions: https://ml-explore.github.io/mlx/build/html/dev/extensions.html
- Headers: /opt/homebrew/include/mlx/

### Final Step — Record Usage

Run after the skill's primary task completes:

```bash
python3 ${PWD}/.claude/skills/skill-stat/scripts/record-stat.py "mlx-guide-cpp"
```
