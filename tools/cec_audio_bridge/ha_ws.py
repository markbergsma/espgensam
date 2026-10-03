"""Minimal Home Assistant WebSocket client for one media_player entity.

It keeps a live copy of the entity's state through `subscribe_entities`, which is filtered on
the server and delivers the initial state followed by compressed diffs. It also issues the few
service calls the bridge needs.

Volume writes are coalesced on a latest-value-wins basis. A held volume key can produce a step
every few hundred milliseconds, faster than a round trip through HA and the RS-485 bus, so
each step is computed from a local target rather than from the last reported state. At most one
volume_set is in flight; when it completes, the newest target is sent if it changed meanwhile.
The target is dropped once HA reports it back, or after a short idle period, after which the
reported state is authoritative again.
"""

import asyncio
import itertools
import json
import logging
import time
from typing import Callable

from websockets.asyncio.client import ClientConnection, connect
from websockets.exceptions import ConnectionClosed, InvalidHandshake, InvalidURI

log = logging.getLogger(__name__)

TARGET_HOLD_S = 1.5
CALL_TIMEOUT_S = 10.0
BACKOFF_MIN_S = 1.0
BACKOFF_MAX_S = 30.0


class AuthError(Exception):
    pass


class HomeAssistant:
    def __init__(self, url: str, token: str, entity_id: str):
        self.url = url
        self.token = token
        self.entity_id = entity_id
        self.on_change: Callable[[], None] = lambda: None

        self._ws: ClientConnection | None = None
        self._ids = itertools.count(1)
        self._pending: dict[int, asyncio.Future] = {}
        self._state: str | None = None
        self._attrs: dict = {}
        self._tasks: set[asyncio.Task] = set()

        self._target_level: float | None = None
        self._target_level_at = 0.0
        self._level_in_flight = False
        self._target_muted: bool | None = None
        self._target_muted_at = 0.0

    # --- State, as seen by the audio system ---

    @property
    def available(self):
        return self._ws is not None and self._state not in (None, "unavailable", "unknown")

    @property
    def is_off(self):
        return self._state == "off"

    @property
    def level(self) -> float | None:
        """Volume 0..1: the pending target if there is one, else what HA last reported."""
        if self._target_level is not None and (
                self._level_in_flight or time.monotonic() - self._target_level_at < TARGET_HOLD_S):
            return self._target_level
        self._target_level = None
        level = self._attrs.get("volume_level")
        return float(level) if level is not None else None

    @property
    def muted(self) -> bool | None:
        if self._target_muted is not None and \
                time.monotonic() - self._target_muted_at < TARGET_HOLD_S:
            return self._target_muted
        self._target_muted = None
        muted = self._attrs.get("is_volume_muted")
        return bool(muted) if muted is not None else None

    # --- Commands ---

    def set_level(self, level: float):
        self._target_level = round(min(max(level, 0.0), 1.0), 4)
        self._target_level_at = time.monotonic()
        if not self._level_in_flight:
            self._spawn(self._flush_level())

    def set_muted(self, muted: bool):
        self._target_muted = muted
        self._target_muted_at = time.monotonic()
        self._spawn(self._call("volume_mute", {"is_volume_muted": muted}))

    def turn_on(self):
        self._spawn(self._call("turn_on"))

    def turn_off(self):
        self._spawn(self._call("turn_off"))

    async def _flush_level(self):
        self._level_in_flight = True
        try:
            while self._target_level is not None:
                sent = self._target_level
                await self._call("volume_set", {"volume_level": sent})
                if self._target_level == sent:
                    break
        finally:
            self._level_in_flight = False
            self._target_level_at = time.monotonic()
            # HA may have echoed the final value while the call was still in flight.
            self._reconcile()
            # If not, the target lapses after the hold; report whatever HA says then.
            asyncio.get_running_loop().call_later(TARGET_HOLD_S + 0.1, self.on_change)

    def _spawn(self, coro):
        task = asyncio.create_task(coro)
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def _call(self, service, data=None):
        ws = self._ws
        if ws is None:
            log.warning("Home Assistant not connected; dropping media_player.%s", service)
            return False
        msg_id = next(self._ids)
        fut = asyncio.get_running_loop().create_future()
        self._pending[msg_id] = fut
        try:
            await ws.send(json.dumps({
                "id": msg_id, "type": "call_service", "domain": "media_player",
                "service": service, "service_data": data or {},
                "target": {"entity_id": self.entity_id},
            }))
            result = await asyncio.wait_for(fut, CALL_TIMEOUT_S)
        except (ConnectionClosed, ConnectionError, asyncio.TimeoutError) as e:
            log.warning("media_player.%s %s: %s", service, data or "", type(e).__name__)
            return False
        finally:
            self._pending.pop(msg_id, None)
        if not result.get("success"):
            log.warning("media_player.%s %s failed: %s", service, data or "",
                        result.get("error", {}).get("message"))
            return False
        log.debug("media_player.%s %s ok", service, data or "")
        return True

    # --- Connection ---

    async def run(self):
        """Stay connected forever, reconnecting with exponential backoff."""
        backoff = BACKOFF_MIN_S
        while True:
            try:
                async with connect(self.url, open_timeout=10) as ws:
                    await self._session(ws)
            except AuthError as e:
                log.error("Home Assistant rejected the token: %s", e)
                backoff = BACKOFF_MAX_S
            except (OSError, ConnectionClosed, InvalidHandshake, InvalidURI,
                    asyncio.TimeoutError) as e:
                log.warning("Home Assistant connection lost: %s", e or type(e).__name__)
            else:
                backoff = BACKOFF_MIN_S
            finally:
                self._disconnected()
            log.info("Reconnecting to Home Assistant in %.0f s", backoff)
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, BACKOFF_MAX_S)

    async def _session(self, ws: ClientConnection):
        msg = json.loads(await ws.recv())
        if msg.get("type") != "auth_required":
            raise ConnectionError(f"unexpected greeting {msg.get('type')!r}")
        await ws.send(json.dumps({"type": "auth", "access_token": self.token}))
        msg = json.loads(await ws.recv())
        if msg.get("type") != "auth_ok":
            raise AuthError(msg.get("message", msg.get("type")))
        log.info("Connected to Home Assistant %s", msg.get("ha_version", ""))

        sub_id = next(self._ids)
        await ws.send(json.dumps({"id": sub_id, "type": "subscribe_entities",
                                  "entity_ids": [self.entity_id]}))
        self._ws = ws
        initial = True
        async for raw in ws:
            msg = json.loads(raw)
            if msg.get("type") == "result":
                if msg.get("id") == sub_id:
                    if not msg.get("success"):
                        raise ConnectionError(f"subscribe_entities failed: {msg.get('error')}")
                    continue
                fut = self._pending.get(msg.get("id"))
                if fut is not None and not fut.done():
                    fut.set_result(msg)
            elif msg.get("type") == "event" and msg.get("id") == sub_id:
                event = msg.get("event", {})
                if initial and self.entity_id not in event.get("a", {}):
                    log.error("%s does not exist in Home Assistant", self.entity_id)
                initial = False
                self._apply(event)

    def _disconnected(self):
        was_available = self.available
        self._ws = None
        self._state = None
        self._attrs = {}
        for fut in self._pending.values():
            if not fut.done():
                fut.set_exception(ConnectionError("disconnected"))
        self._pending.clear()
        if was_available:
            self.on_change()

    def _apply(self, event):
        """Apply a subscribe_entities message: "a" adds, "c" changes, "r" removes."""
        added = event.get("a", {}).get(self.entity_id)
        if added is not None:
            self._state = added.get("s")
            self._attrs = dict(added.get("a", {}))
            log.info("%s is %s, volume %s, muted %s", self.entity_id, self._state,
                     self._attrs.get("volume_level"), self._attrs.get("is_volume_muted"))
        changed = event.get("c", {}).get(self.entity_id)
        if changed is not None:
            plus = changed.get("+", {})
            if "s" in plus:
                self._state = plus["s"]
            self._attrs.update(plus.get("a", {}))
            for key in changed.get("-", {}).get("a", []):
                self._attrs.pop(key, None)
        removed = self.entity_id in event.get("r", [])
        if removed:
            log.warning("%s was removed from Home Assistant", self.entity_id)
            self._state = None
            self._attrs = {}
        if added is None and changed is None and not removed:
            return
        self._reconcile()
        self.on_change()

    def _reconcile(self):
        """Drop local targets that HA has caught up with."""
        reported = self._attrs.get("volume_level")
        if (self._target_level is not None and not self._level_in_flight and reported is not None
                and abs(float(reported) - self._target_level) < 0.005):
            self._target_level = None
        if self._target_muted is not None and \
                self._attrs.get("is_volume_muted") == self._target_muted:
            self._target_muted = None
