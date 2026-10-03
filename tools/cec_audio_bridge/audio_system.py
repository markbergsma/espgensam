"""CEC Audio System behaviour: System Audio Control, forwarded to a Home Assistant media_player.

WHY LOGICAL ADDRESS 5

In CEC, volume belongs to whichever device holds the Audio System logical address (5). With
none present, the TV keeps volume for itself and playback devices such as an Apple TV send their
volume keys to the TV. Once an Audio System is on the bus and System Audio Mode is on, the TV
mutes its own speaker and forwards volume keys to address 5, and playback devices address it
directly. The bridge claims address 5 and implements just enough of the feature to receive
those keys. It never sends <Active Source> or <Image View On>, so the Pi cannot take over the
display input.

SYSTEM AUDIO MODE HANDSHAKE

- The TV sends <System Audio Mode Request> carrying a physical address to turn the mode on, or
  with no operand to turn it off. The Audio System answers by broadcasting
  <Set System Audio Mode> [On/Off].
- The Audio System may also initiate by broadcasting <Set System Audio Mode> [On] by itself.
  This is done whenever address 5 is (re)claimed, so a TV that never asks still hands over.
- <Give System Audio Mode Status> is answered with <System Audio Mode Status>.
- <Give Audio Status> is answered with <Report Audio Status>: bit 7 is mute and bits 0-6 the
  volume 0..100, with 0x7F meaning unknown. The same report follows each volume or mute change,
  and is pushed to the TV when the volume changes elsewhere (the dial, GLM), so on-screen
  displays and CEC 2.0 absolute volume control stay in sync.
- CEC 2.0 <Set Audio Volume Level> carries an absolute level 0..100. It is advertised through
  the device features in <Report Features>, which the kernel sends on our behalf.

KEY REPEAT

<User Control Pressed> repeats at most every 450 ms while a key is held. The follower assumes a
release after 550 ms without a repeat, even if <User Control Released> is lost. Volume Up/Down
steps once per press and once per repeat. Mute is a toggle, so only the first press of a hold
acts; otherwise holding it would flap.

WHAT THE KERNEL DOES FOR US

In exclusive-follower mode (non-passthrough) the kernel itself answers <Give Physical Address>,
<Get CEC Version>, <Give OSD Name>, <Give Device Vendor ID>, <Give Features> and <Abort>. Every
other message reaches this module. CEC requires a <Feature Abort> reply to any *directed*
message we do not support. Broadcasts are never aborted, and nothing is ever sent back to the
Unregistered address (15), because a reply to 15 would be a broadcast.
"""

import asyncio
import logging
import time
from typing import Callable, Protocol

from linux_cec import CEC_LOG_ADDR_BROADCAST, CEC_LOG_ADDR_TV, CecMessage

log = logging.getLogger(__name__)

# --- CEC opcodes (subset) ---

FEATURE_ABORT = 0x00
STANDBY = 0x36
USER_CONTROL_PRESSED = 0x44
USER_CONTROL_RELEASED = 0x45
SYSTEM_AUDIO_MODE_REQUEST = 0x70
GIVE_AUDIO_STATUS = 0x71
SET_SYSTEM_AUDIO_MODE = 0x72
SET_AUDIO_VOLUME_LEVEL = 0x73
REPORT_AUDIO_STATUS = 0x7A
GIVE_SYSTEM_AUDIO_MODE_STATUS = 0x7D
SYSTEM_AUDIO_MODE_STATUS = 0x7E
GIVE_PHYSICAL_ADDR = 0x83
GIVE_DEVICE_VENDOR_ID = 0x8C
GIVE_DEVICE_POWER_STATUS = 0x8F
REPORT_POWER_STATUS = 0x90
GET_CEC_VERSION = 0x9F
GIVE_OSD_NAME = 0x46
REPORT_SHORT_AUDIO_DESCRIPTOR = 0xA3
REQUEST_SHORT_AUDIO_DESCRIPTOR = 0xA4
GIVE_FEATURES = 0xA5
ABORT = 0xFF

KERNEL_HANDLED = {GIVE_PHYSICAL_ADDR, GET_CEC_VERSION, GIVE_OSD_NAME, GIVE_DEVICE_VENDOR_ID,
                  GIVE_FEATURES, ABORT}

OPCODE_NAMES = {
    0x00: "Feature Abort", 0x04: "Image View On", 0x0D: "Text View On", 0x36: "Standby",
    0x44: "User Control Pressed", 0x45: "User Control Released", 0x46: "Give OSD Name",
    0x47: "Set OSD Name", 0x70: "System Audio Mode Request", 0x71: "Give Audio Status",
    0x72: "Set System Audio Mode", 0x73: "Set Audio Volume Level", 0x7A: "Report Audio Status",
    0x7D: "Give System Audio Mode Status", 0x7E: "System Audio Mode Status",
    0x80: "Routing Change", 0x81: "Routing Information", 0x82: "Active Source",
    0x83: "Give Physical Address", 0x84: "Report Physical Address",
    0x85: "Request Active Source", 0x86: "Set Stream Path", 0x87: "Device Vendor ID",
    0x89: "Vendor Command", 0x8C: "Give Device Vendor ID", 0x8F: "Give Device Power Status",
    0x90: "Report Power Status", 0x9E: "CEC Version", 0x9F: "Get CEC Version",
    0xA0: "Vendor Command With ID", 0xA3: "Report Short Audio Descriptor",
    0xA4: "Request Short Audio Descriptor", 0xA5: "Give Features", 0xA6: "Report Features",
    0xC0: "Initiate ARC", 0xC1: "Report ARC Initiated", 0xC2: "Report ARC Terminated",
    0xC3: "Request ARC Initiation", 0xC4: "Request ARC Termination", 0xC5: "Terminate ARC",
    0xFF: "Abort",
}

# Feature Abort reasons
ABORT_UNRECOGNIZED_OP = 0
ABORT_INVALID_OP = 3

# UI commands carried by <User Control Pressed>
UI_VOLUME_UP = 0x41
UI_VOLUME_DOWN = 0x42
UI_MUTE = 0x43
UI_MUTE_FUNCTION = 0x65
UI_RESTORE_VOLUME_FUNCTION = 0x66

POWER_STATUS_ON = 0x00
POWER_STATUS_STANDBY = 0x01

AUDIO_STATUS_UNKNOWN = 0x7F

# Short Audio Descriptor for 2-channel LPCM at 32/44.1/48 kHz, 16/20/24 bit.
SAD_LPCM_2CH = bytes([0x09, 0x07, 0x07])
SAD_FORMAT_LPCM = 0x01

KEY_RELEASE_TIMEOUT_S = 0.55
TV_REPORT_DELAY_S = 0.2


def opcode_name(opcode):
    if opcode is None:
        return "Poll"
    return OPCODE_NAMES.get(opcode, f"0x{opcode:02x}")


def describe(msg: CecMessage):
    ops = msg.operands.hex(" ")
    return f"{msg.initiator:x}->{msg.destination:x} {opcode_name(msg.opcode)}" + \
        (f" [{ops}]" if ops else "")


class MediaPlayer(Protocol):
    """The parts of ha_ws.HomeAssistant the audio system uses."""
    available: bool
    is_off: bool
    level: float | None
    muted: bool | None

    def set_level(self, level: float): ...
    def set_muted(self, muted: bool): ...
    def turn_on(self): ...
    def turn_off(self): ...


class AudioSystem:
    """Handles received CEC messages on behalf of logical address 5.

    `send(destination, opcode, *operands)` queues a transmission and must not block.
    All methods run on the asyncio event loop thread.
    """

    def __init__(self, send: Callable[..., None], player: MediaPlayer, step: float,
                 follow_power: bool, initiate_sam: bool):
        self.send = send
        self.player = player
        self.step = step
        self.follow_power = follow_power
        self.initiate_sam = initiate_sam

        self.claimed = False
        self.system_audio_mode = False
        self._held_key: int | None = None
        self._held_until = 0.0
        self._tv_report: asyncio.TimerHandle | None = None
        self._last_tv_status: int | None = None

        self._handlers = {
            SYSTEM_AUDIO_MODE_REQUEST: self._on_system_audio_mode_request,
            GIVE_SYSTEM_AUDIO_MODE_STATUS: self._on_give_system_audio_mode_status,
            GIVE_AUDIO_STATUS: self._on_give_audio_status,
            USER_CONTROL_PRESSED: self._on_user_control_pressed,
            USER_CONTROL_RELEASED: self._on_user_control_released,
            SET_AUDIO_VOLUME_LEVEL: self._on_set_audio_volume_level,
            REQUEST_SHORT_AUDIO_DESCRIPTOR: self._on_request_short_audio_descriptor,
            GIVE_DEVICE_POWER_STATUS: self._on_give_device_power_status,
            STANDBY: self._on_standby,
        }

    # --- Address lifecycle ---

    def on_claimed(self):
        self.claimed = True
        self._last_tv_status = None
        if self.initiate_sam:
            self._set_system_audio_mode(True)

    def on_lost(self):
        self.claimed = False

    def shutdown(self):
        """Hand audio back to the TV before the address is released."""
        if self.claimed and self.system_audio_mode:
            self._set_system_audio_mode(False)

    # --- Incoming messages ---

    def on_message(self, msg: CecMessage):
        log.debug("rx %s", describe(msg))
        if msg.opcode is None or msg.opcode in KERNEL_HANDLED:
            return
        handler = self._handlers.get(msg.opcode)
        # Standby is the only message handled in both its broadcast and directed form.
        if handler is not None and (not msg.is_broadcast or msg.opcode == STANDBY):
            handler(msg)
        elif not msg.is_broadcast and msg.opcode != FEATURE_ABORT:
            self._reply(msg, FEATURE_ABORT, msg.opcode, ABORT_UNRECOGNIZED_OP)

    def _reply(self, msg: CecMessage, opcode, *operands):
        if msg.initiator == CEC_LOG_ADDR_BROADCAST:
            return
        self.send(msg.initiator, opcode, *operands)
        if msg.initiator == CEC_LOG_ADDR_TV and opcode == REPORT_AUDIO_STATUS:
            self._last_tv_status = operands[0]

    def _on_system_audio_mode_request(self, msg):
        on = len(msg.operands) >= 2
        if on and self.follow_power and self.player.is_off:
            self.player.turn_on()
        self._set_system_audio_mode(on)

    def _on_give_system_audio_mode_status(self, msg):
        self._reply(msg, SYSTEM_AUDIO_MODE_STATUS, int(self.system_audio_mode))

    def _on_give_audio_status(self, msg):
        self._reply(msg, REPORT_AUDIO_STATUS, self._audio_status())

    def _on_user_control_pressed(self, msg):
        if not msg.operands:
            self._reply(msg, FEATURE_ABORT, msg.opcode, ABORT_INVALID_OP)
            return
        key = msg.operands[0]
        now = time.monotonic()
        repeat = key == self._held_key and now < self._held_until
        self._held_key = key
        self._held_until = now + KEY_RELEASE_TIMEOUT_S

        if key in (UI_VOLUME_UP, UI_VOLUME_DOWN):
            self._step_volume(1 if key == UI_VOLUME_UP else -1)
        elif key in (UI_MUTE, UI_MUTE_FUNCTION, UI_RESTORE_VOLUME_FUNCTION):
            if repeat:
                return
            if self._ready():
                if key == UI_MUTE:
                    self.player.set_muted(not self.player.muted)
                else:
                    self.player.set_muted(key == UI_MUTE_FUNCTION)
        else:
            log.debug("ignoring UI command 0x%02x", key)
            return
        self._reply(msg, REPORT_AUDIO_STATUS, self._audio_status())

    def _on_user_control_released(self, msg):
        self._held_key = None

    def _on_set_audio_volume_level(self, msg):
        if not msg.operands:
            self._reply(msg, FEATURE_ABORT, msg.opcode, ABORT_INVALID_OP)
            return
        level = msg.operands[0]
        if level <= 100 and self._ready():
            self.player.set_level(level / 100)
        self._reply(msg, REPORT_AUDIO_STATUS, self._audio_status())

    def _on_request_short_audio_descriptor(self, msg):
        # Each operand byte is (format id << 6) | format code; only base-format LPCM is offered.
        if SAD_FORMAT_LPCM in msg.operands:
            self._reply(msg, REPORT_SHORT_AUDIO_DESCRIPTOR, *SAD_LPCM_2CH)
        else:
            self._reply(msg, FEATURE_ABORT, msg.opcode, ABORT_INVALID_OP)

    def _on_give_device_power_status(self, msg):
        standby = self.follow_power and self.player.is_off
        self._reply(msg, REPORT_POWER_STATUS,
                    POWER_STATUS_STANDBY if standby else POWER_STATUS_ON)

    def _on_standby(self, msg):
        if self.follow_power and self.player.available and not self.player.is_off:
            log.info("Standby from %x: turning the speakers off", msg.initiator)
            self.player.turn_off()

    # --- Volume ---

    def _ready(self):
        if self.player.available and self.player.level is not None:
            return True
        log.warning("media player unavailable; ignoring volume command")
        return False

    def _step_volume(self, direction):
        if not self._ready():
            return
        if self.player.muted:
            self.player.set_muted(False)
        self.player.set_level(self.player.level + direction * self.step)

    def _audio_status(self):
        level = self.player.level
        if not self.player.available or level is None:
            return AUDIO_STATUS_UNKNOWN
        volume = min(max(round(level * 100), 0), 100)
        return (0x80 if self.player.muted else 0) | volume

    def _set_system_audio_mode(self, on):
        if on != self.system_audio_mode:
            log.info("System Audio Mode %s", "on" if on else "off")
        self.system_audio_mode = on
        self.send(CEC_LOG_ADDR_BROADCAST, SET_SYSTEM_AUDIO_MODE, int(on))

    # --- Changes made outside CEC ---

    def on_player_changed(self):
        """The media_player changed in HA; tell the TV, coalescing bursts."""
        if not self.claimed or not self.system_audio_mode or self._tv_report is not None:
            return
        self._tv_report = asyncio.get_running_loop().call_later(
            TV_REPORT_DELAY_S, self._report_to_tv)

    def _report_to_tv(self):
        self._tv_report = None
        status = self._audio_status()
        if status != self._last_tv_status and self.claimed and self.system_audio_mode:
            self._last_tv_status = status
            self.send(CEC_LOG_ADDR_TV, REPORT_AUDIO_STATUS, status)
