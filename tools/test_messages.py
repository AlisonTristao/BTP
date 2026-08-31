#!/usr/bin/env python3
"""Generate or verify the BTP message-layer conformance vectors.

A sibling of tools/test_vectors_v2.py, one layer up: this covers the logical
payload of a COMMAND / CONTROL object_id (btp::messages), not the BTP frame.
It is an independent reimplementation of the layout rules in docs/commands.md
and docs/session-and-terminal.md -- the same reason the v1 and v2 frame tools
are separate scripts, so a bug in the C++ and a bug here cannot hide each
other.

A message vector .bin is the logical payload octets only, after reassembly.
The frame around it is test_vectors*.py's concern.

    python tools/test_messages.py --root test-vectors/v2/messages
    python tools/test_messages.py --root test-vectors/v2/messages --check
"""

import argparse
import json
import struct
import sys
from pathlib import Path


# Error names -- must match btp::MessageError in include/btp/messages.hpp.
OK = "Ok"

MAX_UTF8_TEXT = 1024
MAX_NAME_OR_UNIT = 128
MAX_RESULT_MESSAGE = 512
MAX_ACTION_BODY = 32768
MAX_ANNOUNCED_VERSIONS = 8


def integer(value):
    return value if isinstance(value, int) else int(value, 0)


class Reader:
    """Sticky, bounds-checked cursor mirroring btp::detail::Reader."""

    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.error = OK

    def _take(self, count, on_overflow):
        if self.error != OK:
            return None
        if count > len(self.data) - self.pos:
            self.error = on_overflow
            return None
        chunk = self.data[self.pos:self.pos + count]
        self.pos += count
        return chunk

    def u8(self):
        chunk = self._take(1, "PayloadTooShort")
        return chunk[0] if chunk is not None else 0

    def u16(self):
        chunk = self._take(2, "PayloadTooShort")
        return struct.unpack("<H", chunk)[0] if chunk is not None else 0

    def u32(self):
        chunk = self._take(4, "PayloadTooShort")
        return struct.unpack("<I", chunk)[0] if chunk is not None else 0

    def u64(self):
        chunk = self._take(8, "PayloadTooShort")
        return struct.unpack("<Q", chunk)[0] if chunk is not None else 0

    def f64(self):
        chunk = self._take(8, "PayloadTooShort")
        return struct.unpack("<d", chunk)[0] if chunk is not None else 0.0

    def raw(self, count):
        chunk = self._take(count, "PayloadTooShort")
        return chunk if chunk is not None else b""

    def utf8_u16(self, limit=MAX_UTF8_TEXT):
        length = self.u16()
        if self.error != OK:
            return b""
        if length > limit:
            self.error = "CountTooLarge"
            return b""
        chunk = self._take(length, "LengthOverflow")
        return chunk if chunk is not None else b""

    def bytes_u32(self, limit):
        length = self.u32()
        if self.error != OK:
            return b""
        if length > limit:
            self.error = "CountTooLarge"
            return b""
        chunk = self._take(length, "LengthOverflow")
        return chunk if chunk is not None else b""

    def expect_zero(self, count):
        for _ in range(count):
            octet = self.u8()
            if self.error == OK and octet != 0:
                self.error = "ReservedNotZero"

    def require_exhausted(self):
        if self.error == OK and self.pos != len(self.data):
            self.error = "TrailingBytes"
        return self.error


def _u16(v):
    return struct.pack("<H", v)


def _u32(v):
    return struct.pack("<I", v)


def _u64(v):
    return struct.pack("<Q", v)


def _utf8_u16(text):
    raw = text.encode("utf-8") if isinstance(text, str) else bytes(text)
    return _u16(len(raw)) + raw


def _bytes_u32(hexstr):
    raw = bytes.fromhex(hexstr) if hexstr else b""
    return _u32(len(raw)) + raw


def _request_ref_bytes(ref):
    return _u32(integer(ref["request_source_id"])) + \
        _u32(integer(ref["request_boot_id"])) + \
        _u32(integer(ref["reply_to_sequence"]))


def _read_request_ref(reader):
    return {
        "request_source_id": reader.u32(),
        "request_boot_id": reader.u32(),
        "reply_to_sequence": reader.u32(),
    }


def _valid_result_status(status):
    return status <= 6


# ---------------------------------------------------------------------------
# HELLO
# ---------------------------------------------------------------------------

def encode_hello(m):
    versions = [integer(v) for v in m["versions"]]
    out = bytearray()
    out.append(integer(m["role"]))
    out.append(len(versions))
    out += b"\x00\x00"
    out += _u32(integer(m["max_logical_payload"]))
    out += _u16(integer(m["max_inflight_reassemblies"]))
    out += _u16(integer(m["max_subscriptions"]))
    out += _u32(integer(m["max_dedup_entries"]))
    out += _u32(integer(m["session_timeout_ms"]))
    out += bytes.fromhex(m["peer_uuid_hex"])
    out += _u32(integer(m["config_revision"]))
    out += bytes(versions)
    return bytes(out)


def decode_hello(data):
    r = Reader(data)
    role = r.u8()
    vcount = r.u8()
    r.expect_zero(2)
    mlp = r.u32()
    mir = r.u16()
    msub = r.u16()
    mded = r.u32()
    sto = r.u32()
    uuid = r.raw(16)
    crev = r.u32()
    if r.error != OK:
        return None, r.error
    if vcount == 0:
        return None, "ZeroField"
    if vcount > MAX_ANNOUNCED_VERSIONS:
        return None, "CountTooLarge"
    versions = []
    prev = 0
    for i in range(vcount):
        v = r.u8()
        if r.error != OK:
            return None, r.error
        if v == 0:
            return None, "ZeroField"
        if i != 0 and v <= prev:
            return None, "NotAscending"
        versions.append(v)
        prev = v
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    if not 1 <= role <= 4:
        return None, "InvalidValue"
    if 0 in (mlp, mir, msub, mded, sto):
        return None, "ZeroField"
    if uuid == b"\x00" * 16:
        return None, "ZeroField"
    return {
        "role": role,
        "versions": versions,
        "max_logical_payload": mlp,
        "max_inflight_reassemblies": mir,
        "max_subscriptions": msub,
        "max_dedup_entries": mded,
        "session_timeout_ms": sto,
        "peer_uuid_hex": uuid.hex(),
        "config_revision": crev,
    }, OK


# ---------------------------------------------------------------------------
# HELLO_RESULT
# ---------------------------------------------------------------------------

def encode_hello_result(m):
    out = bytearray()
    out += _request_ref_bytes(m["request"])
    out.append(integer(m["status"]))
    out.append(integer(m["selected_version"]))
    out += _u16(integer(m["error_code"]))
    out += _u32(integer(m["max_logical_payload"]))
    out += _u16(integer(m["max_inflight_reassemblies"]))
    out += _u16(integer(m["max_subscriptions"]))
    out += _u32(integer(m["max_dedup_entries"]))
    out += _u32(integer(m["session_timeout_ms"]))
    out += bytes.fromhex(m["peer_uuid_hex"])
    out += _u32(integer(m["config_revision"]))
    return bytes(out)


def decode_hello_result(data):
    r = Reader(data)
    ref = _read_request_ref(r)
    status = r.u8()
    selected_version = r.u8()
    error_code = r.u16()
    mlp = r.u32()
    mir = r.u16()
    msub = r.u16()
    mded = r.u32()
    sto = r.u32()
    uuid = r.raw(16)
    crev = r.u32()
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    if status not in (0, 5):
        return None, "InvalidValue"
    return {
        "request": ref,
        "status": status,
        "selected_version": selected_version,
        "error_code": error_code,
        "max_logical_payload": mlp,
        "max_inflight_reassemblies": mir,
        "max_subscriptions": msub,
        "max_dedup_entries": mded,
        "session_timeout_ms": sto,
        "peer_uuid_hex": uuid.hex(),
        "config_revision": crev,
    }, OK


# ---------------------------------------------------------------------------
# SESSION_CLOSE / control results
# ---------------------------------------------------------------------------

def encode_session_close(m):
    return bytes([integer(m["reason"])]) + b"\x00\x00\x00" + \
        _u32(integer(m["drain_timeout_ms"]))


def decode_session_close(data):
    r = Reader(data)
    reason = r.u8()
    r.expect_zero(3)
    drain = r.u32()
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    if reason > 3:
        return None, "InvalidValue"
    return {"reason": reason, "drain_timeout_ms": drain}, OK


def encode_control_result(m):
    out = bytearray()
    out += _request_ref_bytes(m["request"])
    out.append(integer(m["status"]))
    out += b"\x00"
    out += _u16(integer(m["error_code"]))
    return bytes(out)


def decode_control_result(data):
    r = Reader(data)
    ref = _read_request_ref(r)
    status = r.u8()
    r.expect_zero(1)
    error_code = r.u16()
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    if not _valid_result_status(status):
        return None, "InvalidValue"
    return {"request": ref, "status": status, "error_code": error_code}, OK


# ---------------------------------------------------------------------------
# COMMAND_REQUEST / COMMAND_RESULT
# ---------------------------------------------------------------------------

def encode_command_request(m):
    out = bytearray()
    out += _u32(integer(m["target_source_id"]))
    out += _u32(integer(m["target_boot_id"]))
    out += _u16(integer(m["action_id"]))
    out += _u16(integer(m["action_version"]))
    out += b"\x00\x00\x00\x00"
    out += _bytes_u32(m.get("parameters_hex", ""))
    return bytes(out)


def decode_command_request(data):
    r = Reader(data)
    tsid = r.u32()
    tbid = r.u32()
    aid = r.u16()
    aver = r.u16()
    r.expect_zero(2)
    r.expect_zero(2)
    params = r.bytes_u32(MAX_ACTION_BODY)
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    if 0 in (tsid, tbid, aid, aver):
        return None, "ZeroField"
    return {
        "target_source_id": tsid,
        "target_boot_id": tbid,
        "action_id": aid,
        "action_version": aver,
        "parameters_hex": params.hex(),
    }, OK


def encode_command_result(m):
    out = bytearray()
    out += _request_ref_bytes(m["request"])
    out += _u16(integer(m["action_id"]))
    out += _u16(integer(m["action_version"]))
    out.append(integer(m["status"]))
    out += b"\x00"
    out += _u16(integer(m["error_code"]))
    out += _utf8_u16(m.get("message", ""))
    out += _bytes_u32(m.get("result_hex", ""))
    return bytes(out)


def decode_command_result(data):
    r = Reader(data)
    ref = _read_request_ref(r)
    aid = r.u16()
    aver = r.u16()
    status = r.u8()
    r.expect_zero(1)
    error_code = r.u16()
    message = r.utf8_u16(MAX_RESULT_MESSAGE)
    result = r.bytes_u32(MAX_ACTION_BODY)
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    if not _valid_result_status(status):
        return None, "InvalidValue"
    return {
        "request": ref,
        "action_id": aid,
        "action_version": aver,
        "status": status,
        "error_code": error_code,
        "message": message.decode("utf-8"),
        "result_hex": result.hex(),
    }, OK


# ---------------------------------------------------------------------------
# MANIFEST_REQUEST
# ---------------------------------------------------------------------------

def encode_manifest_request(m):
    return _u32(integer(m["target_source_id"])) + \
        _u32(integer(m["target_boot_id"])) + \
        _u32(integer(m["known_config_revision"]))


def decode_manifest_request(data):
    r = Reader(data)
    tsid = r.u32()
    tbid = r.u32()
    kcr = r.u32()
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    return {
        "target_source_id": tsid,
        "target_boot_id": tbid,
        "known_config_revision": kcr,
    }, OK


# ---------------------------------------------------------------------------
# SUBSCRIBE / SUBSCRIBE_RESULT / UNSUBSCRIBE
# ---------------------------------------------------------------------------

def encode_subscribe(m):
    out = bytearray()
    out += _u32(integer(m["target_source_id"]))
    out += _u32(integer(m["target_boot_id"]))
    out += _u16(integer(m["topic_id"]))
    out += b"\x00\x00"
    out += _u32(integer(m["requested_rate_millihz"]))
    out += _u32(integer(m["requested_lease_ms"]))
    return bytes(out)


def decode_subscribe(data):
    r = Reader(data)
    tsid = r.u32()
    tbid = r.u32()
    topic = r.u16()
    r.expect_zero(2)
    rate = r.u32()
    lease = r.u32()
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    if 0 in (tsid, tbid, topic, rate, lease):
        return None, "ZeroField"
    return {
        "target_source_id": tsid,
        "target_boot_id": tbid,
        "topic_id": topic,
        "requested_rate_millihz": rate,
        "requested_lease_ms": lease,
    }, OK


def encode_subscribe_result(m):
    out = bytearray()
    out += _request_ref_bytes(m["request"])
    out.append(integer(m["status"]))
    out += b"\x00"
    out += _u16(integer(m["error_code"]))
    out += _u32(integer(m["subscription_id"]))
    out += _u32(integer(m["effective_rate_millihz"]))
    out += _u32(integer(m["granted_lease_ms"]))
    return bytes(out)


def decode_subscribe_result(data):
    r = Reader(data)
    ref = _read_request_ref(r)
    status = r.u8()
    r.expect_zero(1)
    error_code = r.u16()
    sub_id = r.u32()
    eff_rate = r.u32()
    lease = r.u32()
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    if not _valid_result_status(status):
        return None, "InvalidValue"
    return {
        "request": ref,
        "status": status,
        "error_code": error_code,
        "subscription_id": sub_id,
        "effective_rate_millihz": eff_rate,
        "granted_lease_ms": lease,
    }, OK


def encode_unsubscribe(m):
    return _u32(integer(m["target_source_id"])) + \
        _u32(integer(m["target_boot_id"])) + \
        _u32(integer(m["subscription_id"]))


def decode_unsubscribe(data):
    r = Reader(data)
    tsid = r.u32()
    tbid = r.u32()
    sub_id = r.u32()
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    if 0 in (tsid, tbid, sub_id):
        return None, "ZeroField"
    return {
        "target_source_id": tsid,
        "target_boot_id": tbid,
        "subscription_id": sub_id,
    }, OK


# ---------------------------------------------------------------------------
# STATUS (docs/commands.md section 5)
# ---------------------------------------------------------------------------

STATUS_COUNTERS = [
    "uptime_us", "frames_rx", "frames_tx", "frames_dropped", "crc_errors",
    "decode_errors", "reassembly_completed", "reassembly_timeouts",
    "reassembly_rejected", "command_duplicates", "telemetry_dropped",
]


def encode_status(m):
    version = integer(m["status_version"])
    out = bytearray()
    out += _u16(version)
    out += _u16(integer(m["flags"]))
    for key in STATUS_COUNTERS:
        out += _u64(integer(m[key]))
    if version == 2:
        topics = m.get("topics", [])
        out += _u16(len(topics))
        for t in topics:
            out += _u32(integer(t["source_id"]))
            out += _u16(integer(t["topic_id"]))
            out += _u16(integer(t["subscriber_count"]))
            out += _u32(integer(t["effective_rate_millihz"]))
            out += _u64(integer(t["bytes_total"]))
            out += _u64(integer(t["samples_dropped_total"]))
    return bytes(out)


def decode_status(data):
    r = Reader(data)
    version = r.u16()
    flags = r.u16()
    counters = {key: r.u64() for key in STATUS_COUNTERS}
    if r.error != OK:
        return None, r.error
    if version not in (1, 2):
        return None, "UnsupportedFormat"
    model = {"status_version": version, "flags": flags}
    model.update(counters)
    if version == 1:
        tail = r.require_exhausted()
        if tail != OK:
            return None, tail
        return model, OK
    declared = r.u16()
    if r.error != OK:
        return None, r.error
    expected = 92 + 2 + 28 * declared
    if len(data) < expected:
        return None, "PayloadTooShort"
    if len(data) > expected:
        return None, "TrailingBytes"
    topics = []
    for _ in range(declared):
        topics.append({
            "source_id": r.u32(),
            "topic_id": r.u16(),
            "subscriber_count": r.u16(),
            "effective_rate_millihz": r.u32(),
            "bytes_total": r.u64(),
            "samples_dropped_total": r.u64(),
        })
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    model["topics"] = topics
    return model, OK


# ---------------------------------------------------------------------------
# MANIFEST_DATA (docs/commands.md section 3) -- independent implementation
# ---------------------------------------------------------------------------

MAX_NAME = 128


def _field_bytes(f):
    body = bytearray()
    body += _u16(integer(f["field_id"]))
    body += _u16(integer(f["order"]))
    body.append(integer(f["type"]))
    body.append(integer(f["flags"]))
    body += _u16(integer(f["element_count"]))
    body += _u16(integer(f["max_element_count"]))
    body += struct.pack("<d", float(f["scale"]))
    body += struct.pack("<d", float(f["offset"]))
    enums = f.get("enums", [])
    body += _u16(len(enums))
    body += _utf8_u16(f.get("name", ""))
    body += _utf8_u16(f.get("unit", ""))
    body += _utf8_u16(f.get("description", ""))
    for e in enums:
        body += _u16(integer(e["value"]))
        body += _utf8_u16(e.get("label", ""))
    return _u32(len(body)) + bytes(body)


def _topic_bytes(t):
    body = bytearray()
    body += _u16(integer(t["topic_id"]))
    body += _u16(integer(t["schema_version"]))
    body.append(integer(t["encoding"]))
    body.append(integer(t["flags"]))
    fields = t.get("fields", [])
    body += _u16(len(fields))
    body += _u32(integer(t["max_rate_millihz"]))
    body += _utf8_u16(t.get("name", ""))
    body += _utf8_u16(t.get("description", ""))
    for f in fields:
        body += _field_bytes(f)
    return _u32(len(body)) + bytes(body)


def _action_bytes(a):
    body = bytearray()
    body += _u16(integer(a["action_id"]))
    body += _u16(integer(a["action_version"]))
    body += _u16(integer(a["flags"]))
    body.append(integer(a["parameter_encoding"]))
    body.append(integer(a["result_encoding"]))
    params = a.get("parameters", [])
    results = a.get("results", [])
    errors = a.get("errors", [])
    body += _u16(len(params))
    body += _u16(len(results))
    body += _u32(integer(a["execution_timeout_ms"]))
    body += _utf8_u16(a.get("name", ""))
    body += _utf8_u16(a.get("description", ""))
    body += _utf8_u16(a.get("confirmation_text", ""))
    for f in params:
        body += _field_bytes(f)
    for f in results:
        body += _field_bytes(f)
    body += _u16(len(errors))
    for e in errors:
        body += _u16(integer(e["error_code"]))
        body += _utf8_u16(e.get("label", ""))
    return _u32(len(body)) + bytes(body)


def encode_manifest_data(m):
    fmt = integer(m["manifest_format_version"])
    topics = m.get("topics", [])
    actions = m.get("actions", [])
    out = bytearray()
    out += _request_ref_bytes(m["request"])
    out.append(integer(m["status"]))
    out.append(integer(m["flags"]))
    out += _u16(integer(m["error_code"]))
    out += _u16(fmt)
    out += b"\x00\x00"
    out += _u32(integer(m["config_revision"]))
    out += bytes.fromhex(m["source_uuid_hex"])
    out += _u32(integer(m["described_source_id"]))
    out += _u32(integer(m["described_boot_id"]))
    out.append(integer(m["source_role"]))
    out.append(integer(m["source_flags"]))
    out += _u16(integer(m["catalog_index"]))
    out += _u16(integer(m["catalog_count"]))
    out += _u16(len(topics))
    out += _u16(len(actions))
    out += _utf8_u16(m.get("source_name", ""))
    if fmt == 2:
        entries = m.get("source_info", [])
        out += _u16(len(entries))
        for e in entries:
            out += _utf8_u16(e["key"])
            out += _utf8_u16(e.get("label", ""))
            out += _utf8_u16(e["value"])
    for t in topics:
        out += _topic_bytes(t)
    for a in actions:
        out += _action_bytes(a)
    return bytes(out)


def _read_field(r):
    size = r.u32()
    end = r.pos + size
    fr = Reader(r.data[r.pos:end])
    r.pos = end
    f = {
        "field_id": fr.u16(),
        "order": fr.u16(),
        "type": fr.u8(),
        "flags": fr.u8(),
        "element_count": fr.u16(),
        "max_element_count": fr.u16(),
        "scale": fr.f64(),
        "offset": fr.f64(),
    }
    enum_count = fr.u16()
    f["name"] = fr.utf8_u16(MAX_NAME).decode("utf-8")
    f["unit"] = fr.utf8_u16(MAX_NAME).decode("utf-8")
    f["description"] = fr.utf8_u16().decode("utf-8")
    enums = []
    for _ in range(enum_count):
        enums.append({"value": fr.u16(),
                      "label": fr.utf8_u16().decode("utf-8")})
    tail = fr.require_exhausted()
    if tail != OK:
        raise ValueError(tail)
    if enums:
        f["enums"] = enums
    return f


def decode_manifest_data(data):
    r = Reader(data)
    m = {
        "request": _read_request_ref(r),
        "status": r.u8(),
        "flags": r.u8(),
        "error_code": r.u16(),
        "manifest_format_version": r.u16(),
    }
    r.expect_zero(2)
    m["config_revision"] = r.u32()
    m["source_uuid_hex"] = r.raw(16).hex()
    m["described_source_id"] = r.u32()
    m["described_boot_id"] = r.u32()
    m["source_role"] = r.u8()
    m["source_flags"] = r.u8()
    m["catalog_index"] = r.u16()
    m["catalog_count"] = r.u16()
    topic_count = r.u16()
    action_count = r.u16()
    m["source_name"] = r.utf8_u16().decode("utf-8")
    if r.error != OK:
        return None, r.error
    if m["manifest_format_version"] not in (1, 2):
        return None, "UnsupportedFormat"
    if m["status"] > 6 or not 1 <= m["source_role"] <= 4:
        return None, "InvalidValue"
    if m["manifest_format_version"] == 2:
        info_count = r.u16()
        entries = []
        for _ in range(info_count):
            entries.append({
                "key": r.utf8_u16(64).decode("utf-8"),
                "label": r.utf8_u16(256).decode("utf-8"),
                "value": r.utf8_u16(256).decode("utf-8"),
            })
        if entries:
            m["source_info"] = entries
    try:
        topics = [_read_topic(r) for _ in range(topic_count)]
        actions = [_read_action(r) for _ in range(action_count)]
    except ValueError as exc:
        return None, str(exc)
    if r.error != OK:
        return None, r.error
    tail = r.require_exhausted()
    if tail != OK:
        return None, tail
    if topics:
        m["topics"] = topics
    if actions:
        m["actions"] = actions
    return m, OK


def _read_topic(r):
    size = r.u32()
    end = r.pos + size
    tr = Reader(r.data[r.pos:end])
    r.pos = end
    t = {
        "topic_id": tr.u16(),
        "schema_version": tr.u16(),
        "encoding": tr.u8(),
        "flags": tr.u8(),
    }
    field_count = tr.u16()
    t["max_rate_millihz"] = tr.u32()
    t["name"] = tr.utf8_u16(MAX_NAME).decode("utf-8")
    t["description"] = tr.utf8_u16().decode("utf-8")
    fields = [_read_field(tr) for _ in range(field_count)]
    tail = tr.require_exhausted()
    if tail != OK:
        raise ValueError(tail)
    if fields:
        t["fields"] = fields
    return t


def _read_action(r):
    size = r.u32()
    end = r.pos + size
    ar = Reader(r.data[r.pos:end])
    r.pos = end
    a = {
        "action_id": ar.u16(),
        "action_version": ar.u16(),
        "flags": ar.u16(),
        "parameter_encoding": ar.u8(),
        "result_encoding": ar.u8(),
    }
    param_count = ar.u16()
    result_count = ar.u16()
    a["execution_timeout_ms"] = ar.u32()
    a["name"] = ar.utf8_u16(MAX_NAME).decode("utf-8")
    a["description"] = ar.utf8_u16().decode("utf-8")
    a["confirmation_text"] = ar.utf8_u16().decode("utf-8")
    params = [_read_field(ar) for _ in range(param_count)]
    results = [_read_field(ar) for _ in range(result_count)]
    error_count = ar.u16()
    errors = []
    for _ in range(error_count):
        errors.append({"error_code": ar.u16(),
                       "label": ar.utf8_u16().decode("utf-8")})
    tail = ar.require_exhausted()
    if tail != OK:
        raise ValueError(tail)
    if params:
        a["parameters"] = params
    if results:
        a["results"] = results
    if errors:
        a["errors"] = errors
    return a


CODECS = {
    "hello": (encode_hello, decode_hello),
    "status": (encode_status, decode_status),
    "manifest_data": (encode_manifest_data, decode_manifest_data),
    "hello_result": (encode_hello_result, decode_hello_result),
    "session_close": (encode_session_close, decode_session_close),
    "session_close_result": (encode_control_result, decode_control_result),
    "command_request": (encode_command_request, decode_command_request),
    "command_result": (encode_command_result, decode_command_result),
    "manifest_request": (encode_manifest_request, decode_manifest_request),
    "subscribe": (encode_subscribe, decode_subscribe),
    "subscribe_result": (encode_subscribe_result, decode_subscribe_result),
    "unsubscribe": (encode_unsubscribe, decode_unsubscribe),
    "unsubscribe_result": (encode_control_result, decode_control_result),
}


def canonical(model):
    """Normalize for comparison: ints stay ints, "0x.." strings become ints,
    empty lists are dropped (a decoder omits them, a hand-written model may
    spell them out), dicts recurse sorted."""
    if isinstance(model, dict):
        return {k: canonical(v) for k, v in sorted(model.items())
                if not (isinstance(v, list) and len(v) == 0)}
    if isinstance(model, list):
        return [canonical(v) for v in model]
    if isinstance(model, str):
        try:
            return int(model, 0)
        except ValueError:
            return model
    return model


def apply_mutations(data, mutations):
    result = bytearray(data)
    for mutation in mutations:
        op = mutation["operation"]
        if op == "truncate":
            result = result[:integer(mutation["length"])]
        elif op == "append":
            result += bytes.fromhex(mutation["hex"])
        elif op == "set_u8":
            result[integer(mutation["offset"])] = integer(mutation["value"])
        elif op == "set_u16_le":
            off = integer(mutation["offset"])
            result[off:off + 2] = struct.pack("<H", integer(mutation["value"]))
        elif op == "set_u32_le":
            off = integer(mutation["offset"])
            result[off:off + 4] = struct.pack("<I", integer(mutation["value"]))
        else:
            raise ValueError("unknown mutation operation: " + op)
    return bytes(result)


def load_json(path):
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--check", action="store_true",
                        help="compare generated bytes with checked-in .bin files")
    arguments = parser.parse_args()
    root = arguments.root.resolve()

    valid = {}
    failures = []

    for path in sorted((root).glob("*/valid/*.json")):
        description = load_json(path)
        try:
            if description["format"] != "btp-test-vector-msg-v2":
                raise ValueError("unsupported vector description format")
            obj = description["object"]
            encode, decode = CODECS[obj]
            model = description["payload_model"]
            data = encode(model)

            decoded, error = decode(data)
            if error != OK:
                raise ValueError("payload_model does not encode a valid message: " + error)
            if canonical(decoded) != canonical(model):
                raise ValueError("round-trip mismatch:\n  in:  %s\n  out: %s"
                                 % (canonical(model), canonical(decoded)))

            key = description["id"]
            if key in valid:
                raise ValueError("duplicate vector id: " + key)
            valid[key] = (description, data)

            binary_path = path.with_suffix(".bin")
            if arguments.check:
                if not binary_path.exists() or binary_path.read_bytes() != data:
                    raise ValueError("checked-in .bin differs; run without --check")
            else:
                binary_path.write_bytes(data)
        except (KeyError, ValueError, struct.error) as error:
            failures.append("%s: %s" % (path, error))

    for path in sorted((root).glob("*/invalid/*.json")):
        description = load_json(path)
        try:
            if description["format"] != "btp-test-vector-msg-v2":
                raise ValueError("unsupported vector description format")
            obj = description["object"]
            _, decode = CODECS[obj]
            base_description, base_data = valid[description["base"]]
            if base_description["object"] != obj:
                raise ValueError("invalid vector object differs from its base")
            data = apply_mutations(base_data, description["mutations"])

            _, error = decode(data)
            if error != description["expected_error"]:
                raise ValueError("expected %s, got %s"
                                 % (description["expected_error"], error))

            binary_path = path.with_suffix(".bin")
            if arguments.check:
                if not binary_path.exists() or binary_path.read_bytes() != data:
                    raise ValueError("checked-in .bin differs; run without --check")
            else:
                binary_path.write_bytes(data)
        except (KeyError, ValueError, struct.error) as error:
            failures.append("%s: %s" % (path, error))

    invalid_ids = set()
    for path in root.glob("*/invalid/*.json"):
        invalid_ids.add(load_json(path)["id"])

    try:
        manifest = load_json(root / "manifest.json")
        if manifest["format"] != "btp-test-vector-msg-manifest-v2":
            raise ValueError("unsupported vector manifest format")
        if set(manifest["valid_vectors"]) != set(valid):
            raise ValueError("manifest valid_vectors does not match valid/*.json")
        if set(manifest["invalid_vectors"]) != invalid_ids:
            raise ValueError("manifest invalid_vectors does not match invalid/*.json")
    except (KeyError, ValueError) as error:
        failures.append("%s: %s" % (root / "manifest.json", error))

    described_bins = {p.with_suffix(".bin") for p in root.glob("*/valid/*.json")}
    described_bins.update(p.with_suffix(".bin") for p in root.glob("*/invalid/*.json"))
    orphans = set(root.glob("*/*/*.bin")) - described_bins
    if orphans:
        failures.append("orphan .bin files: " +
                        ", ".join(str(p) for p in sorted(orphans)))

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    action = "verified" if arguments.check else "generated"
    print("BTP message vectors %s: %d files" % (action, len(described_bins)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
