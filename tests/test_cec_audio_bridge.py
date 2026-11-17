#!/usr/bin/env python3
"""Verify CEC bridge control mapping to Home Assistant number and switch entities."""

import asyncio
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools" / "cec_audio_bridge"))
from ha_ws import HomeAssistant


class FakeWebSocket:
    def __init__(self, client):
        self.client = client
        self.requests = []

    async def send(self, raw):
        request = json.loads(raw)
        self.requests.append(request)
        if request["type"] == "call_service":
            future = self.client._pending[request["id"]]
            asyncio.get_running_loop().call_soon(future.set_result, {"success": True})


class HomeAssistantEntityTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self):
        self.client = HomeAssistant(
            "ws://localhost", "token", "number.volume_db", "switch.mute", "switch.system_power"
        )
        self.client._ws = FakeWebSocket(self.client)
        self.client._apply({
            "a": {
                "number.volume_db": {
                    "s": "-55",
                    "a": {"min": -80, "max": -30},
                },
                "switch.mute": {"s": "off", "a": {}},
                "switch.system_power": {"s": "on", "a": {}},
            }
        })

    async def test_state_and_service_mapping(self):
        self.assertTrue(self.client.available)
        self.assertAlmostEqual(self.client.level, 0.5)
        self.assertFalse(self.client.muted)
        self.assertFalse(self.client.is_off)

        self.client.set_level(0.6)
        self.client.set_muted(True)
        self.client.turn_off()
        await asyncio.gather(*self.client._tasks)

        requests = self.client._ws.requests
        by_entity = {request["target"]["entity_id"]: request for request in requests}
        volume = by_entity["number.volume_db"]
        self.assertEqual((volume["domain"], volume["service"]), ("number", "set_value"))
        self.assertAlmostEqual(volume["service_data"]["value"], -50.0)
        self.assertEqual(by_entity["switch.mute"]["service"], "turn_on")
        self.assertEqual(by_entity["switch.system_power"]["service"], "turn_off")

    def test_external_state_updates(self):
        self.client._apply({
            "c": {
                "number.volume_db": {"+": {"s": "-65"}},
                "switch.mute": {"+": {"s": "on"}},
                "switch.system_power": {"+": {"s": "off"}},
            }
        })
        self.assertAlmostEqual(self.client.level, 0.3)
        self.assertTrue(self.client.muted)
        self.assertTrue(self.client.is_off)


if __name__ == "__main__":
    unittest.main()
