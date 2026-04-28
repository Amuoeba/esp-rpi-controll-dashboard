"""Reokto-demo Flask dashboard.

Serves a single-page UI with a frequency slider. Bridges the browser
with the local Mosquitto broker:

  * Browser -> POST /api/freq -> publish on `led/freq/set` (retained)
  * `led/freq/state` and `led/status` -> in-memory state -> SSE /events

A single background paho-mqtt client maintains the broker connection
for the whole process; per-request publishes go through the same
client (paho is thread-safe for publish).
"""

from __future__ import annotations

import json
import os
import queue
import threading
import time
from typing import Any

import paho.mqtt.client as mqtt
from flask import Flask, Response, jsonify, render_template, request

MQTT_HOST = os.environ.get("REOKTO_MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.environ.get("REOKTO_MQTT_PORT", "1883"))

TOPIC_FREQ_SET = "led/freq/set"
TOPIC_FREQ_STATE = "led/freq/state"
TOPIC_STATUS = "led/status"

FREQ_MIN = 1
FREQ_MAX = 50
FREQ_DEFAULT = 20

app = Flask(__name__)

_state_lock = threading.Lock()
_state: dict[str, Any] = {
    "freq": FREQ_DEFAULT,
    "status": "offline",
    "ts": time.time(),
}

# Each connected SSE client gets its own bounded queue. We never block
# the MQTT thread on a slow browser; if the queue fills up we drop the
# oldest event for that client.
_subscribers: list[queue.Queue[str]] = []
_subscribers_lock = threading.Lock()


def _publish_event(payload: dict[str, Any]) -> None:
    msg = f"data: {json.dumps(payload)}\n\n"
    with _subscribers_lock:
        for q in _subscribers:
            try:
                q.put_nowait(msg)
            except queue.Full:
                try:
                    q.get_nowait()
                except queue.Empty:
                    pass
                try:
                    q.put_nowait(msg)
                except queue.Full:
                    pass


def _snapshot() -> dict[str, Any]:
    with _state_lock:
        return dict(_state)


def _update_state(**kwargs: Any) -> dict[str, Any]:
    with _state_lock:
        _state.update(kwargs)
        _state["ts"] = time.time()
        snap = dict(_state)
    _publish_event(snap)
    return snap


def _on_connect(client: mqtt.Client, _userdata: Any, _flags: Any, rc: int) -> None:
    if rc != 0:
        app.logger.warning("MQTT connect failed rc=%s", rc)
        return
    client.subscribe([(TOPIC_FREQ_STATE, 1), (TOPIC_STATUS, 1)])
    app.logger.info("MQTT connected, subscribed to state topics")


def _on_message(_client: mqtt.Client, _userdata: Any, msg: mqtt.MQTTMessage) -> None:
    try:
        payload = msg.payload.decode("utf-8", errors="replace").strip()
    except Exception:
        return

    if msg.topic == TOPIC_FREQ_STATE:
        try:
            freq = int(payload)
        except ValueError:
            return
        freq = max(FREQ_MIN, min(FREQ_MAX, freq))
        _update_state(freq=freq)
    elif msg.topic == TOPIC_STATUS:
        _update_state(status=payload or "offline")


def _build_mqtt_client() -> mqtt.Client:
    client = mqtt.Client(client_id="reokto-dashboard", clean_session=True)
    client.on_connect = _on_connect
    client.on_message = _on_message
    client.reconnect_delay_set(min_delay=1, max_delay=10)
    return client


_mqtt_client = _build_mqtt_client()


def _mqtt_thread() -> None:
    while True:
        try:
            _mqtt_client.connect(MQTT_HOST, MQTT_PORT, keepalive=30)
            _mqtt_client.loop_forever(retry_first_connection=True)
        except Exception as exc:  # noqa: BLE001 - we genuinely want to retry on anything
            app.logger.warning("MQTT loop crashed: %s; retrying in 2s", exc)
            time.sleep(2)


threading.Thread(target=_mqtt_thread, name="mqtt-loop", daemon=True).start()


@app.route("/")
def index() -> str:
    return render_template(
        "index.html",
        freq_min=FREQ_MIN,
        freq_max=FREQ_MAX,
        initial=_snapshot(),
    )


@app.route("/api/state")
def api_state() -> Response:
    return jsonify(_snapshot())


@app.route("/api/freq", methods=["POST"])
def api_freq() -> Response:
    data = request.get_json(silent=True) or request.form
    try:
        freq = int(data.get("freq"))
    except (TypeError, ValueError):
        return jsonify({"error": "freq must be an integer"}), 400

    if not FREQ_MIN <= freq <= FREQ_MAX:
        return (
            jsonify(
                {
                    "error": f"freq must be in [{FREQ_MIN}, {FREQ_MAX}]",
                }
            ),
            400,
        )

    _mqtt_client.publish(TOPIC_FREQ_SET, str(freq), qos=1, retain=True)
    return jsonify({"ok": True, "freq": freq})


@app.route("/events")
def events() -> Response:
    q: queue.Queue[str] = queue.Queue(maxsize=32)
    with _subscribers_lock:
        _subscribers.append(q)

    def stream():
        try:
            yield f"data: {json.dumps(_snapshot())}\n\n"
            while True:
                try:
                    msg = q.get(timeout=15)
                except queue.Empty:
                    yield ": keepalive\n\n"
                    continue
                yield msg
        finally:
            with _subscribers_lock:
                if q in _subscribers:
                    _subscribers.remove(q)

    return Response(
        stream(),
        mimetype="text/event-stream",
        headers={
            "Cache-Control": "no-cache",
            "X-Accel-Buffering": "no",
        },
    )


if __name__ == "__main__":
    # Bound on 0.0.0.0 so AP-connected clients can reach it.
    app.run(host="0.0.0.0", port=int(os.environ.get("REOKTO_PORT", "80")))
