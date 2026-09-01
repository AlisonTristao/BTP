#!/usr/bin/env python3
"""Generate or verify the BTP telemetry-body conformance vectors.

A sibling of tools/test_messages.py: this covers a TELEMETRY logical payload
(schema_version + PACKED_LE / TLV_LE body) against a schema, i.e. btp::telemetry.
It is an independent reimplementation of docs/telemetry.md sections 6, 10, 11
and 13 -- a bug in the C++ and a bug here cannot hide each other.

    python tools/test_telemetry.py --root test-vectors/v2/telemetry
    python tools/test_telemetry.py --root test-vectors/v2/telemetry --check
"""

import argparse
import json
import struct
import sys
from pathlib import Path

OK = "Ok"

# name -> (width, kind). kind: u unsigned, i signed, f float, b bool, e enum.
TYPES = {
    "uint8": (1, "u"), "uint16": (2, "u"), "uint32": (4, "u"), "uint64": (8, "u"),
    "int8": (1, "i"), "int16": (2, "i"), "int32": (4, "i"), "int64": (8, "i"),
    "float32": (4, "f"), "float64": (8, "f"),
    "bool": (1, "b"), "enum8": (1, "e"), "enum16": (2, "e"),
}


def as_int(value):
    return value if isinstance(value, int) else int(value, 0)


class DecodeError(Exception):
    def __init__(self, name):
        super().__init__(name)
        self.name = name


# --------------------------------------------------------------------------
# schema / sample helpers
# --------------------------------------------------------------------------

def load_schema(entries):
    schema = []
    for index, entry in enumerate(entries):
        assert entry["order"] == index, "schema must be in `order`"
        width, kind = TYPES[entry["type"]]
        schema.append({
            "field_id": entry["field_id"],
            "order": index,
            "type": entry["type"],
            "width": width,
            "kind": kind,
            "scale": entry.get("scale", 1.0),
            "offset": entry.get("offset", 0.0),
            "nullable": entry.get("nullable", False),
            "variable": entry.get("variable_count", False),
            "element_count": entry.get("element_count", 1),
            "max_element_count": entry.get("max_element_count", 0),
        })
    return schema


def sample_map(sample):
    return {item["field_id"]: item for item in sample}


def pack_one(field, raw):
    kind, width = field["kind"], field["width"]
    if kind == "f":
        return struct.pack("<f" if width == 4 else "<d", float(raw))
    if kind == "b":
        return bytes([1 if raw else 0])
    if kind == "i":
        return int(raw).to_bytes(width, "little", signed=True)
    return int(raw).to_bytes(width, "little")


def unpack_one(field, data):
    kind, width = field["kind"], field["width"]
    if kind == "f":
        value = struct.unpack("<f" if width == 4 else "<d", data)[0]
        if value != value or value in (float("inf"), float("-inf")):
            raise DecodeError("InvalidValue")
        return value
    if kind == "b":
        if data[0] > 1:
            raise DecodeError("InvalidValue")
        return data[0]
    if kind == "i":
        return int.from_bytes(data, "little", signed=True)
    return int.from_bytes(data, "little")


# --------------------------------------------------------------------------
# PACKED_LE
# --------------------------------------------------------------------------

def encode_packed(schema, sample, schema_version):
    values = sample_map(sample)
    out = struct.pack("<H", schema_version)

    nullable = [f for f in schema if f["nullable"]]
    if nullable:
        bits = 0
        for bit, field in enumerate(nullable):
            item = values[field["field_id"]]
            if not item.get("null", False):
                bits |= 1 << bit
        out += bits.to_bytes((len(nullable) + 7) // 8, "little")

    for field in schema:
        item = values[field["field_id"]]
        if field["nullable"] and item.get("null", False):
            continue
        raw = item["raw"]
        if field["variable"]:
            out += struct.pack("<H", len(raw))
        for element in raw:
            out += pack_one(field, element)
    return out


def decode_packed(schema, data):
    if len(data) < 2:
        raise DecodeError("PayloadTooShort")
    pos = 2
    nullable = [f for f in schema if f["nullable"]]
    present = {}
    if nullable:
        nbytes = (len(nullable) + 7) // 8
        if pos + nbytes > len(data):
            raise DecodeError("PayloadTooShort")
        bitmap = int.from_bytes(data[pos:pos + nbytes], "little")
        used = len(nullable) % 8
        if used and (data[pos + nbytes - 1] >> used):
            raise DecodeError("ReservedNotZero")
        for bit, field in enumerate(nullable):
            present[field["field_id"]] = bool(bitmap & (1 << bit))
        pos += nbytes

    result = []
    for field in schema:
        if field["nullable"] and not present[field["field_id"]]:
            result.append({"field_id": field["field_id"], "null": True})
            continue
        count = field["element_count"]
        if field["variable"]:
            if pos + 2 > len(data):
                raise DecodeError("PayloadTooShort")
            count = struct.unpack("<H", data[pos:pos + 2])[0]
            pos += 2
            if count > field["max_element_count"]:
                raise DecodeError("CountTooLarge")
        width = field["width"]
        if pos + width * count > len(data):
            raise DecodeError("PayloadTooShort")
        raw = []
        for _ in range(count):
            raw.append(unpack_one(field, data[pos:pos + width]))
            pos += width
        result.append({"field_id": field["field_id"], "raw": raw})

    if pos != len(data):
        raise DecodeError("TrailingBytes")
    return result


# --------------------------------------------------------------------------
# TLV_LE
# --------------------------------------------------------------------------

def encode_tlv(schema, sample, schema_version):
    values = sample_map(sample)
    out = struct.pack("<H", schema_version)
    for field in sorted(schema, key=lambda f: f["field_id"]):
        item = values[field["field_id"]]
        if field["nullable"] and item.get("null", False):
            continue
        value = b""
        raw = item["raw"]
        if field["variable"]:
            value += struct.pack("<H", len(raw))
        for element in raw:
            value += pack_one(field, element)
        out += struct.pack("<HH", field["field_id"], len(value)) + value
    return out


def decode_tlv(schema, data):
    if len(data) < 2:
        raise DecodeError("PayloadTooShort")
    by_id = {f["field_id"]: f for f in schema}
    pos = 2
    seen = {}
    previous = None
    while pos < len(data):
        if pos + 4 > len(data):
            raise DecodeError("PayloadTooShort")
        field_id, size = struct.unpack("<HH", data[pos:pos + 4])
        if previous is not None and field_id <= previous:
            raise DecodeError("NotAscending")
        previous = field_id
        if pos + 4 + size > len(data):
            raise DecodeError("LengthOverflow")
        if field_id not in by_id:
            raise DecodeError("InvalidValue")
        seen[field_id] = data[pos + 4:pos + 4 + size]
        pos += 4 + size
    if pos != len(data):
        raise DecodeError("TrailingBytes")

    result = []
    for field in schema:
        if field["field_id"] not in seen:
            if not field["nullable"]:
                raise DecodeError("CountMismatch")
            result.append({"field_id": field["field_id"], "null": True})
            continue
        value = seen[field["field_id"]]
        vpos, count = 0, field["element_count"]
        if field["variable"]:
            if len(value) < 2:
                raise DecodeError("PayloadTooShort")
            count = struct.unpack("<H", value[:2])[0]
            vpos = 2
            if count > field["max_element_count"]:
                raise DecodeError("CountTooLarge")
        width = field["width"]
        if vpos + width * count != len(value):
            raise DecodeError("TrailingBytes")
        raw = []
        for _ in range(count):
            raw.append(unpack_one(field, value[vpos:vpos + width]))
            vpos += width
        result.append({"field_id": field["field_id"], "raw": raw})
    return result


ENCODERS = {"PACKED_LE": encode_packed, "TLV_LE": encode_tlv}
DECODERS = {"PACKED_LE": decode_packed, "TLV_LE": decode_tlv}


# --------------------------------------------------------------------------
# mutations (invalid vectors)
# --------------------------------------------------------------------------

def apply_mutations(data, mutations):
    buf = bytearray(data)
    for mutation in mutations:
        op = mutation["operation"]
        if op == "set_u8":
            buf[as_int(mutation["offset"])] = as_int(mutation["value"]) & 0xFF
        elif op == "set_u16_le":
            struct.pack_into("<H", buf, as_int(mutation["offset"]),
                             as_int(mutation["value"]) & 0xFFFF)
        elif op == "set_u32_le":
            struct.pack_into("<I", buf, as_int(mutation["offset"]),
                             as_int(mutation["value"]) & 0xFFFFFFFF)
        elif op == "truncate":
            del buf[as_int(mutation["length"]):]
        elif op == "append":
            buf += bytes([as_int(mutation["value"]) & 0xFF])
        else:
            raise SystemExit(f"unknown mutation {op}")
    return bytes(buf)


# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------

def sample_equal(a, b):
    if len(a) != len(b):
        return False
    for x, y in zip(a, b):
        if x["field_id"] != y["field_id"]:
            return False
        if x.get("null", False) != y.get("null", False):
            return False
        if not x.get("null", False):
            xr, yr = x["raw"], y["raw"]
            if len(xr) != len(yr):
                return False
            for u, v in zip(xr, yr):
                if isinstance(u, float) or isinstance(v, float):
                    if abs(float(u) - float(v)) > 1e-6 * max(1.0, abs(float(v))):
                        return False
                elif u != v:
                    return False
    return True


def run(root, check):
    root = Path(root)
    manifest_path = root / "manifest.json"
    listed = set()
    if manifest_path.exists():
        listed = set(json.loads(manifest_path.read_text())["vectors"])

    files = 0
    seen_ids = set()
    for path in sorted(root.glob("*/*.json")):
        model = json.loads(path.read_text())
        vid = model["id"]
        seen_ids.add(vid)
        schema = load_schema(model["schema"])
        version = model["schema_version"]
        encoding = model["encoding"]
        bin_path = path.with_suffix(".bin")

        if path.parent.name == "valid":
            data = ENCODERS[encoding](schema, model["sample"], version)
            if check:
                if not bin_path.exists() or bin_path.read_bytes() != data:
                    raise SystemExit(f"{vid}: .bin does not match the model")
            else:
                bin_path.write_bytes(data)
            decoded = DECODERS[encoding](schema, data)
            if not sample_equal(decoded, model["sample"]):
                raise SystemExit(f"{vid}: decode does not round-trip the sample")
            files += 1
        else:  # invalid: build the base, mutate, expect a named error
            base = ENCODERS[encoding](schema, model["sample"], version)
            data = apply_mutations(base, model["mutations"])
            if check:
                if not bin_path.exists() or bin_path.read_bytes() != data:
                    raise SystemExit(f"{vid}: .bin does not match the mutated base")
            else:
                bin_path.write_bytes(data)
            try:
                DECODERS[encoding](schema, data)
            except DecodeError as error:
                if error.name != model["expected_error"]:
                    raise SystemExit(
                        f"{vid}: expected {model['expected_error']}, got {error.name}")
            else:
                raise SystemExit(f"{vid}: expected {model['expected_error']}, decoded cleanly")
            files += 1

    # every checked-in .bin must have a .json
    for path in sorted(root.glob("*/*.bin")):
        if not path.with_suffix(".json").exists():
            raise SystemExit(f"orphan vector: {path}")

    if listed and listed != seen_ids:
        raise SystemExit(
            f"manifest.json mismatch: {sorted(listed ^ seen_ids)}")

    print(f"BTP telemetry vectors verified: {files} vectors")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    run(args.root, args.check)


if __name__ == "__main__":
    main()
