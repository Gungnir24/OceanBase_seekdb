/* Host-only lifecycle test: every GPU API below is replaced by a mock.
 * This verifies allocation reuse/failure recovery; it is not a GPU quality test.
 * Compile against the installed hipVS/ROCm headers, without GPU libraries. */
#include <cuvs/core/c_api.h>
#include <cuvs/neighbors/cagra.h>
#include <cuvs/cuda_runtime.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>
#include <unistd.h>

static cudaError_t mock_memcpy(void *, const void *, size_t, hipMemcpyKind);
#undef cudaMemcpy
#define cudaMemcpy mock_memcpy
#include "../hipvs_bridge/seekdb_cuvs_bridge.c"

namespace {
struct Shape { int64_t n = 1000, dim = 128; };
std::mutex allocation_mutex;
std::unordered_map<void *, size_t> allocations;
std::atomic<int> alloc_calls{0}, free_calls{0}, params_created{0}, params_destroyed{0};
std::atomic<int> fail_alloc{0}, fail_free{0}, fail_params{0}, fail_search{0};
std::atomic<int> memcpy_calls{0}, fail_memcpy_at{0}, active_searches{0};
}

extern "C" {
const char *cuvsGetLastErrorText() { return "injected host mock failure"; }
cuvsError_t cuvsResourcesCreate(cuvsResources_t *res)
{ *res = reinterpret_cast<uintptr_t>(malloc(1)); return CUVS_SUCCESS; }
cuvsError_t cuvsResourcesDestroy(cuvsResources_t res)
{ free(reinterpret_cast<void *>(res)); return CUVS_SUCCESS; }
cuvsError_t cuvsRMMAlloc(cuvsResources_t, void **ptr, size_t bytes)
{
  *ptr = malloc(bytes);
  assert(*ptr != nullptr);
  { std::lock_guard<std::mutex> lock(allocation_mutex); allocations[*ptr] = bytes; }
  ++alloc_calls;
  // Deliberately expose a partially allocated result on failure.
  return fail_alloc.exchange(0) ? CUVS_ERROR : CUVS_SUCCESS;
}
cuvsError_t cuvsRMMFree(cuvsResources_t, void *ptr, size_t bytes)
{
  if (fail_free.exchange(0)) { return CUVS_ERROR; }
  { std::lock_guard<std::mutex> lock(allocation_mutex);
    assert(allocations.at(ptr) == bytes); allocations.erase(ptr); }
  ++free_calls;
  free(ptr);
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexParamsCreate(cuvsCagraIndexParams_t *params)
{ *params = new cuvsCagraIndexParams{}; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexParamsDestroy(cuvsCagraIndexParams_t params)
{ delete params; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexCreate(cuvsCagraIndex_t *index)
{
  *index = new cuvsCagraIndex{};
  (*index)->addr = reinterpret_cast<uintptr_t>(new Shape{});
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraIndexDestroy(cuvsCagraIndex_t index)
{ delete reinterpret_cast<Shape *>(index->addr); delete index; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraBuild(cuvsResources_t, cuvsCagraIndexParams_t params,
                           DLManagedTensor *dataset, cuvsCagraIndex_t index)
{
  assert(params->build_algo == NN_DESCENT && params->graph_degree == 128);
  assert(params->intermediate_graph_degree == 256 && params->nn_descent_niter == 20);
  auto *shape = reinterpret_cast<Shape *>(index->addr);
  shape->n = dataset->dl_tensor.shape[0]; shape->dim = dataset->dl_tensor.shape[1];
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearchParamsCreate(cuvsCagraSearchParams_t *params)
{
  if (fail_params.exchange(0)) { *params = nullptr; return CUVS_ERROR; }
  *params = new cuvsCagraSearchParams{};
  (*params)->itopk_size = 64;
  ++params_created;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSearchParamsDestroy(cuvsCagraSearchParams_t params)
{ ++params_destroyed; delete params; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraSearch(cuvsResources_t, cuvsCagraSearchParams_t params,
                            cuvsCagraIndex_t index, DLManagedTensor *queries,
                            DLManagedTensor *neighbors, DLManagedTensor *distances, cuvsFilter)
{
  if (fail_search.exchange(0)) { return CUVS_ERROR; }
  const auto *shape = reinterpret_cast<Shape *>(index->addr);
  const int64_t nq = queries->dl_tensor.shape[0], k = neighbors->dl_tensor.shape[1];
  assert(params->itopk_size == std::max<size_t>(1024, k * 4));
  assert(params->search_width == 4 && params->algo == MULTI_CTA);
  assert(active_searches.fetch_add(1) == 0);
  usleep(10);
  for (int64_t q = 0; q < nq; ++q) {
    const unsigned seed = static_cast<const float *>(queries->dl_tensor.data)[q * shape->dim];
    for (int64_t j = 0; j < k; ++j) {
      static_cast<unsigned *>(neighbors->dl_tensor.data)[q * k + j] = (seed + j) % shape->n;
      static_cast<float *>(distances->dl_tensor.data)[q * k + j] = seed + j;
    }
  }
  --active_searches;
  return CUVS_SUCCESS;
}
cuvsError_t cuvsCagraSerialize(cuvsResources_t, const char *, cuvsCagraIndex_t, bool)
{ return CUVS_SUCCESS; }
cuvsError_t cuvsCagraDeserialize(cuvsResources_t, const char *, cuvsCagraIndex_t)
{ return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexGetSize(cuvsCagraIndex_t index, int64_t *n)
{ *n = reinterpret_cast<Shape *>(index->addr)->n; return CUVS_SUCCESS; }
cuvsError_t cuvsCagraIndexGetDims(cuvsCagraIndex_t index, int64_t *dim)
{ *dim = reinterpret_cast<Shape *>(index->addr)->dim; return CUVS_SUCCESS; }
}

static cudaError_t mock_memcpy(void *destination, const void *source, size_t bytes, hipMemcpyKind)
{
  const int call = ++memcpy_calls;
  if (call == fail_memcpy_at.load()) { return hipErrorInvalidValue; }
  memcpy(destination, source, bytes);
  return cudaSuccess;
}

static int search(void *handle, int nq, int k, int seed = 10)
{
  std::vector<float> queries(nq * 128, static_cast<float>(seed));
  std::vector<unsigned> ids(nq * k);
  std::vector<float> distances(nq * k);
  int rc = seekdb_cuvs_search(handle, queries.data(), nq, k, ids.data(), distances.data());
  if (rc == 0) {
    for (int q = 0; q < nq; ++q) for (int j = 0; j < k; ++j) {
      assert(ids[q * k + j] == static_cast<unsigned>((seed + j) % 1000));
      assert(distances[q * k + j] == seed + j);
    }
  }
  return rc;
}

int main()
{
  std::vector<float> base(1000 * 128, 0);
  void *handle = seekdb_cuvs_build(base.data(), 1000, 128);
  assert(handle);
  assert(search(handle, 1, 10) == 0);
  const int initial_allocs = alloc_calls;
  for (int i = 0; i < 10; ++i) { assert(search(handle, 1, 10) == 0); }
  assert(alloc_calls == initial_allocs && free_calls == 0 && params_created == 1);
  assert(search(handle, 1, 100) == 0);
  assert(alloc_calls == initial_allocs + 2 && free_calls == 2);
  assert(search(handle, 1, 200) == 0);
  assert(search(handle, 1, 10) == 0); // Search effort resets after a larger k.
  int before = alloc_calls;
  fail_free = 1;
  assert(search(handle, 10, 100) == 10);
  assert(search(handle, 10, 100) == 0);
  assert(alloc_calls == before + 3);
  fail_alloc = 1;
  assert(search(handle, 20, 100) == 10);
  assert(search(handle, 20, 100) == 0);
  fail_search = 1;
  assert(search(handle, 1, 10) == 15 && search(handle, 1, 10) == 0);
  memcpy_calls = 0; fail_memcpy_at = 3;
  assert(search(handle, 1, 10) == 17);
  fail_memcpy_at = 0;
  assert(search(handle, 1, 10) == 0);
  size_t bytes = 0;
  assert(!seekdb_cuvs_bytes(LONG_MAX, LONG_MAX, sizeof(float), &bytes));
  unsigned id = 0;
  assert(seekdb_cuvs_search(handle, base.data(), LONG_MAX, 10, &id, nullptr) == 2);
  assert(seekdb_cuvs_search(handle, base.data(), 1, 1001, &id, nullptr) == 2);
  std::vector<std::thread> callers;
  for (int thread = 0; thread < 8; ++thread) {
    callers.emplace_back([&, thread] {
      for (int i = 0; i < 10; ++i) { assert(search(handle, 1, i % 2 ? 100 : 10, thread * 10) == 0); }
    });
  }
  for (auto &caller : callers) { caller.join(); }
  seekdb_cuvs_free(handle);
  handle = seekdb_cuvs_deserialize("host-mock-only", 1000, 128);
  assert(handle);
  fail_params = 1;
  assert(search(handle, 1, 10) == 14 && search(handle, 1, 10) == 0);
  seekdb_cuvs_free(handle);
  assert(allocations.empty());
  assert(alloc_calls == free_calls && params_created == params_destroyed);
}
