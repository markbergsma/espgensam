import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import sensor, binary_sensor, button, text_sensor, switch, number, select, light
from esphome.const import (
    CONF_ID,
    CONF_NAME,
    CONF_DEVICE_ID,
    CONF_FILTERS,
    CONF_RX_PIN,
    CONF_TX_PIN,
    CONF_LIGHT,
    CONF_BRIGHTNESS,
    UNIT_CELSIUS,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_IDENTIFY,
    STATE_CLASS_MEASUREMENT,
    ENTITY_CATEGORY_DIAGNOSTIC,
    ENTITY_CATEGORY_CONFIG,
    CONF_DISABLED_BY_DEFAULT,
)

AUTO_LOAD = ["sensor", "binary_sensor", "button", "text_sensor", "switch", "number", "select", "light"]
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

CONF_REDISCOVER_BUTTON = "rediscover_button"
CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY = "bass_management_crossover_frequency"
CONF_VOLUME_DB = "volume_db"
CONF_AUDIO_SOURCE = "audio_source"
CONF_AES3_CHANNEL = "aes3_channel"
CONF_INITIAL_AES3_CHANNEL = "initial_aes3_channel"
CONF_BUS_STATUS = "bus_status"
CONF_STATUS_LED = "status_led"
CONF_MODE = "mode"

gensam_ns = cg.esphome_ns.namespace("gensam")
GenSAMHub = gensam_ns.class_("GenSAMHub", cg.Component)
GenSAMMuteSwitch = gensam_ns.class_("GenSAMMuteSwitch", switch.Switch)
GenSAMIdentifyButton = gensam_ns.class_("GenSAMIdentifyButton", button.Button)
GenSAMRediscoverButton = gensam_ns.class_("GenSAMRediscoverButton", button.Button)
GenSAMCrossoverNumber = gensam_ns.class_("GenSAMCrossoverNumber", number.Number)
GenSAMVolumeNumber = gensam_ns.class_("GenSAMVolumeNumber", number.Number)
GenSAMSourceSelect = gensam_ns.class_("GenSAMSourceSelect", select.Select)
GenSAMAES3ChannelSelect = gensam_ns.class_("GenSAMAES3ChannelSelect", select.Select)
GenSAMBusStatusSensor = gensam_ns.class_("GenSAMBusStatusSensor", text_sensor.TextSensor)
GenSAMStatusLED = gensam_ns.class_("GenSAMStatusLED", cg.Component)
StatusLEDMode = gensam_ns.enum("StatusLEDMode", is_class=True)
STATUS_LED_MODES = {
    "bus_status": StatusLEDMode.BUS_STATUS,
}
GenSAMMonitorBinding = gensam_ns.struct("GenSAMMonitorBinding")

def _parse_aes3_channel(val, monitor_name="", serial_number=""):
    """Parse an AES3 channel value, or infer a smart default.

    Smart defaults when no explicit channel is configured:
      - Serial number starting with "7" (7xxx subwoofers) → Channel A+B (Sum)
      - Monitor name containing "right" → Channel B (Right)
      - Monitor name containing "sub" → Channel A+B (Sum)
      - Otherwise → Channel A (Left)
    """
    if val is not None:
        s = str(val).strip().lower()
        if s in ("a", "left", "channel a", "channel a (left)", "1"):
            return 1  # AES3_CHANNEL_A
        if s in ("b", "right", "channel b", "channel b (right)", "2"):
            return 2  # AES3_CHANNEL_B
        if s in ("sum", "a+b", "a + b", "channel a+b", "channel a+b (sum)", "both", "3"):
            return 3  # AES3_CHANNEL_SUM
        raise cv.Invalid(f"Invalid AES3 channel: '{val}'. Valid options: 'a' ('left'), 'b' ('right'), 'sum'")

    # Smart default: 7xxx series serials are subwoofers → summed mono
    if serial_number and serial_number.startswith("7"):
        return 3  # AES3_CHANNEL_SUM
    # Smart default based on monitor name keywords
    name_lower = monitor_name.lower()
    if "right" in name_lower:
        return 2  # AES3_CHANNEL_B
    if "sub" in name_lower:
        return 3  # AES3_CHANNEL_SUM
    return 1  # AES3_CHANNEL_A


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

    if CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY not in conf:
        c = {
            CONF_NAME: f"{name} Bass Management Crossover Frequency",
            CONF_DISABLED_BY_DEFAULT: True,
        }
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY] = number.number_schema(
            GenSAMCrossoverNumber,
            icon="mdi:sine-wave",
            unit_of_measurement="Hz",
            entity_category=ENTITY_CATEGORY_CONFIG,
        )(c)

    if CONF_AES3_CHANNEL in conf:
        val = conf[CONF_AES3_CHANNEL]
        if isinstance(val, str):
            conf[CONF_INITIAL_AES3_CHANNEL] = val
            c = {CONF_NAME: f"{name} AES3 Channel"}
            if dev_id:
                c[CONF_DEVICE_ID] = dev_id
            conf[CONF_AES3_CHANNEL] = select.select_schema(
                GenSAMAES3ChannelSelect,
                icon="mdi:audio-input-xlr",
                entity_category=ENTITY_CATEGORY_CONFIG,
            )(c)
        elif isinstance(val, dict):
            if "channel" in val:
                conf[CONF_INITIAL_AES3_CHANNEL] = val.pop("channel")
            elif "initial_channel" in val:
                conf[CONF_INITIAL_AES3_CHANNEL] = val.pop("initial_channel")
    else:
        c = {CONF_NAME: f"{name} AES3 Channel"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_AES3_CHANNEL] = select.select_schema(
            GenSAMAES3ChannelSelect,
            icon="mdi:audio-input-xlr",
            entity_category=ENTITY_CATEGORY_CONFIG,
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
            cv.Optional(CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY): number.number_schema(
                GenSAMCrossoverNumber,
                icon="mdi:sine-wave",
                unit_of_measurement="Hz",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            cv.Optional(CONF_AES3_CHANNEL): cv.Any(
                cv.string,
                select.select_schema(
                    GenSAMAES3ChannelSelect,
                    icon="mdi:audio-input-xlr",
                    entity_category=ENTITY_CATEGORY_CONFIG,
                ),
            ),
        }
    ),
    _validate_monitor,
)

def _validate_hub(config):
    if CONF_AUDIO_SOURCE not in config:
        config[CONF_AUDIO_SOURCE] = select.select_schema(
            GenSAMSourceSelect,
            icon="mdi:audio-input-xlr",
        )({CONF_NAME: "Audio Source"})

    if CONF_STATUS_LED in config:
        led_conf = config[CONF_STATUS_LED]
        mode = led_conf.get(CONF_MODE, "bus_status")
        if mode == "bus_status" and CONF_BUS_STATUS not in config:
            config[CONF_BUS_STATUS] = text_sensor.text_sensor_schema(
                GenSAMBusStatusSensor,
                icon="mdi:information-outline",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            )({CONF_NAME: "Bus Status"})

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
        cv.Optional(CONF_GLM_INACTIVITY_COOLDOWN, default="15s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_POLL_INTERVAL, default="1s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_TELEMETRY_AVERAGING_PERIOD, default="60s"): cv.time_period_str_unit,
        cv.Optional(CONF_MIN_VOLUME_DB, default=-80.0): cv.float_,
        cv.Optional(CONF_MAX_VOLUME_DB, default=0.0): cv.float_,
        cv.Optional(CONF_STARTUP_VOLUME_DB, default=-30.0): cv.float_,
        cv.Optional(CONF_MONITORS): cv.ensure_list(MONITOR_SCHEMA),
        cv.Optional(CONF_REDISCOVER_BUTTON): button.button_schema(
            GenSAMRediscoverButton,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_VOLUME_DB): number.number_schema(
            GenSAMVolumeNumber,
            icon="mdi:volume-high",
            unit_of_measurement="dB",
        ),
        cv.Optional(CONF_AUDIO_SOURCE): select.select_schema(
            GenSAMSourceSelect,
            icon="mdi:audio-input-xlr",
        ),
        cv.Optional(CONF_BUS_STATUS): text_sensor.text_sensor_schema(
            GenSAMBusStatusSensor,
            icon="mdi:information-outline",
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_STATUS_LED): cv.Schema(
            {
                cv.GenerateID(): cv.declare_id(GenSAMStatusLED),
                cv.Required(CONF_LIGHT): cv.use_id(light.LightState),
                cv.Optional(CONF_MODE, default="bus_status"): cv.enum(
                    STATUS_LED_MODES, lower=True
                ),
                cv.Optional(CONF_BRIGHTNESS, default=0.5): cv.percentage,
            }
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

    if CONF_REDISCOVER_BUTTON in config:
        btn = await button.new_button(config[CONF_REDISCOVER_BUTTON])
        cg.add(btn.set_hub(var))

    if CONF_VOLUME_DB in config:
        vol_conf = config[CONF_VOLUME_DB]
        min_val = config[CONF_MIN_VOLUME_DB]
        max_val = config[CONF_MAX_VOLUME_DB]
        vol_num = await number.new_number(
            vol_conf,
            min_value=min_val,
            max_value=max_val,
            step=0.5,
        )
        cg.add(vol_num.set_hub(var))
        cg.add(var.set_volume_number(vol_num))

    if CONF_AUDIO_SOURCE in config:
        source_sel = await select.new_select(
            config[CONF_AUDIO_SOURCE],
            options=["Analog", "Digital (AES3)"],
        )
        cg.add(source_sel.set_hub(var))
        cg.add(var.set_audio_source_select(source_sel))

    bus_sens = None
    if CONF_BUS_STATUS in config:
        bus_sens = await text_sensor.new_text_sensor(config[CONF_BUS_STATUS])
        cg.add(bus_sens.set_hub(var))
        cg.add(var.set_bus_status_sensor(bus_sens))

    if CONF_STATUS_LED in config:
        led_conf = config[CONF_STATUS_LED]
        led_var = cg.new_Pvariable(led_conf[CONF_ID])
        await cg.register_component(led_var, led_conf)
        light_var = await cg.get_variable(led_conf[CONF_LIGHT])
        cg.add(led_var.set_light(light_var))
        cg.add(led_var.set_mode(led_conf[CONF_MODE]))
        cg.add(led_var.set_brightness(led_conf[CONF_BRIGHTNESS]))
        cg.add(led_var.set_hub(var))
        if bus_sens is not None:
            cg.add(led_var.set_source_sensor(bus_sens))

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

            # 10. Bass management crossover frequency number
            xo_num = "nullptr"
            if CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY in mon_conf:
                xo_var = await number.new_number(
                    mon_conf[CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY],
                    min_value=50.0,
                    max_value=120.0,
                    step=5.0,
                )
                cg.add(xo_var.set_hub(var))
                cg.add(xo_var.set_serial_or_id(target_id))
                xo_num = f"{xo_var}"

            # 11. AES3 channel select
            aes3_ch_conf = mon_conf[CONF_AES3_CHANNEL]
            initial_ch = mon_conf.get(CONF_INITIAL_AES3_CHANNEL)
            ch_num = _parse_aes3_channel(initial_ch, name, serial)
            aes3_sel = await select.new_select(
                aes3_ch_conf,
                options=["Channel A (Left)", "Channel B (Right)", "Channel A+B (Sum)"],
            )
            cg.add(aes3_sel.set_hub(var))
            cg.add(aes3_sel.set_serial_or_id(target_id))

            # Register binding in C++ hub
            cg.add(
                var.add_monitor_binding(
                    cg.RawExpression(
                        f'gensam::GenSAMMonitorBinding{{"{name}", "{serial}", '
                        f"{unique_id}U, "
                        f"{temp_sens}, {in_sens}, {out_sens}, {online_sens}, "
                        f"{mute_sw}, "
                        f"{model_sens}, {serial_sens}, {fw_sens}, {hw_id_sens}, "
                        f"{xo_num}, 85U, false, "
                        f"{aes3_sel}, {ch_num}U, false}}"
                    )
                )
            )
