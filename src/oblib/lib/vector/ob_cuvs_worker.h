/* Synchronous dispatch onto a reusable large-stack GPU worker. */
#ifndef OCEANBASE_LIB_VECTOR_OB_CUVS_WORKER_H_
#define OCEANBASE_LIB_VECTOR_OB_CUVS_WORKER_H_

#include <condition_variable>
#include <mutex>
#include <pthread.h>

namespace oceanbase {
namespace common {
namespace obvsag {
namespace detail {

class CuvsWorker {
public:
  CuvsWorker() = default;
  ~CuvsWorker()
  {
    std::lock_guard<std::mutex> submit(submit_mutex_);
    if (started_) {
      {
        std::lock_guard<std::mutex> lock(state_mutex_);
        stopping_ = true;
      }
      changed_.notify_one();
      pthread_join(thread_, nullptr);
    }
  }

  CuvsWorker(const CuvsWorker &) = delete;
  CuvsWorker &operator=(const CuvsWorker &) = delete;

  // Calls remain synchronous: the caller's argument is no longer used on return.
  // A failed pthread creation is retried by the next call.
  bool run(void *(*callback)(void *), void *argument)
  {
    if (callback == nullptr) { return false; }
    std::lock_guard<std::mutex> submit(submit_mutex_);
    if (!started_ && !start()) { return false; }
    std::unique_lock<std::mutex> lock(state_mutex_);
    callback_ = callback;
    argument_ = argument;
    pending_ = true;
    changed_.notify_one();
    completed_.wait(lock, [this] { return !pending_; });
    return succeeded_;
  }

private:
  bool start()
  {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) { return false; }
    int rc = pthread_attr_setstacksize(&attr, 32UL * 1024 * 1024);
    if (rc == 0) { rc = pthread_create(&thread_, &attr, loop, this); }
    pthread_attr_destroy(&attr);
    started_ = rc == 0;
    return started_;
  }

  static void *loop(void *argument)
  {
    auto *self = static_cast<CuvsWorker *>(argument);
    std::unique_lock<std::mutex> lock(self->state_mutex_);
    for (;;) {
      self->changed_.wait(lock, [self] { return self->pending_ || self->stopping_; });
      if (self->stopping_) { break; }
      auto callback = self->callback_;
      void *job = self->argument_;
      lock.unlock();
      bool succeeded = true;
      try { callback(job); } catch (...) { succeeded = false; }
      lock.lock();
      self->succeeded_ = succeeded;
      self->callback_ = nullptr;
      self->argument_ = nullptr;
      self->pending_ = false;
      self->completed_.notify_one();
    }
    return nullptr;
  }

  std::mutex submit_mutex_, state_mutex_;
  std::condition_variable changed_, completed_;
  pthread_t thread_{};
  bool started_ = false, stopping_ = false, pending_ = false, succeeded_ = false;
  void *(*callback_)(void *) = nullptr;
  void *argument_ = nullptr;
};

} // namespace detail
} // namespace obvsag
} // namespace common
} // namespace oceanbase
#endif
