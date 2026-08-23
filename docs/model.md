# Protocol model

This chapter defines the BTP communication model and the terminology used throughout the specification.

It defines:

* communication roles;
* logical messages and frames;
* traffic classes;
* message identity;
* timestamp semantics;
* delivery behavior;
* traffic priority;
* validation and rejection.

## 1. Communication roles

BTP communication occurs between endpoints.

An endpoint may:

* produce messages;
* consume messages;
* or perform both functions.

The transport used between endpoints is independent from the BTP message model.

### 1.1 Producer

A producer creates BTP logical messages.

For each message, the producer defines:

* `source_id`;
* `boot_id`;
* `sequence`;
* `timestamp_us`;
* `type`;
* `object_id`;
* payload.

`source_id` identifies the producer.

`boot_id` identifies the current execution of that producer.

`sequence` identifies a logical message within that execution.

`timestamp_us` represents when the event associated with the message occurred.

These values define the origin and identity of the logical message.

### 1.2 Consumer

A consumer receives and processes BTP messages.

Depending on the message type, it may:

* decode frames;
* reassemble fragmented messages;
* process telemetry;
* process logs;
* send or receive commands;
* exchange terminal data;
* process protocol control messages.

A consumer defines its own resource limits, such as:

* receive queue capacity;
* reassembly capacity;
* application buffers.

These implementation limits do not change the BTP wire format.

---

## 2. Logical messages and frames

BTP distinguishes between a **logical message** and a **frame**.

A logical message represents one application-level operation or data unit.

It contains:

```text
identity
timestamp
type
object_id
payload
```

A frame is the encoded unit transmitted through the communication channel.

If a logical message fits within the frame capacity supported by the transport, it is transmitted as one frame.

```text
Logical message
      |
      v
+-------------+
|    Frame    |
+-------------+
```

If the logical message is larger than the available frame capacity, it is divided into multiple fragments.

```text
Logical message
      |
      +----------+----------+
      |          |          |
      v          v          v
+---------+  +---------+  +---------+
|Fragment |  |Fragment |  |Fragment |
|    0    |  |    1    |  |    2    |
+---------+  +---------+  +---------+
```

All fragments belonging to the same logical message share the same message identity.

Fragmentation changes how a logical message is transmitted. It does not create additional logical messages.

The following terminology is used throughout the specification:

| Term                | Meaning                                                    |
| ------------------- | ---------------------------------------------------------- |
| **Logical message** | Complete application-level BTP message                     |
| **Frame**           | Encoded unit transmitted through the communication channel |
| **Fragment**        | Frame containing part of a fragmented logical message      |
| **Payload**         | Application data carried by the logical message            |

---

## 3. Traffic classes

BTP allows different types of traffic to share the same communication channel.

The `type` field identifies the class of each message.

BTP defines five traffic classes:

| `type` | Class       | Direction           | Purpose                      |
| ------ | ----------- | ------------------- | ---------------------------- |
| `0x01` | `TELEMETRY` | producer → consumer | Measurement data             |
| `0x02` | `LOG`       | producer → consumer | Events and diagnostics       |
| `0x03` | `COMMAND`   | bidirectional       | Command requests and results |
| `0x04` | `TERMINAL`  | bidirectional       | Interactive byte data        |
| `0x05` | `CONTROL`   | bidirectional       | Protocol management          |

`0x00` is `INVALID`.

Unassigned values are reserved.

### 3.1 TELEMETRY

`TELEMETRY` carries measurement data.

Telemetry is generally continuous and may operate at medium or high message rates.

Its payload contains compact binary values associated with a known telemetry schema.

Telemetry delivery is best-effort.

### 3.2 LOG

`LOG` carries diagnostic and event information.

Logs are independent from telemetry and are also delivered on a best-effort basis.

### 3.3 COMMAND

`COMMAND` provides request/result communication.

A command transaction consists of:

```text
COMMAND_REQUEST
       |
       v
 command execution
       |
       v
 COMMAND_RESULT
```

The request and result model allows the requester to determine whether an operation was completed.

Command retries preserve the identity of the original request so that repeated transmission does not necessarily cause repeated execution.

### 3.4 TERMINAL

`TERMINAL` carries interactive byte data in both directions.

The payload is opaque to the BTP protocol.

BTP does not interpret:

* commands;
* shell syntax;
* text encoding;
* terminal application semantics.

It only identifies and transports terminal data.

### 3.5 CONTROL

`CONTROL` carries protocol management operations.

These may include:

* session management;
* manifest requests;
* manifest data;
* subscriptions;
* status information;
* protocol control results.

### 3.6 `object_id`

The meaning of `object_id` depends on `type`.

It may identify, for example:

* a telemetry topic;
* a command;
* a terminal operation;
* a control operation.

Each traffic class defines its own `object_id` namespace.

The same numeric value may therefore have different meanings for different message types.

---

## 4. Message identity

Every logical BTP message is identified by:

```text
(source_id, boot_id, sequence)
```

The complete tuple identifies one logical message.

### 4.1 `source_id`

`source_id` identifies the producer.

It must be non-zero.

It is a protocol identity and is independent from addresses used by the underlying transport.

How `source_id` values are assigned is outside the BTP wire protocol.

### 4.2 `boot_id`

`boot_id` identifies one execution of a producer.

It must be non-zero and changes after every restart.

For example:

```text
boot_id = A

sequence:
0
1
2
3

    restart

boot_id = B

sequence:
0
1
2
3
```

This prevents a sequence value generated after a restart from being confused with a message generated during a previous execution.

### 4.3 `sequence`

`sequence` identifies a logical message within one `boot_id`.

All fragments of the same logical message use the same `sequence`.

```text
Logical message
sequence = 105

fragment 0 -> sequence = 105
fragment 1 -> sequence = 105
fragment 2 -> sequence = 105
```

The sequence space is shared by all message types generated by the same producer.

For example:

```text
sequence 100 -> TELEMETRY
sequence 101 -> TELEMETRY
sequence 102 -> LOG
sequence 103 -> COMMAND
sequence 104 -> TELEMETRY
```

A consumer observing only telemetry may therefore receive:

```text
100
101
104
```

This does not necessarily indicate telemetry loss.

`sequence` must not wrap within the same `boot_id`.

If its range is exhausted, a new `boot_id` must be used before additional messages are generated.

---

## 5. Identity and transport addressing

BTP identity is independent from transport addressing.

The BTP protocol does not define:

* MAC addresses;
* IP addresses;
* serial ports;
* USB endpoints;
* routing identifiers.

These belong to the underlying communication system.

A transport address determines where data is sent.

The tuple:

```text
(source_id, boot_id, sequence)
```

identifies the BTP logical message.

These mechanisms serve different purposes and must not be treated as equivalent.

---

## 6. Time

Each logical message contains `timestamp_us`.

`timestamp_us` is a 64-bit value expressed in microseconds from the producer's monotonic clock.

It represents the time associated with the event carried by the message.

For telemetry, this normally corresponds to the acquisition time.

The timestamp is assigned by the producer rather than by the consumer.

```text
event
  |
  | timestamp_us
  v
Producer
  |
  | communication delay
  v
Consumer
```

This prevents transmission delay, buffering, and receiver scheduling from changing the recorded event time.

`timestamp_us` is not wall-clock time.

Values from different producer executions are not inherently comparable.

Values from different producers are also not inherently comparable unless the system provides an external time synchronization mechanism.

BTP defines the source timestamp representation. Time synchronization is outside its scope.

---

## 7. Delivery model

BTP distinguishes transport delivery from application completion.

Sending a frame successfully does not necessarily mean that the corresponding application operation has completed.

### 7.1 Telemetry

Telemetry is best-effort.

BTP does not retransmit individual telemetry samples.

If the producer generates telemetry faster than the communication system can transmit it, samples may be dropped according to the implementation queue policy.

### 7.2 Logs

Logs are best-effort.

No protocol-level retransmission is required for individual log messages.

### 7.3 Terminal

Terminal data is interactive and bidirectional.

BTP does not define an acknowledgement for each terminal payload.

### 7.4 Commands

Commands use an explicit request/result model.

A command operation is completed when the requester receives the corresponding `COMMAND_RESULT`.

Transport-level delivery does not indicate command completion.

When retrying the same command, the requester preserves the identity and payload of the original logical request.

This allows the receiver to identify duplicate requests and avoid executing the same operation multiple times.

A request using a new `sequence` represents a new command operation.

---

## 8. Traffic priority

Different traffic classes have different latency requirements.

High-rate telemetry must not prevent control or command traffic from being processed.

BTP therefore allows traffic to be scheduled according to priority.

The defined priority order is:

| Priority | Traffic                                  |
| -------: | ---------------------------------------- |
|        1 | Session messages and `COMMAND_RESULT`    |
|        2 | `COMMAND_REQUEST` and control results    |
|        3 | Subscription control and degraded status |
|        4 | Terminal traffic                         |
|        5 | Manifest data, periodic status, and logs |
|        6 | Telemetry                                |

Lower numeric values represent higher priority.

FIFO ordering is preserved within the same priority level.

Priority affects transmission scheduling only.

It does not change:

* message identity;
* message type;
* payload;
* delivery guarantees.

When communication capacity is insufficient, lower-priority traffic may be discarded before higher-priority traffic.

Telemetry is therefore generally the first traffic class affected by sustained congestion.

---

## 9. Validation and rejection

BTP uses deterministic validation.

A receiver either accepts a frame according to the protocol rules or rejects it.

Malformed data is not reinterpreted heuristically.

### 9.1 Structural validation

A frame is rejected when its encoded representation violates protocol rules.

Examples include:

* invalid frame length;
* invalid CRC;
* invalid authentication tag;
* invalid field combinations;
* invalid fragmentation information;
* non-zero reserved fields;
* unassigned values.

Rejected frames are discarded before application processing.

### 9.2 Application payload validation

A structurally valid BTP message may still contain application data that the consumer cannot interpret.

For example:

```text
BTP frame      -> valid
Telemetry data -> unknown schema
```

In this case the BTP frame itself is valid.

The application may discard the payload or request the information required to interpret it, such as the corresponding manifest.

### 9.3 Reserved values

Reserved fields and bits must contain their defined reserved values.

Unless specified otherwise, reserved fields are zero.

Unsupported or unassigned protocol values are rejected rather than interpreted automatically.

### 9.4 No implicit compatibility mode

BTP does not automatically attempt alternative wire formats when a frame is incompatible.

There is no implicit:

* legacy decoder;
* fallback serialization;
* format autodetection;
* alternative field interpretation.

Compatibility between protocol versions must be explicitly defined.

---

## 10. Model summary

BTP communication occurs between endpoints.

An endpoint may produce messages, consume messages, or perform both roles.

Every logical message contains:

```text
(source_id, boot_id, sequence)
timestamp_us
type
object_id
payload
```

The `type` field separates:

* telemetry;
* logs;
* commands;
* terminal data;
* protocol control.

A logical message may be transmitted as one frame or divided into multiple fragments when required by the communication channel.

Message identity is independent from transport addressing.

Timestamps are assigned by the producer and represent the time of the associated event rather than the time of reception.

Telemetry and logs use best-effort delivery.

Commands use explicit request/result completion.

Traffic classes can be prioritized so that continuous telemetry does not prevent command, control, or interactive communication.

Invalid frames are rejected deterministically before application processing.

These rules define the common message model used by the remaining BTP specification.
