# Wire v1 canonical vectors

Each `.json` describes the frame that the `.bin` of the same name contains.
`manifest.json` lists which vectors exist, split into `valid/` and `invalid/`.

A `.bin` holds the raw frame — header, payload and CRC — with no COBS wrapping
and no transport framing around it.

An entry under `invalid/` carries the exact `expected_error` a decoder must
produce. Rejecting it is not enough; it has to be rejected for that reason.
Some also set `recompute_crc`, because the envelope CRC is checked before the
semantic rules: without recomputing it, a mutated version octet would trip
`CrcMismatch` and never reach the rule the vector is meant to exercise.

Verify against the checked-in bytes, or regenerate them from the descriptions
by dropping `--check`:

```bash
python tools/test_vectors.py --root test-vectors/v1 --check
```

`tools/test_vectors.py` is an independent reimplementation of the decode rules,
not a wrapper around the C++ library, so a bug in one cannot hide inside the
other.

Changing a vector is changing the contract, not editing a test. See
[Using the library](../../docs/library.md).
