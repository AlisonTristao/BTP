# Session and terminal

A BTP session defines the protocol state shared by two peers while they communicate.

The session establishes:

* which BTP envelope version will be used;
* the effective communication limits;
* the session inactivity timeout;
* the identity and role of each peer;
* the configuration revision announced by each peer.

The session begins with `HELLO` / `HELLO_RESULT` and ends through `SESSION_CLOSE`, timeout, or loss of the underlying transport.

The terminal channel operates inside an established session but remains logically separate from telemetry, commands and control traffic.

---

## 1. `HELLO`

`HELLO` is the first BTP message used to establish a session.

No application message may be exchanged before a successful `HELLO_RESULT`.

The logical payload is:

| Offset | Size | Field                       | Wire type                 |
| -----: | ---: | --------------------------- | ------------------------- |
|      0 |    1 | `role`                      | `uint8`                   |
|      1 |    1 | `version_count`             | `uint8`                   |
|      2 |    2 | `flags`                     | `uint16_le`, zero         |
|      4 |    4 | `max_logical_payload`       | `uint32_le`               |
|      8 |    2 | `max_inflight_reassemblies` | `uint16_le`               |
|     10 |    2 | `max_subscriptions`         | `uint16_le`               |
|     12 |    4 | `max_dedup_entries`         | `uint32_le`               |
|     16 |    4 | `session_timeout_ms`        | `uint32_le`               |
|     20 |   16 | `peer_uuid`                 | 16 octets                 |
|     36 |    4 | `config_revision`           | `uint32_le`               |
|     40 |    V | `versions`                  | `version_count` × `uint8` |

The message announces the capabilities of the sender.

Conceptually:

```text
Peer A                                  Peer B
   |                                      |
   |--------------- HELLO -------------->|
   |                                      |
   |       versions                       |
   |       limits                         |
   |       role                           |
   |       UUID                           |
   |       config revision                |
   |                                      |
```

The receiver uses these values to determine whether a compatible session can be established.

### 1.1 `peer_uuid`

`peer_uuid` is a stable opaque identity containing exactly:

```text
16 octets
```

It is compared byte for byte.

No byte-order conversion is applied.

The all-zero UUID is invalid.

BTP does not assign additional structure to these 16 octets.

---

### 1.2 `config_revision`

`config_revision` identifies the currently announced configuration of the peer.

A value of:

```text
0
```

means that the peer does not publish a manifest.

When a peer publishes a manifest, the revision starts at:

```text
1
```

and increases monotonically when the described configuration changes.

The manifest itself is defined in [Commands and discovery](commands.md#3-the-manifest).

---

### 1.3 Announced limits

The following capabilities must all be non-zero:

```text
max_logical_payload
max_inflight_reassemblies
max_subscriptions
max_dedup_entries
session_timeout_ms
```

They describe the maximum values supported by the sender.

For example:

```text
Peer A

max_logical_payload        = 16384
max_inflight_reassemblies  = 4
max_subscriptions          = 16
max_dedup_entries          = 32
session_timeout_ms         = 10000
```

does not require the other peer to support those same maximums.

The effective session values are determined by `HELLO_RESULT`.

On a communication path containing an ESP-NOW hop:

```text
max_logical_payload <= 53550
```

because of the transport limits defined in [Getting it across the link](fragmentation-and-transports.md).

---

### 1.4 Supported versions

`versions` contains exactly:

```text
version_count
```

BTP envelope version values.

They appear:

* in increasing order;
* without duplicates;
* without zero.

The currently defined envelope versions are:

```text
0x01
0x02
```

For example:

```text
version_count = 2

versions:
    0x01
    0x02
```

announces support for both versions.

Version negotiation is completed by `HELLO_RESULT`.

---

### 1.5 Roles

The `role` field uses the following values:

|  Value | Role                      |
| -----: | ------------------------- |
| `0x01` | producer                  |
| `0x02` | gateway                   |
| `0x03` | consumer                  |
| `0x04` | consumer, diagnostic tool |

The following values are reserved:

```text
0x00
0x05 .. 0xFF
```

The role transmitted on the wire is the numeric value.

Both:

```text
0x03
```

and:

```text
0x04
```

represent consumers from the protocol perspective.

BTP defines no different message behavior for these two roles.

The distinction allows the peer to identify that one consumer announced itself specifically as a diagnostic tool.

---

## 2. `HELLO_RESULT`

`HELLO_RESULT` completes session negotiation.

Its logical payload is:

| Offset | Size | Field                       | Wire type                                                   |
| -----: | ---: | --------------------------- | ----------------------------------------------------------- |
|      0 |   12 | request reference           | [Commands and discovery](commands.md#13-request-references) |
|     12 |    1 | `status`                    | `SUCCESS` or `UNSUPPORTED`                                  |
|     13 |    1 | `selected_version`          | `uint8`                                                     |
|     14 |    2 | `error_code`                | `uint16_le`                                                 |
|     16 |    4 | `max_logical_payload`       | effective value                                             |
|     20 |    2 | `max_inflight_reassemblies` | effective value                                             |
|     22 |    2 | `max_subscriptions`         | effective value                                             |
|     24 |    4 | `max_dedup_entries`         | effective value                                             |
|     28 |    4 | `session_timeout_ms`        | effective value                                             |
|     32 |   16 | `peer_uuid`                 | responder UUID                                              |
|     48 |    4 | `config_revision`           | responder revision                                          |

A successful exchange is:

```text
Peer A                                  Peer B
   |                                      |
   |--------------- HELLO -------------->|
   |                                      |
   |<----------- HELLO_RESULT ------------|
   |                                      |
   |        session established           |
```

---

### 2.1 Version selection

The responder selects the highest mutually supported envelope version that can be used across the complete session path.

For example:

```text
Peer A supports:

0x01
0x02


Peer B supports:

0x01


selected_version:

0x01
```

A gateway must also respect the versions supported by every link participating in the session.

A version cannot be selected if another required hop cannot carry it.

---

### 2.2 Effective limits

Every negotiated session limit is the minimum of the capabilities available to the peers and the communication path.

Conceptually:

```text
Peer A maximum
      |
      v

Peer B maximum
      |
      v

path maximum
      |
      v

effective session limit
```

For example:

```text
Peer A max_logical_payload = 32768

Peer B max_logical_payload = 8192
```

produces:

```text
effective max_logical_payload = 8192
```

The larger peer cannot force the smaller peer to accept a value beyond its announced capability.

This rule applies to:

```text
max_logical_payload
max_inflight_reassemblies
max_subscriptions
max_dedup_entries
session_timeout_ms
```

On success, all effective limits are non-zero.

---

### 2.3 No compatible version

If the peers have no supported version in common:

```text
status           = UNSUPPORTED
error_code       = UNSUPPORTED_VERSION
selected_version = 0
```

All negotiated limits are zero.

After transmitting the result, the session is closed.

No application traffic follows an unsuccessful `HELLO_RESULT`.

---

## 3. Entering protocol mode on serial

Serial has an additional state because the same physical port may operate as a human-readable console before BTP begins.

The serial port starts in:

```text
console mode
```

BTP binary framing does not begin automatically when frame-like bytes are observed.

The transition to protocol mode uses an explicit text exchange.

The client sends:

```text
BTP/1 ENTER NNNNNNNNNNNNNNNN\r\n
```

where:

```text
NNNNNNNNNNNNNNNN
```

contains exactly 16 hexadecimal digits chosen by the client.

Uppercase and lowercase hexadecimal digits are accepted.

For example:

```text
BTP/1 ENTER A10F22CC8899ABCD\r\n
```

The port owner responds using the same nonce converted to lowercase:

```text
BTP/1 READY a10f22cc8899abcd\r\n
```

The sequence is:

```text
Client                                Port owner
   |                                      |
   | BTP/1 ENTER <nonce>\r\n ------------>|
   |                                      |
   |<---- BTP/1 READY <nonce>\r\n --------|
   |                                      |
   |          protocol mode               |
```

Binary BTP framing begins only after the final:

```text
\n
```

of the `READY` response.

At that moment, the serial COBS decoder state is cleared.

The client must then transmit `HELLO` within:

```text
2000 ms
```

No other BTP message may precede it.

---

### 3.1 Invalid entry lines

A line that does not exactly match the `ENTER` syntax remains ordinary console input.

For example:

```text
BTP ENTER
```

or an incomplete:

```text
BTP/1 ENTER 1234
```

does not switch the port into protocol mode.

BTP frames are never automatically detected inside console traffic.

The state transition is explicit:

```text
console
   |
   | valid ENTER / READY
   v
BTP protocol
```

not:

```text
console bytes
   |
   | "looks like a BTP frame"
   v
BTP protocol
```

This keeps console data and protocol framing unambiguous.

---

### 3.2 Other transports

ESP-NOW and USB HID do not define the serial console state.

They operate directly in protocol mode.

Once the link becomes available, they must be ready to begin the BTP session with `HELLO`.

Conceptually:

```text
Serial

console
   |
ENTER / READY
   |
   v
HELLO
   |
   v
session


ESP-NOW / USB HID

link available
   |
   v
HELLO
   |
   v
session
```

---

## 4. Leaving protocol mode

An established session is closed using BTP messages.

The protocol does not scan binary data for a special escape sequence.

A normal close uses:

```text
SESSION_CLOSE
        |
        v
SESSION_CLOSE_RESULT
```

---

### 4.1 `SESSION_CLOSE`

The logical payload contains:

| Offset | Size | Field              | Wire type   |
| -----: | ---: | ------------------ | ----------- |
|      0 |    1 | `reason`           | `uint8`     |
|      1 |    3 | `reserved`         | zero        |
|      4 |    4 | `drain_timeout_ms` | `uint32_le` |

The defined close reasons are:

|  Value | Name               |
| -----: | ------------------ |
| `0x00` | `NORMAL`           |
| `0x01` | `VERSION_MISMATCH` |
| `0x02` | `CLIENT_SHUTDOWN`  |
| `0x03` | `PROTOCOL_ERROR`   |

Unassigned values are reserved.

`drain_timeout_ms` defines the requested time available to complete the close exchange.

---

### 4.2 `SESSION_CLOSE_RESULT`

The result contains:

```text
request reference
status:uint8
reserved:uint8
error_code:uint16_le
```

For a valid close, the port owner:

1. stops accepting new work;
2. transmits the close result within the allowed drain interval;
3. discards incomplete reassemblies;
4. terminates protocol mode.

The maximum drain interval is:

```text
min(drain_timeout_ms, 2000 ms)
```

For serial, after cleanup completes, the port owner emits exactly:

```text
BTP/1 CONSOLE\r\n
```

The serial state becomes:

```text
BTP protocol
      |
      | SESSION_CLOSE
      v
drain
      |
      v
SESSION_CLOSE_RESULT
      |
      v
BTP/1 CONSOLE\r\n
      |
      v
console
```

The console transition happens only after the BTP session has been terminated.

---

## 5. The watchdog

Every established session has an inactivity watchdog.

Its timeout is:

```text
session_timeout_ms
```

as negotiated during the `HELLO` exchange.

The watchdog is renewed when a **valid BTP frame** is received.

If no valid frame arrives before the timeout expires, the session is terminated.

For serial, the same cleanup used by a normal session close is performed before returning to console mode.

---

### 5.1 Initial serial timeout

After the serial `READY` response, `HELLO` must arrive within:

```text
2000 ms
```

If it does not, protocol mode is abandoned.

Conceptually:

```text
READY
  |
  v
wait for HELLO
  |
  +---- valid HELLO within 2000 ms ---> session
  |
  `---- timeout ----------------------> console
```

---

### 5.2 Invalid traffic

Invalid traffic does not renew the watchdog.

Examples include:

* malformed frames;
* invalid CRC;
* invalid envelope;
* other traffic that does not constitute a valid BTP frame.

Therefore:

```text
valid frame
     |
     v
watchdog renewed
```

while:

```text
invalid bytes
     |
     v
watchdog unchanged
```

A continuous stream of invalid data cannot keep a BTP session active indefinitely.

---

### 5.3 Session loss and command deduplication

Ending a session or losing the underlying transport does not clear command deduplication state.

Command deduplication is scoped to the executor boot, not to one session.

Therefore:

```text
session lost
      |
      v
new session
      |
      v
same executor boot
      |
      v
previous command identities
remain protected
```

The deduplication rules are defined in [Commands and discovery](commands.md#25-command-deduplication).

A session reconnect does not authorize execution of an already processed command a second time.

---

## 6. Timeouts

BTP defines bounded lifetimes for operations that cannot remain incomplete indefinitely.

These include:

* fragmented-message reassembly;
* session inactivity;
* command execution.

The applicable timeout depends on the operation.

---

### 6.1 Reassembly timeout

Fragmented logical messages use the reassembly rules and timeout defined in [Getting it across the link](fragmentation-and-transports.md).

An incomplete logical message is not retained indefinitely.

When its reassembly timeout expires, the incomplete message is discarded.

---

### 6.2 Session timeout

Session inactivity uses:

```text
session_timeout_ms
```

negotiated through `HELLO_RESULT`.

Expiration terminates the session as described in [The watchdog](#5-the-watchdog).

---

### 6.3 Command execution timeout

Each synchronous action may announce:

```text
execution_timeout_ms
```

in its manifest descriptor.

When execution exceeds this limit, the command completes with:

```text
TIMEOUT
EXECUTION_TIMEOUT
```

After that result, the action must not continue producing effects as part of that command execution.

The timeout belongs to the action descriptor and is therefore known before the corresponding `COMMAND_REQUEST` is executed.

---

## 7. The terminal

BTP defines two terminal message objects:

```text
TERMINAL_IN
TERMINAL_OUT
```

They provide a bidirectional opaque byte channel inside the BTP session.

The complete logical payload is the terminal data itself.

There is:

* no terminal-specific header;
* no text length prefix;
* no terminator;
* no encoding requirement.

The payload boundary is determined only by the BTP logical message size.

Conceptually:

```text
BTP envelope
+---------------------------------------+
| type = TERMINAL                       |
| object_id = TERMINAL_IN / OUT         |
+---------------------------------------+
|                                       |
| opaque terminal bytes                 |
|                                       |
+---------------------------------------+
```

---

### 7.1 Direction

`TERMINAL_IN` flows toward the source terminal input:

```text
session controller
       |
       | TERMINAL_IN
       v
source terminal
```

`TERMINAL_OUT` flows in the opposite direction:

```text
source terminal
       |
       | TERMINAL_OUT
       v
session controller
```

The two object IDs therefore identify the direction of the terminal stream.

---

### 7.2 Opaque payload

Terminal data is not required to be UTF-8.

All byte values are valid, including:

```text
0x00
CR
LF
```

and byte sequences that happen to resemble encoded BTP data.

For example, all of these are valid terminal payloads:

```text
hello\r\n
```

```text
00 01 02 ff 7e
```

or:

```text
invalid UTF-8 bytes
```

BTP does not inspect or reinterpret the contents.

The terminal layer carries bytes.

---

### 7.3 Fragmentation

A terminal logical message may use normal BTP fragmentation when required.

Fragments are not exposed individually as terminal data.

The receiver first performs:

```text
fragment 0
fragment 1
fragment 2
     |
     v
reassembly
     |
     v
complete TERMINAL payload
     |
     v
terminal delivery
```

An incomplete fragmented terminal message is not partially delivered.

---

### 7.4 Sequence ordering

Complete terminal messages from the same origin use the normal BTP envelope `sequence`.

This sequence may assist in ordering received terminal messages.

However, BTP sequence numbers are shared by all message types from the same source.

For example:

```text
sequence 100 -> TERMINAL_OUT
sequence 101 -> TELEMETRY
sequence 102 -> COMMAND_RESULT
sequence 103 -> TERMINAL_OUT
```

From the terminal perspective, receiving:

```text
100
103
```

does not mean that terminal messages:

```text
101
102
```

were lost.

Those sequence values may belong to other BTP channels.

Therefore, a terminal receiver must not fabricate missing terminal bytes based only on sequence gaps.

No padding or replacement data is generated.

---

### 7.5 Separation from telemetry

Terminal and telemetry are different BTP message types.

Terminal bytes remain terminal bytes.

Telemetry payloads remain telemetry payloads.

There is no implicit conversion between them.

For example:

```text
TERMINAL_OUT
    |
    v
terminal channel
```

and:

```text
TELEMETRY
    |
    v
telemetry decoder
```

follow separate logical paths.

A `TELEMETRY` message is not terminal output.

A `TERMINAL_OUT` message is not a telemetry sample.

This distinction remains true even if both happen to contain human-readable text.

---

## 8. Priority

BTP defines a logical priority order for outgoing traffic under congestion.

Messages are grouped into six classes.

Within one class, messages use FIFO order.

The priority classes are:

| Priority | Traffic                                           |
| -------: | ------------------------------------------------- |
|        1 | Session messages and `COMMAND_RESULT`             |
|        2 | `COMMAND_REQUEST` and control results             |
|        3 | Subscription control and `STATUS` with `DEGRADED` |
|        4 | `TERMINAL_IN` and `TERMINAL_OUT`                  |
|        5 | `MANIFEST_DATA`, periodic `STATUS` and `LOG`      |
|        6 | `TELEMETRY`                                       |

Priority 1 is the highest.

Priority 6 is the lowest.

Conceptually:

```text
highest priority

1  session / COMMAND_RESULT
|
2  COMMAND_REQUEST / control results
|
3  subscriptions / DEGRADED status
|
4  terminal
|
5  manifest / normal status / log
|
6  telemetry

lowest priority
```

This order allows control and completion traffic to remain serviceable when the communication channel is congested.

---

### 8.1 FIFO within a class

Priority does not reorder messages within the same class.

For example:

```text
TERMINAL_OUT A
TERMINAL_OUT B
TERMINAL_OUT C
```

remain:

```text
A
B
C
```

inside the terminal priority class.

---

### 8.2 Sequence values

Scheduling priority does not modify envelope sequence numbers.

A message retains the sequence assigned when it was created.

The scheduler does not renumber messages according to the order in which they eventually reach the transport.

---

### 8.3 Frames already in flight

Priority does not interrupt a frame that has already begun transmission.

A higher-priority message may be selected before the next message or eligible fragment, but it does not modify an already transmitted partial frame.

---

### 8.4 Reliability

Priority does not change the reliability properties of the underlying transport.

A best-effort transport remains best-effort.

For example:

```text
high priority
```

does not imply:

```text
guaranteed delivery
```

The priority mechanism determines which traffic is serviced first when capacity is limited.

It does not provide acknowledgement or retransmission by itself.

---

### 8.5 Congestion behavior

A sender reserves enough capacity to service at least one message from priority classes:

```text
1
2
```

Under pressure, telemetry is the first defined traffic class to be dropped.

Dropped telemetry contributes to the corresponding protocol counters.

Large lower-priority traffic, including:

```text
MANIFEST_DATA
TERMINAL
```

must be scheduled so that it does not indefinitely block higher-priority command traffic.

Conceptually:

```text
large MANIFEST_DATA
fragment 0
fragment 1
fragment 2
fragment 3
fragment 4

        command arrives

fragment 0
fragment 1
COMMAND_RESULT
fragment 2
fragment 3
fragment 4
```

The exact transport scheduling mechanism is implementation-dependent, but it must preserve the BTP priority rules.

---

## 9. Session lifecycle

The complete session lifecycle can be represented as:

```text
                 link available
                       |
                       v
              +------------------+
              | pre-session      |
              +------------------+
                       |
                       | HELLO
                       v
              +------------------+
              | negotiation      |
              +------------------+
                       |
                       | HELLO_RESULT
                       | SUCCESS
                       v
              +------------------+
              | active session   |
              +------------------+
                 |            |
                 |            |
     SESSION_CLOSE            | timeout /
                 |            | transport loss
                 v            v
              +------------------+
              | cleanup          |
              +------------------+
                       |
                       v
                 session ended
```

For serial, an additional console transition surrounds the BTP session:

```text
console
   |
   | ENTER / READY
   v
pre-session
   |
   | HELLO / HELLO_RESULT
   v
active session
   |
   | close or timeout
   v
cleanup
   |
   | BTP/1 CONSOLE
   v
console
```

For ESP-NOW and USB HID, there is no console state:

```text
link available
      |
      v
HELLO
      |
      v
session
```

---

## 10. Summary

A BTP session begins by negotiating a common protocol version and effective communication limits.

The basic exchange is:

```text
HELLO
   |
   v
HELLO_RESULT
```

A successful result establishes:

```text
selected envelope version

max logical payload

max inflight reassemblies

max subscriptions

max dedup entries

session timeout

peer identities

configuration revisions
```

Serial additionally uses an explicit:

```text
ENTER
  |
  v
READY
```

exchange before binary protocol mode begins.

Protocol mode ends through:

```text
SESSION_CLOSE
       |
       v
SESSION_CLOSE_RESULT
```

or through a timeout or transport failure.

A valid frame renews the session watchdog.

Invalid traffic does not.

The terminal channel carries opaque bytes through:

```text
TERMINAL_IN
TERMINAL_OUT
```

and remains separate from telemetry and other BTP message classes.

Under congestion, BTP uses six logical priority classes so that session and command traffic are serviced before lower-priority data such as telemetry.

These rules define the lifecycle and multiplexing behavior of a BTP session without changing the framing, fragmentation or message identities defined by the rest of the protocol.
