import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import sensor, binary_sensor, button, text_sensor, switch
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_DEVICE_ID,
    CONF_FILTERS,
    CONF_RX_PIN,
    CONF_TX_PIN,
    UNIT_CELSIUS,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_IDENTIFY,
    DEVICE_CLASS_OCCUPANCY,
    STATE_CLASS_MEASUREMENT,
    ENTITY_CATEGORY_DIAGNOSTIC,
)

AUTO_LOAD = ["sensor", "binary_sensor", "button", "text_sensor", "switch"]
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
CONF_TELEMETRY_AVERAGING_PERIOD = "telemetry_averaging_period"

CONF_MIN_VOLUME_DB = "min_volume_db"
CONF_MAX_VOLUME_DB = "max_volume_db"
CONF_STARTUP_VOLUME_DB = "startup_volume_db"

CONF_MONITORS = "monitors"
CONF_SERIAL_NUMBER = "serial_number"
CONF_UNIQUE_ID = "unique_id"

CONF_TEMPERATURE = "temperature"
CONF_INPUT_LEVEL = "input_level"
CONF_OUTPUT_LEVEL = "output_level"
CONF_ONLINE = "online"
CONF_MUTE = "mute"
CONF_IDENTIFY = "identify"

CONF_MODEL = "model"
CONF_SERIAL_NUMBER_SENSOR = "serial_number_sensor"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_HARDWARE_ID = "hardware_id"

CONF_GLM_ADAPTER_ACTIVE = "glm_adapter_active"
CONF_REDISCOVER_BUTTON = "rediscover_button"

gensam_ns = cg.esphome_ns.namespace("gensam")
GenSAMHub = gensam_ns.class_("GenSAMHub", cg.Component)
GenSAMMuteSwitch = gensam_ns.class_("GenSAMMuteSwitch", switch.Switch)
GenSAMIdentifyButton = gensam_ns.class_("GenSAMIdentifyButton", button.Button)
GenSAMRediscoverButton = gensam_ns.class_("GenSAMRediscoverButton", button.Button)
GenSAMMonitorBinding = gensam_ns.struct("GenSAMMonitorBinding")


def _validate_monitor(conf):
    name = conf[CONF_NAME]
    dev_id = conf.get(CONF_DEVICE_ID)

    if CONF_TEMPERATURE not in conf:
        c = {CONF_NAME: f"{name} Temperature"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_TEMPERATURE] = sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )(c)

    if CONF_INPUT_LEVEL not in conf:
        c = {CONF_NAME: f"{name} Input Level"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_INPUT_LEVEL] = sensor.sensor_schema(
            unit_of_measurement="dBFS",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )(c)

    if CONF_OUTPUT_LEVEL not in conf:
        c = {CONF_NAME: f"{name} Output Level"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_OUTPUT_LEVEL] = sensor.sensor_schema(
            unit_of_measurement="dBFS",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )(c)

    if CONF_ONLINE not in conf:
        c = {CONF_NAME: f"{name} Online"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_ONLINE] = binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )(c)

    if CONF_MUTE not in conf:
        c = {CONF_NAME: f"{name} Mute"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_MUTE] = switch.switch_schema(
            GenSAMMuteSwitch,
            icon="mdi:volume-off",
        )(c)

    if CONF_IDENTIFY not in conf:
        c = {CONF_NAME: f"{name} Identify"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_IDENTIFY] = button.button_schema(
            GenSAMIdentifyButton,
            device_class=DEVICE_CLASS_IDENTIFY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )(c)

    if CONF_MODEL not in conf:
        c = {CONF_NAME: f"{name} Model"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_MODEL] = text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        )(c)

    if CONF_SERIAL_NUMBER_SENSOR not in conf:
        c = {CONF_NAME: f"{name} Serial Number"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_SERIAL_NUMBER_SENSOR] = text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        )(c)

    if CONF_FIRMWARE_VERSION not in conf:
        c = {CONF_NAME: f"{name} Firmware Version"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_FIRMWARE_VERSION] = text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        )(c)

    if CONF_HARDWARE_ID not in conf:
        c = {CONF_NAME: f"{name} Hardware ID"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_HARDWARE_ID] = text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        )(c)

    return conf


MONITOR_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_NAME): cv.string,
            cv.Optional(CONF_SERIAL_NUMBER, default=""): cv.string,
            cv.Optional(CONF_UNIQUE_ID, default=0): cv.positive_int,
            cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_INPUT_LEVEL): sensor.sensor_schema(
                unit_of_measurement="dBFS",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_OUTPUT_LEVEL): sensor.sensor_schema(
                unit_of_measurement="dBFS",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_ONLINE): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_MUTE): switch.switch_schema(
                GenSAMMuteSwitch,
                icon="mdi:volume-off",
            ),
            cv.Optional(CONF_IDENTIFY): button.button_schema(
                GenSAMIdentifyButton,
                device_class=DEVICE_CLASS_IDENTIFY,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_MODEL): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SERIAL_NUMBER_SENSOR): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_FIRMWARE_VERSION): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_HARDWARE_ID): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
        }
    ),
    _validate_monitor,
)

def _validate_hub(config):
    period = config.get(CONF_TELEMETRY_AVERAGING_PERIOD)
    for mon_conf in config.get(CONF_MONITORS, []):
        # 1. Temperature default filter: 1.0 degC delta + 60s heartbeat
        temp_c = mon_conf.get(CONF_TEMPERATURE, {})
        if CONF_FILTERS not in temp_c:
            temp_c[CONF_FILTERS] = sensor.validate_filters([
                {"delta": 1.0},
                {"heartbeat": "60s"},
            ])
        # 2. Input/Output levels: throttle average over averaging period
        if period and period.total_seconds > 0:
            in_c = mon_conf.get(CONF_INPUT_LEVEL, {})
            if CONF_FILTERS not in in_c:
                in_c[CONF_FILTERS] = sensor.validate_filters([
                    {"throttle_average": str(period)},
                ])
            out_c = mon_conf.get(CONF_OUTPUT_LEVEL, {})
            if CONF_FILTERS not in out_c:
                out_c[CONF_FILTERS] = sensor.validate_filters([
                    {"throttle_average": str(period)},
                ])
    return config


_CONFIG_SCHEMA = cv.Schema(
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
        cv.Optional(CONF_TELEMETRY_AVERAGING_PERIOD, default="60s"): cv.time_period_str_unit,
        cv.Optional(CONF_MIN_VOLUME_DB, default=-80.0): cv.float_,
        cv.Optional(CONF_MAX_VOLUME_DB, default=0.0): cv.float_,
        cv.Optional(CONF_STARTUP_VOLUME_DB, default=-30.0): cv.float_,
        cv.Optional(CONF_MONITORS): cv.ensure_list(MONITOR_SCHEMA),
        cv.Optional(CONF_GLM_ADAPTER_ACTIVE): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_OCCUPANCY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_REDISCOVER_BUTTON): button.button_schema(
            GenSAMRediscoverButton,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
    }
).extend(cv.COMPONENT_SCHEMA)

CONFIG_SCHEMA = cv.All(_CONFIG_SCHEMA, _validate_hub)


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

    cg.add(var.set_min_volume_db(config[CONF_MIN_VOLUME_DB]))
    cg.add(var.set_max_volume_db(config[CONF_MAX_VOLUME_DB]))
    cg.add(var.set_startup_volume_db(config[CONF_STARTUP_VOLUME_DB]))

    if CONF_GLM_ADAPTER_ACTIVE in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_GLM_ADAPTER_ACTIVE])
        cg.add(var.set_glm_usb_adapter_active_sensor(sens))

    if CONF_REDISCOVER_BUTTON in config:
        btn = await button.new_button(config[CONF_REDISCOVER_BUTTON])
        cg.add(btn.set_hub(var))

    if CONF_MONITORS in config:
        for mon_conf in config[CONF_MONITORS]:
            name = mon_conf[CONF_NAME]
            serial = mon_conf.get(CONF_SERIAL_NUMBER, "")
            unique_id = mon_conf.get(CONF_UNIQUE_ID, 0)

            # 1. Temperature sensor
            temp_sens = await sensor.new_sensor(mon_conf[CONF_TEMPERATURE])

            # 2. Input level sensor
            in_sens = await sensor.new_sensor(mon_conf[CONF_INPUT_LEVEL])

            # 3. Output level sensor
            out_sens = await sensor.new_sensor(mon_conf[CONF_OUTPUT_LEVEL])

            # 4. Online status binary sensor
            online_sens = await binary_sensor.new_binary_sensor(mon_conf[CONF_ONLINE])

            # 5. Mute switch
            mute_sw = await switch.new_switch(mon_conf[CONF_MUTE])
            cg.add(mute_sw.set_hub(var))
            target_id = serial if serial else str(unique_id)
            cg.add(mute_sw.set_serial_or_id(target_id))

            # 6. Identify button
            id_btn = await button.new_button(mon_conf[CONF_IDENTIFY])
            cg.add(id_btn.set_hub(var))
            cg.add(id_btn.set_serial_or_id(target_id))

            # 7. Model text sensor
            model_sens = await text_sensor.new_text_sensor(mon_conf[CONF_MODEL])

            # 8. Serial number text sensor
            serial_sens = await text_sensor.new_text_sensor(mon_conf[CONF_SERIAL_NUMBER_SENSOR])

            # 9. Firmware version text sensor
            fw_sens = await text_sensor.new_text_sensor(mon_conf[CONF_FIRMWARE_VERSION])

            hw_id_sens = await text_sensor.new_text_sensor(mon_conf[CONF_HARDWARE_ID])

            # Register binding in C++ hub
            cg.add(
                var.add_monitor_binding(
                    cg.RawExpression(
                        f'gensam::GenSAMMonitorBinding{{"{name}", "{serial}", '
                        f"{unique_id}U, "
                        f"{temp_sens}, {in_sens}, {out_sens}, {online_sens}, "
                        f"{mute_sw}, "
                        f"{model_sens}, {serial_sens}, {fw_sens}, {hw_id_sens}}}"
                    )
                )
            )
