#include "lib/vector/ob_cuvs_worker.h"
#include <cassert>
#include <cerrno>
#include <stdexcept>
#include <thread>
#include <vector>

// Link with -Wl,--wrap=pthread_create to inject one startup failure.
extern "C" int __real_pthread_create(pthread_t *, const pthread_attr_t *,
                                    void *(*)(void *), void *);
extern "C" int __wrap_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                                    void *(*callback)(void *), void *argument)
{
  static bool first = true;
  if (first) { first = false; return EAGAIN; }
  return __real_pthread_create(thread, attr, callback, argument);
}

using oceanbase::common::obvsag::detail::CuvsWorker;
struct Job { int value; pthread_t thread; size_t stack_size; };
void *execute(void *arg)
{
  auto *job = static_cast<Job *>(arg);
  job->value += 1;
  job->thread = pthread_self();
  pthread_attr_t attr;
  assert(pthread_getattr_np(pthread_self(), &attr) == 0);
  assert(pthread_attr_getstacksize(&attr, &job->stack_size) == 0);
  pthread_attr_destroy(&attr);
  return nullptr;
}
void *fail(void *) { throw std::runtime_error("test"); }
int main()
{
  CuvsWorker worker;
  Job first{0, {}, 0};
  assert(!worker.run(execute, &first));
  assert(first.value == 0);
  assert(worker.run(execute, &first));
  assert(first.value == 1 && first.stack_size >= 32UL * 1024 * 1024);
  assert(!pthread_equal(first.thread, pthread_self()));
  assert(!worker.run(fail, nullptr));
  std::vector<std::thread> callers;
  for (int i = 0; i < 8; ++i) {
    callers.emplace_back([&] {
      Job job{0, {}, 0};
      for (int j = 0; j < 100; ++j) {
        assert(worker.run(execute, &job));
        assert(job.value == j + 1);
        assert(pthread_equal(job.thread, first.thread));
      }
    });
  }
  for (auto &caller : callers) { caller.join(); }
}
