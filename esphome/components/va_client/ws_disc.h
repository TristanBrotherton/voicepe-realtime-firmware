#pragma once

// Why a WebSocket connection ended, for the `last_disconnect` the device
// reports in its next `start`.
//
// esp_websocket_client ends a connection on a transport failure in two steps:
// it first posts WEBSOCKET_EVENT_ERROR with a text that names the failing
// call ("esp_transport_write() returned 0, ...", "esp_transport_read() failed
// ...", "esp_transport_poll_read() returned ...", "Error, no PONG received
// ..."), then WEBSOCKET_EVENT_DISCONNECTED. Only that DISCONNECTED carries
// the esp-tls / socket error codes, and only when its error type is
// WEBSOCKET_ERROR_TYPE_TCP_TRANSPORT: on the ERROR event they are never
// written and hold whatever was on the posting task's stack (a device
// reported errno 1008406854 = 0x3c1b1146, the address of the client's log tag
// "websocket_client"). Both events are posted synchronously from the task
// that hit the failure, which for a write is the sender task itself, before
// its send call has returned.
//
// No ESP-IDF dependencies: tools/ci/test_ws_rx.cpp checks this on the host.

#include <cstddef>
#include <cstring>

namespace esphome {
namespace va_client {

enum class WsFailWhere : unsigned char {
  NONE = 0,  // no ERROR text seen for this connection
  WRITE,     // esp_transport_write() failed or timed out (an uplink stall)
  READ,      // esp_transport_read() failed
  POLL,      // esp_transport_poll_read() failed
  PONG,      // no PONG for pingpong_timeout_sec
  OTHER,     // some other ERROR text
};

namespace detail {
inline bool contains(const char *s, size_t len, const char *needle) {
  const size_t n = std::strlen(needle);
  if (s == nullptr || n == 0 || len < n)
    return false;
  for (size_t i = 0; i + n <= len; i++) {
    if (std::memcmp(s + i, needle, n) == 0)
      return true;
  }
  return false;
}
}  // namespace detail

// Which call failed, from the text of a WEBSOCKET_EVENT_ERROR (not
// NUL-terminated: data_ptr / data_len).
inline WsFailWhere ws_fail_where(const char *msg, size_t len) {
  if (msg == nullptr || len == 0)
    return WsFailWhere::NONE;
  if (detail::contains(msg, len, "esp_transport_write"))
    return WsFailWhere::WRITE;
  if (detail::contains(msg, len, "esp_transport_poll_read"))
    return WsFailWhere::POLL;
  if (detail::contains(msg, len, "esp_transport_read"))
    return WsFailWhere::READ;
  if (detail::contains(msg, len, "PONG"))
    return WsFailWhere::PONG;
  return WsFailWhere::OTHER;
}

// The `where` token sent in last_disconnect (nullptr: send none).
inline const char *ws_fail_where_name(WsFailWhere w) {
  switch (w) {
    case WsFailWhere::WRITE:
      return "write";
    case WsFailWhere::READ:
      return "read";
    case WsFailWhere::POLL:
      return "poll";
    case WsFailWhere::PONG:
      return "pong";
    case WsFailWhere::OTHER:
      return "other";
    default:
      return nullptr;
  }
}

// The `why`, limited to the values the add-on accepts ("ws_error",
// "ws_closed", "wifi", "tx_timeout"; "reboot" is set at boot).
inline const char *ws_disconnect_why(bool wifi_connected, bool server_closed, WsFailWhere where,
                                     bool recent_tx_fail) {
  if (!wifi_connected)
    return "wifi";
  if (where == WsFailWhere::WRITE || recent_tx_fail)
    return "tx_timeout";
  if (server_closed)
    return "ws_closed";
  return "ws_error";
}

// Whether an event's error_handle.esp_tls_last_esp_err / esp_transport_sock_errno
// hold real values (see above).
inline bool ws_error_codes_valid(bool is_error_event, bool is_tcp_transport_error) {
  return !is_error_event && is_tcp_transport_error;
}

}  // namespace va_client
}  // namespace esphome
