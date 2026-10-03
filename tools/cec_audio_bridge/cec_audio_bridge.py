#!/usr/bin/env python3
"""Make a Raspberry Pi the HDMI CEC Audio System and forward volume and mute to Home Assistant.

The Pi claims CEC logical address 5 (Audio System) through the Linux kernel CEC API, so the TV
or projector hands volume control over to it and playback devices such as an Apple TV send it
their volume keys. Volume Up/Down, Mute and absolute volume (CEC 2.0) are forwarded to a
Home Assistant media_player over the WebSocket API. See audio_system.py for the protocol
details.

USAGE

    HA_TOKEN=... python3 cec_audio_bridge.py \\
        --ha-url ws://homeassistant.local:8123/api/websocket \\
        --entity media_player.genelec_sam_system -v

The token is a Home Assistant long-lived access token. Pass it in the HA_TOKEN environment
variable or with --token-file, never on the command line, where other users can see it.
"""

import argparse
import asyncio
import errno
import logging
import os
import pathlib
import signal
import sys

try:
    from ha_ws import HomeAssistant
except ImportError as e:
    sys.exit(f"{e}\nThis needs python3-websockets 13 or newer: apt install python3-websockets")

from audio_system import AudioSystem, describe
from linux_cec import (CEC_EVENT_FL_INITIAL_STATE, CEC_EVENT_LOST_MSGS, CEC_EVENT_STATE_CHANGE,
                       CEC_LOG_ADDR_AUDIOSYSTEM,
                       CEC_LOG_ADDR_INVALID, CEC_MODE_EXCL_FOLLOWER, CEC_MODE_INITIATOR,
                       CEC_OP_CEC_VERSION_1_4, CEC_OP_CEC_VERSION_2_0, CEC_PHYS_ADDR_INVALID,
                       CEC_TX_STATUS_NACK, CEC_TX_STATUS_OK, CecAdapter, CecEvent, CecMessage,
                       CecReader, format_phys_addr)

log = logging.getLogger("cec_audio_bridge")

CEC_VERSIONS = {"1.4": CEC_OP_CEC_VERSION_1_4, "2.0": CEC_OP_CEC_VERSION_2_0}
SHUTDOWN_DRAIN_S = 2.0


class Bridge:
    def __init__(self, args, token):
        self.args = args
        self.adapter = CecAdapter(args.device)
        self.player = HomeAssistant(args.ha_url, token, args.entity)
        self.tx_queue: asyncio.Queue = asyncio.Queue()
        self.audio = AudioSystem(self.send, self.player, args.step, args.follow_power,
                                 initiate_sam=not args.no_initiate_sam)
        self.player.on_change = self.audio.on_player_changed
        self.stop = asyncio.Event()
        self.failure: BaseException | None = None

    def send(self, destination, opcode, *operands):
        self.tx_queue.put_nowait((destination, opcode, bytes(operands)))

    def fail(self, exc: BaseException):
        if self.failure is None:
            self.failure = exc
        self.stop.set()

    def on_event(self, ev: CecEvent):
        if ev.kind == CEC_EVENT_STATE_CHANGE:
            addrs = [str(la) for la in range(15) if ev.log_addr_mask & (1 << la)]
            log.info("Physical address %s, logical address %s", format_phys_addr(ev.phys_addr),
                     ",".join(addrs) or "none")
            if ev.flags & CEC_EVENT_FL_INITIAL_STATE:
                return  # describes whatever configuration preceded our own claim
            claimed = bool(ev.log_addr_mask & (1 << CEC_LOG_ADDR_AUDIOSYSTEM))
            if claimed and not self.audio.claimed:
                self.audio.on_claimed()
            elif not claimed and self.audio.claimed:
                self.audio.on_lost()
        elif ev.kind == CEC_EVENT_LOST_MSGS:
            log.warning("The kernel dropped %d CEC messages", ev.lost_msgs)

    async def sender(self):
        """Transmit queued messages one at a time, in order, off the event loop thread."""
        while True:
            destination, opcode, operands = await self.tx_queue.get()
            desc = describe(CecMessage(CEC_LOG_ADDR_AUDIOSYSTEM, destination, opcode, operands))
            try:
                status = await asyncio.to_thread(self.adapter.transmit, CEC_LOG_ADDR_AUDIOSYSTEM,
                                                 destination, opcode, operands)
            except OSError as e:
                log.warning("tx %s failed: %s", desc, e)
            else:
                if status & CEC_TX_STATUS_OK:
                    log.debug("tx %s", desc)
                elif status & CEC_TX_STATUS_NACK:
                    log.info("tx %s: not acknowledged", desc)
                else:
                    log.warning("tx %s: status 0x%02x", desc, status)
            finally:
                self.tx_queue.task_done()

    def _watch(self, task: asyncio.Task):
        """Shut down if a long-running task dies unexpectedly."""
        def done(t):
            if not t.cancelled() and t.exception() is not None:
                self.fail(t.exception())
        task.add_done_callback(done)
        return task

    async def run(self):
        loop = asyncio.get_running_loop()
        for sig in (signal.SIGINT, signal.SIGTERM):
            loop.add_signal_handler(sig, self.stop.set)

        self.adapter.open()
        try:
            self.adapter.set_mode(CEC_MODE_INITIATOR | CEC_MODE_EXCL_FOLLOWER)
        except OSError as e:
            if e.errno == errno.EBUSY:
                raise RuntimeError(f"another program already follows {self.args.device} "
                                   "exclusively (cec-follower, or a second bridge?)") from e
            raise
        reader = CecReader(
            self.adapter,
            on_message=lambda m: loop.call_soon_threadsafe(self.audio.on_message, m),
            on_event=lambda ev: loop.call_soon_threadsafe(self.on_event, ev),
            on_error=lambda exc: loop.call_soon_threadsafe(self.fail, exc))
        reader.start()
        tasks = [self._watch(asyncio.create_task(self.sender())),
                 self._watch(asyncio.create_task(self.player.run()))]
        try:
            await asyncio.to_thread(self.adapter.claim_audio_system, self.args.osd_name,
                                    CEC_VERSIONS[self.args.cec_version])
            phys_addr = self.adapter.phys_addr()
            if phys_addr == CEC_PHYS_ADDR_INVALID:
                log.warning("No physical address yet: the display is off or not detected. "
                            "Waiting for it to appear.")
            elif self.adapter.log_addrs().log_addr[0] == CEC_LOG_ADDR_INVALID:
                raise RuntimeError("logical address 5 is already taken: another Audio System "
                                   "(soundbar, AV receiver) is on the bus")
            await self.stop.wait()
        finally:
            self.audio.shutdown()
            try:
                await asyncio.wait_for(self.tx_queue.join(), SHUTDOWN_DRAIN_S)
            except TimeoutError:
                pass
            for task in tasks:
                task.cancel()
            reader.stop()
            try:
                self.adapter.clear_log_addrs()
            except OSError as e:
                log.warning("Could not release logical address 5: %s", e)
            reader.join(1)
            self.adapter.close()
        if self.failure is not None:
            raise self.failure


def read_token(args):
    if args.token_file:
        try:
            return pathlib.Path(args.token_file).read_text().strip()
        except OSError as e:
            sys.exit(f"Cannot read the token file: {e}")
    token = os.environ.get("HA_TOKEN", "").strip()
    if not token:
        sys.exit("No Home Assistant token: set HA_TOKEN or use --token-file")
    return token


def step_fraction(value):
    step = float(value)
    if not 0 < step <= 0.5:
        raise argparse.ArgumentTypeError("must be between 0 and 0.5")
    return step


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", default="/dev/cec0", help="CEC device (default /dev/cec0)")
    ap.add_argument("--ha-url", default="ws://homeassistant.local:8123/api/websocket",
                    help="Home Assistant WebSocket API URL (default %(default)s)")
    ap.add_argument("--token-file", help="file holding a long-lived access token "
                                         "(default: the HA_TOKEN environment variable)")
    ap.add_argument("--entity", default="media_player.genelec_sam_system",
                    help="media_player entity to control (default %(default)s)")
    ap.add_argument("--step", type=step_fraction, default=0.02,
                    help="volume change per key press, as a fraction of the media player's "
                         "range (default 0.02, i.e. 1 dB on espgensam's default 50 dB range)")
    ap.add_argument("--osd-name", default="Genelec",
                    help="name shown by the TV, at most 14 characters (default %(default)s)")
    ap.add_argument("--cec-version", choices=sorted(CEC_VERSIONS), default="2.0",
                    help="CEC version to announce (default 2.0, which enables absolute volume)")
    ap.add_argument("--follow-power", action="store_true",
                    help="turn the media player off on CEC Standby, and on when the TV asks "
                         "for System Audio Mode")
    ap.add_argument("--no-initiate-sam", action="store_true",
                    help="do not announce System Audio Mode on startup; wait for the TV to "
                         "request it")
    ap.add_argument("-v", "--verbose", action="store_true", help="log every CEC message")
    args = ap.parse_args()

    stamp = "%(asctime)s " if sys.stderr.isatty() else ""  # journald adds its own
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format=stamp + "%(levelname)s %(name)s: %(message)s")
    logging.getLogger("websockets").setLevel(logging.INFO)
    logging.getLogger("asyncio").setLevel(logging.INFO)

    token = read_token(args)
    try:
        asyncio.run(Bridge(args, token).run())
    except PermissionError as e:
        log.error("%s: run as a user in the 'video' group", e)
        return 1
    except FileNotFoundError as e:
        log.error("%s: is the vc4-kms-v3d overlay enabled?", e)
        return 1
    except (RuntimeError, OSError) as e:
        log.error("%s", e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
