#pragma once
#include <memory>
#include <string>

namespace deckback {

// Self-update through the Flatpak portal (`org.freedesktop.portal.Flatpak`'s UpdateMonitor).
//
// A sandboxed app cannot run `flatpak update`; the sanctioned path is to ask the portal — a
// privileged host service — to update the app on its behalf. It updates ONLY this app's own ref,
// from this app's own origin remote (our `--user` `deckback` remote), so it needs no root and, for
// a user install, no password/polkit prompt (which Game Mode could not answer anyway). It updates
// nothing else the user has installed — the blast radius is exactly Deckback.
//
// The update is deployed as a new ostree commit WITHOUT swapping the running deployment, so
// playback is never interrupted; the new version binds on the NEXT launch. On completion we show a
// toast
// ("restart to apply") if a CDP port is available.
//
// Backed by libsystemd sd-bus on the SESSION bus; compiles to a no-op stub on a bare toolchain.
// Runs its own event-loop thread — mirrors platform.cpp, but on the session bus, not the system
// bus. NOTHING here is verified until it has run on a Deck (findings/durable/self-update.md): the
// portal must be reachable in the gamescope session, accept a `--user` custom-remote ref, and not
// prompt.
struct UpdaterConfig {
  bool enabled = false;
  // Used only for the "installed — restart to apply" toast. cdp_port <= 0 disables the toast; the
  // update still deploys. Host/port match the engine's --remote-debugging-port (loopback).
  std::string cdp_host = "127.0.0.1";
  int cdp_port = 0;
  std::string toast_text = "Deckback update installed \xE2\x80\x94 restart to apply.";
  int toast_ms = 8000;
};

class Updater {
 public:
  static std::unique_ptr<Updater> create(UpdaterConfig cfg);
  virtual ~Updater() = default;

  // Create the portal update monitor and begin watching, on an owned event-loop thread. No-op if
  // the session-bus/portal connection is not live.
  virtual void start() = 0;
  // Signal + join the event-loop thread. Idempotent.
  virtual void stop() = 0;

  // True if a live session-bus connection was established (the portal itself is probed by
  // selftest).
  virtual bool backend_live() const = 0;

  // Live self-test (needs the portal on the session bus, i.e. run inside the Flatpak): create a
  // monitor and report whether the portal answered. Returns 0 on success. `--selftest-update`.
  virtual int selftest() = 0;

  static bool backend_available();
};

}  // namespace deckback
