#pragma once
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>

namespace deckback {

// The stoppable-background-thread lifecycle shared by every polling subsystem (input, navigator,
// player, touch-mode guard, audio repair). One implementation so the subtle parts — notify outside
// the lock, idempotent stop(), restartability after stop() — cannot drift between copies.
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

  // Sleep up to `ms`, waking early on stop(). Returns true when asked to stop.
  bool wait_or_stop(int ms) {
    std::unique_lock lk(mu_);
    return cv_.wait_for(lk, std::chrono::milliseconds(ms), [this] { return stop_; });
  }

 private:
  std::thread thread_;
  std::mutex mu_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool started_ = false;
};

}  // namespace deckback
