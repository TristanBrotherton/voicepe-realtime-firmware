#include "udp_log.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/components/logger/logger.h"
#include "esphome/components/network/util.h"

#include <cstdio>
#include <cstring>

#include <lwip/netdb.h>

namespace esphome {
namespace udp_log {

static const char *const TAG = "udp_log";

void UdpLog::setup() {
  this->sock_fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (this->sock_fd_ < 0) {
    ESP_LOGE(TAG, "socket() failed: errno=%d", errno);
    return;
  }
  // Non-blocking: a slow/unreachable peer must not stall the logger task.
  int flags = fcntl(this->sock_fd_, F_GETFL, 0);
  fcntl(this->sock_fd_, F_SETFL, flags | O_NONBLOCK);

  this->dest_.sin_family = AF_INET;
  this->dest_.sin_port = htons(this->port_);

  // DO NOT call getaddrinfo() here — DNS isn't up yet during setup() and
  // a blocking lookup against va.local easily hangs >10 s, tripping the
  // task watchdog. We resolve lazily from the periodic retry below, but
  // only when the network is up.
  //
  // 5-second tick, plenty for an idle resolve attempt; stops itself once
  // resolved successfully.
  this->set_interval("udp_log_resolve", 5000, [this]() {
    if (this->resolved_)
      return;
    if (!network::is_connected())
      return;
    if (this->resolve_()) {
      this->cancel_interval("udp_log_resolve");
    }
  });

  // Register with ESPHome's logger callbacks. The real implementation is
  // only compiled in when USE_LOG_LISTENERS is defined — we trigger that
  // via logger.request_log_listener() in our __init__.py codegen.
  logger::global_logger->add_log_callback(this, &UdpLog::log_trampoline_);

  ESP_LOGCONFIG(TAG, "UDP log -> %s:%u (level>=%d)", this->host_.c_str(),
                (unsigned) this->port_, this->min_level_);
}

void UdpLog::dump_config() {
  ESP_LOGCONFIG(TAG, "UDP Log:");
  ESP_LOGCONFIG(TAG, "  Host: %s", this->host_.c_str());
  ESP_LOGCONFIG(TAG, "  Port: %u", (unsigned) this->port_);
  ESP_LOGCONFIG(TAG, "  Min level: %d", this->min_level_);
  ESP_LOGCONFIG(TAG, "  Resolved: %s", this->resolved_ ? "yes" : "no");
}

bool UdpLog::resolve_() {
  if (this->resolved_)
    return true;
  struct addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  struct addrinfo *res = nullptr;
  int rc = ::getaddrinfo(this->host_.c_str(), nullptr, &hints, &res);
  if (rc != 0 || res == nullptr) {
    ESP_LOGD(TAG, "resolve %s: failed rc=%d", this->host_.c_str(), rc);
    return false;
  }
  auto *sin = reinterpret_cast<struct sockaddr_in *>(res->ai_addr);
  this->dest_.sin_addr = sin->sin_addr;
  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &this->dest_.sin_addr, ip, sizeof(ip));
  ::freeaddrinfo(res);
  // IMPORTANT: set resolved_ BEFORE the ESP_LOGI below. ESP_LOG fires our
  // own log callback synchronously; if resolved_ is still false the first
  // log line — the one announcing success — gets dropped at the on_log_
  // guard. Flip the flag first so the announcement reaches the sink too.
  this->resolved_ = true;
  ESP_LOGI(TAG, "resolved %s -> %s:%u", this->host_.c_str(), ip, (unsigned) this->port_);
  return true;
}

void UdpLog::log_trampoline_(void *self_v, uint8_t level, const char *tag,
                             const char *message, size_t length) {
  auto *self = static_cast<UdpLog *>(self_v);
  if (self != nullptr)
    self->on_log_(level, tag, message, length);
}

void UdpLog::on_log_(int level, const char *tag, const char *message, size_t length) {
  if (level > this->min_level_)
    return;
  if (this->sending_)
    return;  // reentrancy guard
  if (this->sock_fd_ < 0 || !this->resolved_)
    return;
  // Skip our own logs to avoid feedback loops if sendto() ever logs an error.
  if (tag != nullptr && std::strcmp(tag, TAG) == 0)
    return;

  this->sending_ = true;
  // ESPHome's callback gives us "[HH:MM:SS][L][tag:line]: msg" shape, may
  // contain ANSI colour escapes — strip so the sink doesn't render garbage.
  char buf[513];
  size_t n = length;
  if (n >= sizeof(buf) - 1)
    n = sizeof(buf) - 2;
  size_t w = 0;
  for (size_t r = 0; r < n; ++r) {
    char c = message[r];
    if (c == 0x1b && r + 1 < n && message[r + 1] == '[') {
      r += 2;
      while (r < n && !((message[r] >= 'A' && message[r] <= 'Z') ||
                        (message[r] >= 'a' && message[r] <= 'z'))) {
        ++r;
      }
      continue;
    }
    buf[w++] = c;
  }
  if (w == 0 || buf[w - 1] != '\n')
    buf[w++] = '\n';
  // sendto on a non-blocking UDP socket — fire and forget; if the kernel
  // buffer is briefly full we'd rather drop this line than block the
  // caller (which can be any task).
  ::sendto(this->sock_fd_, buf, w, 0, reinterpret_cast<const struct sockaddr *>(&this->dest_),
           sizeof(this->dest_));
  this->sending_ = false;
}

}  // namespace udp_log
}  // namespace esphome
