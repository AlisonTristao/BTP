#!/usr/bin/env python3
"""Generate or verify the canonical BTP v1 conformance vectors."""

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path


MAGIC = b"BTP\x00"
HEADER_SIZE = 36
CRC_SIZE = 4
TYPE_VALUES = {
    "TELEMETRY": 0x01,
    "LOG": 0x02,
    "COMMAND": 0x03,
    "TERMINAL": 0x04,
    "CONTROL": 0x05,
}
TRANSPORT_LIMITS = {
    "esp_now": (250, 210),
    "serial": (4096, 4056),
    "usb_hid": (63, 23),
}


def integer(value):
    if isinstance(value, int):
        return value
    return int(value, 0)


def payload_bytes(frame):
    return bytes.fromhex(frame.get("payload_hex", ""))


def encode_frame(description):
    frame = description["frame"]
    payload = payload_bytes(frame)
    header = bytearray()
    header.extend(MAGIC)
    header.extend(struct.pack("<BBHHH", 1, TYPE_VALUES[frame["type"]],
                              integer(frame["flags"]), HEADER_SIZE,
                              len(payload)))
    header.extend(struct.pack("<IIIQHBB",
                              integer(frame["source_id"]),
                              integer(frame["boot_id"]),
                              integer(frame["sequence"]),
                              integer(frame["timestamp_us"]),
                              integer(frame["object_id"]),
                              integer(frame["fragment_index"]),
                              integer(frame["fragment_count"])))
    if len(header) != HEADER_SIZE:
        raise ValueError("internal error: BTP v1 header is not 36 bytes")
    body = bytes(header) + payload
    return body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def apply_mutations(data, mutations):
    result = bytearray(data)
    recompute_crc = False
    for mutation in mutations:
        operation = mutation["operation"]
        offset = integer(mutation["offset"])
        if offset < 0:
            offset += len(result)
        value = integer(mutation["value"])
        if operation == "set_u8":
            result[offset] = value
        elif operation == "xor_u8":
            result[offset] ^= value
        elif operation == "set_u16_le":
            result[offset:offset + 2] = struct.pack("<H", value)
        else:
            raise ValueError("unknown mutation operation: " + operation)
        recompute_crc = recompute_crc or mutation.get("recompute_crc", False)
    if recompute_crc:
        result[-CRC_SIZE:] = struct.pack(
            "<I", zlib.crc32(result[:-CRC_SIZE]) & 0xFFFFFFFF)
    return bytes(result)


def decode_error(data, transport):
    max_frame, max_payload = TRANSPORT_LIMITS[transport]
    if len(data) < HEADER_SIZE + CRC_SIZE:
        return "FrameTooShort"
    if len(data) > max_frame:
        return "FrameTooLarge"
    if data[:4] != MAGIC:
        return "InvalidMagic"
    if data[4] != 1:
        return "UnsupportedVersion"
    if struct.unpack_from("<H", data, 8)[0] != HEADER_SIZE:
        return "InvalidHeaderSize"
    payload_size = struct.unpack_from("<H", data, 10)[0]
    if payload_size > max_payload:
        return "PayloadTooLarge"
    if len(data) != HEADER_SIZE + payload_size + CRC_SIZE:
        return "SizeMismatch"
    expected_crc = struct.unpack_from("<I", data, HEADER_SIZE + payload_size)[0]
    if zlib.crc32(data[:HEADER_SIZE + payload_size]) & 0xFFFFFFFF != expected_crc:
        return "CrcMismatch"
    message_type = data[5]
    flags = struct.unpack_from("<H", data, 6)[0]
    source_id, boot_id = struct.unpack_from("<II", data, 12)
    fragment_index, fragment_count = data[34], data[35]
    if not 1 <= message_type <= 5:
        return "InvalidType"
    if flags & ~0x0001:
        return "InvalidFlags"
    if source_id == 0:
        return "InvalidSourceId"
    if boot_id == 0:
        return "InvalidBootId"
    fragmented = flags & 0x0001
    if fragmented:
        if fragment_count < 2 or fragment_index >= fragment_count:
            return "InvalidFragmentation"
    elif fragment_index != 0 or fragment_count != 1:
        return "InvalidFragmentation"
    return "Ok"


def assert_decoded_matches(data, description):
    frame = description["frame"]
    values = struct.unpack_from("<4sBBHHHIIIQHBB", data, 0)
    actual = {
        "type": values[2],
        "flags": values[3],
        "source_id": values[6],
        "boot_id": values[7],
        "sequence": values[8],
        "timestamp_us": values[9],
        "object_id": values[10],
        "fragment_index": values[11],
        "fragment_count": values[12],
    }
    expected = {
        "type": TYPE_VALUES[frame["type"]],
        "flags": integer(frame["flags"]),
        "source_id": integer(frame["source_id"]),
        "boot_id": integer(frame["boot_id"]),
        "sequence": integer(frame["sequence"]),
        "timestamp_us": integer(frame["timestamp_us"]),
        "object_id": integer(frame["object_id"]),
        "fragment_index": integer(frame["fragment_index"]),
        "fragment_count": integer(frame["fragment_count"]),
    }
    if actual != expected:
        raise ValueError("decoded header differs from JSON description")
    size = struct.unpack_from("<H", data, 10)[0]
    if data[HEADER_SIZE:HEADER_SIZE + size] != payload_bytes(frame):
        raise ValueError("decoded payload differs from JSON description")


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
    for path in sorted((root / "valid").glob("*.json")):
        description = load_json(path)
        try:
            if description["format"] != "btp-test-vector-v1":
                raise ValueError("unsupported vector description format")
            if description["id"] in valid:
                raise ValueError("duplicate vector id: " + description["id"])
            data = encode_frame(description)
            if decode_error(data, description["transport"]) != "Ok":
                raise ValueError("description does not encode a valid frame")
            assert_decoded_matches(data, description)
            valid[description["id"]] = (description, data)
            binary_path = path.with_suffix(".bin")
            if arguments.check:
                if not binary_path.exists() or binary_path.read_bytes() != data:
                    raise ValueError("checked-in .bin differs; run without --check")
            else:
                binary_path.write_bytes(data)
        except (KeyError, ValueError, struct.error) as error:
            failures.append(f"{path}: {error}")

    invalid_ids = set()
    for path in sorted((root / "invalid").glob("*.json")):
        description = load_json(path)
        try:
            if description["format"] != "btp-test-vector-v1":
                raise ValueError("unsupported vector description format")
            if description["id"] in invalid_ids:
                raise ValueError("duplicate vector id: " + description["id"])
            base_description, base_data = valid[description["base"]]
            data = apply_mutations(base_data, description["mutations"])
            actual_error = decode_error(data, base_description["transport"])
            if actual_error != description["expected_error"]:
                raise ValueError(
                    f"expected {description['expected_error']}, got {actual_error}")
            binary_path = path.with_suffix(".bin")
            if arguments.check:
                if not binary_path.exists() or binary_path.read_bytes() != data:
                    raise ValueError("checked-in .bin differs; run without --check")
            else:
                binary_path.write_bytes(data)
            invalid_ids.add(description["id"])
        except (KeyError, ValueError, struct.error) as error:
            failures.append(f"{path}: {error}")

    try:
        manifest = load_json(root / "manifest.json")
        if manifest["format"] != "btp-test-vector-manifest-v1":
            raise ValueError("unsupported vector manifest format")
        if set(manifest["valid_vectors"]) != set(valid):
            raise ValueError("manifest valid_vectors does not match valid/*.json")
        if set(manifest["invalid_vectors"]) != invalid_ids:
            raise ValueError("manifest invalid_vectors does not match invalid/*.json")
        for scenario in manifest["scenarios"]:
            unknown = set(scenario["arrival_order"]) - set(valid)
            if unknown:
                raise ValueError(
                    f"scenario {scenario['id']} references unknown vectors: " +
                    ", ".join(sorted(unknown)))
    except (KeyError, ValueError) as error:
        failures.append(f"{root / 'manifest.json'}: {error}")

    described_bins = {path.with_suffix(".bin")
                      for path in (root / "valid").glob("*.json")}
    described_bins.update(path.with_suffix(".bin")
                          for path in (root / "invalid").glob("*.json"))
    orphan_bins = set(root.glob("*/*.bin")) - described_bins
    if orphan_bins:
        failures.append("orphan .bin files: " +
                        ", ".join(str(path) for path in sorted(orphan_bins)))

    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    action = "verified" if arguments.check else "generated"
    print(f"BTP v1 vectors {action}: {len(described_bins)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
