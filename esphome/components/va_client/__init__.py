import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation
from esphome.components import microphone, speaker
from esphome.components import esp32
from esphome.const import CONF_ID, CONF_URL, CONF_TRIGGER_ID

CODEOWNERS = ["@maxmaxme"]
DEPENDENCIES = ["network", "microphone", "speaker"]

CONF_MICROPHONE = "microphone"
CONF_MIC_CHANNEL = "mic_channel"
CONF_SPEAKER = "speaker"
CONF_BARGE_IN = "barge_in"
CONF_TOKEN = "token"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_ON_PHASE = "on_phase"
CONF_ON_REPEATED_FAILURE = "on_repeated_failure"
CONF_ON_FOLLOWUP_OPENED = "on_followup_opened"
CONF_ON_AUDIBLE_ERROR = "on_audible_error"

va_client_ns = cg.esphome_ns.namespace("va_client")
VaClient = va_client_ns.class_("VaClient", cg.Component)
OnPhaseTrigger = va_client_ns.class_(
    "OnPhaseTrigger", automation.Trigger.template(cg.std_string)
)
OnRepeatedFailureTrigger = va_client_ns.class_(
    "OnRepeatedFailureTrigger", automation.Trigger.template()
)
OnFollowupOpenedTrigger = va_client_ns.class_(
    "OnFollowupOpenedTrigger", automation.Trigger.template()
)
OnAudibleErrorTrigger = va_client_ns.class_(
    "OnAudibleErrorTrigger", automation.Trigger.template()
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VaClient),
        cv.Required(CONF_URL): cv.string,
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Optional(CONF_MIC_CHANNEL, default=0): cv.int_range(min=0, max=1),
        cv.Optional(CONF_BARGE_IN, default=True): cv.boolean,
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
        # Shared secret for the add-on's device_token option. Sent as an
        # "Authorization: Bearer" header on the WebSocket handshake; empty =
        # no header (legacy, unauthenticated backend).
        cv.Optional(CONF_TOKEN, default=""): cv.string,
        # Reported to the backend in the wake message so turn timelines and
        # wake statistics can be split by firmware version.
        cv.Optional(CONF_FIRMWARE_VERSION, default=""): cv.string,
        cv.Optional(CONF_ON_PHASE): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnPhaseTrigger),
            }
        ),
        cv.Optional(CONF_ON_REPEATED_FAILURE): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnRepeatedFailureTrigger),
            }
        ),
        cv.Optional(CONF_ON_FOLLOWUP_OPENED): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnFollowupOpenedTrigger),
            }
        ),
        # Fired when the backend reports an error on a turn the user started
        # ({"type":"error","audible":true}) and could not speak it itself.
        cv.Optional(CONF_ON_AUDIBLE_ERROR): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(OnAudibleErrorTrigger),
            }
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    # esp-idf managed component providing esp_websocket_client.
    esp32.add_idf_component(
        name="espressif/esp_websocket_client",
        ref="1.7.0",
    )

    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_mic_channel(config[CONF_MIC_CHANNEL]))
    cg.add(var.set_barge_in(config[CONF_BARGE_IN]))
    cg.add(var.set_token(config[CONF_TOKEN]))
    cg.add(var.set_firmware_version(config[CONF_FIRMWARE_VERSION]))

    mic = await cg.get_variable(config[CONF_MICROPHONE])
    cg.add(var.set_microphone(mic))

    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_speaker(spk))

    for conf in config.get(CONF_ON_PHASE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.std_string, "phase")], conf)

    for conf in config.get(CONF_ON_REPEATED_FAILURE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_FOLLOWUP_OPENED, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)

    for conf in config.get(CONF_ON_AUDIBLE_ERROR, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [], conf)
