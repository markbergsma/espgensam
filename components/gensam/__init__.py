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
CONF_INPUT = "input"
CONF_INITIAL_INPUT = "initial_input"
CONF_GROUP_MODIFIED = "group_modified"
CONF_BUS_STATUS = "bus_status"
CONF_STATUS_LED = "status_led"
CONF_MODE = "mode"

CONF_GROUPS = "groups"
CONF_GROUP_SELECT = "group_select"
CONF_DEFAULT_GROUP = "default_group"
CONF_PEQ_DESIGN_RATE = "peq_design_rate"
CONF_DEVICES = "devices"
CONF_ENABLED = "enabled"
CONF_SOURCE = "source"
CONF_CROSSOVER = "crossover"
CONF_LEVEL_DB = "level_db"
CONF_DELAY_SAMPLES = "delay_samples"
CONF_DELAY_MS = "delay_ms"
CONF_FREQUENCY = "frequency"
CONF_GAIN = "gain"
CONF_Q = "q"
CONF_TYPE = "type"

# Number of parametric EQ slots per device; mirrors PEQ_BAND_COUNT in const.h.
PEQ_BAND_COUNT = 20

# Bass management crossover bounds; mirror the *_CROSSOVER_HZ constants in const.h.
DEFAULT_CROSSOVER_HZ = 85
MIN_CROSSOVER_HZ = 50
MAX_CROSSOVER_HZ = 120

# Per-device level trim and time-of-flight delay bounds; mirror const.h.
#
# A group device and a monitor's own number entity must agree on these. publish_state() does
# not clamp, so a group value outside the entity's declared range would be published as an
# out-of-range state when the group push mirrors it onto the binding.
MIN_LEVEL_DB = -60.0
MAX_LEVEL_DB = 0.0
LEVEL_STEP_DB = 0.1
MAX_DELAY_SAMPLES = 9216
MAX_DELAY_MS = 192.0
DELAY_STEP_MS = 0.1

# YAML `source:` -> (C++ source constant, C++ AES3 channel constant).
#
# One key covers both bytes because that is how a GLM setup file expresses it, as a single
# `Input:` enum, and because the pair is never usefully mixed: an analog device has no
# sub-channel. The .sam numbering is 1=A, 2=B, 3=A+B sum, 4=analog.
GROUP_SOURCES = {
    "analog": ("gensam::SOURCE_ANALOG", "gensam::AES3_CHANNEL_A"),
    "aes3_a": ("gensam::SOURCE_DIGITAL_AES3", "gensam::AES3_CHANNEL_A"),
    "aes3_b": ("gensam::SOURCE_DIGITAL_AES3", "gensam::AES3_CHANNEL_B"),
    "aes3_sum": ("gensam::SOURCE_DIGITAL_AES3", "gensam::AES3_CHANNEL_SUM"),
}

# YAML `type:` -> C++ PeqType. "notch" is GLM's name for what the DSP computes as a peaking
# filter, and is accepted under both names.
PEQ_TYPES = {
    "notch": "gensam::PeqType::PEAKING",
    "peaking": "gensam::PeqType::PEAKING",
    "low_shelf": "gensam::PeqType::LOW_SHELF",
    "high_shelf": "gensam::PeqType::HIGH_SHELF",
    "bypass": "gensam::PeqType::BYPASS",
}

gensam_ns = cg.esphome_ns.namespace("gensam")
GenSAMHub = gensam_ns.class_("GenSAMHub", cg.Component)
GenSAMMuteSwitch = gensam_ns.class_("GenSAMMuteSwitch", switch.Switch)
GenSAMIdentifyButton = gensam_ns.class_("GenSAMIdentifyButton", button.Button)
GenSAMRediscoverButton = gensam_ns.class_("GenSAMRediscoverButton", button.Button)
GenSAMCrossoverNumber = gensam_ns.class_("GenSAMCrossoverNumber", number.Number)
GenSAMLevelNumber = gensam_ns.class_("GenSAMLevelNumber", number.Number)
GenSAMDelayNumber = gensam_ns.class_("GenSAMDelayNumber", number.Number)
GenSAMVolumeNumber = gensam_ns.class_("GenSAMVolumeNumber", number.Number)
GenSAMInputSelect = gensam_ns.class_("GenSAMInputSelect", select.Select)
GenSAMGroupSelect = gensam_ns.class_("GenSAMGroupSelect", select.Select)
GenSAMBusStatusSensor = gensam_ns.class_("GenSAMBusStatusSensor", text_sensor.TextSensor)
GenSAMStatusLED = gensam_ns.class_("GenSAMStatusLED", cg.Component)
StatusLEDMode = gensam_ns.enum("StatusLEDMode", is_class=True)
STATUS_LED_MODES = {
    "bus_status": StatusLEDMode.BUS_STATUS,
}
GenSAMMonitorBinding = gensam_ns.struct("GenSAMMonitorBinding")

# Select options for a monitor's input, in the order Home Assistant offers them. Must match
# the INPUT_STR_* strings in input.h exactly: those are what the entity publishes back.
INPUT_OPTIONS = [
    "Analog",
    "AES3 Channel A (Left)",
    "AES3 Channel B (Right)",
    "AES3 Channel A+B (Sum)",
]

# Accepted spellings for a monitor's `input:`, sharing GROUP_SOURCES' keys so a hand-written
# monitor and a generated group describe routing the same way.
INPUT_ALIASES = {
    "analog": "analog",
    "a": "aes3_a",
    "left": "aes3_a",
    "aes3_a": "aes3_a",
    "b": "aes3_b",
    "right": "aes3_b",
    "aes3_b": "aes3_b",
    "sum": "aes3_sum",
    "a+b": "aes3_sum",
    "aes3_sum": "aes3_sum",
}


def _parse_input(val):
    """Resolve a monitor's `input:` to (C++ source, C++ channel, configured).

    Returns configured=False when nothing was given, which is what keeps boot
    non-destructive: the binding holds a placeholder, the select publishes nothing, and no
    0x40 frame is sent until a group is applied, Home Assistant picks an option, or a GLM
    frame is snooped. A monitor keeps whatever routing its own flash already held.
    """
    if val is None:
        return GROUP_SOURCES["analog"] + (False,)
    key = INPUT_ALIASES.get(str(val).strip().lower())
    if key is None:
        raise cv.Invalid(
            f"Invalid input: '{val}'. Valid options: "
            + ", ".join(sorted(set(INPUT_ALIASES.values())))
        )
    return GROUP_SOURCES[key] + (True,)


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

    # Level and delay are box-entry rather than sliders: at 0.1 resolution their ranges are
    # 600 and 1920 positions, unlike the crossover's 14.
    if CONF_LEVEL_DB not in conf:
        c = {
            CONF_NAME: f"{name} Level",
            CONF_DISABLED_BY_DEFAULT: True,
            CONF_MODE: "box",
        }
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_LEVEL_DB] = number.number_schema(
            GenSAMLevelNumber,
            icon="mdi:tune-vertical",
            unit_of_measurement="dB",
            entity_category=ENTITY_CATEGORY_CONFIG,
        )(c)

    if CONF_DELAY_MS not in conf:
        c = {
            CONF_NAME: f"{name} Delay",
            CONF_DISABLED_BY_DEFAULT: True,
            CONF_MODE: "box",
        }
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_DELAY_MS] = number.number_schema(
            GenSAMDelayNumber,
            icon="mdi:timer-outline",
            unit_of_measurement="ms",
            entity_category=ENTITY_CATEGORY_CONFIG,
        )(c)

    if CONF_INPUT in conf:
        val = conf[CONF_INPUT]
        if isinstance(val, str):
            conf[CONF_INITIAL_INPUT] = val
            c = {CONF_NAME: f"{name} Input"}
            if dev_id:
                c[CONF_DEVICE_ID] = dev_id
            conf[CONF_INPUT] = select.select_schema(
                GenSAMInputSelect,
                icon="mdi:audio-input-xlr",
                entity_category=ENTITY_CATEGORY_CONFIG,
            )(c)
        elif isinstance(val, dict) and "initial" in val:
            conf[CONF_INITIAL_INPUT] = val.pop("initial")
    else:
        c = {CONF_NAME: f"{name} Input"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_INPUT] = select.select_schema(
            GenSAMInputSelect,
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
            cv.Optional(CONF_LEVEL_DB): number.number_schema(
                GenSAMLevelNumber,
                icon="mdi:tune-vertical",
                unit_of_measurement="dB",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            cv.Optional(CONF_DELAY_MS): number.number_schema(
                GenSAMDelayNumber,
                icon="mdi:timer-outline",
                unit_of_measurement="ms",
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            # Only needed for a model whose PEQ design rate is not yet known; otherwise it
            # is derived from the discovered model. See PEQ_RATE_* in const.h.
            cv.Optional(CONF_PEQ_DESIGN_RATE): cv.int_range(min=8000, max=192000),
            cv.Optional(CONF_INPUT): cv.Any(
                cv.string,
                select.select_schema(
                    GenSAMInputSelect,
                    icon="mdi:audio-input-xlr",
                    entity_category=ENTITY_CATEGORY_CONFIG,
                ),
            ),
        }
    ),
    _validate_monitor,
)

def _validate_peq_band(conf):
    """A peaking band needs a Q; the shelving types have a fixed one and must not carry it."""
    kind = conf[CONF_TYPE]
    if kind in ("notch", "peaking"):
        if CONF_Q not in conf:
            raise cv.Invalid(f"A '{kind}' filter requires 'q'")
    elif CONF_Q in conf:
        raise cv.Invalid(
            f"A '{kind}' filter must not specify 'q': GLM exposes no slope control for the "
            f"shelving bands and uses a fixed value per type"
        )
    return conf


PEQ_BAND_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_TYPE): cv.one_of(*PEQ_TYPES, lower=True),
            cv.Required(CONF_FREQUENCY): cv.positive_float,
            cv.Required(CONF_GAIN): cv.float_,
            cv.Optional(CONF_Q): cv.positive_float,
        }
    ),
    _validate_peq_band,
)

GROUP_DEVICE_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_UNIQUE_ID): cv.positive_int,
        cv.Optional(CONF_ENABLED, default=True): cv.boolean,
        cv.Optional(CONF_SOURCE, default="analog"): cv.one_of(*GROUP_SOURCES, lower=True),
        cv.Optional(CONF_CROSSOVER): cv.int_range(min=MIN_CROSSOVER_HZ, max=MAX_CROSSOVER_HZ),
        # Attenuation only. A GLM setup file writes -999 for "not calibrated", and anything
        # at or below -130 dB encodes as digital silence, so the floor is deliberately well
        # above both: a sentinel leaking through here would mute the speaker.
        #
        # Both bounds are shared with the per-monitor number entities, which the group push
        # publishes to. They must not be looser here than there.
        cv.Optional(CONF_LEVEL_DB, default=0.0): cv.float_range(
            min=MIN_LEVEL_DB, max=MAX_LEVEL_DB
        ),
        cv.Optional(CONF_DELAY_SAMPLES, default=0): cv.int_range(
            min=0, max=MAX_DELAY_SAMPLES
        ),
        cv.Optional(CONF_FILTERS, default=[]): cv.All(
            cv.ensure_list(PEQ_BAND_SCHEMA), cv.Length(max=PEQ_BAND_COUNT)
        ),
    }
)

GROUP_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string_strict,
        cv.Optional(CONF_CROSSOVER, default=DEFAULT_CROSSOVER_HZ): cv.int_range(
            min=MIN_CROSSOVER_HZ, max=MAX_CROSSOVER_HZ
        ),
        cv.Required(CONF_DEVICES): cv.All(cv.ensure_list(GROUP_DEVICE_SCHEMA), cv.Length(min=1)),
    }
)


def _validate_groups(config):
    """Cross-check the group table against the monitors it refers to."""
    groups = config.get(CONF_GROUPS)
    if not groups:
        if CONF_GROUP_SELECT in config:
            raise cv.Invalid(
                "'group_select' needs a 'groups' block to choose from; the entity would have "
                "no options."
            )
        return config

    known = {m.get(CONF_UNIQUE_ID) for m in config.get(CONF_MONITORS, []) if m.get(CONF_UNIQUE_ID)}
    seen_names = set()
    for group in groups:
        name = group[CONF_NAME]
        if name in seen_names:
            raise cv.Invalid(
                f"Duplicate group name '{name}'. Names are the Home Assistant select options "
                f"and are how a group is looked up, so they must be unique."
            )
        seen_names.add(name)

        seen_ids = set()
        for dev in group[CONF_DEVICES]:
            uid = dev[CONF_UNIQUE_ID]
            if uid in seen_ids:
                raise cv.Invalid(f"Group '{name}' lists unique_id {uid} more than once")
            seen_ids.add(uid)
            if known and uid not in known:
                raise cv.Invalid(
                    f"Group '{name}' refers to unique_id {uid}, which is not one of the "
                    f"configured monitors ({', '.join(str(k) for k in sorted(known))}). "
                    f"In a GLM setup file this is the device's 'Serial:' field."
                )
            # Inherit the group crossover so the generated table is always explicit.
            dev.setdefault(CONF_CROSSOVER, group[CONF_CROSSOVER])

    chosen = config.get(CONF_DEFAULT_GROUP)
    if chosen is None:
        config[CONF_DEFAULT_GROUP] = groups[0][CONF_NAME]
    elif chosen.lower() != "none" and chosen not in seen_names:
        raise cv.Invalid(
            f"default_group '{chosen}' is not one of the configured groups "
            f"({', '.join(sorted(seen_names))}). Use 'none' to apply no group at startup."
        )
    return config


def _validate_hub(config):
    # The deviation sensor only means anything when there is a group to deviate from.
    if CONF_GROUPS in config and CONF_GROUP_MODIFIED not in config:
        config[CONF_GROUP_MODIFIED] = binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:tune-vertical-variant",
        )({CONF_NAME: "Group Modified"})

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
        cv.Optional(CONF_GROUPS): cv.All(cv.ensure_list(GROUP_SCHEMA), cv.Length(min=1)),
        # Which group to apply once discovery completes. Defaults to the first one: having
        # configured groups but applied none leaves the speakers in a state nothing here
        # chose, and the select entity reading "unknown". Set to 'none' to keep the older
        # behaviour of not touching a monitor's stored settings at boot.
        cv.Optional(CONF_DEFAULT_GROUP): cv.string,
        cv.Optional(CONF_GROUP_SELECT): select.select_schema(
            GenSAMGroupSelect,
            icon="mdi:tune-variant",
        ),
        cv.Optional(CONF_REDISCOVER_BUTTON): button.button_schema(
            GenSAMRediscoverButton,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        ),
        cv.Optional(CONF_VOLUME_DB): number.number_schema(
            GenSAMVolumeNumber,
            icon="mdi:volume-high",
            unit_of_measurement="dB",
        ),
        cv.Optional(CONF_GROUP_MODIFIED): binary_sensor.binary_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:tune-vertical-variant",
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

CONFIG_SCHEMA = cv.All(_CONFIG_SCHEMA, _validate_hub, _validate_groups)


def _cpp_float(value):
    """Render a Python float as a C++ float literal without losing a representable digit.

    %.9g is the shortest form that round-trips through float32, which is what the literal
    becomes. Truncating matters here: -8.37833 dB and -8.3783 dB encode to level words 11
    counts apart.

    A whole number formats without a decimal point, and `20f` is not a float literal -- it
    parses as a user-defined literal suffix and fails to compile -- so one is added back.
    """
    text = f"{value:.9g}"
    if "." not in text and "e" not in text and "E" not in text and "inf" not in text:
        text += ".0"
    return f"{text}f"


def _cpp_string(value):
    """Render a Python string as a C++ string literal."""
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def _emit_group_table(config):
    """Emit the generated GroupPreset table and return the C++ expression naming it.

    Written as one raw global rather than through constructor arguments: a group holds a few
    hundred filter parameters, which is far past what is readable as a positional initialiser
    and would be brittle to reorder. Everything is `static const` so it lands in flash and
    keeps internal linkage.
    """
    groups = config.get(CONF_GROUPS)
    if not groups:
        return None

    lines = ["", "// Generated from the gensam `groups:` configuration. See groups.h.", ""]

    for gi, group in enumerate(groups):
        for di, dev in enumerate(group[CONF_DEVICES]):
            bands = dev[CONF_FILTERS]
            array = f"gensam_g{gi}_d{di}_bands"
            lines.append(f"static const esphome::gensam::PeqBand {array}[] = {{")
            for band in bands:
                kind = PEQ_TYPES[band[CONF_TYPE]]
                q = band.get(CONF_Q, 0.0)
                lines.append(
                    f"    {{{kind}, {_cpp_float(band[CONF_FREQUENCY])}, "
                    f"{_cpp_float(band[CONF_GAIN])}, {_cpp_float(q)}}},"
                )
            if not bands:
                # A zero-length array is ill-formed in C++; an explicit bypass slot keeps the
                # generated code valid and means the same thing.
                lines.append("    {esphome::gensam::PeqType::BYPASS, 0.0f, 0.0f, 0.0f},")
            lines.append("};")

    for gi, group in enumerate(groups):
        lines.append(f"static const esphome::gensam::GroupDevice gensam_g{gi}_devices[] = {{")
        for di, dev in enumerate(group[CONF_DEVICES]):
            source, channel = GROUP_SOURCES[dev[CONF_SOURCE]]
            count = len(dev[CONF_FILTERS])
            lines.append(
                f"    {{{dev[CONF_UNIQUE_ID]}u, {str(dev[CONF_ENABLED]).lower()}, "
                f"{dev[CONF_CROSSOVER]}u, {source}, {channel}, "
                f"{_cpp_float(dev[CONF_LEVEL_DB])}, {dev[CONF_DELAY_SAMPLES]}u, "
                f"gensam_g{gi}_d{di}_bands, {count}u}},"
            )
        lines.append("};")

    lines.append("static const esphome::gensam::GroupPreset gensam_group_table[] = {")
    for gi, group in enumerate(groups):
        lines.append(
            f"    {{{_cpp_string(group[CONF_NAME])}, gensam_g{gi}_devices, "
            f"{len(group[CONF_DEVICES])}u}},"
        )
    lines.append("};")

    cg.add_global(cg.RawStatement("\n".join(lines)))
    return len(groups)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    group_count = _emit_group_table(config)
    if group_count:
        cg.add(var.set_group_table(cg.RawExpression("gensam_group_table"), group_count))

    chosen = config.get(CONF_DEFAULT_GROUP)
    if group_count and chosen and chosen.lower() != "none":
        names = [g[CONF_NAME] for g in config[CONF_GROUPS]]
        cg.add(var.set_default_group(names.index(chosen)))

    if CONF_GROUP_SELECT in config:
        group_sel = await select.new_select(
            config[CONF_GROUP_SELECT],
            options=[g[CONF_NAME] for g in config[CONF_GROUPS]],
        )
        cg.add(group_sel.set_hub(var))
        cg.add(var.set_group_select(group_sel))

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

    if CONF_GROUP_MODIFIED in config:
        modified_sens = await binary_sensor.new_binary_sensor(config[CONF_GROUP_MODIFIED])
        cg.add(var.set_group_modified_sensor(modified_sens))

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

            # 10b. Level trim and time-of-flight delay numbers
            lvl_num = "nullptr"
            if CONF_LEVEL_DB in mon_conf:
                lvl_var = await number.new_number(
                    mon_conf[CONF_LEVEL_DB],
                    min_value=MIN_LEVEL_DB,
                    max_value=MAX_LEVEL_DB,
                    step=LEVEL_STEP_DB,
                )
                cg.add(lvl_var.set_hub(var))
                cg.add(lvl_var.set_serial_or_id(target_id))
                lvl_num = f"{lvl_var}"

            dly_num = "nullptr"
            if CONF_DELAY_MS in mon_conf:
                dly_var = await number.new_number(
                    mon_conf[CONF_DELAY_MS],
                    min_value=0.0,
                    max_value=MAX_DELAY_MS,
                    step=DELAY_STEP_MS,
                )
                cg.add(dly_var.set_hub(var))
                cg.add(dly_var.set_serial_or_id(target_id))
                dly_num = f"{dly_var}"

            # 11. Optional PEQ design rate override; 0 means derive it from the model
            design_rate = mon_conf.get(CONF_PEQ_DESIGN_RATE, 0)

            # 12. Input routing select
            input_conf = mon_conf[CONF_INPUT]
            src_c, ch_c, configured = _parse_input(mon_conf.get(CONF_INITIAL_INPUT))
            input_sel = await select.new_select(input_conf, options=INPUT_OPTIONS)
            cg.add(input_sel.set_hub(var))
            cg.add(input_sel.set_serial_or_id(target_id))

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
                        f"{lvl_num}, 0.0f, false, "
                        f"{dly_num}, 0U, false, "
                        f"{input_sel}, {src_c}, {ch_c}, "
                        f"{'true' if configured else 'false'}, {design_rate}U}}"
                    )
                )
            )
