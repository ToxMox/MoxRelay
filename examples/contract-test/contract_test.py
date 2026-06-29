# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 ToxMox / MoxRelay contributors
#
# MoxRelay control-API conformance suite + reference client.
#
# Drives a LIVE MoxRelay instance over its control endpoint and checks the wire behavior
# against the contract (docs/control-api.asyncapi.yaml): framing, every method, the error
# vocabulary, event gating, and reconnect semantics. Python standard library only -- the
# embedded WebSocket client below is also a minimal reference implementation of the wire
# (text frames, JSON, request/reply by id echo, id-less event pushes).
#
# Usage:
#   1. Start a MoxRelay instance. Note its control port (the discovery file
#      %APPDATA%/MoxRelay/helper-config.json carries the bare object's "port").
#   2. python contract_test.py <port>
#
# Exit code 0 = every check passed; 1 = at least one failed.
#
# Notes:
#   - The suite creates its own color source for the mutation checks and removes it again;
#     it does not touch sources it did not create.
#   - GetFleet / fleetChanged are gone (single-instance contract): the suite asserts GetFleet
#     replies -32601, that GetStatus carries publishedSenderNames[], and (when the default
#     discovery file is present) that the file is the canonical bare 8-field object.
#   - The Shutdown test runs LAST: it cleanly terminates the instance (idempotent re-ack +
#     the instanceShuttingDown event observed after the reply), so no checks follow it.

import base64
import json
import os
import socket
import struct
import sys
import tempfile
import time

# ----------------------------------------------------------------------------- ws client


class WsClient:
    """Minimal RFC 6455 client: text frames only, client-side masking, blocking reads
    with deadlines. Loopback control connections need nothing more."""

    def __init__(self, host, port, path="/control"):
        self.sock = socket.create_connection((host, port), timeout=5.0)
        key = base64.b64encode(os.urandom(16)).decode()
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        self.sock.sendall(request.encode())
        response = b""
        while b"\r\n\r\n" not in response:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed during handshake")
            response += chunk
        status = response.split(b"\r\n", 1)[0]
        if b"101" not in status:
            raise ConnectionError(f"upgrade refused: {status!r}")
        self.buffer = b""
        self.inbox = []  # parsed JSON objects not yet consumed

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass

    def send_text(self, text):
        payload = text.encode()
        mask = os.urandom(4)
        header = b"\x81"  # FIN + text opcode
        n = len(payload)
        if n < 126:
            header += bytes([0x80 | n])
        elif n < 65536:
            header += bytes([0x80 | 126]) + struct.pack(">H", n)
        else:
            header += bytes([0x80 | 127]) + struct.pack(">Q", n)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        self.sock.sendall(header + mask + masked)

    def _read_exact(self, n, deadline):
        while len(self.buffer) < n:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError()
            self.sock.settimeout(remaining)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed")
            self.buffer += chunk
        out, self.buffer = self.buffer[:n], self.buffer[n:]
        return out

    def _read_frame(self, deadline):
        head = self._read_exact(2, deadline)
        opcode = head[0] & 0x0F
        length = head[1] & 0x7F
        if length == 126:
            length = struct.unpack(">H", self._read_exact(2, deadline))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._read_exact(8, deadline))[0]
        payload = self._read_exact(length, deadline) if length else b""
        return opcode, payload

    def pump(self, timeout):
        """Read frames until timeout, parsing text frames into the inbox."""
        deadline = time.monotonic() + timeout
        try:
            while True:
                opcode, payload = self._read_frame(deadline)
                if opcode == 0x1:  # text
                    try:
                        self.inbox.append(json.loads(payload.decode()))
                    except ValueError:
                        pass
                elif opcode == 0x9:  # ping -> pong (same payload, masked)
                    mask = os.urandom(4)
                    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
                    self.sock.sendall(bytes([0x8A, 0x80 | len(payload)]) + mask + masked)
                elif opcode == 0x8:  # close
                    raise ConnectionError("server sent close")
        except TimeoutError:
            pass

    def take(self, match, timeout):
        """First inbox message satisfying match(msg), pumping until timeout."""
        deadline = time.monotonic() + timeout
        while True:
            for i, msg in enumerate(self.inbox):
                if match(msg):
                    return self.inbox.pop(i)
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            self.pump(min(0.25, remaining))

    # ---- the protocol helpers (reference usage of the contract) ----

    _next_id = [1]

    def request(self, method, params=None, timeout=5.0):
        rid = WsClient._next_id[0]
        WsClient._next_id[0] += 1
        message = {"id": rid, "method": method}
        if params is not None:
            message["params"] = params
        self.send_text(json.dumps(message))
        return self.take(lambda m: m.get("id") == rid, timeout)

    def await_event(self, name, timeout):
        return self.take(lambda m: "id" not in m and m.get("event") == name, timeout)


# ----------------------------------------------------------------------------- test clip


def write_test_clip(path, width=64, height=36, frames=10, fps=30):
    """Generate a tiny uncompressed AVI (raw bottom-up BGR24 'DIB ' frames) with the standard
    library only, so the media checks need no bundled or third-party media file. Each frame is
    a solid color stepping through a small palette; ~70 KB at the defaults, written to a temp
    location at run time and never committed."""
    row_stride = width * 3  # 64*3 = 192, already DWORD-aligned
    frame_size = row_stride * height
    palette = [(255, 64, 0), (0, 255, 64), (64, 0, 255), (255, 255, 0), (0, 192, 255)]

    def chunk(fourcc, payload):
        pad = b"\x00" if len(payload) % 2 else b""
        return fourcc + struct.pack("<I", len(payload)) + payload + pad

    def lst(fourcc, payload):
        return chunk(b"LIST", fourcc + payload)

    avih = struct.pack(
        "<14I",
        1_000_000 // fps,  # dwMicroSecPerFrame
        frame_size * fps,  # dwMaxBytesPerSec
        0,                 # dwPaddingGranularity
        0x10,              # dwFlags: AVIF_HASINDEX
        frames, 0, 1,      # dwTotalFrames, dwInitialFrames, dwStreams
        frame_size,        # dwSuggestedBufferSize
        width, height,
        0, 0, 0, 0,        # dwReserved
    )
    strh = struct.pack(
        "<4s4sIHHIIIIIIII4H",
        b"vids", b"DIB ",
        0, 0, 0, 0,        # dwFlags, wPriority, wLanguage, dwInitialFrames
        1, fps,            # dwScale, dwRate
        0, frames,         # dwStart, dwLength
        frame_size,        # dwSuggestedBufferSize
        0, 0,              # dwQuality, dwSampleSize
        0, 0, width, height,  # rcFrame
    )
    strf = struct.pack("<IiiHHIIiiII", 40, width, height, 1, 24, 0, frame_size, 0, 0, 0, 0)
    hdrl = lst(b"hdrl", chunk(b"avih", avih) + lst(b"strl", chunk(b"strh", strh) + chunk(b"strf", strf)))

    movi_payload = b"movi"
    index = b""
    for i in range(frames):
        b, g, r = palette[i % len(palette)][2], palette[i % len(palette)][1], palette[i % len(palette)][0]
        frame = bytes((b, g, r)) * (width * height)
        index += b"00db" + struct.pack("<III", 0x10, len(movi_payload), frame_size)
        movi_payload += chunk(b"00db", frame)
    riff = hdrl + lst(b"movi", movi_payload[4:]) + chunk(b"idx1", index)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + len(riff)) + b"AVI " + riff)
    return path


# ----------------------------------------------------------------------------- suite

PASSED = []
FAILED = []


def check(name, condition, detail=""):
    if condition:
        PASSED.append(name)
        print(f"PASS  {name}")
    else:
        FAILED.append(name)
        print(f"FAIL  {name}  {detail}")
    return condition


def is_error(reply, code):
    return bool(reply) and "error" in reply and reply["error"].get("code") == code


def media_status(ws, source_id):
    r = ws.request("GetMediaStatus", {"sourceId": source_id})
    return r["result"] if r and "result" in r else {}


def wait_media_state(ws, source_id, state, timeout):
    """Poll GetMediaStatus until the wanted state (transport actions settle on the next video
    tick; opening->playing settles when the first frame decodes). Returns the last status."""
    deadline = time.monotonic() + timeout
    status = {}
    while time.monotonic() < deadline:
        status = media_status(ws, source_id)
        if status.get("state") == state:
            return status
        time.sleep(0.1)
    return status


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        print("usage: python contract_test.py <port>")
        return 1
    port = int(sys.argv[1])
    ws = WsClient("127.0.0.1", port)
    print(f"connected to ws://127.0.0.1:{port}/control")

    # ---- handshake + framing ----
    r = ws.request("GetVersion")
    check("GetVersion replies", bool(r) and "result" in r)
    caps = r["result"]["capabilities"] if r and "result" in r else {}
    check("GetVersion.apiVersion == 1", bool(r) and r.get("result", {}).get("apiVersion") == 1)
    check(
        "GetVersion capabilities complete",
        all(k in caps for k in ("sourceTypes", "filterTypes", "events", "audioOutput")),
    )
    check("capabilities.audioOutput is true", caps.get("audioOutput") is True)
    check("audio events advertised",
          {"audioChanged", "audioLevels"} <= set(caps.get("events", [])))

    # ---- source-type discovery (contract 1.4.0: ListSourceTypes) ----
    r = ws.request("ListSourceTypes")
    types = r["result"]["sourceTypes"] if r and "result" in r else None
    check("ListSourceTypes shape",
          isinstance(types, list) and len(types) >= 1 and
          all(set(t) >= {"type", "label", "hasVideo", "hasAudio", "hasMedia"} for t in types),
          f"got {types if not isinstance(types, list) else [sorted(t) for t in types[:2]]}")
    types = types if isinstance(types, list) else []
    check("ListSourceTypes field types",
          bool(types) and all(
              isinstance(t.get("type"), str) and
              isinstance(t.get("label"), str) and t.get("label") and
              isinstance(t.get("hasVideo"), bool) and
              isinstance(t.get("hasAudio"), bool) and
              isinstance(t.get("hasMedia"), bool) for t in types))
    # Both advertisement surfaces derive from the same vocabulary/registration intersection,
    # so the reply must match capabilities.sourceTypes exactly, order included.
    check("ListSourceTypes matches capabilities.sourceTypes",
          [t.get("type") for t in types] == caps.get("sourceTypes"),
          f"types={[t.get('type') for t in types]} caps={caps.get('sourceTypes')}")
    check("color type flags (video-only generator)",
          any(t.get("type") == "color" and t.get("hasVideo") and
              not t.get("hasAudio") and not t.get("hasMedia") for t in types))
    check("media type flags (video+audio+transport)",
          any(t.get("type") == "media" and t.get("hasVideo") and
              t.get("hasAudio") and t.get("hasMedia") for t in types))
    check("audio_input type flags (audio-only, no rate relevance)",
          any(t.get("type") == "audio_input" and not t.get("hasVideo") and
              t.get("hasAudio") for t in types))
    # An advertised type is creatable: round-trip one cheap generator type picked from the
    # reply itself. (Exclusive device types cannot be opened in this harness; their
    # creatability is covered by the shared registered-set intersection asserted above.)
    advertised_color = next((t for t in types if t.get("type") == "color"), None)
    r = ws.request("CreateSource", {"type": advertised_color["type"],
                                    "displayName": "TypeProbe"}) if advertised_color else None
    probe_sid = r.get("result", {}).get("sourceId") if r and "result" in r else None
    check("advertised type round-trips CreateSource", bool(probe_sid))
    if probe_sid:
        ws.request("RemoveSource", {"sourceId": probe_sid})

    r = ws.request("GetStatus")
    required = ("state", "instanceId", "port", "version", "fpsTier", "spoutPrefix",
                "audioOutputDevice", "fps",
                "avgFrameMs", "totalFrames", "laggedFrames", "sources")
    check("GetStatus shape", bool(r) and "result" in r and all(k in r["result"] for k in required),
          f"got {sorted(r.get('result', {}).keys()) if r else None}")

    r = ws.request("ListSources")
    check("ListSources shape", bool(r) and isinstance(r.get("result", {}).get("sources"), list))
    baseline_sources = len(r["result"]["sources"]) if r and "result" in r else 0

    # ---- protocol errors ----
    ws.send_text("{this is not json")
    r = ws.take(lambda m: "error" in m, 3.0)
    check("-32700 parse error (id null)", is_error(r, -32700) and r.get("id") is None)

    ws.send_text(json.dumps({"id": 9001}))
    r = ws.take(lambda m: m.get("id") == 9001, 3.0)
    check("-32600 missing method", is_error(r, -32600))

    r = ws.request("NoSuchMethod")
    check("-32601 unknown method", is_error(r, -32601))

    r = ws.request("Subscribe", {"events": []})
    check("-32602 empty Subscribe", is_error(r, -32602) and
          r["error"]["message"] == "Invalid params: 'events' must be a non-empty array")

    # ---- subscriptions + event gating ----
    wanted = ["status", "sourceAdded", "sourceRemoved", "senderNameResolved",
              "broadcastChanged"]
    r = ws.request("Subscribe", {"events": wanted})
    acked = r["result"]["subscribed"] if r and "result" in r else []
    check("Subscribe ack subset", bool(r) and "result" in r and set(acked) <= set(wanted))

    evt = ws.await_event("status", 2.5)
    check("status push while subscribed", bool(evt) and "fps" in evt["data"] and "ts" in evt["data"])

    # ---- audio routing ----
    r = ws.request("ListAudioDevices")
    devices = r["result"]["devices"] if r and "result" in r else None
    check("ListAudioDevices shape", isinstance(devices, list) and
          all(set(d) >= {"id", "name", "flow", "isDefault"} for d in devices))

    r = ws.request("ListAudioDevices", {"flow": "render"})
    rdevs = r["result"]["devices"] if r and "result" in r else []
    check("ListAudioDevices(render) has a device", len(rdevs) >= 1)
    check("exactly one default render endpoint",
          sum(1 for d in rdevs if d.get("isDefault")) == 1,
          f"defaults={[d['id'] for d in rdevs if d.get('isDefault')]}")

    r = ws.request("GetStatus")
    prior_device = r["result"]["audioOutputDevice"] if r and "result" in r else "default"
    check("GetStatus.audioOutputDevice is a string", isinstance(prior_device, str) and prior_device)

    # Select a concrete endpoint (the default one), read the selection back, then restore.
    concrete = next((d["id"] for d in rdevs if d.get("isDefault")), None)
    r = ws.request("SetAudioOutputDevice", {"deviceId": concrete})
    check("SetAudioOutputDevice stored echo",
          bool(concrete) and bool(r) and r.get("result", {}).get("deviceId") == concrete)
    r = ws.request("GetStatus")
    check("device selection reads back",
          bool(r) and r.get("result", {}).get("audioOutputDevice") == concrete)

    r = ws.request("SetAudioOutputDevice", {"deviceId": "not-an-endpoint-id"})
    check("unknown device -> 1006 naming deviceId", is_error(r, 1006) and
          r["error"].get("data", {}).get("property") == "deviceId")

    r = ws.request("SetAudioOutputDevice", {"deviceId": prior_device})
    check("device selection restored",
          bool(r) and r.get("result", {}).get("deviceId") == prior_device)

    # ---- source lifecycle ----
    r = ws.request("CreateSource", {
        "type": "color",
        "displayName": "ConformanceColor",
        "settings": {"color": 4280303808, "width": 320, "height": 180},
    })
    sid = r["result"]["sourceId"] if r and "result" in r else None
    check("CreateSource replies sourceId", bool(sid))
    check("CreateSource senderName is null", bool(r) and "result" in r and r["result"]["senderName"] is None)
    evt = ws.await_event("sourceAdded", 2.5)
    check("sourceAdded event", bool(evt) and evt["data"]["source"]["sourceId"] == sid)

    # externalId (1.5.2): a client-supplied stable id echoes back verbatim in the CreateSource
    # reply and in ListSources; a source created without it echoes null (legacy/absent).
    r = ws.request("CreateSource", {
        "type": "color",
        "displayName": "ConformanceExtId",
        "externalId": "conformance-ext-7b3f",
        "settings": {"color": 4278255360, "width": 64, "height": 64},
    })
    ext_sid = r["result"]["sourceId"] if r and "result" in r else None
    check("CreateSource echoes externalId in reply",
          bool(r) and "result" in r and r["result"].get("externalId") == "conformance-ext-7b3f")
    r = ws.request("ListSources")
    ext_entry = next((s for s in r["result"]["sources"] if s.get("sourceId") == ext_sid), {}) \
        if r and "result" in r else {}
    base_entry = next((s for s in r["result"]["sources"] if s.get("sourceId") == sid), {}) \
        if r and "result" in r else {}
    check("ListSources echoes externalId", ext_entry.get("externalId") == "conformance-ext-7b3f")
    check("ListSources externalId null when absent", base_entry.get("externalId", "MISSING") is None)
    if ext_sid:
        ws.request("RemoveSource", {"sourceId": ext_sid})

    r = ws.request("CreateSource", {"type": "definitely_not_a_type"})
    check("CreateSource bad type -> 1003", is_error(r, 1003))

    r = ws.request("ListSourceProperties", {"sourceId": sid})
    props = r["result"]["properties"] if r and "result" in r else []
    settings = r["result"]["settings"] if r and "result" in r else {}
    check("ListSourceProperties shape",
          isinstance(props, list) and isinstance(settings, dict) and
          all({"name", "label", "type", "visible", "enabled"} <= set(p) for p in props))
    check("color settings discoverable", "color" in settings, f"settings keys: {sorted(settings)}")

    r = ws.request("SetSourceProperties", {"sourceId": sid, "settings": {"color": 4278255360}})
    check("SetSourceProperties applied echo",
          bool(r) and r.get("result", {}).get("applied", {}).get("color") == 4278255360)

    r = ws.request("SetSourceProperties", {"sourceId": sid, "settings": {"no_such_key": 1}})
    check("unknown settings key -> 1006", is_error(r, 1006) and
          r["error"].get("data", {}).get("property") == "no_such_key")

    # ---- filters ----
    r = ws.request("ListAvailableFilters", {"sourceId": sid})
    filters = r["result"]["filters"] if r and "result" in r else []
    check("ListAvailableFilters (video source -> video kinds)",
          bool(filters) and all(f["kind"] == "video" for f in filters))

    r = ws.request("AddFilter", {"sourceId": sid, "filterType": "color_correction", "name": "Grade"})
    fid = r["result"]["filterId"] if r and "result" in r else None
    check("AddFilter replies filterId", bool(fid))

    r = ws.request("AddFilter", {"sourceId": sid, "filterType": "gain"})
    check("audio filter on video source -> 1004", is_error(r, 1004))

    r = ws.request("ListFilterProperties", {"sourceId": sid, "filterId": fid})
    fprops = r["result"]["properties"] if r and "result" in r else []
    check("ListFilterProperties shape", isinstance(fprops, list) and len(fprops) > 0)

    r = ws.request("SetFilterProperties", {"sourceId": sid, "filterId": fid, "settings": {"gamma": 0.1}})
    check("SetFilterProperties applied echo",
          bool(r) and r.get("result", {}).get("applied", {}).get("gamma") == 0.1)

    r = ws.request("RemoveFilter", {"sourceId": sid, "filterId": fid})
    check("RemoveFilter", bool(r) and r.get("result", {}).get("removed") is True)

    r = ws.request("RemoveFilter", {"sourceId": sid, "filterId": "flt_999"})
    check("unknown filter -> 1002", is_error(r, 1002))

    # ---- broadcast ----
    r = ws.request("StartBroadcast", {"sourceIds": [sid]})
    check("StartBroadcast started", bool(r) and r.get("result", {}).get("started") == [sid])
    evt = ws.await_event("broadcastChanged", 2.5)
    check("broadcastChanged on start", bool(evt) and evt["data"]["broadcasting"] is True)
    evt = ws.await_event("senderNameResolved", 6.0)
    sender = evt["data"]["senderName"] if evt else None
    check("senderNameResolved", bool(evt) and evt["data"]["sourceId"] == sid and bool(sender))

    time.sleep(0.3)
    r = ws.request("GetStatus")
    mine = [s for s in r["result"]["sources"] if s["sourceId"] == sid] if r and "result" in r else []
    check("GetStatus shows the slot sending",
          bool(mine) and mine[0]["broadcasting"] is True and mine[0]["sends"] > 0 and
          mine[0]["senderName"] == sender)

    r = ws.request("StopBroadcast", {"sourceIds": [sid]})
    check("StopBroadcast stopped", bool(r) and r.get("result", {}).get("stopped") == [sid])
    evt = ws.await_event("broadcastChanged", 2.5)
    check("broadcastChanged on stop", bool(evt) and evt["data"]["broadcasting"] is False)

    # ---- removal + 1001 ----
    r = ws.request("RemoveSource", {"sourceId": sid})
    check("RemoveSource", bool(r) and r.get("result", {}).get("removed") is True)
    evt = ws.await_event("sourceRemoved", 2.5)
    check("sourceRemoved event", bool(evt) and evt["data"]["sourceId"] == sid)

    r = ws.request("RemoveSource", {"sourceId": sid})
    check("removed source -> 1001", is_error(r, 1001))

    r = ws.request("ListSources")
    check("source count restored", bool(r) and len(r["result"]["sources"]) == baseline_sources)

    # ---- media + text source types ----
    r = ws.request("GetVersion")
    advertised = set(r["result"]["capabilities"]["sourceTypes"]) if r and "result" in r else set()
    expected_types = {"camera", "display", "window", "game", "media", "image", "color", "text",
                      "audio_input", "audio_output"}
    check("sourceTypes closed set", advertised == expected_types,
          f"got {sorted(advertised)}")

    # ---- audio capture source types (closed-set members; audio-only slot semantics) ----
    # Creation succeeds regardless of physical devices (the capture source retries device
    # acquisition internally); the property surface must stay within the contract's nine
    # descriptor types and expose the device list with its default-device entry.
    r = ws.request("CreateSource", {"type": "audio_input", "displayName": "ConformanceMic"})
    aid_in = r["result"]["sourceId"] if r and "result" in r else None
    check("CreateSource audio_input succeeds", bool(aid_in) and r["result"]["senderName"] is None)
    if aid_in:
        r = ws.request("ListSourceProperties", {"sourceId": aid_in})
        props = r["result"]["properties"] if r and "result" in r else []
        prop_types = {p["type"] for p in props}
        legal_types = {"bool", "int", "float", "text", "path", "list", "color", "font", "button"}
        check("audio_input descriptor types legal", prop_types <= legal_types,
              f"got {sorted(prop_types)}")
        dev = next((p for p in props if p["name"] == "device_id"), None)
        check("audio_input device_id list descriptor",
              bool(dev) and dev["type"] == "list" and dev.get("listFormat") == "string")
        check("audio_input device_id has default entry",
              bool(dev) and any(i.get("value") == "default" for i in dev.get("items", [])))
        timing = next((p for p in props if p["name"] == "use_device_timing"), None)
        check("audio_input use_device_timing bool", bool(timing) and timing["type"] == "bool")
        r = ws.request("RemoveSource", {"sourceId": aid_in})
        check("audio_input removed", bool(r) and r.get("result", {}).get("removed") is True)

    # An attached audio_output (loopback) source is the canonical audio-only slot: it
    # broadcasts with senderName null forever (no video to publish), zero dims, zero sends.
    r = ws.request("CreateSource", {"type": "audio_output", "displayName": "ConformanceLoopback"})
    aid_out = r["result"]["sourceId"] if r and "result" in r else None
    check("CreateSource audio_output succeeds", bool(aid_out))
    if aid_out:
        r = ws.request("StartBroadcast", {"sourceIds": [aid_out]})
        check("audio_output StartBroadcast", bool(r) and r.get("result", {}).get("started") == [aid_out])
        time.sleep(2.0)  # long enough for a sender name to resolve if one were (wrongly) allocated
        r = ws.request("GetStatus")
        row = next((s for s in r["result"]["sources"] if s["sourceId"] == aid_out), None) \
            if r and "result" in r else None
        check("audio_output broadcasting senderName null",
              bool(row) and row["broadcasting"] is True and row["senderName"] is None)
        check("audio_output zero dims zero sends",
              bool(row) and row["width"] == 0 and row["height"] == 0 and row["sends"] == 0)
        r = ws.request("RemoveSource", {"sourceId": aid_out})
        check("audio_output removed", bool(r) and r.get("result", {}).get("removed") is True)

    clip = write_test_clip(os.path.join(tempfile.gettempdir(), "moxrelay-conformance-clip.avi"))
    # Deliberately DEFAULT playback settings: media playback gates on the source being active,
    # and the engine marks a source active exactly while it is attached to its sender -- so
    # StartBroadcast alone must start playback for the dims check below to pass.
    r = ws.request("CreateSource", {
        "type": "media",
        "displayName": "ConformanceMedia",
        "settings": {"local_file": clip, "is_local_file": True, "looping": True},
    })
    mid = r["result"]["sourceId"] if r and "result" in r else None
    check("CreateSource media", bool(mid) and r["result"]["format"] == "srgb87")
    ws.await_event("sourceAdded", 2.5)
    r = ws.request("StartBroadcast", {"sourceIds": [mid]})
    check("media StartBroadcast", bool(r) and r.get("result", {}).get("started") == [mid])
    evt = ws.await_event("senderNameResolved", 10.0)
    check("media senderNameResolved", bool(evt) and evt["data"]["sourceId"] == mid and
          bool(evt["data"]["senderName"]))
    time.sleep(0.3)
    r = ws.request("GetStatus")
    mine = [s for s in r["result"]["sources"] if s["sourceId"] == mid] if r and "result" in r else []
    check("media dims non-zero", bool(mine) and mine[0]["width"] > 0 and mine[0]["height"] > 0,
          f"got {mine[0]['width']}x{mine[0]['height']}" if mine else "source missing")
    ws.request("StopBroadcast", {"sourceIds": [mid]})
    ws.await_event("broadcastChanged", 2.5)
    r = ws.request("RemoveSource", {"sourceId": mid})
    check("media RemoveSource", bool(r) and r.get("result", {}).get("removed") is True)
    ws.await_event("sourceRemoved", 2.5)
    try:
        os.remove(clip)  # safe now: the source holding the file open is gone
    except OSError:
        pass

    r = ws.request("CreateSource", {
        "type": "text",
        "displayName": "ConformanceText",
        "settings": {"text": "Conformance"},
    })
    tid = r["result"]["sourceId"] if r and "result" in r else None
    check("CreateSource text", bool(tid))
    ws.await_event("sourceAdded", 2.5)
    r = ws.request("ListSourceProperties", {"sourceId": tid})
    tprops = r["result"]["properties"] if r and "result" in r else []
    tsettings = r["result"]["settings"] if r and "result" in r else {}
    check("text font + color descriptors",
          any(p["type"] == "font" for p in tprops) and any(p["type"] == "color" for p in tprops))
    check("text font settings object",
          isinstance(tsettings.get("font"), dict) and "face" in tsettings["font"] and
          "size" in tsettings["font"])
    font_value = {"face": "Arial", "style": "Regular", "size": 64, "flags": 0}
    r = ws.request("SetSourceProperties", {"sourceId": tid, "settings": {"font": font_value}})
    applied_font = r.get("result", {}).get("applied", {}).get("font", {}) if r else {}
    check("text FontValue round-trip",
          applied_font.get("face") == "Arial" and applied_font.get("size") == 64)
    r = ws.request("RemoveSource", {"sourceId": tid})
    check("text RemoveSource", bool(r) and r.get("result", {}).get("removed") is True)
    ws.await_event("sourceRemoved", 2.5)

    # ---- media transport (contract 1.1.0: GetMediaStatus/ControlMedia/SeekMedia + mediaChanged) ----
    r = ws.request("GetMediaStatus", {"sourceId": "src_99999"})
    check("GetMediaStatus unknown source -> 1001", is_error(r, 1001))

    r = ws.request("CreateSource", {"type": "color", "displayName": "TransportColor",
                                    "settings": {"width": 64, "height": 36}})
    cid = r["result"]["sourceId"] if r and "result" in r else None
    ws.await_event("sourceAdded", 2.5)
    r = ws.request("GetMediaStatus", {"sourceId": cid})
    check("GetMediaStatus non-media -> 1012", is_error(r, 1012) and
          r["error"].get("data", {}).get("sourceId") == cid)
    r = ws.request("ControlMedia", {"sourceId": cid, "action": "play"})
    check("ControlMedia non-media -> 1012", is_error(r, 1012))
    r = ws.request("SeekMedia", {"sourceId": cid, "positionMs": 0})
    check("SeekMedia non-media -> 1012", is_error(r, 1012))
    ws.request("RemoveSource", {"sourceId": cid})
    ws.await_event("sourceRemoved", 2.5)

    # A second connection observes mediaChanged: transitions driven by the primary connection
    # must be visible to every subscriber (and the primary stays unsubscribed -- its inbox is
    # request/reply + the structural events only).
    ws2 = WsClient("127.0.0.1", port)
    r = ws2.request("Subscribe", {"events": ["mediaChanged"]})
    check("mediaChanged subscribable (second connection)",
          bool(r) and r.get("result", {}).get("subscribed") == ["mediaChanged"])

    def media_event(state, timeout):
        return ws2.take(lambda m: "id" not in m and m.get("event") == "mediaChanged" and
                        m.get("data", {}).get("state") == state, timeout)

    clip2 = write_test_clip(os.path.join(tempfile.gettempdir(), "moxrelay-transport-clip.avi"),
                            frames=150)  # 150 frames @30fps = a 5 s clip
    r = ws.request("CreateSource", {
        "type": "media",
        "displayName": "TransportMedia",
        "settings": {"local_file": clip2, "is_local_file": True, "looping": False},
    })
    tmid = r["result"]["sourceId"] if r and "result" in r else None
    ws.await_event("sourceAdded", 2.5)
    ws.request("StartBroadcast", {"sourceIds": [tmid]})
    ws.await_event("senderNameResolved", 10.0)

    status = wait_media_state(ws, tmid, "playing", 5.0)
    check("media playing after attach", status.get("state") == "playing", f"got {status}")
    dur = status.get("durationMs")
    check("media duration sane + looping false",
          isinstance(dur, int) and 3000 < dur <= 6000 and status.get("looping") is False,
          f"durationMs={dur} looping={status.get('looping')}")

    # Drain the start-of-playback transitions (opening/playing from the attach) so every state
    # matched below can only come from a NEW transition.
    ws2.pump(0.5)
    ws2.inbox.clear()

    r = ws.request("ControlMedia", {"sourceId": tmid, "action": "pause"})
    evt = media_event("paused", 3.0)
    check("ControlMedia pause -> paused + event",
          bool(r) and r.get("result", {}).get("state") == "paused" and bool(evt) and
          evt["data"]["sourceId"] == tmid)
    p1 = media_status(ws, tmid).get("positionMs", -1)
    time.sleep(0.7)
    p2 = media_status(ws, tmid).get("positionMs", -2)
    check("pause freezes position", p1 >= 0 and abs(p2 - p1) <= 150, f"{p1} -> {p2}")

    r = ws.request("ControlMedia", {"sourceId": tmid, "action": "play"})
    evt = media_event("playing", 3.0)
    check("ControlMedia play -> playing + event",
          bool(r) and r.get("result", {}).get("state") == "playing" and bool(evt))

    r = ws.request("SeekMedia", {"sourceId": tmid, "positionMs": 1000})
    check("SeekMedia effective echo", bool(r) and r.get("result", {}).get("positionMs") == 1000)
    time.sleep(0.6)
    pos = media_status(ws, tmid).get("positionMs", -1)
    check("position advances from the seek target", 900 <= pos < 3000, f"positionMs={pos}")

    ws.request("ControlMedia", {"sourceId": tmid, "action": "restart"})
    time.sleep(0.5)
    pos = media_status(ws, tmid).get("positionMs", -1)
    check("ControlMedia restart resets position", 0 <= pos < 1500, f"positionMs={pos}")

    r = ws.request("ControlMedia", {"sourceId": tmid, "action": "stop"})
    evt = media_event("stopped", 3.0)
    check("ControlMedia stop -> stopped + event",
          bool(r) and r.get("result", {}).get("state") == "stopped" and bool(evt))

    # Natural end: play (from stopped -- the server's restart path), seek to the clamp limit
    # (also the clamp assertion), and the EOF must push mediaChanged{ended}.
    r = ws.request("ControlMedia", {"sourceId": tmid, "action": "play"})
    check("play on stopped resumes", bool(r) and r.get("result", {}).get("state") == "playing")
    r = ws.request("SeekMedia", {"sourceId": tmid, "positionMs": 10_000_000})
    clamped = r.get("result", {}).get("positionMs") if r and "result" in r else None
    check("SeekMedia clamps to duration", isinstance(clamped, int) and clamped == dur,
          f"clamped={clamped} durationMs={dur}")
    evt = media_event("ended", 8.0)
    check("natural end -> mediaChanged ended", bool(evt) and evt["data"]["sourceId"] == tmid)
    check("GetMediaStatus shows ended", wait_media_state(ws, tmid, "ended", 2.0).get("state") == "ended")

    # Drain ws2 (the play-on-stopped above emitted its own 'playing') so the next check can
    # only be satisfied by a NEW event.
    ws2.pump(0.3)
    ws2.inbox.clear()
    r = ws.request("ControlMedia", {"sourceId": tmid, "action": "play"})
    evt = media_event("playing", 4.0)
    check("play on ended resumes (restart path)",
          bool(r) and r.get("result", {}).get("state") == "playing" and bool(evt))

    r = ws.request("SetSourceProperties", {"sourceId": tmid, "settings": {"looping": True}})
    check("looping setting round-trips mid-playback",
          bool(r) and r.get("result", {}).get("applied", {}).get("looping") is True and
          media_status(ws, tmid).get("looping") is True)

    # Loop-wrap silence: park the playhead near the end, let the clip wrap, keep playing well
    # past the natural duration -- the state must stay 'playing' and NO mediaChanged may arrive
    # (a looping wrap is not a transition; contract mediaChanged prose).
    ws.request("SeekMedia", {"sourceId": tmid, "positionMs": (dur or 5000) - 1000})
    ws2.pump(0.5)
    ws2.inbox.clear()
    ws2.pump(3.5)  # wraps ~1 s in; observe 2.5 s of post-wrap playback
    wrap_events = [m for m in ws2.inbox if m.get("event") == "mediaChanged"]
    status = media_status(ws, tmid)
    check("looping wrap emits no mediaChanged + keeps playing",
          not wrap_events and status.get("state") == "playing",
          f"events={wrap_events} state={status.get('state')}")

    ws.request("StopBroadcast", {"sourceIds": [tmid]})
    ws.await_event("broadcastChanged", 2.5)
    r = ws.request("RemoveSource", {"sourceId": tmid})
    check("media transport RemoveSource", bool(r) and r.get("result", {}).get("removed") is True)
    ws.await_event("sourceRemoved", 2.5)
    ws2.close()
    try:
        os.remove(clip2)
    except OSError:
        pass

    # ---- propertyChanged (contract 1.1.0: every successful settings apply announces itself) ----
    r = ws.request("Subscribe", {"events": ["propertyChanged"]})
    check("propertyChanged subscribable", bool(r) and
          r.get("result", {}).get("subscribed") == ["propertyChanged"])
    ws3 = WsClient("127.0.0.1", port)
    r = ws3.request("Subscribe", {"events": ["propertyChanged"]})
    check("propertyChanged subscribable (second connection)",
          bool(r) and r.get("result", {}).get("subscribed") == ["propertyChanged"])

    r = ws.request("CreateSource", {"type": "color", "displayName": "PropsColor",
                                    "settings": {"width": 64, "height": 36}})
    pid = r["result"]["sourceId"] if r and "result" in r else None
    ws.await_event("sourceAdded", 2.5)

    r = ws.request("SetSourceProperties", {"sourceId": pid, "settings": {"color": 4286611456}})
    evt = ws.await_event("propertyChanged", 3.0)
    check("propertyChanged on the issuing connection",
          bool(r) and "result" in r and bool(evt) and evt["data"]["sourceId"] == pid and
          evt["data"]["applied"].get("color") == 4286611456 and "filterId" not in evt["data"])
    evt = ws3.await_event("propertyChanged", 3.0)
    check("propertyChanged on a second connection",
          bool(evt) and evt["data"]["sourceId"] == pid and
          evt["data"]["applied"].get("color") == 4286611456)

    r = ws.request("AddFilter", {"sourceId": pid, "filterType": "color_correction"})
    pfid = r["result"]["filterId"] if r and "result" in r else None
    r = ws.request("SetFilterProperties", {"sourceId": pid, "filterId": pfid,
                                           "settings": {"gamma": 0.25}})
    evt = ws.await_event("propertyChanged", 3.0)
    check("filter-scoped propertyChanged carries filterId",
          bool(r) and "result" in r and bool(evt) and evt["data"]["sourceId"] == pid and
          evt["data"].get("filterId") == pfid and "gamma" in evt["data"]["applied"])

    # A REJECTED apply must announce nothing (drain first so only a new event could match).
    ws.pump(0.3)
    ws.inbox = [m for m in ws.inbox if m.get("event") != "propertyChanged"]
    r = ws.request("SetSourceProperties", {"sourceId": pid, "settings": {"no_such_key": 1}})
    evt = ws.await_event("propertyChanged", 1.5)
    check("rejected apply emits no propertyChanged", is_error(r, 1006) and evt is None)

    # ---- filter chain (contract 1.1.0: ListFilters / SetFilterEnabled / ReorderFilter /
    # ---- SetFilterName + filterAdded/filterRemoved/filterChanged) ----
    r = ws.request("Subscribe", {"events": ["filterAdded", "filterRemoved", "filterChanged"]})
    check("filter events subscribable", bool(r) and
          r.get("result", {}).get("subscribed") == ["filterAdded", "filterRemoved", "filterChanged"])
    r = ws3.request("Subscribe", {"events": ["filterChanged"]})
    check("filterChanged subscribable (second connection)",
          bool(r) and r.get("result", {}).get("subscribed") == ["filterChanged"])

    # pid already carries pfid (color_correction); add a second video filter behind it.
    r = ws.request("AddFilter", {"sourceId": pid, "filterType": "sharpness", "name": "Crisp"})
    sfid = r["result"]["filterId"] if r and "result" in r else None
    evt = ws.await_event("filterAdded", 3.0)
    check("filterAdded carries the full FilterInfo",
          bool(sfid) and bool(evt) and evt["data"]["sourceId"] == pid and
          evt["data"]["filter"]["filterId"] == sfid and evt["data"]["filter"]["kind"] == "video" and
          evt["data"]["filter"]["name"] == "Crisp" and evt["data"]["filter"]["enabled"] is True and
          evt["data"]["filter"]["index"] == 1)

    r = ws.request("ListFilters", {"sourceId": pid})
    chain = r["result"]["filters"] if r and "result" in r else []
    check("ListFilters order matches add order",
          [f["filterId"] for f in chain] == [pfid, sfid] and
          [f["index"] for f in chain] == [0, 1])

    # Enable/disable: the transition emits filterChanged{enabled} (both connections see it);
    # re-setting the current state is an idempotent no-op and emits nothing.
    r = ws.request("SetFilterEnabled", {"sourceId": pid, "filterId": sfid, "enabled": False})
    evt = ws.await_event("filterChanged", 3.0)
    check("SetFilterEnabled transition + filterChanged{enabled}",
          bool(r) and r.get("result", {}).get("enabled") is False and bool(evt) and
          evt["data"]["filterId"] == sfid and evt["data"].get("enabled") is False and
          "name" not in evt["data"] and "index" not in evt["data"])
    evt = ws3.await_event("filterChanged", 3.0)
    check("filterChanged on a second connection",
          bool(evt) and evt["data"]["filterId"] == sfid and evt["data"].get("enabled") is False)
    # Gating, negative direction: ws3 never subscribed to filterAdded, and an AddFilter
    # happened after its Subscribe ack -- nothing may have been delivered to it.
    ws3.pump(0.3)
    check("unsubscribed connection receives no filterAdded",
          all(m.get("event") != "filterAdded" for m in ws3.inbox))
    ws.pump(0.3)
    ws.inbox = [m for m in ws.inbox if m.get("event") != "filterChanged"]
    r = ws.request("SetFilterEnabled", {"sourceId": pid, "filterId": sfid, "enabled": False})
    evt = ws.await_event("filterChanged", 1.5)
    check("SetFilterEnabled no-op emits nothing",
          bool(r) and r.get("result", {}).get("enabled") is False and evt is None)

    # Reorder: effective index, exactly ONE filterChanged (the moved filter), order visible
    # in ListFilters; out-of-range clamps to the valid range.
    r = ws.request("ReorderFilter", {"sourceId": pid, "filterId": sfid, "index": 0})
    evt = ws.await_event("filterChanged", 3.0)
    check("ReorderFilter effective index + filterChanged{index}",
          bool(r) and r.get("result", {}).get("index") == 0 and bool(evt) and
          evt["data"]["filterId"] == sfid and evt["data"].get("index") == 0)
    follow = ws.await_event("filterChanged", 0.8)
    check("reorder fires ONE filterChanged (moved filter only)", follow is None)
    r = ws.request("ListFilters", {"sourceId": pid})
    chain = r["result"]["filters"] if r and "result" in r else []
    check("ListFilters reflects the new order", [f["filterId"] for f in chain] == [sfid, pfid])
    r = ws.request("ReorderFilter", {"sourceId": pid, "filterId": sfid, "index": 500})
    ws.await_event("filterChanged", 3.0)  # drain the clamp move's event
    check("ReorderFilter clamps out-of-range", bool(r) and r.get("result", {}).get("index") == 1)

    # Rename: stored-label echo + filterChanged{name}; the no-op rename emits nothing.
    r = ws.request("SetFilterName", {"sourceId": pid, "filterId": sfid, "name": "Crisper"})
    evt = ws.await_event("filterChanged", 3.0)
    check("SetFilterName round-trip + filterChanged{name}",
          bool(r) and r.get("result", {}).get("name") == "Crisper" and bool(evt) and
          evt["data"]["filterId"] == sfid and evt["data"].get("name") == "Crisper")
    ws.pump(0.3)
    ws.inbox = [m for m in ws.inbox if m.get("event") != "filterChanged"]
    r = ws.request("SetFilterName", {"sourceId": pid, "filterId": sfid, "name": "Crisper"})
    evt = ws.await_event("filterChanged", 1.5)
    check("SetFilterName no-op emits nothing",
          bool(r) and r.get("result", {}).get("name") == "Crisper" and evt is None)

    # Negatives: error precedence per the contract (-32602 params, 1001 source, 1002 filter).
    r = ws.request("ListFilters", {"sourceId": "src_99999"})
    check("ListFilters unknown source -> 1001", is_error(r, 1001))
    r = ws.request("SetFilterEnabled", {"sourceId": pid, "filterId": "flt_99999", "enabled": True})
    check("SetFilterEnabled unknown filter -> 1002", is_error(r, 1002))
    r = ws.request("ReorderFilter", {"sourceId": pid, "filterId": sfid})
    check("ReorderFilter missing index -> -32602", is_error(r, -32602))

    # Removal: filterRemoved fires and the chain closes up (indexes re-pack).
    r = ws.request("RemoveFilter", {"sourceId": pid, "filterId": sfid})
    evt = ws.await_event("filterRemoved", 3.0)
    check("filterRemoved on RemoveFilter",
          bool(r) and bool(evt) and evt["data"]["sourceId"] == pid and
          evt["data"]["filterId"] == sfid)
    r = ws.request("ListFilters", {"sourceId": pid})
    chain = r["result"]["filters"] if r and "result" in r else []
    check("chain closes up after removal",
          [f["filterId"] for f in chain] == [pfid] and chain[0]["index"] == 0)

    # ---- InvokeSourceButton (contract 1.1.0) ----
    r = ws.request("InvokeSourceButton", {"sourceId": pid, "property": "width"})
    check("InvokeSourceButton non-button -> 1006", is_error(r, 1006) and
          r["error"].get("data", {}).get("property") == "width")
    r = ws.request("InvokeSourceButton", {"sourceId": pid, "property": "no_such_button"})
    check("InvokeSourceButton unknown property -> 1006", is_error(r, 1006))
    r = ws.request("InvokeSourceButton", {"sourceId": "src_99999", "property": "x"})
    check("InvokeSourceButton unknown source -> 1001", is_error(r, 1001))

    ws.request("RemoveSource", {"sourceId": pid})
    ws.await_event("sourceRemoved", 2.5)
    ws3.close()

    # Positive button invoke: the only v1 buttons live on the camera source (activate /
    # video_config / xbar_config). Conditional on the type being creatable here, and ONLY the
    # 'activate' toggle is invoked -- the config buttons open native driver dialogs, which a
    # headless conformance run must never trigger.
    r = ws.request("CreateSource", {"type": "camera", "displayName": "ButtonProbe"})
    if r and "result" in r:
        bid = r["result"]["sourceId"]
        ws.await_event("sourceAdded", 2.5)
        r = ws.request("ListSourceProperties", {"sourceId": bid})
        bprops = r["result"]["properties"] if r and "result" in r else []
        has_activate = any(p["name"] == "activate" and p["type"] == "button" for p in bprops)
        if has_activate:
            r = ws.request("InvokeSourceButton", {"sourceId": bid, "property": "activate"})
            check("InvokeSourceButton button -> invoked",
                  bool(r) and r.get("result", {}).get("invoked") is True)
        else:
            print("SKIP  InvokeSourceButton positive (camera exposes no 'activate' button here)")
        ws.request("RemoveSource", {"sourceId": bid})
        ws.await_event("sourceRemoved", 2.5)
    else:
        print("SKIP  InvokeSourceButton positive (camera source not creatable on this instance)")

    # ---- per-source format (contract 1.1.0: SourceFormat + SetSourceFormat) ----
    r = ws.request("CreateSource", {"type": "color", "displayName": "FmtProbe",
                                    "settings": {"color": 4280303808, "width": 64, "height": 64},
                                    "format": "fp16", "startBroadcast": True})
    fmt_sid = r["result"]["sourceId"] if r and "result" in r else ""
    check("CreateSource format param honored",
          bool(fmt_sid) and r["result"]["format"] == "fp16")
    ws.await_event("sourceAdded", 2.5)
    r = ws.request("ListSources")
    entry = next((s for s in r["result"]["sources"] if s["sourceId"] == fmt_sid), {}) \
        if r and "result" in r else {}
    check("ListSources shows the format", entry.get("format") == "fp16")

    # The source was created broadcasting: wait for its sender to resolve, then change the
    # format -- the sender is recreated (format is fixed per attach) and a FRESH
    # senderNameResolved fires (the contract's format-change observability).
    evt = ws.await_event("senderNameResolved", 8.0)
    check("fp16 sender resolves", bool(evt) and evt["data"]["sourceId"] == fmt_sid)
    ws.pump(0.3)
    ws.inbox = [m for m in ws.inbox if m.get("event") != "senderNameResolved"]
    r = ws.request("SetSourceFormat", {"sourceId": fmt_sid, "format": "srgb87"})
    check("SetSourceFormat result echo",
          bool(r) and r.get("result", {}) == {"sourceId": fmt_sid, "format": "srgb87"})
    evt = ws.await_event("senderNameResolved", 8.0)
    check("senderNameResolved re-fires after a broadcasting format change",
          bool(evt) and evt["data"]["sourceId"] == fmt_sid)
    r = ws.request("GetStatus")
    entry = next((s for s in r["result"]["sources"] if s["sourceId"] == fmt_sid), {}) \
        if r and "result" in r else {}
    check("GetStatus shows the new format",
          entry.get("format") == "srgb87" and entry.get("broadcasting") is True)

    # Same-format set is an idempotent no-op: result echo, NO sender churn (no re-resolve).
    ws.pump(0.3)
    ws.inbox = [m for m in ws.inbox if m.get("event") != "senderNameResolved"]
    r = ws.request("SetSourceFormat", {"sourceId": fmt_sid, "format": "srgb87"})
    evt = ws.await_event("senderNameResolved", 1.5)
    check("SetSourceFormat same-format no-op",
          bool(r) and "result" in r and evt is None)

    r = ws.request("SetSourceFormat", {"sourceId": fmt_sid, "format": "bgr0"})
    check("SetSourceFormat unknown value -> 1006", is_error(r, 1006) and
          r["error"].get("data", {}).get("property") == "format")
    r = ws.request("SetSourceFormat", {"sourceId": "src_99999", "format": "fp16"})
    check("SetSourceFormat unknown source -> 1001", is_error(r, 1001))
    r = ws.request("CreateSource", {"type": "color", "displayName": "FmtBad",
                                    "format": "rgba32"})
    check("CreateSource unknown format -> 1006", is_error(r, 1006))

    ws.request("RemoveSource", {"sourceId": fmt_sid})
    ws.await_event("sourceRemoved", 2.5)

    # ---- per-source audio (contract 1.3.0: GetSourceAudio / SetSourceAudio / audioChanged,
    # ----                    incl. the syncOffsetMs audio-delay knob) ----
    ws4 = WsClient("127.0.0.1", port)
    r = ws4.request("Subscribe", {"events": ["audioChanged"]})
    check("audioChanged subscribable (second connection)",
          bool(r) and r.get("result", {}).get("subscribed") == ["audioChanged"])

    r = ws.request("CreateSource", {"type": "color", "displayName": "AudioProbe",
                                    "settings": {"width": 64, "height": 36}})
    aid = r["result"]["sourceId"] if r and "result" in r else None
    ws.await_event("sourceAdded", 2.5)
    r = ws.request("GetSourceAudio", {"sourceId": aid})
    check("GetSourceAudio defaults",
          bool(r) and r.get("result", {}) == {"sourceId": aid, "gain": 1.0,
                                              "muted": False, "balance": 0.5,
                                              "syncOffsetMs": 0})

    r = ws.request("SetSourceAudio", {"sourceId": aid, "gain": 1.5, "balance": 0.25})
    check("SetSourceAudio full-state echo",
          bool(r) and r.get("result", {}) == {"sourceId": aid, "gain": 1.5,
                                              "muted": False, "balance": 0.25,
                                              "syncOffsetMs": 0})
    evt = ws4.await_event("audioChanged", 3.0)
    check("audioChanged changed-fields-only on a second connection",
          bool(evt) and evt["data"]["sourceId"] == aid and evt["data"].get("gain") == 1.5 and
          evt["data"].get("balance") == 0.25 and "muted" not in evt["data"])

    # The ceiling clamp: setting above 20.0 echoes the EFFECTIVE value back.
    r = ws.request("SetSourceAudio", {"sourceId": aid, "gain": 25.0})
    check("gain clamp echo (25 -> 20)",
          bool(r) and r.get("result", {}).get("gain") == 20.0)

    # A no-op set succeeds and emits nothing.
    ws4.pump(0.3)
    ws4.inbox.clear()
    r = ws.request("SetSourceAudio", {"sourceId": aid, "gain": 20.0})
    evt = ws4.await_event("audioChanged", 1.5)
    check("SetSourceAudio no-op emits nothing", bool(r) and "result" in r and evt is None)

    r = ws.request("SetSourceAudio", {"sourceId": aid})
    check("SetSourceAudio with no fields -> -32602", is_error(r, -32602))
    r = ws.request("SetSourceAudio", {"sourceId": "src_99999", "gain": 1.0})
    check("SetSourceAudio unknown source -> 1001", is_error(r, 1001))

    # syncOffsetMs (the per-source audio-delay knob): round-trip, changed-fields-only event,
    # ceiling clamp, no-op gating, validation, and the stored-and-inert rule (the probe source
    # is a color generator -- no audio pipeline, the value is stored state).
    ws4.pump(0.3)
    ws4.inbox.clear()
    r = ws.request("SetSourceAudio", {"sourceId": aid, "syncOffsetMs": 120})
    check("syncOffsetMs round-trip echo",
          bool(r) and r.get("result", {}).get("syncOffsetMs") == 120)
    evt = ws4.await_event("audioChanged", 3.0)
    check("audioChanged carries syncOffsetMs changed-field-only",
          bool(evt) and evt["data"]["sourceId"] == aid and
          evt["data"].get("syncOffsetMs") == 120 and "gain" not in evt["data"] and
          "muted" not in evt["data"] and "balance" not in evt["data"])
    r = ws.request("SetSourceAudio", {"sourceId": aid, "syncOffsetMs": 1200})
    check("syncOffsetMs clamp echo (1200 -> 950)",
          bool(r) and r.get("result", {}).get("syncOffsetMs") == 950)
    ws4.pump(0.3)
    ws4.inbox.clear()
    r = ws.request("SetSourceAudio", {"sourceId": aid, "syncOffsetMs": 950})
    evt = ws4.await_event("audioChanged", 1.5)
    check("syncOffsetMs no-op emits nothing", bool(r) and "result" in r and evt is None)
    r = ws.request("SetSourceAudio", {"sourceId": aid, "syncOffsetMs": -5})
    check("syncOffsetMs negative -> -32602", is_error(r, -32602))
    r = ws.request("SetSourceAudio", {"sourceId": aid, "syncOffsetMs": "big"})
    check("syncOffsetMs ill-typed -> -32602", is_error(r, -32602))
    r = ws.request("GetSourceAudio", {"sourceId": aid})
    check("syncOffsetMs stored and inert on a no-audio source",
          bool(r) and r.get("result", {}).get("syncOffsetMs") == 950)

    ws.request("RemoveSource", {"sourceId": aid})
    ws.await_event("sourceRemoved", 2.5)

    # CreateSource audio seeds (param siblings, like format) read back via GetSourceAudio.
    r = ws.request("CreateSource", {"type": "color", "displayName": "AudioSeeded",
                                    "settings": {"width": 64, "height": 36},
                                    "gain": 0.5, "muted": True, "balance": 0.75,
                                    "syncOffsetMs": 80})
    seeded = r["result"]["sourceId"] if r and "result" in r else None
    ws.await_event("sourceAdded", 2.5)
    r = ws.request("GetSourceAudio", {"sourceId": seeded})
    check("CreateSource audio seeds reflected",
          bool(r) and r.get("result", {}) == {"sourceId": seeded, "gain": 0.5,
                                              "muted": True, "balance": 0.75,
                                              "syncOffsetMs": 80})
    ws.request("RemoveSource", {"sourceId": seeded})
    ws.await_event("sourceRemoved", 2.5)

    # ---- audioLevels (contract 1.2.0: 10 Hz opt-in periodic, emission only while subscribed) ----
    r = ws.request("Subscribe", {"events": ["audioLevels"]})
    check("audioLevels subscribable", bool(r) and
          r.get("result", {}).get("subscribed") == ["audioLevels"])
    evt = ws.await_event("audioLevels", 2.5)
    check("audioLevels push shape",
          bool(evt) and isinstance(evt["data"].get("master"), dict) and
          {"peak", "rms", "clipped"} <= set(evt["data"]["master"]) and
          isinstance(evt["data"]["master"]["peak"], (int, float)) and
          isinstance(evt["data"]["master"]["clipped"], bool) and
          isinstance(evt["data"].get("sources"), list))
    count = 0
    deadline = time.monotonic() + 1.2
    while time.monotonic() < deadline:
        if ws.await_event("audioLevels", 0.3):
            count += 1
    check("audioLevels cadence ~10 Hz", 8 <= count <= 16, f"got {count} pushes in 1.2s")
    ws4.pump(0.5)
    check("unsubscribed connection receives no audioLevels",
          all(m.get("event") != "audioLevels" for m in ws4.inbox))
    r = ws.request("Unsubscribe", {"events": ["audioLevels"]})
    check("audioLevels Unsubscribe ack",
          bool(r) and r.get("result", {}).get("unsubscribed") == ["audioLevels"])
    ws.pump(0.3)
    ws.inbox = [m for m in ws.inbox if m.get("event") != "audioLevels"]
    evt = ws.await_event("audioLevels", 0.8)
    check("no audioLevels after Unsubscribe", evt is None)
    ws4.close()

    # ---- event gating: Unsubscribe stops the pushes ----
    r = ws.request("Unsubscribe", {"events": ["status"]})
    check("Unsubscribe ack", bool(r) and r.get("result", {}).get("unsubscribed") == ["status"])
    ws.inbox = [m for m in ws.inbox if m.get("event") != "status"]  # drop in-flight pushes
    evt = ws.await_event("status", 2.5)
    check("no status push after Unsubscribe", evt is None)

    # ---- reconnect: no session, no replay; re-handshake + re-subscribe restores flow ----
    ws.close()
    ws = WsClient("127.0.0.1", port)
    r = ws.request("GetVersion")
    check("reconnect GetVersion", bool(r) and "result" in r)
    evt = ws.await_event("status", 1.5)
    check("no events before re-subscribe", evt is None)
    r = ws.request("Subscribe", {"events": ["status"]})
    check("re-subscribe ack", bool(r) and r.get("result", {}).get("subscribed") == ["status"])
    evt = ws.await_event("status", 2.5)
    check("status flows after re-subscribe", bool(evt))
    r = ws.request("ListSources")
    check("re-snapshot ListSources", bool(r) and "result" in r)

    # ---- discovery contract: GetFleet removed, publishedSenderNames present ----
    # GetFleet / fleetChanged are gone (single-instance contract). The discovery file is now a
    # BARE single-instance object whose canonical key set is the eight fields below; the
    # GetStatus reply carries the pre-live picker names as a distinct publishedSenderNames[]
    # array (the relocated old GetFleet sources[].name projection).
    fleet_keys = {"instanceId", "port", "version", "fpsTier", "spoutPrefix",
                  "ownerId", "controlToken", "sources"}
    r = ws.request("GetFleet")
    check("GetFleet removed (-32601)", is_error(r, -32601))
    r = ws.request("GetStatus")
    status = r["result"] if r and "result" in r else {}
    check("GetStatus has publishedSenderNames[]", isinstance(status.get("publishedSenderNames"), list),
          f"got {type(status.get('publishedSenderNames')).__name__}")
    # If the instance wrote to the default discovery path, its on-disk shape MUST be the bare
    # single-instance object carrying exactly the canonical eight fields (incl. controlToken +
    # port). Skipped when the helper used --discovery-path (the file is elsewhere).
    appdata = os.environ.get("APPDATA")
    cfg = os.path.join(appdata, "MoxRelay", "helper-config.json") if appdata else ""
    if cfg and os.path.isfile(cfg):
        with open(cfg, "r", encoding="utf-8") as f:
            doc = json.load(f)
        if isinstance(doc, dict) and doc.get("port") == port:
            check("discovery file is the canonical bare 8-field object",
                  set(doc) == fleet_keys and isinstance(doc.get("sources"), list),
                  f"keys={sorted(doc)}")
        else:
            print("      (default discovery file is a different instance; on-disk shape check skipped)")
    else:
        print("      (no default discovery file; on-disk shape check skipped -- likely --discovery-path)")
    ws.close()

    # ---- Shutdown (contract 1.5.0: clean exit, idempotent re-ack, reply-before-exit ordering) ----
    # Runs LAST: it terminates the instance. Two connections -- one issues Shutdown, both must
    # observe instanceShuttingDown AFTER the reply, then the socket closes (the process exits).
    shut = WsClient("127.0.0.1", port)
    observer = WsClient("127.0.0.1", port)
    r = shut.request("Shutdown", {"drain": True})
    check("Shutdown accepted (drain:true echoed)",
          bool(r) and "result" in r and r["result"].get("accepted") is True and
          r["result"].get("drain") is True and "alreadyStopping" not in r["result"],
          f"got {r}")
    # A second Shutdown while stopping -> idempotent re-ack, NOT 1009. A later drain:false cannot
    # un-drain the in-progress drain (the first committed value is echoed).
    r2 = shut.request("Shutdown", {"drain": False}, timeout=3.0)
    check("repeat Shutdown re-acks (alreadyStopping, not 1009)",
          bool(r2) and "result" in r2 and r2["result"].get("alreadyStopping") is True and
          r2["result"].get("drain") is True,
          f"got {r2}")
    # instanceShuttingDown is ALWAYS delivered (no subscription), and arrives AFTER the reply.
    evt = observer.await_event("instanceShuttingDown", 5.0)
    check("instanceShuttingDown observed on the wire after the reply",
          bool(evt) and evt["data"].get("reason") == "shutdown")
    # The process exits: the sockets close. (We connected to a process we do not own, so a closed
    # socket -- not a PID check -- is the portable exit signal here.)
    closed = False
    try:
        observer.pump(6.0)
    except (ConnectionError, OSError):
        closed = True
    check("control socket closes (process exited)", closed)
    shut.close()
    observer.close()

    print(f"\nRESULT: {'PASS' if not FAILED else 'FAIL'} ({len(PASSED)} passed, {len(FAILED)} failed)")
    return 0 if not FAILED else 1


if __name__ == "__main__":
    sys.exit(main())
