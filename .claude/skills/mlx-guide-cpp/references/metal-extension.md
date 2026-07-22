# MLX Metal Extension Guide

This guide covers writing custom Metal GPU operations for MLX in C++. For MSL syntax, data types, address spaces, and Metal kernel programming fundamentals, also invoke the [[metal-guide]] skill.

Reference: https://ml-explore.github.io/mlx/build/html/dev/extensions.html

---

## Architecture: Operations vs. Primitives

- **Operation** (`mx::array my_op(...)`) — user-facing function that builds the computation graph
- **Primitive** (`class MyOp : public Primitive`) — a node in the graph; knows how to compute the output

A Primitive implements:
- `eval_cpu()` — CPU execution
- `eval_gpu()` — Metal GPU execution
- `vjp()` — vector-Jacobian product for reverse-mode autodiff
- `jvp()` — Jacobian-vector product for forward-mode autodiff
- `vmap()` — vectorization rules (optional)

---

## Directory Layout

```
my_extension/
  my_op.h          # Primitive class declaration + op function signature
  my_op.cpp        # CPU eval + user-facing function + VJP/JVP
  my_op.metal      # Metal kernel
  CMakeLists.txt
```

---

## Step 1: The Primitive Class (`my_op.h`)

```cpp
#pragma once
#include "mlx/mlx.h"

namespace mx = mlx::core;

// User-facing operation
mx::array my_axpby(
    const mx::array& x,
    const mx::array& y,
    float alpha,
    float beta,
    mx::StreamOrDevice s = {});

// Primitive: one per output array in the graph
class Axpby : public mx::Primitive {
 public:
  explicit Axpby(mx::Stream stream, float alpha, float beta)
      : Primitive(stream), alpha_(alpha), beta_(beta) {}

  // Called to run CPU eval
  void eval_cpu(const std::vector<mx::array>& inputs,
                std::vector<mx::array>& outputs) override;

  // Called to run GPU eval (Metal)
  void eval_gpu(const std::vector<mx::array>& inputs,
                std::vector<mx::array>& outputs) override;

  // Reverse-mode autodiff
  std::vector<mx::array> vjp(
      const std::vector<mx::array>& primals,
      const std::vector<mx::array>& cotangents,
      const std::vector<int>& argnums,
      const std::vector<mx::array>& outputs) override;

  // Forward-mode autodiff
  std::vector<mx::array> jvp(
      const std::vector<mx::array>& primals,
      const std::vector<mx::array>& tangents,
      const std::vector<int>& argnums) override;

  // Shape/type inference (not always needed if ops do it)
  std::pair<std::vector<mx::array>, std::vector<int>> vmap(
      const std::vector<mx::array>& inputs,
      const std::vector<int>& axes) override;

 private:
  float alpha_;
  float beta_;
};
```

---

## Step 2: CPU Backend (`my_op.cpp`)

```cpp
#include "my_op.h"
#include "mlx/backend/cpu/encoder.h"
#include "mlx/utils.h"

namespace mx = mlx::core;

// User-facing function: builds graph node
mx::array my_axpby(
    const mx::array& x,
    const mx::array& y,
    float alpha,
    float beta,
    mx::StreamOrDevice s /* = {} */) {
  // Validate inputs
  if (x.shape() != y.shape()) {
    throw std::invalid_argument("x and y must have the same shape");
  }

  // Return an array whose value is computed by Axpby primitive
  return mx::array::make_arrays(
      {x.shape()},           // output shapes
      {x.dtype()},           // output dtypes
      std::make_shared<Axpby>(mx::to_stream(s), alpha, beta),
      {x, y}                 // inputs
  )[0];
}

// CPU eval implementation
template <typename T>
void axpby_impl(
    const mx::array& x, const mx::array& y, mx::array& out,
    float alpha_, float beta_, mx::Stream stream) {

  out.set_data(mx::allocator::malloc(out.nbytes()));  // MUST allocate output

  auto& encoder = mx::cpu::get_command_encoder(stream);
  encoder.dispatch(
      [x_ptr = x.data<T>(), y_ptr = y.data<T>(), out_ptr = out.data<T>(),
       alpha = static_cast<T>(alpha_), beta = static_cast<T>(beta_),
       size = out.size(),
       shape = out.shape(), x_strides = x.strides(), y_strides = y.strides()]() {
        for (size_t i = 0; i < size; i++) {
          auto xi = mx::elem_to_loc(i, shape, x_strides);
          auto yi = mx::elem_to_loc(i, shape, y_strides);
          out_ptr[i] = alpha * x_ptr[xi] + beta * y_ptr[yi];
        }
      });
}

void Axpby::eval_cpu(const std::vector<mx::array>& inputs,
                     std::vector<mx::array>& outputs) {
  auto& x = inputs[0];
  auto& y = inputs[1];
  auto& out = outputs[0];

  switch (out.dtype()) {
    case mx::float32: axpby_impl<float>(x, y, out, alpha_, beta_, stream()); break;
    case mx::float16: axpby_impl<mx::float16_t>(x, y, out, alpha_, beta_, stream()); break;
    case mx::bfloat16: axpby_impl<mx::bfloat16_t>(x, y, out, alpha_, beta_, stream()); break;
    default:
      throw std::runtime_error("Axpby: unsupported dtype");
  }
}

// Reverse-mode autodiff: dL/dx = alpha * dL/dout, dL/dy = beta * dL/dout
std::vector<mx::array> Axpby::vjp(
    const std::vector<mx::array>& primals,
    const std::vector<mx::array>& cotangents,
    const std::vector<int>& argnums,
    const std::vector<mx::array>& /*outputs*/) {
  std::vector<mx::array> vjps;
  auto& cotan = cotangents[0];
  for (int arg : argnums) {
    float scale = (arg == 0) ? alpha_ : beta_;
    vjps.push_back(mx::multiply(mx::array(scale, cotan.dtype()), cotan, stream()));
  }
  return vjps;
}

// Forward-mode autodiff
std::vector<mx::array> Axpby::jvp(
    const std::vector<mx::array>& primals,
    const std::vector<mx::array>& tangents,
    const std::vector<int>& argnums) {
  // JVP of alpha*x + beta*y wrt x: alpha * dx
  //                               wrt y: beta * dy
  auto out = mx::zeros_like(primals[0]);
  for (int i = 0; i < static_cast<int>(argnums.size()); i++) {
    float scale = (argnums[i] == 0) ? alpha_ : beta_;
    out = mx::add(out,
                  mx::multiply(mx::array(scale, tangents[i].dtype()),
                               tangents[i], stream()),
                  stream());
  }
  return {out};
}
```

---

## Step 3: Metal Kernel (`my_op.metal`)

```metal
// my_op.metal
#include <metal_stdlib>
using namespace metal;

// elem_to_loc: converts flat index to strided offset (MLX utility)
uint elem_to_loc(uint elem, constant const int* shape,
                 constant const size_t* strides, int ndim) {
  uint loc = 0;
  for (int i = ndim - 1; i >= 0; --i) {
    loc += (elem % shape[i]) * strides[i];
    elem /= shape[i];
  }
  return loc;
}

template <typename T>
[[kernel]] void axpby_general(
    device const T* x          [[buffer(0)]],
    device const T* y          [[buffer(1)]],
    device       T* out        [[buffer(2)]],
    constant const float& alpha [[buffer(3)]],
    constant const float& beta  [[buffer(4)]],
    constant const int* shape   [[buffer(5)]],
    constant const size_t* x_strides [[buffer(6)]],
    constant const size_t* y_strides [[buffer(7)]],
    constant const int& ndim   [[buffer(8)]],
    uint index [[thread_position_in_grid]])
{
  auto xi = elem_to_loc(index, shape, x_strides, ndim);
  auto yi = elem_to_loc(index, shape, y_strides, ndim);
  out[index] = static_cast<T>(alpha) * x[xi] + static_cast<T>(beta) * y[yi];
}

// Explicit template instantiations
template [[host_name("axpby_general_float")]]
[[kernel]] decltype(axpby_general<float>) axpby_general<float>;

template [[host_name("axpby_general_half")]]
[[kernel]] decltype(axpby_general<half>) axpby_general<half>;

template [[host_name("axpby_general_bfloat")]]
[[kernel]] decltype(axpby_general<bfloat16_t>) axpby_general<bfloat16_t>;
```

---

## Step 4: GPU Backend (in `my_op.cpp`)

```cpp
#ifdef MLX_HAS_METAL

#include "mlx/backend/metal/device.h"
#include "mlx/backend/metal/utils.h"

void Axpby::eval_gpu(const std::vector<mx::array>& inputs,
                     std::vector<mx::array>& outputs) {
  auto& x = inputs[0];
  auto& y = inputs[1];
  auto& out = outputs[0];

  out.set_data(mx::allocator::malloc(out.nbytes()));  // allocate GPU memory

  auto& d = mx::metal::device(stream().device);
  auto& compute_encoder = d.get_command_encoder(stream().index);
  {
    auto kernel_name = std::string("axpby_general_");
    switch (out.dtype()) {
      case mx::float32:  kernel_name += "float";  break;
      case mx::float16:  kernel_name += "half";   break;
      case mx::bfloat16: kernel_name += "bfloat"; break;
      default: throw std::runtime_error("Axpby GPU: unsupported dtype");
    }

    // Load compiled Metal library (must match name from cmake)
    auto kernel = d.get_kernel(kernel_name, "mlx_ext");
    compute_encoder->setComputePipelineState(kernel);

    // Bind buffers
    mx::metal::set_array_buffer(compute_encoder, x, 0);
    mx::metal::set_array_buffer(compute_encoder, y, 1);
    mx::metal::set_array_buffer(compute_encoder, out, 2);
    compute_encoder->setBytes(&alpha_, sizeof(float), 3);
    compute_encoder->setBytes(&beta_, sizeof(float), 4);

    // Shape and strides
    compute_encoder->setBytes(out.shape().data(),
                               out.ndim() * sizeof(int), 5);
    compute_encoder->setBytes(x.strides().data(),
                               x.ndim() * sizeof(size_t), 6);
    compute_encoder->setBytes(y.strides().data(),
                               y.ndim() * sizeof(size_t), 7);
    int ndim = static_cast<int>(out.ndim());
    compute_encoder->setBytes(&ndim, sizeof(int), 8);

    // Launch: one thread per output element
    size_t n = out.size();
    NS::UInteger thread_group_size =
        kernel->maxTotalThreadsPerThreadgroup();
    MTL::Size grid_dims = MTL::Size(n, 1, 1);
    MTL::Size group_dims = MTL::Size(
        std::min(thread_group_size, static_cast<NS::UInteger>(n)), 1, 1);
    compute_encoder->dispatchThreads(grid_dims, group_dims);
  }
}

#else  // CPU-only fallback

void Axpby::eval_gpu(const std::vector<mx::array>& inputs,
                     std::vector<mx::array>& outputs) {
  eval_cpu(inputs, outputs);
}

#endif
```

---

## Step 5: CMakeLists.txt for Extension

```cmake
cmake_minimum_required(VERSION 3.27)
project(mlx_ext LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(MLX CONFIG REQUIRED)

# C++ library with CPU (and GPU dispatch) code
add_library(mlx_ext SHARED)
target_sources(mlx_ext PUBLIC my_op.cpp)
target_link_libraries(mlx_ext PUBLIC mlx)
target_include_directories(mlx_ext PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Metal GPU kernels (only when Metal is available)
if(MLX_BUILD_METAL)
  mlx_build_metallib(
    TARGET   mlx_ext_metallib
    SOURCES  my_op.metal
    OUTPUT_DIRECTORY ${CMAKE_LIBRARY_OUTPUT_DIRECTORY}
  )
  add_dependencies(mlx_ext mlx_ext_metallib)
  target_compile_definitions(mlx_ext PRIVATE MLX_HAS_METAL)
endif()

# Optional Python bindings via nanobind
# find_package(nanobind CONFIG REQUIRED)
# nanobind_add_module(_ext NB_STATIC STABLE_ABI bindings.cpp)
# target_link_libraries(_ext PRIVATE mlx_ext)
```

---

## Key Rules and Pitfalls

**Memory allocation:**
- The primitive MUST call `out.set_data(mx::allocator::malloc(out.nbytes()))` before writing any data
- Do NOT use `new` or `malloc` directly — use `mx::allocator::malloc`

**Stream ordering:**
- Always use `stream()` (inherited from `Primitive`) when scheduling work
- Use `mx::cpu::get_command_encoder(stream())` for CPU dispatch
- Use `d.get_command_encoder(stream().index)` for GPU dispatch

**Thread safety:**
- Lambdas dispatched via `encoder.dispatch()` run later — capture by value, not reference

**Metal library naming:**
- The library name passed to `d.get_kernel(kernel_name, "mlx_ext")` must match the `TARGET` in `mlx_build_metallib`

**Contiguity:**
- If your kernel assumes contiguous arrays, call `mx::contiguous(x, stream())` on inputs before `eval_gpu`
- Use `mx::elem_to_loc()` for strided access if inputs may not be contiguous

**Autograd:**
- VJP/JVP can use any MLX ops scheduled on `stream()`
- VJP for `z = alpha*x + beta*y`: `dL/dx = alpha * dL/dz`, `dL/dy = beta * dL/dz`

**dtype support:**
- Explicitly handle each dtype; use switch/case and throw for unsupported types
- Use `float16_t`, `bfloat16_t`, `complex64_t` for the corresponding MLX dtypes

---

## Performance Benchmark Pattern

```cpp
// Warmup + timing
auto warmup = [&]() { mx::eval(my_axpby(x, y, 2.0f, 3.0f)); };
warmup();
mx::synchronize();

auto start = std::chrono::high_resolution_clock::now();
for (int i = 0; i < 100; i++) {
    auto r = my_axpby(x, y, 2.0f, 3.0f);
    mx::eval(r);
}
mx::synchronize();
auto end = std::chrono::high_resolution_clock::now();

double ms = std::chrono::duration<double, std::milli>(end - start).count() / 100.0;
std::cout << "Custom axpby: " << ms << " ms\n";
```

Typical speedup of custom Metal kernel vs. Python-dispatched naive: ~2x for large arrays.
