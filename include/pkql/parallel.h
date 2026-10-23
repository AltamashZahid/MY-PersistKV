#pragma once

// The worker pool for parallel work (scans, merges, building column
// blocks): threads started once and woken per parallel phase.

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace pkql {

constexpr size_t kMaxThreads = 16;

// Worker threads kept for parallel scans: started once, then woken per
// parallel phase. Creating and joining 16 threads per query cost ~1.5 ms on
// Windows, more than a whole small scan.
class WorkerPool {
 public:
  static WorkerPool& instance() {
    static WorkerPool pool;
    return pool;
  }

  // Runs fn on n threads (the caller is one of them) and waits for all.
  void run(size_t n, const std::function<void()>& fn) {
    std::lock_guard<std::mutex> one_job(run_mu_);
    size_t helpers = n > 0 ? n - 1 : 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      while (workers_.size() < helpers) workers_.emplace_back([this] { loop(); });
      job_ = &fn;
      wanted_ = helpers;
      claimed_ = 0;
      running_ = helpers;
      generation_++;
    }
    work_cv_.notify_all();
    fn();
    std::unique_lock<std::mutex> lock(mu_);
    done_cv_.wait(lock, [&] { return running_ == 0; });
    job_ = nullptr;
  }

  ~WorkerPool() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      stopping_ = true;
    }
    work_cv_.notify_all();
    for (std::thread& t : workers_) t.join();
  }

 private:
  void loop() {
    uint64_t seen = 0;
    std::unique_lock<std::mutex> lock(mu_);
    while (true) {
      work_cv_.wait(lock, [&] { return stopping_ || (generation_ != seen && claimed_ < wanted_); });
      if (stopping_) return;
      seen = generation_;
      claimed_++;
      const std::function<void()>* job = job_;
      lock.unlock();
      (*job)();
      lock.lock();
      if (--running_ == 0) done_cv_.notify_all();
    }
  }

  std::mutex run_mu_;  // one parallel phase at a time
  std::mutex mu_;
  std::condition_variable work_cv_, done_cv_;
  std::vector<std::thread> workers_;
  const std::function<void()>* job_ = nullptr;
  size_t wanted_ = 0, claimed_ = 0, running_ = 0;
  uint64_t generation_ = 0;
  bool stopping_ = false;
};

// Threads for parallel scans: the hardware's, at most kMaxThreads, or the
// PKQL_THREADS environment variable (1 turns parallelism off).
inline size_t workerThreads() {
  static const size_t n = [] {
    const char* env = std::getenv("PKQL_THREADS");
    size_t hw = std::max<size_t>(1, std::thread::hardware_concurrency());
    size_t want = env ? static_cast<size_t>(std::strtoul(env, nullptr, 10)) : hw;
    return std::max<size_t>(1, std::min(want, kMaxThreads));
  }();
  return n;
}

}  // namespace pkql
