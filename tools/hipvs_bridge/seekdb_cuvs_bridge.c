/* seekdb <-> hipVS bridge: plain-C CAGRA kNN; all cuVS/ROCm deps live here.
 *
 * The bridge is a plain C ABI so the server never imports cuVS C++ types. It
 * owns one process-wide cuvsResources handle and one reusable set of device
 * buffers per index, so a query no longer pays cuvsResourcesCreate() plus three
 * device allocations, and a rebuild reuses the already warm runtime.
 */
#include "seekdb_cuvs_bridge.h"
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <dlpack/dlpack.h>
#ifdef __HIP_PLATFORM_AMD__
#include <cuvs/cuda_runtime.h>
#else
#include <cuda_runtime.h>
#endif
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Set SEEKDB_CUVS_TIMING=1 to emit per-build timings on stderr; the observer
 * log then contains the GPU build time separately from the SQL/DDL path. */
static int sk_timing_enabled(void)
{
  static int cached = -1;
  if (cached < 0) {
    const char *env = getenv("SEEKDB_CUVS_TIMING");
    cached = (env != NULL && env[0] == '1') ? 1 : 0;
  }
  return cached;
}

static double sk_now_ms(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* One cuVS runtime (stream + workspace) for the whole process. */
static pthread_once_t g_res_once = PTHREAD_ONCE_INIT;
/* cuvsResources_t is an opaque unsigned long on hipVS and a pointer on CUDA;\n * 0 is the portable null value for both. */
static cuvsResources_t g_res = 0;

static void sk_create_shared_resources(void)
{
  if (cuvsResourcesCreate(&g_res) != CUVS_SUCCESS) {
    g_res = 0;
  }
}

static cuvsResources_t sk_resources(void)
{
  pthread_once(&g_res_once, sk_create_shared_resources);
  return g_res;
}

typedef struct {
  cuvsResources_t        res;
  cuvsCagraIndexParams_t ip;
  cuvsCagraIndex_t       idx;
  cuvsCagraSearchParams_t sp;
  long n, dim;
  /* Reusable device scratch for search (grown on demand, freed with the index). */
  float    *q_dev;
  unsigned *n_dev;
  float    *d_dev;
  long      cap_nq;
  long      cap_topk;
} sk_cuvs_index;

static void sk_free_buffers(sk_cuvs_index *h)
{
  if (h->q_dev) { cuvsRMMFree(h->res, h->q_dev, sizeof(float) * h->cap_nq * h->dim); h->q_dev = NULL; }
  if (h->n_dev) { cuvsRMMFree(h->res, h->n_dev, sizeof(unsigned) * h->cap_nq * h->cap_topk); h->n_dev = NULL; }
  if (h->d_dev) { cuvsRMMFree(h->res, h->d_dev, sizeof(float) * h->cap_nq * h->cap_topk); h->d_dev = NULL; }
  h->cap_nq = 0;
  h->cap_topk = 0;
}

/* Grow the search scratch so it can hold at least nq x topk results. */
static int sk_reserve(sk_cuvs_index *h, long nq, long topk)
{
  int rc = 0;
  if (nq <= h->cap_nq && topk <= h->cap_topk && h->q_dev && h->n_dev && h->d_dev) {
    return 0;
  }
  long cap_nq = nq > h->cap_nq ? nq : h->cap_nq;
  long cap_topk = topk > h->cap_topk ? topk : h->cap_topk;
  float *q_dev = NULL;
  unsigned *n_dev = NULL;
  float *d_dev = NULL;
  if (cuvsRMMAlloc(h->res, (void **)&q_dev, sizeof(float) * cap_nq * h->dim) != CUVS_SUCCESS) { rc = 10; goto done; }
  if (cuvsRMMAlloc(h->res, (void **)&n_dev, sizeof(unsigned) * cap_nq * cap_topk) != CUVS_SUCCESS) { rc = 11; goto done; }
  if (cuvsRMMAlloc(h->res, (void **)&d_dev, sizeof(float) * cap_nq * cap_topk) != CUVS_SUCCESS) { rc = 12; goto done; }
  sk_free_buffers(h);
  h->q_dev = q_dev;
  h->n_dev = n_dev;
  h->d_dev = d_dev;
  h->cap_nq = cap_nq;
  h->cap_topk = cap_topk;
  return 0;
done:
  if (h->res) {
    if (q_dev) cuvsRMMFree(h->res, q_dev, sizeof(float) * cap_nq * h->dim);
    if (n_dev) cuvsRMMFree(h->res, n_dev, sizeof(unsigned) * cap_nq * cap_topk);
    if (d_dev) cuvsRMMFree(h->res, d_dev, sizeof(float) * cap_nq * cap_topk);
  }
  return rc;
}

void* seekdb_cuvs_build(const float* base, long n, long dim)
{
  sk_cuvs_index* h = (sk_cuvs_index*)calloc(1, sizeof(*h));
  if (!h) return NULL;
  h->res = sk_resources();
  if (h->res == 0) { free(h); return NULL; }

  DLManagedTensor dset; long ds[2] = {n, dim};
  dset.dl_tensor.data = (void*)base;
  dset.dl_tensor.device.device_type = kDLCPU; dset.dl_tensor.device.device_id = 0;
  dset.dl_tensor.ndim = 2;
  dset.dl_tensor.dtype.code = kDLFloat; dset.dl_tensor.dtype.bits = 32; dset.dl_tensor.dtype.lanes = 1;
  dset.dl_tensor.shape = ds; dset.dl_tensor.strides = 0; dset.dl_tensor.byte_offset = 0;

  double started = sk_timing_enabled() ? sk_now_ms() : 0.0;
  if (cuvsCagraIndexParamsCreate(&h->ip) != CUVS_SUCCESS) goto fail_res;
  if (cuvsCagraIndexCreate(&h->idx)      != CUVS_SUCCESS) goto fail_ip;
  if (cuvsCagraBuild(h->res, h->ip, &dset, h->idx) != CUVS_SUCCESS) goto fail_idx;
  if (cuvsCagraSearchParamsCreate(&h->sp) != CUVS_SUCCESS) goto fail_idx;
  h->n = n; h->dim = dim;
  if (sk_timing_enabled()) {
    fprintf(stderr, "[seekdb_cuvs] build n=%ld dim=%ld ms=%.1f\n", n, dim, sk_now_ms() - started);
    fflush(stderr);
  }
  return h;

fail_idx: cuvsCagraIndexDestroy(h->idx);
fail_ip:  cuvsCagraIndexParamsDestroy(h->ip);
fail_res: free(h);
  fprintf(stderr, "[seekdb_cuvs] build failed: %s\n", cuvsGetLastErrorText());
  return NULL;
}

int seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                       unsigned int* out_ids, float* out_dist)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return 1;
  int rc = 0;
  double started = sk_timing_enabled() ? sk_now_ms() : 0.0;
  if (sk_reserve(h, nq, topk) != 0) { rc = 9; goto done; }
  if (cudaMemcpy(h->q_dev, query, sizeof(float)*nq*h->dim, cudaMemcpyDefault) != cudaSuccess) { rc=13; goto done; }
  {
    DLManagedTensor qt, nt, dt; long qs[2]={nq,h->dim}, nsh[2]={nq,topk}, dsh[2]={nq,topk};
    qt.dl_tensor.data=h->q_dev; qt.dl_tensor.device.device_type=kDLCUDA; qt.dl_tensor.device.device_id=0;
    qt.dl_tensor.ndim=2; qt.dl_tensor.dtype.code=kDLFloat; qt.dl_tensor.dtype.bits=32; qt.dl_tensor.dtype.lanes=1;
    qt.dl_tensor.shape=qs; qt.dl_tensor.strides=0; qt.dl_tensor.byte_offset=0;
    nt=qt; nt.dl_tensor.data=h->n_dev; nt.dl_tensor.dtype.code=kDLUInt; nt.dl_tensor.shape=nsh;
    dt=qt; dt.dl_tensor.data=h->d_dev; dt.dl_tensor.dtype.code=kDLFloat; dt.dl_tensor.shape=dsh;
    cuvsFilter f; f.type=NO_FILTER; f.addr=(uintptr_t)0;
    if (cuvsCagraSearch(h->res, h->sp, h->idx, &qt, &nt, &dt, f) != CUVS_SUCCESS) { rc=15; goto done; }
    if (cudaMemcpy(out_ids, h->n_dev, sizeof(unsigned)*nq*topk, cudaMemcpyDefault) != cudaSuccess) { rc=16; goto done; }
    if (out_dist && cudaMemcpy(out_dist, h->d_dev, sizeof(float)*nq*topk, cudaMemcpyDefault) != cudaSuccess) {
      rc=17; goto done;
    }
  }
  if (sk_timing_enabled()) {
    fprintf(stderr, "[seekdb_cuvs] search nq=%ld topk=%ld ms=%.2f\n", nq, topk, sk_now_ms() - started);
    fflush(stderr);
  }
done:
  if (rc) fprintf(stderr, "[seekdb_cuvs] search rc=%d: %s\n", rc, cuvsGetLastErrorText());
  return rc;
}

void seekdb_cuvs_free(void* handle)
{
  sk_cuvs_index* h = (sk_cuvs_index*)handle;
  if (!h) return;
  sk_free_buffers(h);
  if (h->sp) { cuvsCagraSearchParamsDestroy(h->sp); }
  cuvsCagraIndexDestroy(h->idx);
  cuvsCagraIndexParamsDestroy(h->ip);
  free(h);
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
