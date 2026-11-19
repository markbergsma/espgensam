import copy
import hashlib
import logging

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import final_validate as fv
from esphome import pins
from esphome.components import sensor, binary_sensor, button, text_sensor, switch, number, select, light, web_server
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
    UNIT_VOLT,
    UNIT_PERCENT,
    ICON_CHIP,
    DEVICE_CLASS_EMPTY,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_PROBLEM,
    DEVICE_CLASS_IDENTIFY,
    STATE_CLASS_MEASUREMENT,
    ENTITY_CATEGORY_DIAGNOSTIC,
    ENTITY_CATEGORY_CONFIG,
    CONF_DISABLED_BY_DEFAULT,
    CONF_INVERTED,
)
from esphome.core import CORE
from esphome.core.config import Device

from . import sam_import

_LOGGER = logging.getLogger(__name__)

AUTO_LOAD = [
    "sensor",
    "binary_sensor",
    "button",
    "text_sensor",
    "switch",
    "number",
    "select",
    "light",
    "web_server_base",
]
MULTI_CONF = True
CONF_DE_PIN = "de_pin"
CONF_RE_PIN = "re_pin"
CONF_POWER_PIN = "power_pin"
CONF_SE_PIN = "se_pin"
CONF_TX_ECHOES_RX = "tx_echoes_rx"
CONF_RX_BUFFER_SIZE = "rx_buffer_size"
CONF_LISTEN_ONLY = "listen_only"
CONF_YIELD_TO_GLM = "yield_to_glm"
CONF_GLM_INACTIVITY_COOLDOWN = "glm_inactivity_cooldown"
CONF_POLL_INTERVAL = "poll_interval"
CONF_CONTROL_WINDOW = "control_window"
CONF_VOLUME_TX_INTERVAL = "volume_tx_interval"
CONF_TELEMETRY_AVERAGING_PERIOD = "telemetry_averaging_period"

CONF_MIN_VOLUME_DB = "min_volume_db"
CONF_MAX_VOLUME_DB = "max_volume_db"
CONF_STARTUP_VOLUME_DB = "startup_volume_db"

CONF_MONITORS = "monitors"
CONF_DISCOVERY_SLOTS = "discovery_slots"
CONF_DISCOVERY_SLOT = "_discovery_slot"
CONF_GENERATED_DEVICE_ID = "_generated_device_id"
CONF_DISCOVERY_SLOT_SETTINGS = "discovery_slot_settings"
CONF_SLOT = "slot"
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
CONF_SAM_NAME_SENSOR = "sam_name"

# Per-driver voice-coil temperatures and supply rails (telemetry tags 0x81-0x83, 0x88/0x89).
# The coil figures are modelled by the speaker's DSP, not measured; a two-way never reports
# the midrange, and only three-way models report the rails.
CONF_TWEETER_COIL_TEMPERATURE = "tweeter_coil_temperature"
CONF_MIDRANGE_COIL_TEMPERATURE = "midrange_coil_temperature"
CONF_WOOFER_COIL_TEMPERATURE = "woofer_coil_temperature"
CONF_PSU_MAIN_VOLTAGE = "psu_main_voltage"
CONF_PSU_LOW_VOLTAGE = "psu_low_voltage"
CONF_PROCESSOR_LOAD = "processor_load"
CONF_DIP_SWITCH_STATUS = "dip_switch_status"

# key -> (default name suffix, unit, device class, accuracy decimals). Order matters: it is the
# order of the trailing sensor pointers in GenSAMMonitorBinding (monitor.h).
EXTENDED_TELEMETRY_SENSORS = {
    CONF_TWEETER_COIL_TEMPERATURE: ("Tweeter Coil Temperature", UNIT_CELSIUS, DEVICE_CLASS_TEMPERATURE, 0),
    CONF_MIDRANGE_COIL_TEMPERATURE: ("Midrange Coil Temperature", UNIT_CELSIUS, DEVICE_CLASS_TEMPERATURE, 0),
    CONF_WOOFER_COIL_TEMPERATURE: ("Woofer Coil Temperature", UNIT_CELSIUS, DEVICE_CLASS_TEMPERATURE, 0),
    CONF_PSU_MAIN_VOLTAGE: ("Main Supply Voltage", UNIT_VOLT, DEVICE_CLASS_VOLTAGE, 1),
    CONF_PSU_LOW_VOLTAGE: ("Low Supply Voltage", UNIT_VOLT, DEVICE_CLASS_VOLTAGE, 1),
    CONF_PROCESSOR_LOAD: ("Processor Load", UNIT_PERCENT, DEVICE_CLASS_EMPTY, 1),
}


def _extended_telemetry_schema(key):
    _, unit, device_class, decimals = EXTENDED_TELEMETRY_SENSORS[key]
    return sensor.sensor_schema(
        unit_of_measurement=unit,
        icon=ICON_CHIP if key == CONF_PROCESSOR_LOAD else cv.UNDEFINED,
        accuracy_decimals=decimals,
        device_class=device_class,
        state_class=STATE_CLASS_MEASUREMENT,
        entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
    )

CONF_REDISCOVER_BUTTON = "rediscover_button"
CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY = "bass_management_crossover_frequency"
CONF_VOLUME_DB = "volume_db"
CONF_SYSTEM_POWER = "system_power"
CONF_SYSTEM_MUTE = "system_mute"
CONF_SYSTEM_LEDS = "system_leds"
CONF_ISS_SLEEP_DELAY = "iss_sleep_delay"
CONF_ISS_SENSITIVITY = "iss_sensitivity"
CONF_PHASE_CORRECTION_BYPASS = "phase_correction_bypass"
CONF_INPUT = "input"
CONF_INITIAL_INPUT = "initial_input"
CONF_CALIBRATION_BYPASS = "calibration_bypass"
CONF_STATUS_LED = "status_led"
CONF_MODE = "mode"

CONF_GROUPS = "groups"
CONF_SAM_FILE = "sam_file"
CONF_SAM_UPLOAD = "sam_upload"
CONF_GROUP_SELECT = "group_select"
CONF_PEQ_DESIGN_RATE = "peq_design_rate"
CONF_DEVICES = "devices"
CONF_ENABLED = "enabled"
CONF_SOURCE = "source"
CONF_CROSSOVER = "crossover"
CONF_LEVEL_DB = "level_db"
CONF_LFE_CHANNEL = "lfe_channel"
CONF_LFE_LEVEL_DB = "lfe_level_db"
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
CROSSOVER_STEP_HZ = 5
# The 0x3B word that turns bass management off; CROSSOVER_FULL_BAND in const.h. Spelled
# `full_band` in YAML, because a GLM setup file's "1" is not a frequency.
CROSSOVER_FULL_BAND = 1
CROSSOVER_FULL_BAND_KEY = "full_band"

# Per-device level trim and time-of-flight delay bounds; mirror const.h.
#
# A group device and a monitor's own number entity must agree on these. publish_state() does
# not clamp, so a group value outside the entity's declared range would be published as an
# out-of-range state when the group push mirrors it onto the binding.
MIN_LEVEL_DB = -60.0
MAX_LEVEL_DB = 0.0
LEVEL_STEP_DB = 0.1
MAX_DELAY_SAMPLES = 9216

# Bounds of the LFE level wire field, a signed byte of whole decibels; mirror const.h.
# These are the encoding's own limits, not a range GLM is known to offer.
MIN_LFE_LEVEL_DB = -128.0
MAX_LFE_LEVEL_DB = 127.0
MAX_DELAY_MS = 192.0
DELAY_STEP_MS = 0.1

# YAML `source:` -> (C++ source constant, C++ AES3 channel constant).
#
# One key covers both bytes because that is how a GLM setup file expresses it, as a single
# `Input:` enum, and because the pair is never usefully mixed: an analog device has no
# sub-channel. The .sam numbering is 1=A, 2=B, 3=A+B sum, 4=analog.
# YAML `lfe_channel:` -> C++ AES3 sub-channel constant. Subwoofers only: this is the feed
# carried on input 1, and "none" is both the default and what a stereo or 2.1 group wants.
GROUP_LFE_CHANNELS = {
    "none": "0",
    "aes3_a": "gensam::AES3_CHANNEL_A",
    "aes3_b": "gensam::AES3_CHANNEL_B",
    "aes3_sum": "gensam::AES3_CHANNEL_SUM",
}

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
SamUploadComponent = gensam_ns.class_("SamUploadComponent", cg.Component)
GenSAMMuteSwitch = gensam_ns.class_("GenSAMMuteSwitch", switch.Switch)
GenSAMSystemPowerSwitch = gensam_ns.class_("GenSAMSystemPowerSwitch", switch.Switch)
GenSAMSystemMuteSwitch = gensam_ns.class_("GenSAMSystemMuteSwitch", switch.Switch)
GenSAMSystemLEDSwitch = gensam_ns.class_("GenSAMSystemLEDSwitch", switch.Switch)
GenSAMCalibrationBypassSwitch = gensam_ns.class_("GenSAMCalibrationBypassSwitch", switch.Switch)
GenSAMMonitorCalibrationBypassSwitch = gensam_ns.class_(
    "GenSAMMonitorCalibrationBypassSwitch", switch.Switch
)
GenSAMIdentifyButton = gensam_ns.class_("GenSAMIdentifyButton", button.Button)
GenSAMRediscoverButton = gensam_ns.class_("GenSAMRediscoverButton", button.Button)
GenSAMLevelNumber = gensam_ns.class_("GenSAMLevelNumber", number.Number)
GenSAMDelayNumber = gensam_ns.class_("GenSAMDelayNumber", number.Number)
GenSAMVolumeNumber = gensam_ns.class_("GenSAMVolumeNumber", number.Number)
GenSAMInputSelect = gensam_ns.class_("GenSAMInputSelect", select.Select)
GenSAMGroupSelect = gensam_ns.class_("GenSAMGroupSelect", select.Select)
GenSAMCrossoverSelect = gensam_ns.class_("GenSAMCrossoverSelect", select.Select)
GenSAMIssSleepDelaySelect = gensam_ns.class_("GenSAMIssSleepDelaySelect", select.Select)
GenSAMIssSensitivitySelect = gensam_ns.class_("GenSAMIssSensitivitySelect", select.Select)
GenSAMPhaseCorrectionBypassSwitch = gensam_ns.class_(
    "GenSAMPhaseCorrectionBypassSwitch", switch.Switch
)
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

# Select options for a monitor's crossover, in order. Must match what crossover_to_str() in
# crossover.h produces for every valid setting; tests/test_crossover.cpp spells the same list.
CROSSOVER_OPTIONS = ["Full band"] + [
    f"{hz} Hz" for hz in range(MIN_CROSSOVER_HZ, MAX_CROSSOVER_HZ + 1, CROSSOVER_STEP_HZ)
]

# ISS select options, in order. Must match ISS_SLEEP_DELAY_OPTIONS and ISS_THRESHOLD_OPTIONS in
# iss.h exactly: those are what the entities publish back.
ISS_SLEEP_DELAY_OPTIONS = [
    "Speaker default", "2 min", "10 min", "30 min", "60 min", "120 min", "180 min", "240 min",
    "Never",
]
ISS_SENSITIVITY_OPTIONS = [
    "Speaker default", "Low (-61 dBu)", "Medium (-68 dBu)", "High (-75 dBu)",
]


def _crossover(value):
    """A group crossover: `full_band`, or a frequency in Hz on the 5 Hz grid.

    Returns the 0x3B word. The integer 1 is rejected even though it is what goes on the wire,
    so that a typo cannot switch bass management off.
    """
    if isinstance(value, str) and value.strip().lower() == CROSSOVER_FULL_BAND_KEY:
        return CROSSOVER_FULL_BAND
    try:
        hz = cv.int_range(min=MIN_CROSSOVER_HZ, max=MAX_CROSSOVER_HZ)(value)
    except cv.Invalid as err:
        raise cv.Invalid(f"{err.msg}, or '{CROSSOVER_FULL_BAND_KEY}' for no bass management") from err
    if (hz - MIN_CROSSOVER_HZ) % CROSSOVER_STEP_HZ:
        raise cv.Invalid(f"crossover must be a multiple of {CROSSOVER_STEP_HZ} Hz, got {hz}")
    return hz


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

# Calibration bypass (switch.h section 5). The hub-wide switch is the primary control, as in
# GLM; the per-monitor one sits with the other per-speaker settings, hidden until wanted.
CALIBRATION_BYPASS_SCHEMA = switch.switch_schema(
    GenSAMCalibrationBypassSwitch,
    icon="mdi:equalizer",
)
MONITOR_CALIBRATION_BYPASS_SCHEMA = switch.switch_schema(
    GenSAMMonitorCalibrationBypassSwitch,
    icon="mdi:equalizer",
    entity_category=ENTITY_CATEGORY_CONFIG,
)


def _validate_monitor(conf):
    name = conf[CONF_NAME]
    dev_id = conf.get(CONF_DEVICE_ID)
    prefix = "" if dev_id and conf.get(CONF_DISCOVERY_SLOT) else f"{name} "

    if CONF_TEMPERATURE not in conf:
        c = {CONF_NAME: f"{prefix}Temperature"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_TEMPERATURE] = sensor.sensor_schema(
            unit_of_measurement=UNIT_CELSIUS,
            accuracy_decimals=0,
            device_class=DEVICE_CLASS_TEMPERATURE,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )(c)

    for key, (suffix, *_rest) in EXTENDED_TELEMETRY_SENSORS.items():
        if key not in conf:
            c = {CONF_NAME: f"{prefix}{suffix}"}
            if dev_id:
                c[CONF_DEVICE_ID] = dev_id
            conf[key] = _extended_telemetry_schema(key)(c)

    if CONF_INPUT_LEVEL not in conf:
        c = {CONF_NAME: f"{prefix}Input Level"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_INPUT_LEVEL] = sensor.sensor_schema(
            unit_of_measurement="dBFS",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )(c)

    if CONF_OUTPUT_LEVEL not in conf:
        c = {CONF_NAME: f"{prefix}Output Level"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_OUTPUT_LEVEL] = sensor.sensor_schema(
            unit_of_measurement="dBFS",
            accuracy_decimals=0,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )(c)

    if CONF_ONLINE not in conf:
        c = {CONF_NAME: f"{prefix}Online"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_ONLINE] = binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )(c)

    if CONF_MUTE not in conf:
        c = {CONF_NAME: f"{prefix}Mute"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_MUTE] = switch.switch_schema(
            GenSAMMuteSwitch,
            icon="mdi:volume-off",
            entity_category=ENTITY_CATEGORY_CONFIG,
        )(c)

    if CONF_IDENTIFY not in conf:
        c = {CONF_NAME: f"{prefix}Identify"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_IDENTIFY] = button.button_schema(
            GenSAMIdentifyButton,
            device_class=DEVICE_CLASS_IDENTIFY,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
        )(c)

    if CONF_MODEL not in conf:
        c = {CONF_NAME: f"{prefix}Model"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_MODEL] = text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        )(c)

    if CONF_SERIAL_NUMBER_SENSOR not in conf:
        c = {CONF_NAME: f"{prefix}Serial Number"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_SERIAL_NUMBER_SENSOR] = text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        )(c)

    if CONF_FIRMWARE_VERSION not in conf:
        c = {CONF_NAME: f"{prefix}Firmware Version"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_FIRMWARE_VERSION] = text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        )(c)

    if CONF_HARDWARE_ID not in conf:
        c = {CONF_NAME: f"{prefix}Hardware ID"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_HARDWARE_ID] = text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        )(c)

    if CONF_DIP_SWITCH_STATUS not in conf:
        c = {CONF_NAME: f"{prefix}DIP Switch Status"}
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_DIP_SWITCH_STATUS] = text_sensor.text_sensor_schema(
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC
        )(c)

    if CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY not in conf:
        c = {
            CONF_NAME: f"{prefix}Bass Management Crossover",
            CONF_DISABLED_BY_DEFAULT: True,
        }
        if dev_id:
            c[CONF_DEVICE_ID] = dev_id
        conf[CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY] = select.select_schema(
            GenSAMCrossoverSelect,
            icon="mdi:sine-wave",
            entity_category=ENTITY_CATEGORY_CONFIG,
        )(c)

    # Level and delay are box-entry rather than sliders: at 0.1 resolution their ranges are
    # 600 and 1920 positions, unlike the crossover select's 16 options.
    if CONF_LEVEL_DB not in conf:
        c = {
            CONF_NAME: f"{prefix}Level",
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
            CONF_NAME: f"{prefix}Delay",
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
            c = {CONF_NAME: f"{prefix}Input"}
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
        c = {CONF_NAME: f"{prefix}Input"}
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
            cv.Optional(CONF_DISCOVERY_SLOT): cv.boolean,
            cv.Optional(CONF_GENERATED_DEVICE_ID): cv.declare_id(Device),
            cv.Optional(CONF_TEMPERATURE): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=0,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            **{
                cv.Optional(key): _extended_telemetry_schema(key)
                for key in EXTENDED_TELEMETRY_SENSORS
            },
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
                entity_category=ENTITY_CATEGORY_CONFIG,
            ),
            cv.Optional(CONF_CALIBRATION_BYPASS): MONITOR_CALIBRATION_BYPASS_SCHEMA,
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
            cv.Optional(CONF_DIP_SWITCH_STATUS): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY): select.select_schema(
                GenSAMCrossoverSelect,
                icon="mdi:sine-wave",
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

DISCOVERY_SLOT_SETTINGS_SCHEMA = web_server.WEBSERVER_SORTING_SCHEMA.extend({
    cv.Required(CONF_SLOT): cv.int_range(min=1, max=32),
    cv.Optional(CONF_DEVICE_ID): cv.sub_device_id,
})

MONITOR_DIAGNOSTICS = (
    CONF_ONLINE, CONF_INPUT_LEVEL, CONF_OUTPUT_LEVEL, CONF_TEMPERATURE, CONF_MODEL,
    CONF_SERIAL_NUMBER_SENSOR, CONF_FIRMWARE_VERSION, CONF_HARDWARE_ID, CONF_IDENTIFY,
    CONF_DIP_SWITCH_STATUS, CONF_SAM_NAME_SENSOR, *EXTENDED_TELEMETRY_SENSORS,
)

MONITOR_CONTROLS = (
    CONF_MUTE, CONF_INPUT, CONF_CALIBRATION_BYPASS, CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY,
    CONF_LEVEL_DB, CONF_DELAY_MS,
)

MONITOR_WEB_GROUP_BASE_WEIGHT = 0


def _monitor_web_cards(config):
    """Plan one web_server v3 sorting group per monitor holding all of its entities.

    The web page otherwise files per-monitor entities under the shared Configuration and
    Diagnostic sections, mixing speakers together. A monitor whose entities already carry
    `web_server:` settings, from `discovery_slot_settings:` or written by hand, is left as
    configured.

    Returns (group_key, group_name, group_weight, [(entity_id, entity_weight), ...]) tuples,
    controls first and diagnostics after, in MONITOR_CONTROLS + MONITOR_DIAGNOSTICS order.
    """
    cards = []
    for index, mon_conf in enumerate(config.get(CONF_MONITORS, [])):
        keys = [k for k in MONITOR_CONTROLS + MONITOR_DIAGNOSTICS if k in mon_conf]
        if any("web_server" in mon_conf[k] for k in keys):
            continue
        entities = [(mon_conf[k][CONF_ID], 10 * (n + 1)) for n, k in enumerate(keys)]
        cards.append((
            f"gensam_monitor_{index + 1}_card",
            mon_conf[CONF_NAME],
            MONITOR_WEB_GROUP_BASE_WEIGHT + index + 1,
            entities,
        ))
    return cards


async def _add_monitor_web_cards(config):
    web_conf = CORE.config.get("web_server")
    if not web_conf or web_conf.get("version") != 3:
        return
    web_server_var = await cg.get_variable(web_conf[CONF_ID])
    cg.add_define("USE_WEBSERVER_SORTING")
    for key, name, weight, entities in _monitor_web_cards(config):
        # Same 64-bit key space as ESPHome's hash() of a sorting group ID; a stable digest
        # keeps the generated source reproducible across builds.
        digest = int.from_bytes(hashlib.sha256(key.encode()).digest()[:8], "little")
        group = cg.RawExpression(f"{digest}ULL")
        cg.add(web_server_var.add_sorting_group(group, name, weight))
        for entity_id, entity_weight in entities:
            entity = await cg.get_variable(entity_id)
            cg.add(web_server_var.add_entity_config(entity, entity_weight, group))


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

def _validate_group_device(conf):
    """Reject an LFE feed that the program input would also carry.

    A subwoofer fed the A+B sum on input 0 and an LFE channel on input 1 gets the LFE content
    twice, once on its own input and once folded into the sum, so it plays several dB hot.
    GLM narrows the program input off the LFE channel rather than allowing this, and
    sam_import.py does the same on the way in; this catches a group written by hand.
    """
    if conf[CONF_LFE_CHANNEL] != "none" and conf[CONF_SOURCE] == "aes3_sum":
        raise cv.Invalid(
            f"a device with an LFE feed on '{conf[CONF_LFE_CHANNEL]}' cannot also take the "
            f"AES3 A+B sum as its program input: the sum already carries the LFE channel, so "
            f"it would be reproduced twice. Name the other channel as 'source'.",
            path=[CONF_SOURCE],
        )
    return conf


GROUP_DEVICE_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.Required(CONF_UNIQUE_ID): cv.positive_int,
            cv.Optional(CONF_ENABLED, default=True): cv.boolean,
            cv.Optional(CONF_SOURCE, default="analog"): cv.one_of(*GROUP_SOURCES, lower=True),
            cv.Optional(CONF_CROSSOVER): _crossover,
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
            # The discrete ".1" feed, on a subwoofer's second input. Ignored for anything else,
            # and the level is ignored unless a channel is set: with no LFE feed there is nothing
            # for it to apply to.
            cv.Optional(CONF_LFE_CHANNEL, default="none"): cv.one_of(
                *GROUP_LFE_CHANNELS, lower=True
            ),
            cv.Optional(CONF_LFE_LEVEL_DB, default=0.0): cv.float_range(
                min=MIN_LFE_LEVEL_DB, max=MAX_LFE_LEVEL_DB
            ),
            cv.Optional(CONF_FILTERS, default=[]): cv.All(
                cv.ensure_list(PEQ_BAND_SCHEMA), cv.Length(max=PEQ_BAND_COUNT)
            ),
        }
    ),
    _validate_group_device,
)

GROUP_SCHEMA = cv.Schema(
    {
        cv.Required(CONF_NAME): cv.string_strict,
        cv.Optional(CONF_CROSSOVER, default=DEFAULT_CROSSOVER_HZ): _crossover,
        cv.Required(CONF_DEVICES): cv.All(cv.ensure_list(GROUP_DEVICE_SCHEMA), cv.Length(min=1)),
    }
)


def _import_sam_groups(config):
    """Expand `sam_file:` into group presets, ahead of any written out in `groups:`.

    Runs before _validate_hub and _validate_groups so that everything downstream -- the
    duplicate-name check, crossover inheritance and generated C++ table
    -- only ever sees plain groups and needs no
    knowledge that a file was involved.

    Imported groups come first, in the order the setup file lists them, so hand-written extras
    follow the calibrated positions in the generated table.

    This is a whole-config validator rather than a validator on the key because it needs the
    `monitors:` block to know which devices are really yours. _CONFIG_SCHEMA has validated the
    entire mapping by the time it runs, so that does not depend on the order the two keys are
    written in.
    """
    path = config.get(CONF_SAM_FILE)
    if path is None:
        return config

    # Mirrors _validate_groups: a monitor may be declared without a unique_id, which defaults
    # to 0, names no device, and must not filter anything.
    known = {m[CONF_UNIQUE_ID] for m in config.get(CONF_MONITORS, []) if m.get(CONF_UNIQUE_ID)}
    if not known:
        known = None
        _LOGGER.warning(
            "gensam: no monitors with a unique_id are configured, so every device in '%s' is "
            "taken as yours. GLM setup files can retain a speaker that was removed, or one "
            "that was never really there; list your monitors to have those skipped.",
            path,
        )

    try:
        imported = sam_import.load_groups(path, known)
    except sam_import.SamError as err:
        raise cv.Invalid(f"{path}: {err}", path=[CONF_SAM_FILE]) from err
    except OSError as err:
        raise cv.Invalid(f"Could not read '{path}': {err}", path=[CONF_SAM_FILE]) from err

    if not imported:
        raise cv.Invalid(
            f"'{path}' produced no usable groups. Either it holds none, or none of its "
            f"devices are among the configured monitors; the warnings above say which.",
            path=[CONF_SAM_FILE],
        )

    # Validated through the same schema as a hand-written group, not trusted as-is: the level
    # and delay bounds are what keep an uncalibrated setup file from reaching a speaker.
    groups = []
    for group in imported:
        try:
            groups.append(GROUP_SCHEMA(group))
        except cv.Invalid as err:
            where = "".join(
                f"[{p}]" if isinstance(p, int) else f".{p}" for p in err.path
            ).lstrip(".")
            raise cv.Invalid(
                f"'{path}' group '{group[CONF_NAME]}' cannot be used as it stands: "
                f"{err.msg}{f' ({where})' if where else ''}. Convert the file with "
                f"tools/sam2yaml.py and include the result if you need to correct it by hand.",
                path=[CONF_SAM_FILE],
            ) from err

    groups.extend(config.get(CONF_GROUPS, []))
    config[CONF_GROUPS] = groups
    _LOGGER.info(
        "gensam: imported %d group(s) from %s: %s",
        len(imported), path, ", ".join(g[CONF_NAME] for g in imported),
    )
    return config


def _validate_groups(config):
    """Cross-check the group table against the monitors it refers to."""
    groups = config.get(CONF_GROUPS)
    if not groups:
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

    return config


def _validate_hub(config):
    if CONF_GROUP_SELECT in config and CONF_SAM_UPLOAD not in config:
        raise cv.Invalid(
            "'group_select:' requires 'sam_upload:'; upload a setup to choose groups"
        )

    # A transceiver that drives its direction line is normally one whose /RE is tied to DE, so it
    # is deaf while transmitting and nothing loops back. Auto-direction modules keep receiving and
    # do echo. Deriving the default this way means no board config has to state it, while an
    # explicit value still wins for a board that breaks the correlation.
    if CONF_TX_ECHOES_RX not in config:
        config[CONF_TX_ECHOES_RX] = CONF_DE_PIN not in config

    discovery_slots = config.get(CONF_DISCOVERY_SLOTS, 0)
    if discovery_slots and config.get(CONF_MONITORS):
        raise cv.Invalid(
            "'discovery_slots' cannot be combined with static 'monitors:' bindings; "
            "use one binding mode"
        )
    slot_settings = {}
    for settings in config.get(CONF_DISCOVERY_SLOT_SETTINGS, []):
        slot = settings[CONF_SLOT]
        if slot in slot_settings:
            raise cv.Invalid(f"Duplicate discovery slot settings for slot {slot}")
        if slot > discovery_slots:
            raise cv.Invalid(f"Discovery slot {slot} exceeds discovery_slots ({discovery_slots})")
        slot_settings[slot] = settings
    if discovery_slots:
        monitors = config.setdefault(CONF_MONITORS, [])
        hub_id = config.get(CONF_ID)
        device_prefix = hub_id.id if hub_id is not None else "gensam"
        for slot in range(1, discovery_slots + 1):
            settings = slot_settings.get(slot, {})
            monitor = {CONF_NAME: f"Monitor {slot}", CONF_DISCOVERY_SLOT: True}
            if settings.get(CONF_DEVICE_ID):
                monitor[CONF_DEVICE_ID] = settings[CONF_DEVICE_ID]
            else:
                device_id = f"{device_prefix}_monitor_{slot}_device"
                monitor[CONF_DEVICE_ID] = device_id
                monitor[CONF_GENERATED_DEVICE_ID] = device_id
            slot_conf = MONITOR_SCHEMA(monitor)
            monitors.append(slot_conf)

    if CONF_SAM_UPLOAD in config:
        for mon_conf in config.get(CONF_MONITORS, []):
            if CONF_SAM_NAME_SENSOR in mon_conf:
                continue
            prefix = (
                ""
                if mon_conf.get(CONF_DEVICE_ID) and mon_conf.get(CONF_DISCOVERY_SLOT)
                else f"{mon_conf[CONF_NAME]} "
            )
            c = {CONF_NAME: f"{prefix}SAM Name"}
            if mon_conf.get(CONF_DEVICE_ID):
                c[CONF_DEVICE_ID] = mon_conf[CONF_DEVICE_ID]
            mon_conf[CONF_SAM_NAME_SENSOR] = text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC
            )(c)

    # So does bypassing it: only a group push ever sends calibration, so without one there is
    # nothing to take away, and turning the bypass off again could not give back whatever a
    # speaker holds from GLM. Filled here rather than in _validate_monitor, which cannot see
    # the groups, and after _import_sam_groups, so a sam_file counts.
    if CONF_GROUPS in config or CONF_SAM_UPLOAD in config:
        if CONF_CALIBRATION_BYPASS not in config:
            config[CONF_CALIBRATION_BYPASS] = CALIBRATION_BYPASS_SCHEMA(
                {CONF_NAME: "Bypass Calibration"}
            )
        for mon_conf in config.get(CONF_MONITORS, []):
            if CONF_CALIBRATION_BYPASS in mon_conf:
                continue
            c = {
                CONF_NAME: (
                    "Bypass Calibration"
                    if mon_conf.get(CONF_DISCOVERY_SLOT) and mon_conf.get(CONF_DEVICE_ID)
                    else f"{mon_conf[CONF_NAME]} Bypass Calibration"
                ),
                CONF_DISABLED_BY_DEFAULT: True,
            }
            if mon_conf.get(CONF_DEVICE_ID):
                c[CONF_DEVICE_ID] = mon_conf[CONF_DEVICE_ID]
            mon_conf[CONF_CALIBRATION_BYPASS] = MONITOR_CALIBRATION_BYPASS_SCHEMA(c)

    for slot, settings in slot_settings.items():
        if "web_server" not in settings:
            continue
        mon_conf = config[CONF_MONITORS][slot - 1]
        web_settings = settings["web_server"]
        base_weight = web_settings.get(web_server.CONF_SORTING_WEIGHT, 50)
        entities = MONITOR_CONTROLS + MONITOR_DIAGNOSTICS
        for index, key in enumerate(entities):
            if key not in mon_conf:
                continue
            entity_web_settings = copy.deepcopy(web_settings)
            entity_web_settings[web_server.CONF_SORTING_WEIGHT] = (
                base_weight + 10 * (index - len(MONITOR_CONTROLS))
            )
            mon_conf[key]["web_server"] = entity_web_settings

    period = config.get(CONF_TELEMETRY_AVERAGING_PERIOD)
    for mon_conf in config.get(CONF_MONITORS, []):
        # 1. Temperature default filter: 1.0 degC delta + 60s heartbeat
        temp_c = mon_conf.get(CONF_TEMPERATURE, {})
        if CONF_FILTERS not in temp_c:
            temp_c[CONF_FILTERS] = sensor.validate_filters([
                {"delta": 1.0},
                {"heartbeat": "60s"},
            ])
        # 1b. Coil temperatures, supply rails and processor load: same delta + heartbeat, at
        # their resolution
        for key, (_, _, _, decimals) in EXTENDED_TELEMETRY_SENSORS.items():
            ext_c = mon_conf.get(key, {})
            if CONF_FILTERS not in ext_c:
                ext_c[CONF_FILTERS] = sensor.validate_filters([
                    {"delta": 1.0 if decimals == 0 else 0.5},
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


def _final_validate_discovery_devices(config):
    generated_devices = [
        monitor
        for monitor in config.get(CONF_MONITORS, [])
        if CONF_GENERATED_DEVICE_ID in monitor
    ]
    if not generated_devices:
        return config

    esphome_config = fv.full_config.get().get("esphome")
    if esphome_config is None:
        raise cv.Invalid("Generated discovery devices require an 'esphome:' section")

    devices = esphome_config.setdefault("devices", [])
    for monitor in generated_devices:
        devices.append(
            {
                CONF_ID: monitor[CONF_GENERATED_DEVICE_ID],
                CONF_NAME: monitor[CONF_NAME],
            }
        )
    return config


FINAL_VALIDATE_SCHEMA = _final_validate_discovery_devices


_CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(GenSAMHub),
        cv.Required(CONF_TX_PIN): pins.gpio_output_pin_schema,
        cv.Required(CONF_RX_PIN): pins.gpio_input_pin_schema,
        cv.Optional(CONF_DE_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_RE_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_POWER_PIN): pins.gpio_output_pin_schema,
        cv.Optional(CONF_SE_PIN): pins.gpio_output_pin_schema,
        # Whether any of our own transmission can reach RX. Defaults to "no de_pin configured",
        # which is right for every board supported so far. Note this is not about whole frames
        # looping back: auto-direction modules tie /RE to DE too, but their one-shot is triggered
        # by our own start bit and so is necessarily late, letting the opening bits escape. A
        # deliberately driven direction line is asserted before the first bit and leaks nothing.
        # Set it explicitly only for a board that drives DE but leaves its receiver permanently
        # enabled. See components/gensam/rs485.h.
        # Note: `inverted:` is honoured on all of the pins above; `mode:` is not, since the
        # driver configures pull-ups itself and RMT rebinds tx_pin/rx_pin regardless.
        cv.Optional(CONF_TX_ECHOES_RX): cv.boolean,
        cv.Optional(CONF_RX_BUFFER_SIZE, default=512): cv.positive_int,
        cv.Optional(CONF_LISTEN_ONLY, default=False): cv.boolean,
        cv.Optional(CONF_YIELD_TO_GLM, default=True): cv.boolean,
        cv.Optional(CONF_GLM_INACTIVITY_COOLDOWN, default="15s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_POLL_INTERVAL, default="1s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_CONTROL_WINDOW): cv.invalid(
            "control_window was removed: the hub now always polls and manages ISS sleep itself"
        ),
        cv.Optional(CONF_VOLUME_TX_INTERVAL, default="50ms"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_TELEMETRY_AVERAGING_PERIOD, default="60s"): cv.time_period_str_unit,
        cv.Optional(CONF_MIN_VOLUME_DB, default=-80.0): cv.float_,
        cv.Optional(CONF_MAX_VOLUME_DB, default=0.0): cv.float_,
        cv.Optional(CONF_STARTUP_VOLUME_DB, default=-30.0): cv.float_,
        cv.Optional(CONF_MONITORS): cv.ensure_list(MONITOR_SCHEMA),
        cv.Optional(CONF_DISCOVERY_SLOTS, default=0): cv.int_range(min=0, max=32),
        cv.Optional(CONF_DISCOVERY_SLOT_SETTINGS): cv.ensure_list(DISCOVERY_SLOT_SETTINGS_SCHEMA),
        cv.Optional(CONF_GROUPS): cv.All(cv.ensure_list(GROUP_SCHEMA), cv.Length(min=1)),
        # A GLM 5 setup file to read the groups from, converted while the configuration is
        # validated. cv.file_ resolves it against the directory holding this YAML, expands a
        # leading `~` and takes an absolute path as given, so it can point straight at GLM's
        # own setup directory. See sam_import.py.
        cv.Optional(CONF_SAM_FILE): cv.file_,
        cv.Optional(CONF_SAM_UPLOAD): cv.Schema(
            {
                cv.GenerateID(): cv.declare_id(SamUploadComponent),
            }
        ),
        cv.Optional(CONF_GROUP_SELECT): select.select_schema(
            GenSAMGroupSelect,
            icon="mdi:tune-variant",
        ),
        cv.Optional(CONF_REDISCOVER_BUTTON): button.button_schema(
            GenSAMRediscoverButton,
        ),
        cv.Optional(CONF_VOLUME_DB): number.number_schema(
            GenSAMVolumeNumber,
            icon="mdi:volume-high",
            unit_of_measurement="dB",
        ),
        cv.Optional(CONF_CALIBRATION_BYPASS): CALIBRATION_BYPASS_SCHEMA,
        cv.Optional(CONF_SYSTEM_POWER): switch.switch_schema(
            GenSAMSystemPowerSwitch, icon="mdi:power"
        ),
        cv.Optional(CONF_SYSTEM_MUTE): switch.switch_schema(
            GenSAMSystemMuteSwitch, icon="mdi:volume-off"
        ),
        cv.Optional(CONF_SYSTEM_LEDS): switch.switch_schema(
            GenSAMSystemLEDSwitch, icon="mdi:led-on"
        ),
        cv.Optional(CONF_ISS_SLEEP_DELAY): select.select_schema(
            GenSAMIssSleepDelaySelect,
            icon="mdi:sleep",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
        cv.Optional(CONF_ISS_SENSITIVITY): select.select_schema(
            GenSAMIssSensitivitySelect,
            icon="mdi:waveform",
            entity_category=ENTITY_CATEGORY_CONFIG,
        ),
        cv.Optional(CONF_PHASE_CORRECTION_BYPASS): switch.switch_schema(
            GenSAMPhaseCorrectionBypassSwitch,
            icon="mdi:sine-wave",
            entity_category=ENTITY_CATEGORY_CONFIG,
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

CONFIG_SCHEMA = cv.All(_CONFIG_SCHEMA, _import_sam_groups, _validate_hub, _validate_groups)


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
                f"{GROUP_LFE_CHANNELS[dev[CONF_LFE_CHANNEL]]}, "
                f"{_cpp_float(dev[CONF_LFE_LEVEL_DB])}, "
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

    if CONF_GROUP_SELECT in config:
        group_sel = await select.new_select(
            config[CONF_GROUP_SELECT],
            options=["No uploaded groups"],
        )
        cg.add(group_sel.set_hub(var))
        cg.add(var.set_group_select(group_sel))

    if CONF_SAM_UPLOAD in config:
        if not CORE.is_esp32:
            raise cv.Invalid("sam_upload requires an ESP32 target with ESP-IDF partition support")
        from esphome.components.esp32 import add_partition

        # Two complete slots allow validation and CRC verification before an atomic commit;
        # the previous valid setup survives power loss while the replacement is written.
        add_partition("sam_slot_a", "data", 0x40, 0x11000)
        add_partition("sam_slot_b", "data", 0x40, 0x11000)
        upload_conf = config[CONF_SAM_UPLOAD]
        upload = cg.new_Pvariable(upload_conf[CONF_ID])
        await cg.register_component(upload, upload_conf)
        cg.add(upload.set_hub(var))
        if CONF_GROUP_SELECT in config:
            cg.add(upload.set_group_select(group_sel))

    # The GPIO number and its polarity are passed as plain values rather than the GPIOPin object:
    # the RMT and gpio_ll layers need a gpio_num_t, and the direction line is written from an ISR
    # where a virtual digital_write() into flash would not be safe. gpio_pin_expression() is still
    # awaited for each pin so ESPHome's pin-conflict detection sees them.
    async def _add_pin(key, setter):
        if key not in config:
            return
        pin = await cg.gpio_pin_expression(config[key])
        cg.add(setter(pin.get_pin(), config[key][CONF_INVERTED]))

    await _add_pin(CONF_TX_PIN, var.set_tx_pin)
    await _add_pin(CONF_RX_PIN, var.set_rx_pin)
    await _add_pin(CONF_DE_PIN, var.set_de_pin)
    await _add_pin(CONF_RE_PIN, var.set_re_pin)
    await _add_pin(CONF_POWER_PIN, var.set_power_pin)
    await _add_pin(CONF_SE_PIN, var.set_se_pin)

    cg.add(var.set_tx_echoes_rx(config[CONF_TX_ECHOES_RX]))

    cg.add(var.set_rx_buffer_size(config[CONF_RX_BUFFER_SIZE]))
    cg.add(var.set_listen_only(config[CONF_LISTEN_ONLY]))
    cg.add(var.set_yield_to_glm(config[CONF_YIELD_TO_GLM]))
    cg.add(var.set_glm_inactivity_cooldown(config[CONF_GLM_INACTIVITY_COOLDOWN]))
    cg.add(var.set_poll_interval(config[CONF_POLL_INTERVAL]))
    cg.add(var.set_volume_tx_interval(config[CONF_VOLUME_TX_INTERVAL]))

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

    if CONF_CALIBRATION_BYPASS in config:
        bypass_sw = await switch.new_switch(config[CONF_CALIBRATION_BYPASS])
        cg.add(bypass_sw.set_hub(var))
        cg.add(var.set_calibration_bypass_switch(bypass_sw))

    if CONF_SYSTEM_MUTE in config:
        mute_sw = await switch.new_switch(config[CONF_SYSTEM_MUTE])
        cg.add(mute_sw.set_hub(var))

    if CONF_SYSTEM_POWER in config:
        power_sw = await switch.new_switch(config[CONF_SYSTEM_POWER])
        cg.add(power_sw.set_hub(var))

    if CONF_SYSTEM_LEDS in config:
        leds_sw = await switch.new_switch(config[CONF_SYSTEM_LEDS])
        cg.add(leds_sw.set_hub(var))

    if CONF_ISS_SLEEP_DELAY in config:
        iss_delay_sel = await select.new_select(
            config[CONF_ISS_SLEEP_DELAY], options=ISS_SLEEP_DELAY_OPTIONS
        )
        cg.add(iss_delay_sel.set_hub(var))
        cg.add(var.set_iss_sleep_delay_select(iss_delay_sel))

    if CONF_ISS_SENSITIVITY in config:
        iss_sens_sel = await select.new_select(
            config[CONF_ISS_SENSITIVITY], options=ISS_SENSITIVITY_OPTIONS
        )
        cg.add(iss_sens_sel.set_hub(var))
        cg.add(var.set_iss_threshold_select(iss_sens_sel))

    if CONF_PHASE_CORRECTION_BYPASS in config:
        phase_sw = await switch.new_switch(config[CONF_PHASE_CORRECTION_BYPASS])
        cg.add(phase_sw.set_hub(var))
        cg.add(var.set_phase_correction_bypass_switch(phase_sw))

    if CONF_STATUS_LED in config:
        led_conf = config[CONF_STATUS_LED]
        led_var = cg.new_Pvariable(led_conf[CONF_ID])
        await cg.register_component(led_var, led_conf)
        light_var = await cg.get_variable(led_conf[CONF_LIGHT])
        cg.add(led_var.set_light(light_var))
        cg.add(led_var.set_mode(led_conf[CONF_MODE]))
        cg.add(led_var.set_brightness(led_conf[CONF_BRIGHTNESS]))
        cg.add(led_var.set_hub(var))

    if CONF_MONITORS in config:
        for mon_conf in config[CONF_MONITORS]:
            name = mon_conf[CONF_NAME]
            serial = mon_conf.get(CONF_SERIAL_NUMBER, "")
            unique_id = mon_conf.get(CONF_UNIQUE_ID, 0)
            discovery_slot = mon_conf.get(CONF_DISCOVERY_SLOT, False)

            # 1. Temperature sensor
            temp_sens = await sensor.new_sensor(mon_conf[CONF_TEMPERATURE])

            # 2. Input level sensor
            in_sens = await sensor.new_sensor(mon_conf[CONF_INPUT_LEVEL])

            # 3. Output level sensor
            out_sens = await sensor.new_sensor(mon_conf[CONF_OUTPUT_LEVEL])

            # 3b. Coil temperature, supply rail and processor load sensors
            ext_sens = [
                str(await sensor.new_sensor(mon_conf[key]))
                for key in EXTENDED_TELEMETRY_SENSORS
            ]

            # 4. Online status binary sensor
            online_sens = await binary_sensor.new_binary_sensor(mon_conf[CONF_ONLINE])

            # 5. Mute switch
            mute_sw = await switch.new_switch(mon_conf[CONF_MUTE])
            cg.add(mute_sw.set_hub(var))
            target_id = name if discovery_slot else (serial if serial else str(unique_id))
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
            dip_status_sens = await text_sensor.new_text_sensor(
                mon_conf[CONF_DIP_SWITCH_STATUS]
            )
            sam_name_sens = "nullptr"
            if CONF_SAM_NAME_SENSOR in mon_conf:
                sam_name_sens = await text_sensor.new_text_sensor(
                    mon_conf[CONF_SAM_NAME_SENSOR]
                )

            # 10. Bass management crossover select
            xo_sel = "nullptr"
            if CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY in mon_conf:
                xo_var = await select.new_select(
                    mon_conf[CONF_BASS_MANAGEMENT_CROSSOVER_FREQUENCY],
                    options=CROSSOVER_OPTIONS,
                )
                cg.add(xo_var.set_hub(var))
                cg.add(xo_var.set_serial_or_id(target_id))
                xo_sel = f"{xo_var}"

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

            # 13. Calibration bypass switch, present only when there are groups
            bypass_sw = "nullptr"
            if CONF_CALIBRATION_BYPASS in mon_conf:
                bypass_var = await switch.new_switch(mon_conf[CONF_CALIBRATION_BYPASS])
                cg.add(bypass_var.set_hub(var))
                cg.add(bypass_var.set_serial_or_id(target_id))
                bypass_sw = f"{bypass_var}"

            # Register binding in C++ hub
            cg.add(
                var.add_monitor_binding(
                    cg.RawExpression(
                        f'gensam::GenSAMMonitorBinding{{"{name}", "{serial}", '
                        f"{unique_id}U, "
                        f"{temp_sens}, {in_sens}, {out_sens}, {online_sens}, "
                        f"{mute_sw}, "
                        f"{model_sens}, {serial_sens}, {fw_sens}, {hw_id_sens}, "
                        f"{xo_sel}, {DEFAULT_CROSSOVER_HZ}U, false, "
                        f"{lvl_num}, 0.0f, false, "
                        f"{dly_num}, 0U, false, "
                        f"{input_sel}, {src_c}, {ch_c}, "
                        f"{'true' if configured else 'false'}, {design_rate}U, "
                        f"{bypass_sw}, false, "
                        f"{'true' if discovery_slot else 'false'}, {sam_name_sens}, "
                        f"{', '.join(ext_sens)}, {dip_status_sens}}}"
                    )
                )
            )

        await _add_monitor_web_cards(config)
