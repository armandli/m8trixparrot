# MLX C++ Operations Reference

Complete function signatures from `/opt/homebrew/include/mlx/ops.h`. All functions are in `namespace mlx::core`. The last parameter of every op is `StreamOrDevice s = {}` (defaulting to the current default device/stream) unless noted otherwise.

Shorthand: `SOD` = `StreamOrDevice s = {}`

---

## Array Creation

```cpp
// arange — 1D sequence
array arange(double start, double stop, double step, Dtype dtype, SOD);
array arange(double start, double stop, double step, SOD);
array arange(double start, double stop, Dtype dtype, SOD);
array arange(double start, double stop, SOD);
array arange(double stop, Dtype dtype, SOD);
array arange(double stop, SOD);
array arange(int start, int stop, int step, SOD);
array arange(int start, int stop, SOD);
array arange(int stop, SOD);

// linspace — evenly spaced
array linspace(double start, double stop, int num = 50, Dtype dtype = float32, SOD);

// zeros / ones / full
array zeros(const Shape& shape, Dtype dtype, SOD);
array zeros(const Shape& shape, SOD);           // defaults to float32
array zeros_like(const array& a, SOD);
array ones(const Shape& shape, Dtype dtype, SOD);
array ones(const Shape& shape, SOD);            // defaults to float32
array ones_like(const array& a, SOD);
array full(Shape shape, array vals, Dtype dtype, SOD);
array full(Shape shape, array vals, SOD);
template<T> array full(Shape shape, T val, Dtype dtype, SOD);
template<T> array full(Shape shape, T val, SOD);
array full_like(const array& a, array vals, Dtype dtype, SOD);
array full_like(const array& a, array vals, SOD);
template<T> array full_like(const array& a, T val, Dtype dtype, SOD);
template<T> array full_like(const array& a, T val, SOD);

// identity matrices
array eye(int n, int m, int k, Dtype dtype, SOD);  // n rows, m cols, diagonal k
array eye(int n, Dtype dtype, SOD);
array eye(int n, int m, SOD);
array eye(int n, int m, int k, SOD);
array eye(int n, SOD);
array identity(int n, Dtype dtype, SOD);           // square identity
array identity(int n, SOD);

// triangular
array tri(int n, int m, int k, Dtype type, SOD);
array tri(int n, Dtype type, SOD);
array tril(array x, int k = 0, SOD);              // lower triangular
array triu(array x, int k = 0, SOD);              // upper triangular
```

---

## Type Conversion

```cpp
array astype(array a, Dtype dtype, SOD);
array view(const array& a, const Dtype& dtype, SOD);   // reinterpret bits
array as_strided(array a, Shape shape, Strides strides, size_t offset, SOD);
array copy(array a, SOD);
array contiguous(const array& a, bool allow_col_major = false, SOD);
array from_fp8(array x, Dtype dtype, SOD);        // E4M3 float8 -> dtype
array to_fp8(array x, SOD);                       // float -> E4M3 float8
```

---

## Shape Manipulation

```cpp
array reshape(const array& a, Shape shape, SOD);
array unflatten(const array& a, int axis, Shape shape, SOD);
array flatten(const array& a, int start_axis, int end_axis = -1, SOD);
array flatten(const array& a, SOD);                // full flatten to 1D

array squeeze(const array& a, const std::vector<int>& axes, SOD);
array squeeze(const array& a, int axis, SOD);
array squeeze(const array& a, SOD);                // remove all singletons
array expand_dims(const array& a, const std::vector<int>& axes, SOD);
array expand_dims(const array& a, int axis, SOD);

array transpose(const array& a, std::vector<int> axes, SOD);
array transpose(const array& a, SOD);              // reverse all axes
array swapaxes(const array& a, int axis1, int axis2, SOD);
array moveaxis(const array& a, int source, int destination, SOD);

array pad(const array& a,
          const std::vector<int>& axes,
          const Shape& low_pad_size,
          const Shape& high_pad_size,
          const array& pad_value = array(0),
          const std::string& mode = "constant",
          SOD);
array pad(const array& a,
          const std::vector<std::pair<int,int>>& pad_width,
          const array& pad_value = array(0),
          const std::string& mode = "constant",
          SOD);
array pad(const array& a, const std::pair<int,int>& pad_width,
          const array& pad_value = array(0),
          const std::string& mode = "constant", SOD);
array pad(const array& a, int pad_width,
          const array& pad_value = array(0),
          const std::string& mode = "constant", SOD);

array repeat(const array& arr, int repeats, int axis, SOD);
array repeat(const array& arr, int repeats, SOD);
array tile(const array& arr, std::vector<int> reps, SOD);

array broadcast_to(const array& a, const Shape& shape, SOD);
std::vector<array> broadcast_arrays(const std::vector<array>& inputs, SOD);

array atleast_1d(const array& a, SOD);
array atleast_2d(const array& a, SOD);
array atleast_3d(const array& a, SOD);
std::vector<array> atleast_1d(const std::vector<array>& a, SOD);
std::vector<array> atleast_2d(const std::vector<array>& a, SOD);
std::vector<array> atleast_3d(const std::vector<array>& a, SOD);

// roll: shift elements circularly
array roll(const array& a, int shift, SOD);
array roll(const array& a, const Shape& shift, SOD);
array roll(const array& a, int shift, int axis, SOD);
array roll(const array& a, int shift, const std::vector<int>& axes, SOD);
array roll(const array& a, const Shape& shift, int axis, SOD);
array roll(const array& a, const Shape& shift, const std::vector<int>& axes, SOD);
```

---

## Combining and Splitting

```cpp
array concatenate(std::vector<array> arrays, int axis, SOD);
array concatenate(std::vector<array> arrays, SOD);   // flattens first
array stack(const std::vector<array>& arrays, int axis, SOD);
array stack(const std::vector<array>& arrays, SOD);  // new axis 0

std::vector<array> split(const array& a, int num_splits, int axis, SOD);
std::vector<array> split(const array& a, int num_splits, SOD);
std::vector<array> split(const array& a, const Shape& indices, int axis, SOD);
std::vector<array> split(const array& a, const Shape& indices, SOD);

std::vector<array> meshgrid(const std::vector<array>& arrays,
                             bool sparse = false,
                             const std::string& indexing = "xy",
                             SOD);
```

---

## Slicing and Indexing

```cpp
array slice(const array& a, Shape start, Shape stop, Shape strides, SOD);
array slice(const array& a, Shape start, Shape stop, SOD);  // stride 1
// Dynamic start indices:
array slice(const array& a, const array& start, std::vector<int> axes,
            Shape slice_size, SOD);

array slice_update(const array& src, const array& update,
                   Shape start, Shape stop, Shape strides, SOD);
array slice_update(const array& src, const array& update,
                   Shape start, Shape stop, SOD);
// Dynamic:
array slice_update(const array& src, const array& update,
                   const array& start, std::vector<int> axes, SOD);

// Atomic slice updates (also: _prod, _max, _min variants):
array slice_update_add(const array& src, const array& update,
                       Shape start, Shape stop, Shape strides, SOD);
array slice_update_add(const array& src, const array& update,
                       Shape start, Shape stop, SOD);

array take(const array& a, const array& indices, int axis, SOD);
array take(const array& a, int index, int axis, SOD);
array take(const array& a, const array& indices, SOD);  // flat
array take(const array& a, int index, SOD);              // flat

array take_along_axis(const array& a, const array& indices, int axis, SOD);
array put_along_axis(const array& a, const array& indices,
                     const array& values, int axis, SOD);
array scatter_add_axis(const array& a, const array& indices,
                       const array& values, int axis, SOD);

array gather(const array& a, const std::vector<array>& indices,
             const std::vector<int>& axes, const Shape& slice_sizes, SOD);
array gather(const array& a, const array& indices, int axis,
             const Shape& slice_sizes, SOD);

// scatter: write updates to indexed locations
array scatter(const array& a, const std::vector<array>& indices,
              const array& updates, const std::vector<int>& axes, SOD);
array scatter(const array& a, const array& indices,
              const array& updates, int axis, SOD);
// Variants: scatter_add, scatter_prod, scatter_max, scatter_min
// (same signatures as scatter)

array masked_scatter(const array& a, const array& mask, const array& src, SOD);
```

---

## Comparison and Masking

```cpp
array equal(const array& a, const array& b, SOD);
array not_equal(const array& a, const array& b, SOD);
array greater(const array& a, const array& b, SOD);
array greater_equal(const array& a, const array& b, SOD);
array less(const array& a, const array& b, SOD);
array less_equal(const array& a, const array& b, SOD);
array array_equal(const array& a, const array& b, bool equal_nan, SOD);
array array_equal(const array& a, const array& b, SOD);  // equal_nan=false

array isnan(const array& a, SOD);
array isinf(const array& a, SOD);
array isfinite(const array& a, SOD);
array isposinf(const array& a, SOD);
array isneginf(const array& a, SOD);

array where(const array& condition, const array& x, const array& y, SOD);
array nan_to_num(const array& a, float nan = 0.0f,
                 const std::optional<float> posinf = std::nullopt,
                 const std::optional<float> neginf = std::nullopt, SOD);
array clip(const array& a,
           const std::optional<array>& a_min = std::nullopt,
           const std::optional<array>& a_max = std::nullopt, SOD);

array allclose(const array& a, const array& b,
               double rtol = 1e-5, double atol = 1e-8,
               bool equal_nan = false, SOD);
array isclose(const array& a, const array& b,
              double rtol = 1e-5, double atol = 1e-8,
              bool equal_nan = false, SOD);
```

---

## Reductions

All reduction ops: optional `axes` / `axis`, `keepdims = false`.

```cpp
// Boolean reductions
array all(const array& a, bool keepdims, SOD);
array all(const array& a, SOD);
array all(const array& a, const std::vector<int>& axes, bool keepdims = false, SOD);
array all(const array& a, int axis, bool keepdims = false, SOD);
// (same overload pattern for any, sum, mean, median, prod, min, max, var, std)

array any(const array& a, ...);
array sum(const array& a, ...);
array mean(const array& a, ...);
array median(const array& a, ...);
array prod(const array& a, ...);
array min(const array& a, ...);
array max(const array& a, ...);

// var and std have extra ddof parameter
array var(const array& a, bool keepdims, int ddof = 0, SOD);
array var(const array& a, const std::vector<int>& axes, bool keepdims = false, int ddof = 0, SOD);
array var(const array& a, int axis, bool keepdims = false, int ddof = 0, SOD);
// std: same as var

array argmin(const array& a, bool keepdims, SOD);
array argmin(const array& a, int axis, bool keepdims = false, SOD);
array argmax(const array& a, bool keepdims, SOD);
array argmax(const array& a, int axis, bool keepdims = false, SOD);

array logsumexp(const array& a, bool keepdims, SOD);
array logsumexp(const array& a, const std::vector<int>& axes, bool keepdims = false, SOD);
array logsumexp(const array& a, int axis, bool keepdims = false, SOD);
```

---

## Cumulative Operations

```cpp
// All have: bool reverse = false, bool inclusive = true
array cumsum(const array& a, SOD);
array cumsum(const array& a, int axis, bool reverse = false, bool inclusive = true, SOD);
array cumprod(const array& a, SOD);
array cumprod(const array& a, int axis, bool reverse = false, bool inclusive = true, SOD);
array cummax(const array& a, SOD);
array cummax(const array& a, int axis, bool reverse = false, bool inclusive = true, SOD);
array cummin(const array& a, SOD);
array cummin(const array& a, int axis, bool reverse = false, bool inclusive = true, SOD);
array logcumsumexp(const array& a, bool reverse = false, bool inclusive = true, SOD);
array logcumsumexp(const array& a, int axis, bool reverse = false, bool inclusive = true, SOD);
```

---

## Elementwise Math

```cpp
array abs(const array& a, SOD);
array negative(const array& a, SOD);      // also: operator-
array sign(const array& a, SOD);
array reciprocal(const array& a, SOD);    // 1/x
array sqrt(const array& a, SOD);
array rsqrt(const array& a, SOD);         // 1/sqrt(x)
array square(const array& a, SOD);
array floor(const array& a, SOD);
array ceil(const array& a, SOD);
array round(const array& a, int decimals, SOD);
array round(const array& a, SOD);         // decimals=0
array exp(const array& a, SOD);
array expm1(const array& a, SOD);         // exp(x)-1
array log(const array& a, SOD);
array log2(const array& a, SOD);
array log10(const array& a, SOD);
array log1p(const array& a, SOD);         // log(1+x)
array logaddexp(const array& a, const array& b, SOD);
array sigmoid(const array& a, SOD);       // 1/(1+exp(-x))
array erf(const array& a, SOD);
array erfinv(const array& a, SOD);
array power(const array& a, const array& b, SOD);  // element-wise a^b

// Two-array arithmetic
array add(const array& a, const array& b, SOD);
array subtract(const array& a, const array& b, SOD);
array multiply(const array& a, const array& b, SOD);
array divide(const array& a, const array& b, SOD);
array remainder(const array& a, const array& b, SOD);
array floor_divide(const array& a, const array& b, SOD);
std::vector<array> divmod(const array& a, const array& b, SOD);
array maximum(const array& a, const array& b, SOD);
array minimum(const array& a, const array& b, SOD);
```

---

## Trigonometry

```cpp
array sin(const array& a, SOD);     array arcsin(const array& a, SOD);
array cos(const array& a, SOD);     array arccos(const array& a, SOD);
array tan(const array& a, SOD);     array arctan(const array& a, SOD);
array arctan2(const array& a, const array& b, SOD);
array sinh(const array& a, SOD);    array arcsinh(const array& a, SOD);
array cosh(const array& a, SOD);    array arccosh(const array& a, SOD);
array tanh(const array& a, SOD);    array arctanh(const array& a, SOD);
array degrees(const array& a, SOD); // rad -> deg
array radians(const array& a, SOD); // deg -> rad
```

---

## Logical

```cpp
array logical_not(const array& a, SOD);
array logical_and(const array& a, const array& b, SOD);
array logical_or(const array& a, const array& b, SOD);
```

---

## Bitwise

```cpp
array bitwise_and(const array& a, const array& b, SOD);
array bitwise_or(const array& a, const array& b, SOD);
array bitwise_xor(const array& a, const array& b, SOD);
array bitwise_invert(const array& a, SOD);
array left_shift(const array& a, const array& b, SOD);
array right_shift(const array& a, const array& b, SOD);
```

---

## Linear Algebra

```cpp
array matmul(const array& a, const array& b, SOD);
array addmm(array c, array a, array b,
            const float& alpha = 1.f, const float& beta = 1.f, SOD);
// Computes: beta*c + alpha*(a @ b)

array inner(const array& a, const array& b, SOD);     // dot product / inner
array outer(const array& a, const array& b, SOD);     // outer product
array kron(const array& a, const array& b, SOD);      // Kronecker product

array tensordot(const array& a, const array& b, const int axis = 2, SOD);
array tensordot(const array& a, const array& b,
                const std::vector<int>& axes_a,
                const std::vector<int>& axes_b, SOD);

array diagonal(const array& a, int offset = 0, int axis1 = 0, int axis2 = 1, SOD);
array diag(const array& a, int k = 0, SOD);           // extract diag or make diag matrix
array trace(const array& a, int offset, int axis1, int axis2, Dtype dtype, SOD);
array trace(const array& a, int offset, int axis1, int axis2, SOD);
array trace(const array& a, SOD);

// Advanced matrix ops
array block_masked_mm(array a, array b, int block_size,
                      std::optional<array> mask_out = std::nullopt,
                      std::optional<array> mask_lhs = std::nullopt,
                      std::optional<array> mask_rhs = std::nullopt, SOD);

array gather_mm(array a, array b,
                std::optional<array> lhs_indices = std::nullopt,
                std::optional<array> rhs_indices = std::nullopt,
                bool sorted_indices = false, SOD);

array segmented_mm(array a, array b, array segments, SOD);

array hadamard_transform(const array& a,
                         std::optional<float> scale = std::nullopt, SOD);
```

---

## Convolutions

```cpp
// Input layout: (N, H, W, C_in), weight: (C_out, kH, kW, C_in)
array conv1d(const array& input, const array& weight,
             int stride = 1, int padding = 0, int dilation = 1,
             int groups = 1, SOD);

array conv2d(const array& input, const array& weight,
             const std::pair<int,int>& stride = {1,1},
             const std::pair<int,int>& padding = {0,0},
             const std::pair<int,int>& dilation = {1,1},
             int groups = 1, SOD);

array conv3d(const array& input, const array& weight,
             const std::tuple<int,int,int>& stride = {1,1,1},
             const std::tuple<int,int,int>& padding = {0,0,0},
             const std::tuple<int,int,int>& dilation = {1,1,1},
             int groups = 1, SOD);

array conv_transpose1d(const array& input, const array& weight,
                       int stride = 1, int padding = 0, int dilation = 1,
                       int output_padding = 0, int groups = 1, SOD);

array conv_transpose2d(const array& input, const array& weight,
                       const std::pair<int,int>& stride = {1,1},
                       const std::pair<int,int>& padding = {0,0},
                       const std::pair<int,int>& dilation = {1,1},
                       const std::pair<int,int>& output_padding = {0,0},
                       int groups = 1, SOD);

array conv_transpose3d(const array& input, const array& weight,
                       const std::tuple<int,int,int>& stride = {1,1,1},
                       const std::tuple<int,int,int>& padding = {0,0,0},
                       const std::tuple<int,int,int>& dilation = {1,1,1},
                       const std::tuple<int,int,int>& output_padding = {0,0,0},
                       int groups = 1, SOD);

array conv_general(array input, array weight,
                   std::vector<int> stride = {},
                   std::vector<int> padding_lo = {},
                   std::vector<int> padding_hi = {},
                   std::vector<int> kernel_dilation = {},
                   std::vector<int> input_dilation = {},
                   int groups = 1, bool flip = false, SOD);
```

---

## Quantization

```cpp
std::vector<array> quantize(const array& w,
                             std::optional<int> group_size = std::nullopt,
                             std::optional<int> bits = std::nullopt,
                             const std::string& mode = "affine",
                             const std::optional<array>& global_scale = std::nullopt,
                             SOD);
// Returns: {quantized_w, scales, [biases]}

array dequantize(const array& w, const array& scales,
                 const std::optional<array>& biases = std::nullopt,
                 std::optional<int> group_size = std::nullopt,
                 std::optional<int> bits = std::nullopt,
                 const std::string& mode = "affine",
                 const std::optional<array>& global_scale = std::nullopt,
                 std::optional<Dtype> dtype = std::nullopt, SOD);

array quantized_matmul(array x, array w, array scales,
                       std::optional<array> biases = std::nullopt,
                       bool transpose = true,
                       std::optional<int> group_size = std::nullopt,
                       std::optional<int> bits = std::nullopt,
                       const std::string& mode = "affine", SOD);

array gather_qmm(const array& x, const array& w, const array& scales,
                 const std::optional<array>& biases = std::nullopt,
                 std::optional<array> lhs_indices = std::nullopt,
                 std::optional<array> rhs_indices = std::nullopt,
                 bool transpose = true,
                 std::optional<int> group_size = std::nullopt,
                 std::optional<int> bits = std::nullopt,
                 const std::string& mode = "affine",
                 bool sorted_indices = false, SOD);
```

---

## Neural Net Ops

```cpp
array softmax(const array& a, const std::vector<int>& axes, bool precise = false, SOD);
array softmax(const array& a, bool precise = false, SOD);
array softmax(const array& a, int axis, bool precise = false, SOD);

array stop_gradient(const array& a, SOD);
```

---

## Sorting and Selection

```cpp
array sort(const array& a, SOD);            // stable sort, NaN at end, flat
array sort(const array& a, int axis, SOD);
array argsort(const array& a, SOD);
array argsort(const array& a, int axis, SOD);
array partition(const array& a, int kth, SOD);
array partition(const array& a, int kth, int axis, SOD);
array argpartition(const array& a, int kth, SOD);
array argpartition(const array& a, int kth, int axis, SOD);
array topk(const array& a, int k, SOD);
array topk(const array& a, int k, int axis, SOD);
```

---

## Complex Numbers

```cpp
array real(const array& a, SOD);
array imag(const array& a, SOD);
array conjugate(const array& a, SOD);
```

---

## Window Functions

```cpp
array hanning(int M, SOD);
array hamming(int M, SOD);
array bartlett(int M, SOD);
array blackman(int M, SOD);
```

---

## Graph Utilities

```cpp
// Ensure dependencies are computed before outputs
std::vector<array> depends(const std::vector<array>& inputs,
                            const std::vector<array>& dependencies);

array number_of_elements(const array& a, std::vector<int> axes,
                          bool inverted, Dtype dtype = int32, SOD);
```

---

## Transforms (`mlx/transforms.h`)

```cpp
void eval(std::vector<array> outputs);
template<typename... Arrays> void eval(Arrays&&... outputs);

void async_eval(std::vector<array> outputs);
template<typename... Arrays> void async_eval(Arrays&&... outputs);

// VJP — reverse mode
std::pair<std::vector<array>, std::vector<array>> vjp(
    const std::function<std::vector<array>(const std::vector<array>&)>& fun,
    const std::vector<array>& primals,
    const std::vector<array>& cotangents);
std::pair<array, array> vjp(
    const std::function<array(const array&)>& fun,
    const array& primal, const array& cotangent);

// JVP — forward mode
std::pair<std::vector<array>, std::vector<array>> jvp(
    const std::function<std::vector<array>(const std::vector<array>&)>& fun,
    const std::vector<array>& primals,
    const std::vector<array>& tangents);
std::pair<array, array> jvp(
    const std::function<array(const array&)>& fun,
    const array& primal, const array& tangent);

// value_and_grad
ValueAndGradFn value_and_grad(
    const std::function<std::vector<array>(const std::vector<array>&)>& fun,
    const std::vector<int>& argnums);
ValueAndGradFn value_and_grad(
    const std::function<std::vector<array>(const std::vector<array>&)>& fun,
    int argnum = 0);
// Unary:
std::function<std::pair<array,array>(const array&)> value_and_grad(
    const std::function<array(const array&)>& fun);

// grad
std::function<std::vector<array>(const std::vector<array>&)> grad(
    const std::function<array(const std::vector<array>&)>& fun,
    const std::vector<int>& argnums);
std::function<std::vector<array>(const std::vector<array>&)> grad(
    const std::function<array(const std::vector<array>&)>& fun,
    int argnum = 0);
// Unary:
std::function<array(const array&)> grad(
    const std::function<array(const array&)>& fun);
```
