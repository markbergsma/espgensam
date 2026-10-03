"""Thin wrapper over the Linux kernel CEC API (<linux/cec.h>, /dev/cecN).

Only the kernel interface lives here: struct layouts, ioctl numbers, adapter configuration and
a reader thread. CEC protocol behaviour (opcodes, replies) belongs in audio_system.py.

The ctypes structures mirror the kernel's UAPI structs exactly; their sizes are part of the
ioctl numbers, so a layout mistake shows up as ENOTTY rather than as silent corruption. Expected
sizes: cec_msg 56, cec_log_addrs 92, cec_caps 76, cec_event 80. Compare `python3 linux_cec.py`
against /usr/include/linux/cec.h when in doubt.
"""

import ctypes
import errno
import fcntl
import logging
import os
import select
import struct
import threading
from typing import Callable, NamedTuple

log = logging.getLogger(__name__)

# --- Constants from <linux/cec.h> ---

CEC_MAX_MSG_SIZE = 16
CEC_MAX_LOG_ADDRS = 4

CEC_LOG_ADDR_TV = 0
CEC_LOG_ADDR_AUDIOSYSTEM = 5
CEC_LOG_ADDR_BROADCAST = 15
CEC_LOG_ADDR_INVALID = 0xFF
CEC_PHYS_ADDR_INVALID = 0xFFFF

CEC_LOG_ADDR_TYPE_AUDIOSYSTEM = 4
CEC_OP_PRIM_DEVTYPE_AUDIOSYSTEM = 5
CEC_OP_ALL_DEVTYPE_AUDIOSYSTEM = 0x08

CEC_OP_CEC_VERSION_1_4 = 5
CEC_OP_CEC_VERSION_2_0 = 6

CEC_VENDOR_ID_NONE = 0xFFFFFFFF

# features[] bytes for CEC 2.0 <Report Features>.
CEC_OP_FEAT_RC_SRC_PROFILE = 0x40  # RC profile: source device, no menu keys
CEC_OP_FEAT_DEV_HAS_SET_AUDIO_VOLUME_LEVEL = 0x01

CEC_CAP_LOG_ADDRS = 1 << 1
CEC_CAP_TRANSMIT = 1 << 2

CEC_MODE_INITIATOR = 0x01
CEC_MODE_EXCL_FOLLOWER = 0x20

CEC_TX_STATUS_OK = 1 << 0
CEC_TX_STATUS_NACK = 1 << 2

CEC_EVENT_STATE_CHANGE = 1
CEC_EVENT_LOST_MSGS = 2
CEC_EVENT_FL_INITIAL_STATE = 1 << 0


# --- Kernel structs ---

class cec_msg(ctypes.Structure):
    _fields_ = [
        ("tx_ts", ctypes.c_uint64),
        ("rx_ts", ctypes.c_uint64),
        ("len", ctypes.c_uint32),
        ("timeout", ctypes.c_uint32),
        ("sequence", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("msg", ctypes.c_uint8 * CEC_MAX_MSG_SIZE),
        ("reply", ctypes.c_uint8),
        ("rx_status", ctypes.c_uint8),
        ("tx_status", ctypes.c_uint8),
        ("tx_arb_lost_cnt", ctypes.c_uint8),
        ("tx_nack_cnt", ctypes.c_uint8),
        ("tx_low_drive_cnt", ctypes.c_uint8),
        ("tx_error_cnt", ctypes.c_uint8),
    ]


class cec_log_addrs(ctypes.Structure):
    _fields_ = [
        ("log_addr", ctypes.c_uint8 * CEC_MAX_LOG_ADDRS),
        ("log_addr_mask", ctypes.c_uint16),
        ("cec_version", ctypes.c_uint8),
        ("num_log_addrs", ctypes.c_uint8),
        ("vendor_id", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("osd_name", ctypes.c_char * 15),
        ("primary_device_type", ctypes.c_uint8 * CEC_MAX_LOG_ADDRS),
        ("log_addr_type", ctypes.c_uint8 * CEC_MAX_LOG_ADDRS),
        ("all_device_types", ctypes.c_uint8 * CEC_MAX_LOG_ADDRS),
        ("features", (ctypes.c_uint8 * 12) * CEC_MAX_LOG_ADDRS),
    ]


class cec_caps(ctypes.Structure):
    _fields_ = [
        ("driver", ctypes.c_char * 32),
        ("name", ctypes.c_char * 32),
        ("available_log_addrs", ctypes.c_uint32),
        ("capabilities", ctypes.c_uint32),
        ("version", ctypes.c_uint32),
    ]


class cec_event_state_change(ctypes.Structure):
    _fields_ = [
        ("phys_addr", ctypes.c_uint16),
        ("log_addr_mask", ctypes.c_uint16),
        ("have_conn_info", ctypes.c_uint16),
    ]


class cec_event_union(ctypes.Union):
    _fields_ = [
        ("state_change", cec_event_state_change),
        ("lost_msgs", ctypes.c_uint32),
        ("raw", ctypes.c_uint32 * 16),
    ]


class cec_event(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [
        ("ts", ctypes.c_uint64),
        ("event", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("u", cec_event_union),
    ]


# --- ioctl numbers (asm-generic encoding, as used on ARM and arm64) ---

_IOC_WRITE = 1
_IOC_READ = 2


def _ioc(direction, nr, size):
    return (direction << 30) | (size << 16) | (ord("a") << 8) | nr


CEC_ADAP_G_CAPS = _ioc(_IOC_READ | _IOC_WRITE, 0, ctypes.sizeof(cec_caps))
CEC_ADAP_G_PHYS_ADDR = _ioc(_IOC_READ, 1, 2)
CEC_ADAP_G_LOG_ADDRS = _ioc(_IOC_READ, 3, ctypes.sizeof(cec_log_addrs))
CEC_ADAP_S_LOG_ADDRS = _ioc(_IOC_READ | _IOC_WRITE, 4, ctypes.sizeof(cec_log_addrs))
CEC_TRANSMIT = _ioc(_IOC_READ | _IOC_WRITE, 5, ctypes.sizeof(cec_msg))
CEC_RECEIVE = _ioc(_IOC_READ | _IOC_WRITE, 6, ctypes.sizeof(cec_msg))
CEC_DQEVENT = _ioc(_IOC_READ | _IOC_WRITE, 7, ctypes.sizeof(cec_event))
CEC_S_MODE = _ioc(_IOC_WRITE, 9, 4)


# --- Python-side views ---

class CecMessage(NamedTuple):
    initiator: int
    destination: int
    opcode: int | None  # None for a poll message (header byte only)
    operands: bytes

    @property
    def is_broadcast(self):
        return self.destination == CEC_LOG_ADDR_BROADCAST


class CecEvent(NamedTuple):
    kind: int
    flags: int
    phys_addr: int = CEC_PHYS_ADDR_INVALID
    log_addr_mask: int = 0
    lost_msgs: int = 0


def format_phys_addr(pa):
    if pa == CEC_PHYS_ADDR_INVALID:
        return "f.f.f.f"
    return ".".join(str((pa >> shift) & 0xF) for shift in (12, 8, 4, 0))


class CecAdapter:
    """One open /dev/cecN file handle, configured as a single logical address."""

    def __init__(self, path):
        self.path = path
        self.fd = -1

    def open(self):
        self.fd = os.open(self.path, os.O_RDWR)
        caps = cec_caps()
        fcntl.ioctl(self.fd, CEC_ADAP_G_CAPS, caps, True)
        log.info("%s: driver %s, adapter %s", self.path,
                 caps.driver.decode(errors="replace"), caps.name.decode(errors="replace"))
        needed = CEC_CAP_LOG_ADDRS | CEC_CAP_TRANSMIT
        if caps.capabilities & needed != needed:
            raise RuntimeError(f"{self.path} does not let userspace claim logical addresses "
                               f"(capabilities 0x{caps.capabilities:x})")

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1

    def set_mode(self, mode):
        fcntl.ioctl(self.fd, CEC_S_MODE, struct.pack("I", mode))

    def phys_addr(self):
        buf = bytearray(2)
        fcntl.ioctl(self.fd, CEC_ADAP_G_PHYS_ADDR, buf, True)
        return struct.unpack("H", buf)[0]

    def log_addrs(self):
        las = cec_log_addrs()
        fcntl.ioctl(self.fd, CEC_ADAP_G_LOG_ADDRS, las, True)
        return las

    def clear_log_addrs(self):
        fcntl.ioctl(self.fd, CEC_ADAP_S_LOG_ADDRS, cec_log_addrs(), True)

    def claim_audio_system(self, osd_name, cec_version):
        """Claim logical address 5 as an Audio System. Blocks until the claim completes.

        The adapter must be unconfigured before a new configuration is accepted, so any
        earlier one (e.g. from cec-ctl) is cleared first. Neither ALLOW_UNREG_FALLBACK nor
        ALLOW_RC_PASSTHRU is set: if address 5 is taken we want to know, not silently become
        Unregistered, and key presses must not be injected into the Pi's input subsystem.
        """
        self.clear_log_addrs()
        las = cec_log_addrs()
        las.cec_version = cec_version
        las.num_log_addrs = 1
        las.vendor_id = CEC_VENDOR_ID_NONE
        las.osd_name = osd_name.encode("ascii", errors="replace")[:14]
        las.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_AUDIOSYSTEM
        las.log_addr_type[0] = CEC_LOG_ADDR_TYPE_AUDIOSYSTEM
        las.all_device_types[0] = CEC_OP_ALL_DEVTYPE_AUDIOSYSTEM
        if cec_version >= CEC_OP_CEC_VERSION_2_0:
            las.features[0][0] = CEC_OP_FEAT_RC_SRC_PROFILE
            las.features[0][1] = CEC_OP_FEAT_DEV_HAS_SET_AUDIO_VOLUME_LEVEL
        fcntl.ioctl(self.fd, CEC_ADAP_S_LOG_ADDRS, las, True)
        return las

    def transmit(self, initiator, destination, opcode, operands=b""):
        """Send one message and wait for the bus result. Returns the kernel tx_status."""
        data = bytes([(initiator << 4) | destination, opcode]) + bytes(operands)
        msg = cec_msg()
        msg.len = len(data)
        msg.msg[:len(data)] = data
        fcntl.ioctl(self.fd, CEC_TRANSMIT, msg, True)
        return msg.tx_status

    def receive(self):
        msg = cec_msg()
        fcntl.ioctl(self.fd, CEC_RECEIVE, msg, True)
        data = bytes(msg.msg[:msg.len])
        return CecMessage(initiator=data[0] >> 4, destination=data[0] & 0xF,
                          opcode=data[1] if len(data) > 1 else None, operands=data[2:])

    def dqevent(self):
        ev = cec_event()
        fcntl.ioctl(self.fd, CEC_DQEVENT, ev, True)
        if ev.event == CEC_EVENT_STATE_CHANGE:
            return CecEvent(ev.event, ev.flags, ev.state_change.phys_addr,
                            ev.state_change.log_addr_mask)
        if ev.event == CEC_EVENT_LOST_MSGS:
            return CecEvent(ev.event, ev.flags, lost_msgs=ev.lost_msgs)
        return CecEvent(ev.event, ev.flags)


class CecReader(threading.Thread):
    """Waits on the adapter and hands received messages and events to callbacks.

    The kernel signals pending messages with POLLIN but pending events only with POLLPRI,
    which asyncio's add_reader() cannot watch, so this runs as its own thread. The callbacks
    are invoked on this thread; the caller is expected to marshal them onto its event loop.
    """

    def __init__(self, adapter: CecAdapter, on_message: Callable[[CecMessage], None],
                 on_event: Callable[[CecEvent], None], on_error: Callable[[Exception], None]):
        super().__init__(name="cec-reader", daemon=True)
        self.adapter = adapter
        self.on_message = on_message
        self.on_event = on_event
        self.on_error = on_error
        self._stop = threading.Event()

    def stop(self):
        self._stop.set()

    def run(self):
        poller = select.poll()
        poller.register(self.adapter.fd, select.POLLIN | select.POLLPRI)
        try:
            while not self._stop.is_set():
                for _, mask in poller.poll(500):
                    if mask & (select.POLLERR | select.POLLHUP | select.POLLNVAL):
                        raise OSError(errno.ENODEV, f"{self.adapter.path} went away")
                    if mask & select.POLLPRI:
                        self.on_event(self.adapter.dqevent())
                    if mask & select.POLLIN:
                        self.on_message(self.adapter.receive())
        except Exception as e:  # noqa: BLE001 - reported to the owner, which shuts down
            if not self._stop.is_set():
                self.on_error(e)


if __name__ == "__main__":
    for name in ("cec_msg", "cec_log_addrs", "cec_caps", "cec_event"):
        print(f"sizeof({name}) = {ctypes.sizeof(globals()[name])}")
    for name in ("CEC_ADAP_G_CAPS", "CEC_ADAP_G_PHYS_ADDR", "CEC_ADAP_G_LOG_ADDRS",
                 "CEC_ADAP_S_LOG_ADDRS", "CEC_TRANSMIT", "CEC_RECEIVE", "CEC_DQEVENT",
                 "CEC_S_MODE"):
        print(f"{name} = 0x{globals()[name]:08x}")
