"""ESPHome `udp_log` component.

Forwards the device's log lines as UDP packets to a configured host/port.
Pair with a sink container (e.g. `alpine/socat` listening on UDP) on the
receiving side; Dozzle then picks up the sink's stdout.

YAML:

    udp_log:
      host: va.local
      port: 5140
      level: INFO   # min level forwarded; DEBUG is intentionally avoided
                    # since wifi-amplified spam would dwarf real logs

Skips its own tag to avoid feedback loops if sendto() fails.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import logger
from esphome.const import CONF_ID, CONF_LEVEL, CONF_PORT

CODEOWNERS = ["@maxmaxme"]
DEPENDENCIES = ["network", "logger"]

udp_log_ns = cg.esphome_ns.namespace("udp_log")
UdpLog = udp_log_ns.class_("UdpLog", cg.Component)

CONF_HOST = "host"

# Mirror of the ESPHOME_LOG_LEVEL_* macros — kept as plain ints so we don't
# depend on whichever subset of the names esphome.components.logger exposes
# on a given release.
LOG_LEVELS = {
    "ERROR": 1,
    "WARN": 2,
    "INFO": 3,
    "CONFIG": 4,
    "DEBUG": 5,
    "VERBOSE": 6,
    "VERY_VERBOSE": 7,
}

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UdpLog),
        cv.Required(CONF_HOST): cv.string,
        cv.Optional(CONF_PORT, default=5140): cv.port,
        cv.Optional(CONF_LEVEL, default="INFO"): cv.enum(LOG_LEVELS, upper=True),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # Without this the logger's add_log_callback is a no-op stub (controlled
    # by the USE_LOG_LISTENERS define). Request a listener slot so the real
    # implementation is compiled in.
    logger.request_log_listener()

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_host(config[CONF_HOST]))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add(var.set_min_level(config[CONF_LEVEL]))
