#include "audio.hpp"

#include <chrono>
#include <format>
#include <string_view>
#include <vector>

#include "log.hpp"

#if defined(DECKBACK_HAVE_PULSE) && __has_include(<pulse/mainloop.h>) && \
    __has_include(<pulse/proplist.h>) && __has_include(<pulse/pulseaudio.h>)
#include <pulse/mainloop.h>
#include <pulse/proplist.h>
#include <pulse/pulseaudio.h>
#define DECKBACK_PULSE 1
#endif

namespace deckback {
namespace {

#if defined(DECKBACK_PULSE)
struct PulseRepair {
  pa_mainloop* mainloop = nullptr;
  pa_context* context = nullptr;
  bool ready = false;
  bool failed = false;
  bool done = false;
  int attempted = 0;
  int repaired = 0;
  std::vector<pa_operation*> mute_operations;
};

void context_state_cb(pa_context* context, void* userdata) {
  auto& repair = *static_cast<PulseRepair*>(userdata);
  switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY:
      repair.ready = true;
      break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      repair.failed = true;
      break;
    default:
      break;
  }
}

bool is_deckback_stream(const pa_sink_input_info& info) {
  const char* app_id = pa_proplist_gets(info.proplist, "pipewire.access.portal.app_id");
  const char* binary = pa_proplist_gets(info.proplist, PA_PROP_APPLICATION_PROCESS_BINARY);
  return (app_id && std::string_view(app_id) == "io.github.properrr.deckback") ||
         (binary && std::string_view(binary) == "content_shell");
}

void mute_operation_cb(pa_context*, int success, void* userdata);

void sink_input_info_cb(pa_context* context, const pa_sink_input_info* info, int eol,
                        void* userdata) {
  auto& repair = *static_cast<PulseRepair*>(userdata);
  if (eol) {
    repair.done = true;
    return;
  }
  if (!info || !info->mute || !is_deckback_stream(*info)) return;
  ++repair.attempted;
  if (pa_operation* operation = pa_context_set_sink_input_mute(
          context, info->index, 0, mute_operation_cb, userdata)) {
    repair.mute_operations.push_back(operation);
  }
}

void mute_operation_cb(pa_context*, int success, void* userdata) {
  auto& repair = *static_cast<PulseRepair*>(userdata);
  if (success) {
    ++repair.repaired;
  } else if (repair.context) {
    warn(std::format("audio: mute repair failed: {}", pa_strerror(pa_context_errno(repair.context))));
  }
}

int repair_pulse_streams() {
  PulseRepair repair;
  repair.mainloop = pa_mainloop_new();
  if (!repair.mainloop) return 0;
  repair.context = pa_context_new(pa_mainloop_get_api(repair.mainloop), "Deckback audio repair");
  if (!repair.context) {
    pa_mainloop_free(repair.mainloop);
    return 0;
  }
  pa_context_set_state_callback(repair.context, context_state_cb, &repair);
  if (pa_context_connect(repair.context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
    pa_context_unref(repair.context);
    pa_mainloop_free(repair.mainloop);
    return 0;
  }

  int error = 0;
  while (!repair.ready && !repair.failed) pa_mainloop_iterate(repair.mainloop, 1, &error);
  if (repair.ready) {
    pa_operation* operation =
        pa_context_get_sink_input_info_list(repair.context, sink_input_info_cb, &repair);
    if (operation) {
      while (!repair.done && !repair.failed) pa_mainloop_iterate(repair.mainloop, 1, &error);
      pa_operation_unref(operation);
    }
  }
  bool pending = true;
  while (pending && !repair.failed) {
    pending = false;
    for (pa_operation* operation : repair.mute_operations) {
      if (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) {
        pending = true;
        break;
      }
    }
    if (pending) pa_mainloop_iterate(repair.mainloop, 1, &error);
  }
  for (pa_operation* operation : repair.mute_operations) pa_operation_unref(operation);
  pa_context_disconnect(repair.context);
  pa_context_unref(repair.context);
  pa_mainloop_free(repair.mainloop);
  if (repair.attempted > 0 && repair.repaired != repair.attempted) {
    warn(std::format("audio: mute repair result {}/{}", repair.repaired, repair.attempted));
  }
  return repair.repaired;
}
#endif

}  // namespace

AudioRepair::~AudioRepair() { stop(); }

void AudioRepair::start() {
  std::lock_guard lock(mutex_);
  if (started_) return;
  started_ = true;
  stop_ = false;
  thread_ = std::thread([this] { loop(); });
}

void AudioRepair::stop() {
  {
    std::lock_guard lock(mutex_);
    if (!started_) return;
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard lock(mutex_);
  started_ = false;
}

void AudioRepair::loop() {
#if defined(DECKBACK_PULSE)
  while (true) {
    const int repaired = repair_pulse_streams();
    if (repaired > 0) info(std::format("audio: unmuted {} Deckback sink input(s)", repaired));

    std::unique_lock lock(mutex_);
    if (cv_.wait_for(lock, std::chrono::seconds(2), [this] { return stop_; })) break;
  }
#else
  warn("audio: libpulse unavailable; automatic sink-input mute repair disabled");
  std::unique_lock lock(mutex_);
  cv_.wait(lock, [this] { return stop_; });
#endif
}

}  // namespace deckback
