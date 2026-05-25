#pragma once
#include "esphome/core/component.h"
#include <cstdint>
#include <string>

#include <lwip/sockets.h>

namespace esphome {
namespace udp_log {

class UdpLog : public Component {
 public:
  void set_host(const std::string &host) { host_ = host; }
  void set_port(uint16_t port) { port_ = port; }
  void set_min_level(int level) { min_level_ = level; }

  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  // Resolves host_ → resolved_addr_. Returns true on success. Best-effort:
  // if DNS isn't up yet we skip; loop retries.
  bool resolve_();
  // ESPHome's logger callback (real impl is gated behind USE_LOG_LISTENERS;
  // we enable it via logger.request_log_listener() in __init__.py codegen).
  static void log_trampoline_(void *self, uint8_t level, const char *tag,
                              const char *message, size_t length);
  // Format the log line and ship it.
  void on_log_(int level, const char *tag, const char *message, size_t length);

  std::string host_;
  uint16_t port_{5140};
  int min_level_{ESPHOME_LOG_LEVEL_INFO};

  int sock_fd_{-1};
  struct sockaddr_in dest_{};
  bool resolved_{false};
  // Reentrancy guard: a failed sendto() that triggers a log inside lwip
  // would recursively call us. Drop the inner call.
  bool sending_{false};
};

}  // namespace udp_log
}  // namespace esphome
