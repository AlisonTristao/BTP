# Why BTP exists

This chapter describes the problems addressed by BTP, its design goals, and the resulting trade-offs. It is non-normative.

## 1. Problem

Embedded telemetry devices often use a single communication channel for different types of traffic.

A typical device may need to:

* transmit telemetry continuously at medium or high rates;
* receive commands from a server;
* return command results;
* provide an interactive terminal;
* transmit terminal output;
* exchange configuration and status information.

All of this traffic may share the same physical or logical channel.

Without a common communication model, each traffic class tends to require its own framing, serialization, identification, and parsing rules. This increases complexity on both the device and the receiving application.

BTP provides a common binary framing and message model for these different types of data.

### 1.1 Multiple traffic classes on one channel

Telemetry, commands, and terminal traffic have different characteristics.

Telemetry is generally continuous and may generate messages at medium or high rates.

Commands are less frequent, but usually require identification, correlation, and a corresponding result.

Terminal traffic is interactive and may contain arbitrary byte sequences in both directions.

When these traffic classes share the same channel, the receiver must be able to determine which subsystem should process each incoming message.

A receiver should not need to inspect arbitrary payload contents to determine whether incoming data represents:

* a telemetry sample;
* a command request;
* a command result;
* terminal data;
* protocol control information.

BTP assigns an explicit type to each message. Different traffic classes can therefore share the same channel while remaining independently identifiable.

### 1.2 Message boundaries

A communication channel does not necessarily preserve application message boundaries.

Depending on the transport, one read operation may contain:

* part of a message;
* exactly one message;
* multiple messages.

The protocol must therefore define how messages are identified and separated independently of the application payload.

BTP uses explicit binary framing. Each frame contains the information required to determine its structure and payload length.

### 1.3 Limited channel capacity

Embedded communication channels often have limited bandwidth or restrict the amount of data that can be transmitted in a single frame.

This makes representation overhead relevant, especially when telemetry is transmitted continuously.

A textual representation such as JSON is convenient for development and integration, but carries additional information on every message.

For example:

```json
{
  "temperature": 42.5,
  "pressure": 101.2,
  "speed": 1500
}
```

The field names, quotation marks, separators, and textual number representations are transmitted every time the message is sent.

For low-rate communication this overhead may be acceptable. For continuous telemetry over a constrained channel, repeatedly transmitting this metadata consumes bandwidth that could otherwise be used for measurement data.

A binary representation can encode the same values using fixed-size fields without repeating their names in every message.

BTP therefore separates the description of a telemetry structure from the values transmitted in each sample.

The structure can be described when required, while subsequent telemetry messages contain only the binary values associated with that structure.

### 1.4 Large logical messages

The maximum size of one transport frame may be smaller than the data that the application needs to exchange.

Examples include:

* device descriptions;
* manifests;
* configuration data;
* command results;
* diagnostic information;
* groups of measurements.

BTP separates the logical message from the individual transmitted frame.

When a logical message exceeds the capacity of a frame, it can be divided into multiple fragments and reconstructed by the receiver.

Fragmentation is therefore handled by the protocol rather than independently by each application message type.

### 1.5 Binary structures and schema synchronization

Binary serialization reduces transmission overhead, but introduces another problem: both endpoints must agree on the meaning and layout of the payload.

Consider a telemetry payload represented internally as:

```cpp
struct Telemetry {
    float temperature;
    float pressure;
    uint16_t speed;
};
```

A receiver cannot correctly decode this payload unless it knows:

* which fields are present;
* their order;
* their data types;
* their sizes;
* their identifiers and meaning.

Hard-coding the same structure independently in the producer and consumer creates a synchronization requirement between both implementations.

If the producer adds, removes, or changes a field while the consumer still expects the previous structure, the payload can no longer be interpreted correctly.

This becomes more difficult when different device models expose different measurements or when the same server communicates with multiple firmware versions.

BTP separates **data representation** from **data description**.

Telemetry messages carry compact binary values. The protocol also provides a mechanism for the consumer to obtain the structures and capabilities exposed by a producer.

This information is provided through the BTP manifest mechanism.

A consumer can issue a manifest request and obtain a description of the objects exposed by the producer, including the information required to interpret subsequent payloads.

This allows binary telemetry to remain compact without requiring every supported data structure to be hard-coded in advance by the consumer.

### 1.6 Different traffic priorities

Not all traffic has the same delivery requirements.

A continuous telemetry stream may consume a significant part of the available channel capacity. At the same time, the device may need to process a command or exchange terminal data.

If all messages are handled identically, high-rate telemetry can delay control or interactive traffic.

BTP distinguishes traffic classes so that implementations can apply different priorities to them.

Telemetry, control, terminal, and protocol traffic can therefore coexist on the same channel without requiring independent communication protocols.

### 1.7 Different devices and software environments

The same protocol may be processed by different systems, including:

* microcontrollers;
* embedded computers;
* gateways;
* desktop applications;
* servers;
* software written in different programming languages.

These systems may use different:

* processor architectures;
* memory alignment rules;
* compiler ABIs;
* native type representations;
* programming languages.

The transmitted format must therefore not depend on the native memory representation of a C or C++ structure.

BTP defines field sizes, byte order, and encoding rules explicitly.

An implementation can reconstruct the logical data representation without depending on the memory layout used by another device.

### 1.8 Intermediate devices

A communication path may contain an intermediate device between the producer and the final consumer.

This device may need to:

* receive messages;
* queue them;
* fragment or reassemble them;
* prioritize traffic;
* forward messages.

It should not need to understand the application-specific meaning of every telemetry field or command.

BTP separates protocol framing from application semantics.

An intermediate device can therefore manipulate and forward BTP messages without implementing every application data structure carried by the protocol.

---

## 2. Resulting model

BTP treats telemetry, commands, terminal data, and protocol control as different message types transported through a common framing layer.

```text
                       BTP channel
                            |
           +----------------+----------------+
           |                |                |
           v                v                v
       Telemetry         Commands         Terminal
           |                |                |
       binary data     request/result     byte stream
           |
           v
     schema described
       by manifest
```

The common protocol layer provides mechanisms for:

* message framing;
* message identification;
* source timestamps;
* fragmentation;
* integrity validation;
* traffic classification;
* data structure discovery.

Application-specific payloads remain independent from these mechanisms.

The same communication model can therefore be used by different devices and software implementations while keeping application data compact and explicitly identifiable.

---

## 3. Design goals

### 3.1 Source-owned message identity

Each logical message is identified by:

```text
source_id
boot_id
sequence
```

The producer also assigns `timestamp_us`.

These values belong to the logical message and are preserved while the message is processed or forwarded.

`source_id` identifies the producer.

`boot_id` identifies one execution of that producer.

`sequence` identifies a logical message within that execution.

Together, these fields allow consumers to distinguish messages without depending on transport-specific addressing.

### 3.2 Source timestamps

Telemetry should represent when a measurement was acquired, not when it arrived at the consumer.

A timestamp assigned by the receiver includes transport delay, buffering, queueing, and application scheduling.

BTP therefore assigns `timestamp_us` at the producer.

Intermediate devices and consumers preserve this value.

This allows data from different messages and devices to be correlated using the acquisition time rather than the delivery time.

### 3.3 Transport-independent message model

BTP separates the logical message from the mechanism used to transport it.

The following properties belong to the BTP message model:

* message identity;
* message type;
* timestamp;
* object identification;
* payload representation;
* integrity information.

Transport-specific mechanisms may define different frame-size limits or encapsulation rules without changing the logical meaning of the message.

This allows the same application model to be used across communication channels with different characteristics.

### 3.4 Deterministic serialization

BTP does not transmit native application structures directly.

The wire representation explicitly defines:

* field offsets;
* field widths;
* byte order;
* valid field values;
* reserved fields;
* payload boundaries;
* integrity coverage.

The encoded representation therefore does not depend on:

* compiler structure packing;
* alignment;
* enum representation;
* native endianness;
* programming language.

Independent implementations can generate and consume the same byte representation.

### 3.5 Compact telemetry representation

High-rate telemetry should minimize repeated metadata.

BTP telemetry payloads contain binary values rather than repeatedly transmitting field names and textual representations.

Descriptions of available telemetry objects are provided separately through the manifest.

This separates frequently transmitted data from infrequently transmitted metadata.

### 3.6 Runtime discovery

A consumer should not require every possible producer configuration to be compiled into the application.

BTP provides a manifest mechanism that allows a producer to describe the objects and capabilities it exposes.

A consumer can request this information when required.

The manifest allows the consumer to determine how supported telemetry, commands, and other exposed objects should be interpreted.

This reduces direct coupling between producer firmware and consumer software.

### 3.7 Bounded resource usage

BTP is intended to operate on resource-constrained embedded systems.

The protocol is designed so that implementations can use bounded memory for:

* encoding;
* decoding;
* queues;
* fragmentation;
* reassembly.

The reference implementation does not require dynamic allocation in the core codec.

Applications determine the amount of memory allocated for queues and reassembly according to their system requirements.

### 3.8 Explicit traffic classification

BTP identifies the purpose of each message explicitly.

Traffic classes include different requirements for:

* frequency;
* latency;
* reliability;
* processing priority.

An implementation can therefore prioritize control or interactive traffic independently from continuous telemetry.

### 3.9 Deterministic validation

Invalid frames are rejected according to explicit protocol rules.

Validation may include:

* frame structure;
* field ranges;
* field combinations;
* reserved values;
* payload length;
* integrity checks;
* authentication when enabled.

A decoder should not infer missing information or reinterpret malformed frames using alternative formats.

### 3.10 Verifiable conformance

The protocol should be implementable independently of the reference library.

BTP therefore defines canonical binary test vectors.

Valid vectors specify:

* message fields;
* expected encoded representation.

Invalid vectors specify malformed inputs and their expected rejection.

These vectors allow different implementations to verify byte-level compatibility.

The specification defines the protocol. The reference implementation and conformance vectors provide implementations and tests for that specification.

---

## 4. Design trade-offs

The design goals above introduce explicit limitations.

### 4.1 Binary encoding requires schema information

Binary payloads are more compact than self-describing textual formats, but their fields cannot be interpreted without a corresponding schema.

BTP addresses this using the manifest mechanism.

The consumer must obtain or already know the relevant description before interpreting application-specific binary payloads.

### 4.2 Fragmentation has finite limits

Fragmentation allows logical messages to exceed the size of one transport frame, but it is not intended to provide general-purpose streaming.

The number and size of fragments are bounded by protocol fields and implementation resources.

Large files or continuous bulk transfers should use a transport designed for streaming.

### 4.3 Telemetry may be best-effort

Continuous telemetry can generate data faster than a constrained communication channel can transmit it.

BTP allows implementations to prioritize newer telemetry and higher-priority traffic instead of requiring retransmission of every telemetry sample.

Applications that require guaranteed delivery of every measurement need an additional reliability mechanism.

### 4.4 Static resource limits

Embedded implementations may use fixed-capacity queues and reassembly buffers.

If these resources are exhausted, the implementation must reject or discard data according to defined behavior rather than growing memory usage without limit.

### 4.5 Security scope

BTP can provide authenticated payload encryption, but it is not intended to implement a complete network-security architecture.

Depending on the selected protocol version and configuration, BTP does not necessarily provide:

* anti-replay protection;
* metadata confidentiality;
* automatic key distribution;
* automatic key rotation;
* forward secrecy;
* per-peer cryptographic identity.

Systems requiring these properties must provide them through the surrounding architecture.

### 4.6 Provisioning is external

BTP does not define how device identities, cryptographic keys, or initial endpoint configuration are provisioned.

These values must be established by the application or deployment environment.

### 4.7 Protocol upgrades require coordination

BTP favors deterministic decoding over implicit compatibility behavior.

An incompatible change to the wire format may therefore require coordinated updates of communicating endpoints.

Backward compatibility must be defined explicitly when required.

---

## 5. Intended use cases

BTP is intended for systems where a device must exchange multiple types of data through a common communication channel.

Typical use cases include:

* embedded telemetry devices;
* medium- or high-rate measurement acquisition;
* remote command execution;
* interactive terminal access;
* device configuration;
* status exchange;
* communication through bandwidth-constrained links;
* devices with limited memory;
* systems containing intermediate gateways;
* servers communicating with multiple device models;
* applications implemented in different programming languages.

A typical communication model is:

```text
+------------------+                    +------------------+
| Embedded device  |                    |      Server      |
|                  |                    |                  |
|  Telemetry       | -----------------> | Telemetry parser |
|  Commands        | <----------------> | Command manager  |
|  Terminal        | <----------------> | Terminal client  |
|  Manifest        | <----------------> | Device model     |
+------------------+                    +------------------+
            \_________________________________/
                    common BTP channel
```

All of these functions use the same framing and message model.

---

## 6. Out-of-scope use cases

BTP is not intended to replace every layer of a communication system.

### 6.1 Network routing

BTP does not define:

* route discovery;
* network-layer addressing;
* dynamic topology management;
* multi-hop routing.

The surrounding communication system is responsible for delivering frames between endpoints.

### 6.2 General-purpose file transfer

BTP fragmentation is intended for bounded logical messages.

It is not intended as a replacement for a reliable file-transfer or bulk-streaming protocol.

### 6.3 Fully self-describing messages

BTP does not repeat complete field names and schemas in every telemetry message.

Consumers requiring fully self-describing individual messages may prefer a textual or schema-embedded serialization format.

### 6.4 Automatic network service discovery

The manifest describes the capabilities of a known BTP producer.

It does not discover arbitrary devices or services on a network.

### 6.5 Complete security infrastructure

BTP does not provide key provisioning, certificate infrastructure, user authentication, or network access control.

These functions belong to the surrounding system.

---

## 7. Design summary

BTP provides a common binary message model for embedded telemetry and control systems.

It is designed around the following requirements:

* multiple traffic classes sharing the same communication channel;
* medium- and high-rate telemetry;
* compact binary representation;
* command request and result exchange;
* bidirectional terminal traffic;
* bounded transport frame sizes;
* fragmentation of larger logical messages;
* runtime discovery of device data structures;
* interoperability between different processors and software environments;
* deterministic validation and conformance testing.

BTP separates frequently transmitted data from its description.

Telemetry values can therefore be transmitted in a compact binary representation, while the manifest provides the information required for a consumer to interpret the structures exposed by the producer.

This avoids the repeated overhead of self-describing textual formats while reducing the need to hard-code identical application structures independently on both endpoints.

The protocol focuses on framing, message identity, serialization, fragmentation, traffic classification, discovery, and integrity.

Routing, provisioning, large-scale streaming, and complete network security remain outside its scope.
