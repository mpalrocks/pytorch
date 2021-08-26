#pragma once

#include <ATen/cuda/CUDAContext.h>
#include <ATen/cuda/CUDADataType.h>
#include <ATen/cuda/CUDASparse.h>
#include <ATen/native/LinearAlgebraUtils.h>
#include <ATen/native/cuda/MiscUtils.h>

#include <c10/core/ScalarType.h>

#if AT_USE_CUSPARSE_GENERIC_API()

namespace at {
namespace cuda {
namespace sparse {

namespace {

// If a specific GPU model does not provide native support for a given data
// type, cuSparse routines return CUSPARSE_STATUS_ARCH_MISMATCH error
void check_supported_cuda_type(cudaDataType cuda_type) {
  if (cuda_type == CUDA_R_16F) {
    cudaDeviceProp* prop = at::cuda::getCurrentDeviceProperties();
    TORCH_CHECK(
        prop->major >= 5 && ((10 * prop->major + prop->minor) >= 53),
        "Sparse operations with CUDA tensors of Float16 type are not supported on GPUs with compute capability < 5.3 (current: ",
        prop->major,
        ".",
        prop->minor,
        ")");
  }
#if defined(CUDA_VERSION) && CUDA_VERSION >= 11000
  if (cuda_type == CUDA_R_16BF) {
    cudaDeviceProp* prop = at::cuda::getCurrentDeviceProperties();
    TORCH_CHECK(
        prop->major >= 8,
        "Sparse operations with CUDA tensors of BFloat16 type are not supported on GPUs with compute capability < 8.0 (current: ",
        prop->major,
        ".",
        prop->minor,
        ")");
  }
#endif
}

} // anonymous namespace

inline cusparseIndexType_t getCuSparseIndexType(const c10::ScalarType& scalar_type) {
  if (scalar_type == c10::ScalarType::Int) {
    return CUSPARSE_INDEX_32I;
  } else if (scalar_type == c10::ScalarType::Long) {
    return CUSPARSE_INDEX_64I;
  } else {
    TORCH_INTERNAL_ASSERT(
        false,
        "Cannot convert type ",
        scalar_type,
        " to cusparseIndexType.");
  }
}

template <typename T, cusparseStatus_t (*destructor)(T*)>
struct CuSparseDescriptorDeleter {
  void operator()(T* x) {
    if (x != nullptr) {
      TORCH_CUDASPARSE_CHECK(destructor(x));
    }
  }
};

template <typename T, cusparseStatus_t (*destructor)(T*)>
class CuSparseDescriptor {
 public:
  T* descriptor() const {
    return descriptor_.get();
  }
  T* descriptor() {
    return descriptor_.get();
  }

 protected:
  std::unique_ptr<T, CuSparseDescriptorDeleter<T, destructor>> descriptor_;
};

class TORCH_CUDA_CPP_API CuSparseDnMatDescriptor
    : public CuSparseDescriptor<cusparseDnMatDescr, &cusparseDestroyDnMat> {
 public:
  CuSparseDnMatDescriptor(const Tensor& input) {
    TORCH_INTERNAL_ASSERT(input.layout() == kStrided);
    IntArrayRef input_strides = input.strides();
    IntArrayRef input_sizes = input.sizes();
    auto ndim = input.dim();
    TORCH_INTERNAL_ASSERT(ndim >= 2);
    auto rows = input_sizes[ndim - 2];
    auto cols = input_sizes[ndim - 1];

    bool is_column_major = at::native::is_blas_compatible_column_major_order(input);
    bool is_row_major = at::native::is_blas_compatible_row_major_order(input);
    TORCH_INTERNAL_ASSERT(
        is_column_major || is_row_major,
        "Expected either row or column major contiguous input.");

    auto leading_dimension = is_row_major ? input_strides[ndim - 2] : input_strides[ndim - 1];
    auto order = is_row_major ? CUSPARSE_ORDER_ROW : CUSPARSE_ORDER_COL;

    void* values_ptr = input.data_ptr();

    cudaDataType value_type = ScalarTypeToCudaDataType(input.scalar_type());
    check_supported_cuda_type(value_type);

    cusparseDnMatDescr_t raw_descriptor;
    TORCH_CUDASPARSE_CHECK(cusparseCreateDnMat(
        &raw_descriptor,
        rows,
        cols,
        leading_dimension,
        values_ptr,
        value_type,
        order));

    if (ndim > 2) {
      auto batch_count = at::native::cuda_int_cast(at::native::batchCount(input), "batch_count");
      auto batch_stride = input_strides[ndim - 3];
      TORCH_CUDASPARSE_CHECK(cusparseDnMatSetStridedBatch(
          raw_descriptor, batch_count, batch_stride));
    }

    descriptor_.reset(raw_descriptor);
  }
};

class TORCH_CUDA_CPP_API CuSparseDnVecDescriptor
    : public CuSparseDescriptor<cusparseDnVecDescr, &cusparseDestroyDnVec> {
 public:
  CuSparseDnVecDescriptor(const Tensor& input) {
    // cuSPARSE doesn't support batched vectors
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(input.dim() == 1 || (input.dim() == 2 && input.size(-1) == 1));

    // cuSPARSE doesn't support non-contiguous vectors
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(input.is_contiguous());
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(input.is_non_overlapping_and_dense());

    cudaDataType value_type = ScalarTypeToCudaDataType(input.scalar_type());
    check_supported_cuda_type(value_type);

    cusparseDnVecDescr_t raw_descriptor;
    TORCH_CUDASPARSE_CHECK(cusparseCreateDnVec(
        &raw_descriptor,
        input.numel(),
        input.data_ptr(),
        value_type));
    descriptor_.reset(raw_descriptor);
  }
};

class TORCH_CUDA_CPP_API CuSparseSpMatDescriptor
    : public CuSparseDescriptor<cusparseSpMatDescr, &cusparseDestroySpMat> {};

class TORCH_CUDA_CPP_API CuSparseSpMatCsrDescriptor
    : public CuSparseSpMatDescriptor {
 public:
  CuSparseSpMatCsrDescriptor(const Tensor& input) {
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(input.is_sparse_csr());
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(input.dim() >= 2);

    IntArrayRef input_sizes = input.sizes();
    auto ndim = input.dim();
    auto rows = input_sizes[ndim - 2];
    auto cols = input_sizes[ndim - 1];
    auto nnz = input._nnz();

    auto crow_indices = input.crow_indices();
    auto col_indices = input.col_indices();
    auto values = input.values();

    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(crow_indices.is_contiguous());
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(col_indices.is_contiguous());
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(values.is_contiguous());

    cusparseIndexType_t index_type = getCuSparseIndexType(crow_indices.scalar_type());
    cudaDataType value_type = ScalarTypeToCudaDataType(input.scalar_type());
    check_supported_cuda_type(value_type);

    cusparseSpMatDescr_t raw_descriptor;
    TORCH_CUDASPARSE_CHECK(cusparseCreateCsr(
        &raw_descriptor, // output descriptor
        rows,
        cols,
        nnz,
        crow_indices.data_ptr(), // row offsets of the sparse matrix, size = rows + 1
        col_indices.data_ptr(), // column indices of the sparse matrix, size = nnz
        values.data_ptr(), // values of the sparse matrix, size = nnz
        index_type, // data type of row offsets index
        index_type, // data type of col indices
        CUSPARSE_INDEX_BASE_ZERO, // base index of row offset and col indes
        value_type // data type of values
        ));

#if defined(CUDA_VERSION) && CUDA_VERSION >= 11000
    if (ndim > 2) {
      TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
          at::native::batchCount(input) == at::native::batchCount(values));
      TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
          at::native::batchCount(input) == at::native::batchCount(crow_indices));
      TORCH_INTERNAL_ASSERT_DEBUG_ONLY(
          at::native::batchCount(input) == at::native::batchCount(col_indices));
      auto batch_count = at::native::cuda_int_cast(at::native::batchCount(input), "batch_count");
      auto crow_indices_batch_stride = crow_indices.stride(-2);
      auto columns_values_batch_stride = values.stride(-2);
      TORCH_CUDASPARSE_CHECK(cusparseCsrSetStridedBatch(
          raw_descriptor,
          batch_count,
          crow_indices_batch_stride,
          columns_values_batch_stride));
    }
#endif

    descriptor_.reset(raw_descriptor);
  }

#if defined(CUDA_VERSION) && CUDA_VERSION >= 11000
  std::tuple<int64_t, int64_t, int64_t> get_size() {
    int64_t rows, cols, nnz;
    TORCH_CUDASPARSE_CHECK(cusparseSpMatGetSize(
        this->descriptor(),
        &rows,
        &cols,
        &nnz));
    return std::make_tuple(rows, cols, nnz);
  }

  void set_tensor(const Tensor& input) {
    auto crow_indices = input.crow_indices();
    auto col_indices = input.col_indices();
    auto values = input.values();

    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(crow_indices.is_contiguous());
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(col_indices.is_contiguous());
    TORCH_INTERNAL_ASSERT_DEBUG_ONLY(values.is_contiguous());
    TORCH_CUDASPARSE_CHECK(cusparseCsrSetPointers(
        this->descriptor(),
        crow_indices.data_ptr(),
        col_indices.data_ptr(),
        values.data_ptr()));
  }
#endif

#if AT_USE_CUSPARSE_GENERIC_SPSV()
  void set_mat_fill_mode(bool upper) {
    cusparseFillMode_t fill_mode =
        upper ? CUSPARSE_FILL_MODE_UPPER : CUSPARSE_FILL_MODE_LOWER;
    TORCH_CUDASPARSE_CHECK(cusparseSpMatSetAttribute(
        this->descriptor(),
        CUSPARSE_SPMAT_FILL_MODE,
        &fill_mode,
        sizeof(fill_mode)));
  }

  void set_mat_diag_type(bool unit) {
    cusparseDiagType_t diag_type =
        unit ? CUSPARSE_DIAG_TYPE_UNIT : CUSPARSE_DIAG_TYPE_NON_UNIT;
    TORCH_CUDASPARSE_CHECK(cusparseSpMatSetAttribute(
        this->descriptor(),
        CUSPARSE_SPMAT_DIAG_TYPE,
        &diag_type,
        sizeof(diag_type)));
  }
#endif
};

#if AT_USE_CUSPARSE_GENERIC_SPSV()
class TORCH_CUDA_CPP_API CuSparseSpSVDescriptor
    : public CuSparseDescriptor<cusparseSpSVDescr, &cusparseSpSV_destroyDescr> {
 public:
  CuSparseSpSVDescriptor() {
    cusparseSpSVDescr_t raw_descriptor;
    TORCH_CUDASPARSE_CHECK(cusparseSpSV_createDescr(&raw_descriptor));
    descriptor_.reset(raw_descriptor);
  }
};
#endif

#if AT_USE_CUSPARSE_GENERIC_SPSM()
class TORCH_CUDA_CPP_API CuSparseSpSMDescriptor
    : public CuSparseDescriptor<cusparseSpSMDescr, &cusparseSpSM_destroyDescr> {
 public:
  CuSparseSpSMDescriptor() {
    cusparseSpSMDescr_t raw_descriptor;
    TORCH_CUDASPARSE_CHECK(cusparseSpSM_createDescr(&raw_descriptor));
    descriptor_.reset(raw_descriptor);
  }
};
#endif

#if defined(CUDA_VERSION) && CUDA_VERSION >= 11000
class TORCH_CUDA_CPP_API CuSparseSpGEMMDescriptor
    : public CuSparseDescriptor<cusparseSpGEMMDescr, &cusparseSpGEMM_destroyDescr> {
 public:
  CuSparseSpGEMMDescriptor() {
    cusparseSpGEMMDescr_t raw_descriptor;
    TORCH_CUDASPARSE_CHECK(cusparseSpGEMM_createDescr(&raw_descriptor));
    descriptor_.reset(raw_descriptor);
  }
};
#endif

class TORCH_CUDA_CPP_API CuSparseMatDescriptor
    : public CuSparseDescriptor<cusparseMatDescr, &cusparseDestroyMatDescr> {
 public:
  CuSparseMatDescriptor() {
    cusparseMatDescr_t raw_descriptor;
    TORCH_CUDASPARSE_CHECK(cusparseCreateMatDescr(&raw_descriptor));
    descriptor_.reset(raw_descriptor);
  }
};

} // namespace sparse
} // namespace cuda
} // namespace at

#endif // AT_USE_CUSPARSE_GENERIC_API()
