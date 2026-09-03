import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import (
    CONF_ID,
    CONF_BAUD_RATE,
    CONF_RX_PIN,
    CONF_TX_PIN,
)

AUTO_LOAD = []
MULTI_CONF = True

CONF_DE_PIN = "de_pin"
CONF_RE_PIN = "re_pin"
CONF_POWER_PIN = "power_pin"
CONF_SE_PIN = "se_pin"
CONF_RX_BUFFER_SIZE = "rx_buffer_size"
CONF_LISTEN_ONLY = "listen_only"
CONF_YIELD_TO_GLM = "yield_to_glm"
CONF_GLM_INACTIVITY_COOLDOWN = "glm_inactivity_cooldown"
CONF_POLL_INTERVAL = "poll_interval"

gensam_ns = cg.esphome_ns.namespace("gensam")
GenSAMHub = gensam_ns.class_("GenSAMHub", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(GenSAMHub),
        cv.Required(CONF_TX_PIN): pins.gpio_output_pin_schema,
        cv.Required(CONF_RX_PIN): pins.gpio_input_pin_schema,
        cv.Optional(CONF_DE_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_RE_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_POWER_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_SE_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_RX_BUFFER_SIZE, default=512): cv.positive_int,
        cv.Optional(CONF_LISTEN_ONLY, default=False): cv.boolean,
        cv.Optional(CONF_YIELD_TO_GLM, default=True): cv.boolean,
        cv.Optional(CONF_GLM_INACTIVITY_COOLDOWN, default="30s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_POLL_INTERVAL, default="1s"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    tx_pin = await cg.gpio_pin_expression(config[CONF_TX_PIN])
    rx_pin = await cg.gpio_pin_expression(config[CONF_RX_PIN])

    cg.add(var.set_tx_pin(tx_pin.get_pin()))
    cg.add(var.set_rx_pin(rx_pin.get_pin()))

    if CONF_DE_PIN in config:
        de_pin = await cg.gpio_pin_expression(config[CONF_DE_PIN])
        cg.add(var.set_de_pin(de_pin.get_pin()))
    if CONF_RE_PIN in config:
        re_pin = await cg.gpio_pin_expression(config[CONF_RE_PIN])
        cg.add(var.set_re_pin(re_pin.get_pin()))
    if CONF_POWER_PIN in config:
        power_pin = await cg.gpio_pin_expression(config[CONF_POWER_PIN])
        cg.add(var.set_power_pin(power_pin.get_pin()))
    if CONF_SE_PIN in config:
        se_pin = await cg.gpio_pin_expression(config[CONF_SE_PIN])
        cg.add(var.set_se_pin(se_pin.get_pin()))

    cg.add(var.set_rx_buffer_size(config[CONF_RX_BUFFER_SIZE]))
    cg.add(var.set_listen_only(config[CONF_LISTEN_ONLY]))
    cg.add(var.set_yield_to_glm(config[CONF_YIELD_TO_GLM]))
    cg.add(var.set_glm_inactivity_cooldown(config[CONF_GLM_INACTIVITY_COOLDOWN]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
