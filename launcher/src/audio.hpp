#pragma once

#include <condition_variable>
#include <mutex>
#include <thread>

namespace deckback {

// Repairs a PipeWire/Pulse sink-input mute restored for Deckback's Chromium stream.
// The stream is created by content_shell after launch, so this runs periodically rather than once.
class AudioRepair {
 public:
  AudioRepair() = default;
  ~AudioRepair();

  AudioRepair(const AudioRepair&) = delete;
  AudioRepair& operator=(const AudioRepair&) = delete;

  void start();
  void stop();

 private:
  void loop();

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_ = false;
  bool started_ = false;
};

}  // namespace deckback
