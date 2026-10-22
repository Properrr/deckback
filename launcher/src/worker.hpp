#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace deckback {

// Stoppable background thread for the polling subsystems.
//
// The owner MUST call stop() in its own destructor (before its other members are torn down): this
// class joins the thread on destruction too, but by then the loop's captured `this` has already
// lost the members it touches.
class WorkerThread {
 public:
  WorkerThread() = default;
  ~WorkerThread() { stop(); }

  WorkerThread(const WorkerThread&) = delete;
  WorkerThread& operator=(const WorkerThread&) = delete;

  // Launch `fn` on the worker thread. No-op while already started.
  template <typename Fn>
  void start(Fn&& fn) {
    {
      std::lock_guard lk(mu_);
      if (started_) return;
      started_ = true;
      stop_ = false;
    }
    thread_ = std::thread(std::forward<Fn>(fn));
  }

  // Signal + join. Idempotent; the worker may be started again afterwards.
  void stop() {
    {
      std::lock_guard lk(mu_);
      if (!started_) return;
      stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard lk(mu_);
    started_ = false;
  }

  // For loops that block elsewhere (poll() on device fds) and only need the flag.
  bool stopping() {
    std::lock_guard lk(mu_);
    return stop_;
  }

  // Sleep up to `ms`, waking early on stop() or wake(). Returns true when asked to stop.
  bool wait_or_stop(int ms) {
    std::unique_lock lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stop_ || woken_; });
    woken_ = false;
    return stop_;
  }

  // End the current (or next) wait_or_stop() early without stopping, so the loop looks at whatever
  // flag the caller just set instead of sleeping out its interval.
  void wake() {
    {
      std::lock_guard lk(mu_);
      woken_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool woken_ = false;
  bool started_ = false;
};

}  // namespace deckback
