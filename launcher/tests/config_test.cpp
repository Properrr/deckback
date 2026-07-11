#include "config.hpp"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <string>

using deckback::Config;

int main() {
  const std::string path = "config_test_tmp.json";
  {
    std::ofstream f(path);
    f << R"({
      "url": "https://www.youtube.com/tv",
      "user_agent": "TestUA/1.0",
      "remote_debugging_port": 9222,
      "watchdog": { "restart_on_crash": false, "max_restarts_per_minute": 3 },
      "cobalt_flags": ["--ozone-platform=x11", "--force-device-scale-factor=1"]
    })";
  }

  auto c = Config::load(path);
  std::remove(path.c_str());

  assert(c.has_value());
  assert(c->url == "https://www.youtube.com/tv");
  assert(c->user_agent == "TestUA/1.0");
  assert(c->remote_debugging_port == 9222);
  assert(c->watchdog_restart_on_crash == false);
  assert(c->watchdog_max_restarts_per_minute == 3);
  assert(c->cobalt_flags.size() == 2);
  assert(c->cobalt_flags[0] == "--ozone-platform=x11");

  assert(!Config::load("does_not_exist.json").has_value());

  std::puts("config_test: ok");
  return 0;
}
