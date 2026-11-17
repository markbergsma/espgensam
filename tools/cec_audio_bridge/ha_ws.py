"""Minimal Home Assistant WebSocket client for Genelec volume, mute, and power entities.

It keeps live copies of the entities' states through `subscribe_entities`, which is filtered on
the server and delivers the initial state followed by compressed diffs. It also issues the few
service calls the bridge needs.

Volume writes are coalesced on a latest-value-wins basis. A held volume key can produce a step
every few hundred milliseconds, faster than a round trip through HA and the RS-485 bus, so
each step is computed from a local target rather than from the last reported state. At most one
number.set_value call is in flight; when it completes, the newest target is sent if it changed meanwhile.
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
    def __init__(self, url: str, token: str, volume_entity_id: str,
                 mute_entity_id: str, power_entity_id: str):
        self.url = url
        self.token = token
        self.volume_entity_id = volume_entity_id
        self.mute_entity_id = mute_entity_id
        self.power_entity_id = power_entity_id
        self.entity_ids = [volume_entity_id, mute_entity_id, power_entity_id]
        self.on_change: Callable[[], None] = lambda: None

        self._ws: ClientConnection | None = None
        self._ids = itertools.count(1)
        self._pending: dict[int, asyncio.Future] = {}
        self._entities: dict[str, dict] = {
            entity_id: {"state": None, "attrs": {}} for entity_id in self.entity_ids
        }
        self._tasks: set[asyncio.Task] = set()

        self._target_level: float | None = None
        self._target_level_at = 0.0
        self._level_in_flight = False
        self._target_muted: bool | None = None
        self._target_muted_at = 0.0

    # --- State, as seen by the audio system ---

    @property
    def available(self):
        return self._ws is not None and self._state(self.volume_entity_id) is not None

    @property
    def is_off(self):
        return self._state(self.power_entity_id) == "off"

    def _state(self, entity_id):
        state = self._entities[entity_id]["state"]
        return None if state in (None, "unavailable", "unknown") else state

    def _volume_bounds(self):
        attrs = self._entities[self.volume_entity_id]["attrs"]
        try:
            min_db = float(attrs["min"])
            max_db = float(attrs["max"])
        except (KeyError, TypeError, ValueError):
            return None
        if max_db <= min_db:
            return None
        return min_db, max_db

    @property
    def level(self) -> float | None:
        """Volume 0..1 mapped across the number entity's configured dB range."""
        if self._target_level is not None and (
                self._level_in_flight or time.monotonic() - self._target_level_at < TARGET_HOLD_S):
            return self._target_level
        self._target_level = None
        return self._reported_level()

    def _reported_level(self):
        state = self._state(self.volume_entity_id)
        bounds = self._volume_bounds()
        if state is None or bounds is None:
            return None
        try:
            db = float(state)
        except (TypeError, ValueError):
            return None
        min_db, max_db = bounds
        return min(max((db - min_db) / (max_db - min_db), 0.0), 1.0)

    @property
    def muted(self) -> bool | None:
        if self._target_muted is not None and \
                time.monotonic() - self._target_muted_at < TARGET_HOLD_S:
            return self._target_muted
        self._target_muted = None
        return self._reported_muted()

    def _reported_muted(self):
        state = self._state(self.mute_entity_id)
        return state == "on" if state is not None else None

    # --- Commands ---

    def set_level(self, level: float):
        if self._volume_bounds() is None:
            log.warning("%s has no valid dB range; ignoring volume command",
                        self.volume_entity_id)
            return
        self._target_level = round(min(max(level, 0.0), 1.0), 4)
        self._target_level_at = time.monotonic()
        if not self._level_in_flight:
            self._spawn(self._flush_level())

    def set_muted(self, muted: bool):
        self._target_muted = muted
        self._target_muted_at = time.monotonic()
        service = "turn_on" if muted else "turn_off"
        self._spawn(self._call("switch", service, self.mute_entity_id))

    def turn_on(self):
        self._spawn(self._call("switch", "turn_on", self.power_entity_id))

    def turn_off(self):
        self._spawn(self._call("switch", "turn_off", self.power_entity_id))

    async def _flush_level(self):
        self._level_in_flight = True
        try:
            while self._target_level is not None:
                sent = self._target_level
                bounds = self._volume_bounds()
                if bounds is None:
                    log.warning("%s has no valid dB range; dropping volume command",
                                self.volume_entity_id)
                    break
                min_db, max_db = bounds
                db = min_db + sent * (max_db - min_db)
                await self._call("number", "set_value", self.volume_entity_id, {"value": db})
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

    async def _call(self, domain, service, entity_id, data=None):
        ws = self._ws
        if ws is None:
            log.warning("Home Assistant not connected; dropping %s.%s for %s",
                        domain, service, entity_id)
            return False
        msg_id = next(self._ids)
        fut = asyncio.get_running_loop().create_future()
        self._pending[msg_id] = fut
        try:
            await ws.send(json.dumps({
                "id": msg_id, "type": "call_service", "domain": domain,
                "service": service, "service_data": data or {},
                "target": {"entity_id": entity_id},
            }))
            result = await asyncio.wait_for(fut, CALL_TIMEOUT_S)
        except (ConnectionClosed, ConnectionError, asyncio.TimeoutError) as e:
            log.warning("%s.%s for %s %s: %s", domain, service, entity_id,
                        data or "", type(e).__name__)
            return False
        finally:
            self._pending.pop(msg_id, None)
        if not result.get("success"):
            log.warning("%s.%s for %s %s failed: %s", domain, service, entity_id, data or "",
                        result.get("error", {}).get("message"))
            return False
        log.debug("%s.%s for %s %s ok", domain, service, entity_id, data or "")
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
                                  "entity_ids": self.entity_ids}))
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
                if initial:
                    for entity_id in self.entity_ids:
                        if entity_id not in event.get("a", {}):
                            log.error("%s does not exist in Home Assistant", entity_id)
                initial = False
                self._apply(event)

    def _disconnected(self):
        was_available = self.available
        self._ws = None
        for entity in self._entities.values():
            entity["state"] = None
            entity["attrs"] = {}
        for fut in self._pending.values():
            if not fut.done():
                fut.set_exception(ConnectionError("disconnected"))
        self._pending.clear()
        if was_available:
            self.on_change()

    def _apply(self, event):
        """Apply a subscribe_entities message: "a" adds, "c" changes, "r" removes."""
        changed_any = False
        for entity_id in self.entity_ids:
            added = event.get("a", {}).get(entity_id)
            if added is not None:
                self._entities[entity_id] = {
                    "state": added.get("s"),
                    "attrs": dict(added.get("a", {})),
                }
                changed_any = True
            changed = event.get("c", {}).get(entity_id)
            if changed is not None:
                plus = changed.get("+", {})
                if "s" in plus:
                    self._entities[entity_id]["state"] = plus["s"]
                self._entities[entity_id]["attrs"].update(plus.get("a", {}))
                for key in changed.get("-", {}).get("a", []):
                    self._entities[entity_id]["attrs"].pop(key, None)
                changed_any = True
            if entity_id in event.get("r", []):
                log.warning("%s was removed from Home Assistant", entity_id)
                self._entities[entity_id] = {"state": None, "attrs": {}}
                changed_any = True
        if not changed_any:
            return
        self._reconcile()
        self.on_change()

    def _reconcile(self):
        """Drop local targets that HA has caught up with."""
        reported = self._reported_level()
        if (self._target_level is not None and not self._level_in_flight and reported is not None
                and abs(reported - self._target_level) < 0.005):
            self._target_level = None
        if self._target_muted is not None and self._reported_muted() == self._target_muted:
            self._target_muted = None
