/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here. */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  long n, dim;
  /* Search scratch belongs to this index, not to a benchmark or process cache.
   * Exact high-water capacities avoid geometric VRAM over-allocation. */
  float* query_d;
  unsigned* ids_d;
  float* distances_d;
  size_t query_bytes, ids_bytes, distances_bytes;
  cuvsCagraSearchParams_t search;
  pthread_mutex_t search_mutex;
} sk_cuvs_index;

static sk_cuvs_index* seekdb_cuvs_create_handle(void)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (h && pthread_mutex_init(&h->search_mutex, NULL) != 0) {
    free(h);
    h = NULL;
  }
  return h;
}

static int seekdb_cuvs_bytes(long rows, long cols, size_t element_bytes, size_t* bytes)
{
  if (rows <= 0 || cols <= 0 || (size_t)rows > SIZE_MAX / (size_t)cols ||
      (size_t)rows * (size_t)cols > SIZE_MAX / element_bytes) return 0;
  *bytes = (size_t)rows * (size_t)cols * element_bytes;
  return 1;
}

/* Searches are synchronous, so an old scratch allocation has no outstanding
 * users here. Free before growing to avoid retaining old+new VRAM at the peak.
 * Failed growth leaves a NULL slot that a later call can safely retry. */
static int seekdb_cuvs_reserve(sk_cuvs_index* h, void** slot, size_t* capacity, size_t bytes)
{
  if (bytes <= *capacity) return 1;
  if (*slot) {
    if (cuvsRMMFree(h->res, *slot, *capacity) != CUVS_SUCCESS) return 0;
    *slot = NULL;
    *capacity = 0;
  }
  void* replacement = NULL;
  if (cuvsRMMAlloc(h->res, &replacement, bytes) != CUVS_SUCCESS) {
    if (replacement) cuvsRMMFree(h->res, replacement, bytes);
    return 0;
  }
  *slot = replacement;
  *capacity = bytes;
  return 1;
}

static void seekdb_cuvs_release_scratch(sk_cuvs_index* h)
{
  if (h->search) cuvsCagraSearchParamsDestroy(h->search);
  if (h->distances_d) cuvsRMMFree(h->res, h->distances_d, h->distances_bytes);
  if (h->ids_d) cuvsRMMFree(h->res, h->ids_d, h->ids_bytes);
  if (h->query_d) cuvsRMMFree(h->res, h->query_d, h->query_bytes);
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  if (!base || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* h = seekdb_cuvs_create_handle();
  if (!h) return NULL;
  if (cuvsResourcesCreate(&h->res) != CUVS_SUCCESS) {
    pthread_mutex_destroy(&h->search_mutex); free(h); return NULL;
  }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  h->ip->build_algo = NN_DESCENT;
  h->ip->graph_degree = 128;
  h->ip->intermediate_graph_degree = 256;
  /* Bound graph refinement; the broader official MULTI_CTA search below
   * preserves query quality without changing FP32 inputs or graph degree. */
  h->ip->nn_descent_niter = 20;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: cuvsResourcesDestroy(h->res); pthread_mutex_destroy(&h->search_mutex); free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  size_t query_bytes = 0, ids_bytes = 0, distances_bytes = 0;
  if (!query || !out_ids || topk > h->n || (size_t)topk > SIZE_MAX / 4 ||
      !seekdb_cuvs_bytes(nq, h->dim, sizeof(float), &query_bytes) ||
      !seekdb_cuvs_bytes(nq, topk, sizeof(unsigned), &ids_bytes) ||
      !seekdb_cuvs_bytes(nq, topk, sizeof(float), &distances_bytes)) return 2;
  if (pthread_mutex_lock(&h->search_mutex) != 0) return 3;
  int rc = 0;
  if (!seekdb_cuvs_reserve(h, (void**)&h->query_d, &h->query_bytes, query_bytes)) { rc=10; goto done; }
  if (!seekdb_cuvs_reserve(h, (void**)&h->ids_d, &h->ids_bytes, ids_bytes)) { rc=11; goto done; }
  if (!seekdb_cuvs_reserve(h, (void**)&h->distances_d, &h->distances_bytes, distances_bytes)) { rc=12; goto done; }
  if (cudaMemcpy(h->query_d, query, query_bytes, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->query_d; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->ids_d; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->distances_d; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    if (!h->search && (cuvsCagraSearchParamsCreate(&h->search) != CUVS_SUCCESS || !h->search)) {
      if (h->search) cuvsCagraSearchParamsDestroy(h->search);
      h->search = NULL;
      rc=14; goto done;
    }
    {
      size_t required_itopk = (size_t)topk * 4;
      if (required_itopk < 1024) required_itopk = 1024;
      h->search->itopk_size = required_itopk;
      h->search->search_width = 4;
      h->search->algo = MULTI_CTA;
    }
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->search, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, h->ids_d, ids_bytes, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpy(out_dist, h->distances_d, distances_bytes, cudaMemcpyDefault) != cudaSuccess) {
      rc=17; goto done;
    }
  }
done:
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  pthread_mutex_unlock(&h->search_mutex);
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  seekdb_cuvs_release_scratch(h);
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  cuvsResourcesDestroy(h->res);
  pthread_mutex_destroy(&h->search_mutex);
  free(h);
}

int seekdb_cuvs_serialize(void* handle, const char* filename)
{
  sk_cuvs_index* index = (sk_cuvs_index*)handle;
  if (!index || !filename) return 1;
  if (cuvsCagraSerialize(index->res, filename, index->idx, true) != CUVS_SUCCESS) {
    fprintf(stderr, "[seekdb_cuvs] serialize failed: %s\n", cuvsGetLastErrorText());
    return 1;
  }
  return 0;
}

void* seekdb_cuvs_deserialize(const char* filename, long n, long dim)
{
  if (!filename || n <= 0 || dim <= 0) return NULL;
  sk_cuvs_index* index = seekdb_cuvs_create_handle();
  if (!index) return NULL;
  if (cuvsResourcesCreate(&index->res) != CUVS_SUCCESS) goto fail;
  if (cuvsCagraIndexParamsCreate(&index->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&index->idx) != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraDeserialize(index->res, filename, index->idx) != CUVS_SUCCESS) goto fail_idx;
  {
    int64_t actual_n = 0;
    int64_t actual_dim = 0;
    if (cuvsCagraIndexGetSize(index->idx, &actual_n) != CUVS_SUCCESS ||
        cuvsCagraIndexGetDims(index->idx, &actual_dim) != CUVS_SUCCESS ||
        actual_n != n || actual_dim != dim) {
      fprintf(stderr, "[seekdb_cuvs] snapshot shape mismatch: %ld x %ld, expected %ld x %ld\n",
              (long)actual_n, (long)actual_dim, n, dim);
      goto fail_idx;
    }
  }
  index->n = n;
  index->dim = dim;
  return index;
fail_idx:
  cuvsCagraIndexDestroy(index->idx);
fail_ip:
  cuvsCagraIndexParamsDestroy(index->ip);
fail_res:
  cuvsResourcesDestroy(index->res);
fail:
  fprintf(stderr, "[seekdb_cuvs] deserialize failed: %s\n", cuvsGetLastErrorText());
  pthread_mutex_destroy(&index->search_mutex);
  free(index);
  return NULL;
}

int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids)
{
  void* h = seekdb_cuvs_build(base, n, dim);
  if (!h) return 10;
  int rc = seekdb_cuvs_search(h, query, nq, topk, out_ids, NULL);
  seekdb_cuvs_free(h);
  return rc;
}
