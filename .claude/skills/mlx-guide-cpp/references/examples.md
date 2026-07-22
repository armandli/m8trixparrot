# MLX C++ Code Examples

## Example 1: Array Basics and Lazy Evaluation

```cpp
// tutorial.cpp — demonstrates lazy eval and autograd
#include <cassert>
#include <iostream>
#include "mlx/mlx.h"

namespace mx = mlx::core;

void array_basics() {
    // Scalar and vector creation
    auto x = mx::array(1.0f);                // scalar
    auto y = mx::array({1.0f, 2.0f, 3.0f}); // 1D

    // Lazy evaluation — z has shape and type but no data yet
    auto z = x + y;
    std::cout << "Before eval: z is unscheduled\n";

    mx::eval(z);
    std::cout << "After eval: " << z << "\n"; // [2, 3, 4]

    // 2D array
    auto mat = mx::array({1.f, 2.f, 3.f, 4.f}, {2, 2});
    auto mat2 = mx::matmul(mat, mat);
    mx::eval(mat2);
    std::cout << mat2 << "\n"; // [[7, 10], [15, 22]]

    // Introspection
    std::cout << "shape: " << mat.shape(0) << "x" << mat.shape(1) << "\n";
    std::cout << "dtype: " << mat.dtype() << "\n";   // float32
    std::cout << "ndim: " << mat.ndim() << "\n";     // 2
}

void automatic_differentiation() {
    // Compute gradient of f(x) = sum(x^2), df/dx = 2x
    auto f = [](const mx::array& x) { return mx::sum(mx::square(x)); };
    auto grad_f = mx::grad(f);

    auto x = mx::array({1.0f, 2.0f, 3.0f});
    auto [val, g] = mx::value_and_grad(f)(x);
    mx::eval(val, g);
    std::cout << "f(x)=" << val << " grad=" << g << "\n"; // f=14, grad=[2, 4, 6]

    // Second derivative via grad composition
    auto grad2 = mx::grad(grad_f);
    auto x0 = mx::array(3.0f);
    auto d2 = grad2(x0);
    mx::eval(d2);
    std::cout << "f''(3) = " << d2 << "\n"; // 2.0
}

int main() {
    array_basics();
    automatic_differentiation();
}
```

---

## Example 2: Linear Regression

```cpp
// linear_regression.cpp
#include <chrono>
#include <cmath>
#include <iostream>
#include "mlx/mlx.h"

namespace mx = mlx::core;

int main() {
    // Problem setup
    int num_features = 100;
    int num_examples = 1000;
    int num_iters    = 10000;
    float learning_rate = 0.01f;

    // Generate data: y = X @ w_true + noise
    auto w_true = mx::random::normal({num_features});
    auto X = mx::random::normal({num_examples, num_features});
    auto y = mx::matmul(X, w_true) + mx::random::normal({num_examples}) * 0.1f;

    // Random init weights
    auto w = mx::random::normal({num_features});

    // Loss: MSE = 0.5 * mean((Xw - y)^2)
    auto loss_fn = [&](const std::vector<mx::array>& params) -> mx::array {
        auto pred = mx::matmul(X, params[0]);
        auto diff = pred - y;
        return (0.5f / num_examples) * mx::sum(mx::square(diff));
    };

    auto grad_fn = mx::grad(loss_fn, 0);

    auto t0 = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < num_iters; i++) {
        auto grads = grad_fn({w});
        w = w - learning_rate * grads[0];
        mx::eval(w);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    auto final_loss = loss_fn({w});
    mx::eval(final_loss);

    auto err = mx::sum(mx::square(w - w_true));
    mx::eval(err);

    std::cout << "Loss: " << final_loss.item<float>() << "\n";
    std::cout << "|w - w*|: " << std::sqrt(err.item<float>()) << "\n";
    std::cout << "Throughput: " << num_iters / elapsed << " iters/sec\n";
}
```

---

## Example 3: Device and Stream Control

```cpp
#include <iostream>
#include "mlx/mlx.h"
namespace mx = mlx::core;

int main() {
    // Check GPU availability
    if (!mx::is_available(mx::Device{mx::Device::gpu})) {
        std::cerr << "Metal GPU not available\n";
        return 1;
    }

    // Set GPU as default
    mx::set_default_device(mx::Device{mx::Device::gpu});

    auto a = mx::random::normal({1024, 1024});
    auto b = mx::random::normal({1024, 1024});

    // This runs on GPU (default)
    auto c = mx::matmul(a, b);

    // Explicitly run on CPU
    auto c_cpu = mx::matmul(a, b, mx::Device{mx::Device::cpu});

    // Use a custom stream
    auto gpu = mx::Device{mx::Device::gpu};
    auto s1 = mx::new_stream(gpu);
    auto s2 = mx::new_stream(gpu);

    auto r1 = mx::matmul(a, b, s1);
    auto r2 = mx::matmul(a, b, s2);

    // Eval both streams
    mx::eval(r1, r2);

    mx::synchronize();
    std::cout << "Done\n";
}
```

---

## Example 4: Gradient Descent with Multiple Parameters

```cpp
#include <iostream>
#include "mlx/mlx.h"
namespace mx = mlx::core;

int main() {
    // Simple neural network: y = relu(X @ W1) @ W2
    int N = 256, D = 64, H = 128, K = 10;
    auto X = mx::random::normal({N, D});
    auto Y_true = mx::random::randint(0, K, {N});

    auto W1 = mx::random::normal({D, H}) * 0.1f;
    auto W2 = mx::random::normal({H, K}) * 0.1f;

    auto relu = [](const mx::array& x) {
        return mx::maximum(x, mx::zeros_like(x));
    };

    auto loss_fn = [&](const std::vector<mx::array>& params) {
        auto h = relu(mx::matmul(X, params[0]));
        auto logits = mx::matmul(h, params[1]);
        // Cross-entropy: -log(softmax)[y]
        auto log_sm = logits - mx::logsumexp(logits, 1, true);
        return -mx::mean(mx::take_along_axis(
            log_sm,
            mx::expand_dims(mx::astype(Y_true, mx::int32), 1),
            1));
    };

    auto grad_fn = mx::grad(loss_fn, std::vector<int>{0, 1});

    float lr = 0.01f;
    for (int i = 0; i < 100; i++) {
        auto grads = grad_fn({W1, W2});
        W1 = W1 - lr * grads[0];
        W2 = W2 - lr * grads[1];
        mx::eval(W1, W2);

        if (i % 10 == 0) {
            auto loss = loss_fn({W1, W2});
            mx::eval(loss);
            std::cout << "iter " << i << " loss=" << loss.item<float>() << "\n";
        }
    }
}
```

---

## Example 5: Array Operations Cheatsheet

```cpp
#include "mlx/mlx.h"
namespace mx = mlx::core;

void ops_demo() {
    // Creation
    auto a = mx::zeros({3, 4});                  // 3x4 float32 zeros
    auto b = mx::ones({3, 4}, mx::int32);        // 3x4 int32 ones
    auto c = mx::arange(0, 12, mx::float32);     // [0,1,...,11]
    auto d = mx::reshape(c, {3, 4});             // 3x4
    auto e = mx::linspace(0.0, 1.0, 11);         // 11 values in [0,1]
    auto f = mx::eye(4);                          // 4x4 identity

    // Shape ops
    auto t = mx::transpose(d);                    // 4x3
    auto t2 = mx::transpose(d, {1, 0});          // 4x3
    auto sq = mx::squeeze(mx::expand_dims(d, 0), 0); // round-trip
    auto flat = mx::flatten(d);                   // 12-elem 1D
    auto parts = mx::split(d, 3, 0);             // 3 arrays of shape {1,4}

    // Concat / stack
    auto cat = mx::concatenate({d, d}, 0);        // 6x4
    auto stacked = mx::stack({d, d}, 0);          // 2x3x4

    // Indexing
    auto row = mx::take(d, 1, 0);               // row 1, shape {4}
    auto cols = mx::take(d, mx::array({0,2}), 1); // cols 0 and 2

    // Math
    auto sum_all = mx::sum(d);                    // scalar
    auto sum_ax = mx::sum(d, 1);                  // shape {3}
    auto mm = mx::matmul(d, mx::transpose(d));   // 3x3
    auto norm = mx::sqrt(mx::sum(mx::square(d))); // frobenius norm

    // Masking
    auto mask = d > 5.f;                          // bool array
    auto masked = mx::where(mask, d, mx::zeros_like(d));

    // Eval everything
    mx::eval(t, flat, cat, stacked, sum_all, sum_ax, mm, norm, masked);
}
```

---

## CMakeLists.txt Template

```cmake
cmake_minimum_required(VERSION 3.27)
project(mlx_project LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Homebrew MLX
set(MLX_ROOT "/opt/homebrew")
find_package(MLX CONFIG REQUIRED)

add_executable(myapp main.cpp)
target_link_libraries(myapp PRIVATE mlx)
target_include_directories(myapp PRIVATE ${MLX_INCLUDE_DIRS})

# Optional: Metal extension
if(MLX_BUILD_METAL)
    message(STATUS "Metal GPU support enabled")
    # See metal-extension.md for adding custom Metal kernels
endif()
```

Build:
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/myapp
```
