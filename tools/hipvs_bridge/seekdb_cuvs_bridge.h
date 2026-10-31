#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* One-shot: build a CAGRA index over base (n x dim, row-major f32) on the GPU and
 * search queries (nq x dim), writing top-k uint32 neighbor ids to out_ids
 * (length nq*topk). Returns 0 on success, non-zero on error. */
int seekdb_cuvs_cagra_knn(const float* base, long n, long dim,
                          const float* query, long nq, long topk,
                          unsigned int* out_ids);

/* Build-once / search-many (used by seekdb's ob_vsag_adaptor GPU path).
 *  - seekdb_cuvs_build: builds a CAGRA index on the GPU from base (n x dim,
 *    row-major f32) and returns an opaque handle (NULL on error). cuVS copies
 *    the dataset to the device, so `base` may be freed after this returns.
 *  - seekdb_cuvs_search: runs top-k search for nq queries (row-major f32, dim
 *    cols), writing uint32 row offsets to out_ids[nq*topk] and (if non-NULL) L2
 *    distances to out_dist[nq*topk]. Returns 0 on success.
 *  - seekdb_cuvs_free: releases the handle.
 * All ROCm/cuVS state lives inside the .so; seekdb only ever sees these C symbols. */
void* seekdb_cuvs_build(const float* base, long n, long dim);
int   seekdb_cuvs_search(void* handle, const float* query, long nq, long topk,
                         unsigned int* out_ids, float* out_dist);
void  seekdb_cuvs_free(void* handle);
int seekdb_cuvs_serialize(void* handle, const char* filename);
void* seekdb_cuvs_deserialize(const char* filename, long n, long dim);
/* Raw graph snapshots use only the public CAGRA C API. Graph entries are
 * uint32 row offsets into the unchanged, row-major FP32 dataset. Export never
 * transfers ownership of cuVS device memory. Import copies both host arrays
 * into memory owned by cuVS or this bridge and synchronizes before returning;
 * the caller can release both input arrays immediately afterwards. */
int seekdb_cuvs_graph_degree(void* handle, long* graph_degree);
int seekdb_cuvs_export_graph(void* handle, unsigned int* graph, size_t graph_elements);
void* seekdb_cuvs_import_graph(const float* base, long n, long dim,
                              const unsigned int* graph, long graph_degree);
#ifdef __cplusplus
}
#endif
