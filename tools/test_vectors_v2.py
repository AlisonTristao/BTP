#!/usr/bin/env python3
"""Generate or verify the canonical BTP v2 conformance vectors.

This is a sibling of tools/test_vectors.py, not a wrapper around it: BTP v2
adds the ENCRYPTED flag and the version-2 octet (docs/encryption.md), and
this script's decode_error() is an independent reimplementation of the
current btp::decode() rules for that extension, exactly as tools/test_vectors.py
independently reimplements the v1 rules. Keeping the two scripts separate
means a bug in one reference decoder cannot silently hide in the other.

Scope: btp::codec only understands framing (magic, version, size, envelope
CRC, flags, fragmentation). It does not verify the AEAD authentication tag;
that is the job of a future consumer with mbedtls (or another AEAD library)
linked in. So decode_error() below can only return errors that btp::decode()
itself can detect. A "ciphertext with a corrupted tag" vector would be
structurally valid and is deliberately not modeled here -- see
test-vectors/v2/README.md.
"""

import argparse
import json
import struct
import sys
import zlib
from pathlib import Path


MAGIC = b"BTP\x00"
HEADER_SIZE = 36
CRC_SIZE = 4
V1_VERSION = 1
V2_VERSION = 2
FLAG_FRAGMENTED = 0x0001
FLAG_ENCRYPTED = 0x0002
# CIPHER_ID sub-field, bits 2-3 of flags (docs/frame.md section 3 and docs/encryption.md section 3):
# 0 == AES-128-GCM (default), 1 == ChaCha20-Poly1305, 2/3 reserved.
FLAG_CIPHER_ID_MASK = 0x000C
FLAG_CIPHER_ID_SHIFT = 2
KNOWN_FLAGS_MASK = FLAG_FRAGMENTED | FLAG_ENCRYPTED | FLAG_CIPHER_ID_MASK
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
    "usb_hid": (62, 22),
}


def integer(value):
    if isinstance(value, int):
        return value
    return int(value, 0)


def payload_bytes(frame):
    return bytes.fromhex(frame.get("payload_hex", ""))


def encode_frame(description):
    """Encodes frame bytes the way btp::encode()/write_header() do: the
    version octet is chosen automatically from the ENCRYPTED flag (version 2
    when set, version 1 otherwise), never taken as a free field. This is
    what makes a "no ENCRYPTED" v2 vector byte-identical to its v1
    counterpart, and what makes an ENCRYPTED vector carry version 2 without
    the JSON having to say so redundantly.
    """
    frame = description["frame"]
    payload = payload_bytes(frame)
    flags = integer(frame["flags"])
    version = V2_VERSION if (flags & FLAG_ENCRYPTED) else V1_VERSION
    header = bytearray()
    header.extend(MAGIC)
    header.extend(struct.pack("<BBHHH", version, TYPE_VALUES[frame["type"]],
                              flags, HEADER_SIZE, len(payload)))
    header.extend(struct.pack("<IIIQHBB",
                              integer(frame["source_id"]),
                              integer(frame["boot_id"]),
                              integer(frame["sequence"]),
                              integer(frame["timestamp_us"]),
                              integer(frame["object_id"]),
                              integer(frame["fragment_index"]),
                              integer(frame["fragment_count"])))
    if len(header) != HEADER_SIZE:
        raise ValueError("internal error: BTP header is not 36 bytes")
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
    """Independent reimplementation of btp::decode()'s current rules
    (src/codec.cpp), including the v2 ENCRYPTED/version-2 extension and the
    CIPHER_ID sub-field of flags. The check order below mirrors decode()
    exactly, which matters: the CRC check runs before the
    EncryptedVersionMismatch check, so a vector that mutates only the
    version octet must recompute the CRC to actually exercise
    EncryptedVersionMismatch instead of tripping CrcMismatch first; likewise
    the CIPHER_ID consistency check runs after the reserved-flag-bits check
    (InvalidFlags), mirroring validate_header()'s own order.
    """
    max_frame, max_payload = TRANSPORT_LIMITS[transport]
    if len(data) < HEADER_SIZE + CRC_SIZE:
        return "FrameTooShort"
    if len(data) > max_frame:
        return "FrameTooLarge"
    if data[:4] != MAGIC:
        return "InvalidMagic"
    version = data[4]
    if version not in (V1_VERSION, V2_VERSION):
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

    # docs/frame.md section 2.1: ENCRYPTED marked MUST imply version 2.
    # No inverse requirement: version 2 with ENCRYPTED clear is valid.
    if (flags & FLAG_ENCRYPTED) and version != V2_VERSION:
        return "EncryptedVersionMismatch"

    if not 1 <= message_type <= 5:
        return "InvalidType"
    if flags & ~KNOWN_FLAGS_MASK:
        return "InvalidFlags"

    # docs/encryption.md section 3: with ENCRYPTED clear there is no cipher "in
    # use", so CIPHER_ID must be 0; with ENCRYPTED set, CIPHER_ID must be 0
    # or 1 (the only assigned values) -- 2 and 3 are reserved and rejected,
    # the same principle already applied to reserved flag bits above.
    encrypted = bool(flags & FLAG_ENCRYPTED)
    raw_cipher_id = (flags & FLAG_CIPHER_ID_MASK) >> FLAG_CIPHER_ID_SHIFT
    if not encrypted and raw_cipher_id != 0:
        return "InvalidCipherId"
    if encrypted and raw_cipher_id > 1:
        return "InvalidCipherId"

    # docs/encryption.md section 9: a 16-octet tag over the usb_hid payload ceiling of 22
    # octets is 73% overhead, so ENCRYPTED is refused on that profile
    # outright. btp::decode() enforces this; so must the reference decoder.
    if encrypted and transport == "usb_hid":
        return "EncryptedNotAllowedOnTransport"

    if source_id == 0:
        return "InvalidSourceId"
    if boot_id == 0:
        return "InvalidBootId"
    fragmented = flags & FLAG_FRAGMENTED
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
            if description["format"] != "btp-test-vector-v2":
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
            if description["format"] != "btp-test-vector-v2":
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
        if manifest["format"] != "btp-test-vector-manifest-v2":
            raise ValueError("unsupported vector manifest format")
        if set(manifest["valid_vectors"]) != set(valid):
            raise ValueError("manifest valid_vectors does not match valid/*.json")
        if set(manifest["invalid_vectors"]) != invalid_ids:
            raise ValueError("manifest invalid_vectors does not match invalid/*.json")
        for scenario in manifest.get("scenarios", []):
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
    print(f"BTP v2 vectors {action}: {len(described_bins)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
