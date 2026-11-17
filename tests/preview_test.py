"""Exercise preview HTTP handlers in isolation, never the running preview or ATOM."""
import importlib.util
import io
from pathlib import Path
import unittest
from urllib.parse import urlencode

ROOT = Path(__file__).resolve().parents[1]


class PreviewTests(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("preview_under_test", ROOT / "scripts/preview.py")
        self.preview = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.preview)
        self.preview.scenario("hr_focus")

    def post(self, path, action_header, **fields):
        handler = object.__new__(self.preview.Handler)
        body = urlencode(fields).encode()
        handler.path = path
        handler.headers = {"Content-Length": str(len(body)), "X-AirBike-Action": action_header}
        handler.rfile = io.BytesIO(body)
        replies = []
        handler.send = lambda data, status=200, content_type=None: replies.append((status, data))
        handler.do_POST()
        return replies[-1]

    def test_old_confirmation_cannot_delete_new_saved_ride(self):
        old_id = self.preview.history[0]["id"]
        self.assertEqual(self.post("/api/rides/delete", "delete-ride", id=old_id)[0], 200)
        self.assertEqual(self.post("/api/rides/control", "ride-control", action="finish")[0], 200)
        new_ride = self.preview.history[0].copy()
        status, _ = self.post("/api/rides/delete", "delete-ride", id=old_id)
        self.assertEqual(status, 404)
        self.assertIn(new_ride, self.preview.history)
        self.assertGreater(new_ride["id"], old_id)

    def test_empty_history_does_not_reset_ids(self):
        highest = max(r["id"] for r in self.preview.history)
        current = self.preview.current.copy()
        for ride in self.preview.history[:]:
            self.assertEqual(self.post("/api/rides/delete", "delete-ride", id=ride["id"])[0], 200)
        self.assertEqual(self.preview.current, current)
        self.post("/api/rides/control", "ride-control", action="finish")
        self.assertGreater(self.preview.history[0]["id"], highest)

    def test_invalid_delete_leaves_history_intact(self):
        original = self.preview.history[:]
        for value in ("0", "-1", "abc", "１"):
            self.assertEqual(self.post("/api/rides/delete", "delete-ride", id=value)[0], 400)
        self.assertEqual(self.post("/api/rides/delete", "delete-ride", id=999999)[0], 404)
        self.assertEqual(self.preview.history, original)

    def test_rider_save_returns_exact_saved_identity(self):
        self.post("/api/rides/clear", "clear-ride")
        status, data = self.post("/api/riders", "riders", id=0, name="Test rider", sex=0)
        self.assertEqual(status, 200)
        created = next(r for r in data["riders"] if r["name"] == "Test rider")
        self.assertEqual(data.get("saved_id"), created["id"])
        status, data = self.post("/api/riders", "riders", id=created["id"], name="Edited rider", sex=0)
        self.assertEqual(status, 200)
        self.assertEqual(data.get("saved_id"), created["id"])
        self.assertEqual(len(data["riders"]), 3)

    def test_trend_contract_and_missing_channels(self):
        trend = self.preview.trend_data()
        self.assertEqual(trend['window_s'], 1800)
        self.assertEqual(trend['rider_id'], 1)
        self.assertEqual(trend['points'][-1], [0, 125, 185])
        self.assertEqual(trend['max_hr'], 190)
        self.preview.mode = 'band_wait'
        self.assertTrue(all(point[1] is None for point in self.preview.trend_data()['points']))
        self.assertTrue(all(point[2] is not None for point in self.preview.trend_data()['points']))
        self.preview.scenario('auto_both')
        self.assertEqual(self.preview.trend_data()['points'], [])


if __name__ == "__main__":
    unittest.main()
