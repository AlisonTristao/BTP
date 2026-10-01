# The `loop` profile

The `loop` profile names the topics and actions needed to close a **control loop** across a BTP link: one endpoint owns the sensors, the actuators and the loop clock; another computes the actuation from the measurements it receives.

!!! note "A convention, not part of the protocol"
    This page describes a profile: a naming and semantic **convention** layered on BTP ([Profiles](index.md)). It is **not part of the protocol**.

    * An endpoint does not have to follow it, and BTP can be used in any other way, including for control loops that look nothing like this one.
    * Nothing in the core library depends on it. [`btp/profiles/loop.hpp`](../../include/btp/profiles/loop.hpp) and [`btp/profiles/loopctl.h`](../../include/btp/profiles/loopctl.h) are optional conveniences.
    * Everything here is expressed with ordinary BTP topics, actions and fields. A consumer that has never heard of the profile still sees valid telemetry and valid actions.

The rules common to every profile (reserved prefix, binding by name, additive evolution, units from the manifest, presentation in the client) are in [Profiles](index.md#1-rules-common-to-every-profile). This page does not repeat them.

---

## 1. Roles

| Role           | Owns                                               | Does                                                                                                  |
| -------------- | -------------------------------------------------- | ----------------------------------------------------------------------------------------------------- |
| **Plant**      | Sensors, actuators and the loop clock (the *tick*) | Publishes `loop.state` and `loop.output` every tick; applies actuation; may run its own controller    |
| **Controller** | The control algorithm                              | Consumes `loop.state`; sends actuation with `loop.u`; may inject measurements and change the mode     |

Both are ordinary BTP endpoints. The plant is the producer of the telemetry topics and the target of the actions below; the controller is their consumer and the requester of the commands. One physical endpoint may be a plant for one loop and a controller for another.

### 1.1 Configuration must arrive, samples may be lost

The profile follows one principle, and the mechanism chosen for each name below follows from it:

| Kind of information                                                                        | Mechanism                                                                                   | Loss                                             |
| ------------------------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------- | ------------------------------------------------ |
| Configuration and mode changes (`loop.config`, `loop.mode`)                                | Regular command: deduplicated, answered with `COMMAND_RESULT`                               | Never silent; the requester learns the outcome   |
| Measurements and actuation samples (`loop.state`, `loop.output`, `loop.u`, `loop.inject`)  | Telemetry and STREAM actions ([Commands and discovery §2.7](../commands.md#27-stream-actions)) | Tolerated; a newer sample replaces a lost one    |

A sample retransmitted late is worth less than the next sample, so samples are not retransmitted. Anything that changes *how* the loop behaves is not a sample and must be confirmed.

---

## 2. Telemetry topics (plant → controller)

### 2.1 `loop.state` (required)

Published by the plant **every tick**.

| Field          | Type                | Meaning                                                                                  |
| -------------- | ------------------- | ---------------------------------------------------------------------------------------- |
| `tick`         | `uint32`            | The plant's own loop counter at the time of the measurements                             |
| `mode`         | `enum8`             | Current plant mode, see below                                                            |
| *measurements* | application-defined | One field per measurement, in the order the controller passes them on as `y` (§9). Unit and range come from the manifest |

`mode` values:

| Value | Name     | Meaning                                                                                 |
| ----: | -------- | --------------------------------------------------------------------------------------- |
|   `0` | `OFF`    | The plant applies no actuation from this loop                                           |
|   `1` | `LOCAL`  | The plant runs its own controller                                                       |
|   `2` | `REMOTE` | Actuation comes from `loop.u`                                                           |
|   `3` | `INJECT` | Measurements come from `loop.inject` instead of the sensors (plant-in-the-loop testing) |

`tick` increases by one per loop iteration. It is not the BTP `sequence` ([§5](#5-why-tick-is-a-field)).

Because the topic is published every tick, a gap in `tick` seen by the controller means that `loop.state` samples were lost.

### 2.2 `loop.output` (required)

Published by the plant every tick, describing the actuation it **actually applied**.

| Field       | Type                | Meaning                                                                         |
| ----------- | ------------------- | ------------------------------------------------------------------------------- |
| `ref_tick`  | `uint32`            | `tick` of the `loop.state` from which the applied actuation was computed        |
| `source`    | `enum8`             | Where the applied actuation came from, see below                                |
| *actuation* | application-defined | The values applied, with **the same names** as the inputs of `loop.u`           |

`source` values:

| Value  | Name       | Meaning                                                                   |
| -----: | ---------- | ------------------------------------------------------------------------- |
|    `0` | `NONE`     | No actuation was applied this tick                                        |
|    `1` | `LOCAL`    | Applied by the plant's own controller                                     |
|    `2` | `REMOTE`   | Applied from a `loop.u` request                                           |
|  `≥ 3` | *reserved* | Reserved for the future loss policy ([§6](#6-loss-policy-future-not-normative)) |

When `source` is `LOCAL`, `ref_tick` is the plant tick whose measurements its own controller used. When it is `NONE`, `ref_tick` has no meaning and a consumer ignores it.

A plant SHOULD publish the `loop.state` and `loop.output` of one tick with the same `timestamp_us`, so that an observer can pair them without extra fields.

### 2.3 `loop.stats` (optional, low rate)

Counters kept by the plant about the incoming `loop.u` stream. A plant that does not keep them omits the topic.

| Field       | Type     | Meaning                                                                                                              |
| ----------- | -------- | -------------------------------------------------------------------------------------------------------------------- |
| `rx`        | `uint32` | `loop.u` requests received                                                                                           |
| `late`      | `uint32` | Requests that arrived too late to supply their first element, or with a `ref_tick` not newer than one already accepted |
| `lost`      | `uint32` | Gaps seen in `ref_tick` between consecutively accepted requests                                                      |
| `age_last`  | `uint16` | Age, in ticks, of the actuation applied most recently (`t − ref_tick`, [§4.2](#42-correlation-by-tick-no-clock-synchronization)) |
| `period_us` | `uint32` | Measured duration of one plant tick, in microseconds                                                                 |

Counters wrap modulo 2³² and are meaningful as differences. They exist for monitoring; the loop does not depend on them.

---

## 3. Actions (controller → plant)

### 3.1 `loop.u` (STREAM)

The controller's actuation for the next ticks. It is a STREAM action ([Commands and discovery §2.7](../commands.md#27-stream-actions)): each request is a self-contained sample, is executed without deduplication, is never answered with `COMMAND_RESULT`, and the latest one wins.

| Parameter       | Type                                          | Meaning                                                 |
| --------------- | --------------------------------------------- | ------------------------------------------------------- |
| `ref_tick`      | `uint32`                                      | `tick` of the `loop.state` the plan was computed from   |
| *one per input* | `float32`, variable-count array, maximum `M`  | The planned values of that input                        |

Each input is its own parameter, named after the input; `loop.output` repeats the same names for what was applied. All input arrays of one request have the same length.

**Element `k` of every array is the value intended for plant tick `ref_tick + 1 + k`.** A plant at tick `t` that holds the request uses element

```text
k = t − ref_tick − 1
```

when that element exists.

* A plain controller sends `M = 1`: one value, for the tick after the `loop.state` it answers.
* A predictive controller sends its whole plan. If one request is lost, the previous request still covers the tick; the plant uses a later element of the older plan.

```text
plan with ref_tick = 100, M = 4

element:        0     1     2     3
plant tick:   101   102   103   104
```

Rules:

* Of the requests that have arrived, the plant uses the one with the **highest `ref_tick`**. A request whose `ref_tick` is not higher than one already accepted is discarded (and counted as `late`).
* Whether a plant acts on `loop.u` depends on its mode: in `REMOTE` it does; in other modes it discards the request.

`M` is the `max_element_count` declared in the manifest. It is bounded by the transport ([§7](#7-sizing)).

### 3.2 `loop.inject` (STREAM)

Measurements supplied by the controller side in place of the plant's sensors, for **plant-in-the-loop testing**. Used only in `INJECT` mode; in any other mode the plant discards it.

| Parameter      | Type                | Meaning                                                                    |
| -------------- | ------------------- | -------------------------------------------------------------------------- |
| `tick`         | `uint32`            | The tick the injected measurements stand for                               |
| *measurements* | application-defined | **The same names** and types as the measurement fields of `loop.state`     |

It is a STREAM action: latest wins, no result. In `INJECT` mode the plant publishes `loop.state` as usual, with `mode = INJECT`, taking the measurement values from the injected sample instead of the sensors.

### 3.3 `loop.mode` (regular, DANGEROUS)

Changes the plant mode.

| Parameter | Type    | Meaning                                                          |
| --------- | ------- | ---------------------------------------------------------------- |
| `mode`    | `enum8` | The requested mode, same values as `loop.state.mode`             |
| `arm`     | `bool`  | `true` authorises actuation that does not originate in the plant |

The action is a regular command with the `DANGEROUS` flag ([Commands and discovery §3.9](../commands.md#39-action-records)) and a `confirmation_text` in the manifest, so a client asks the operator before sending it. **Arming is always a confirmed command**: a plant enters `REMOTE` or `INJECT` only through a `loop.mode` request with `arm = true`, answered with `COMMAND_RESULT`, never through a STREAM sample.

The new mode is visible in the next `loop.state`. A request for a mode the plant does not support is rejected with an action-specific error ([Commands and discovery §3.10](../commands.md#310-action-specific-errors)).

### 3.4 `loop.config` (regular, IDEMPOTENT)

The application's own parameters (gains, limits, filter settings, and so on). The parameters are application-defined; the profile fixes only the action name and its kind. The action carries the `IDEMPOTENT` flag, so repeating it with the same values is harmless, and as a regular command it is answered and never lost silently.

---

## 4. Timing

### 4.1 The plant is the clock master

In `REMOTE` mode the loop is driven by the plant's clock.

```text
Plant                                          Controller
  |                                                 |
  |-- loop.state (tick = t) ----------------------->|
  |                                  compute plan   |
  |<---------------------- loop.u (ref_tick = t) ---|
  |  apply element for tick t+1                     |
```

The controller computes **on each `loop.state` arrival**, never on a timer of its own. It has no clock to keep aligned with the plant: if the plant's period drifts or `loop.state` samples are lost, the controller computes whenever a sample arrives.

### 4.2 Correlation by tick, no clock synchronization

Everything is correlated by `tick` and `ref_tick`, both counted by the **plant**. When the plant is at tick `t` and applies actuation computed from `ref_tick`:

```text
age = t − ref_tick
```

`age` is also the **loop latency**, in ticks: how many plant ticks passed between the measurements and the application of their consequence. The controller and any observer obtain it from `loop.state` and `loop.output` alone. No comparison of timestamps between the two endpoints is involved, so no clock synchronization is needed ([The protocol model §6](../model.md#6-time)).

---

## 5. Why `tick` is a field

BTP already numbers every message with `sequence`. The profile still carries `tick` because `sequence` is shared by all message types generated by the producer: telemetry, logs and command results interleave in one counter, so a consumer watching only one topic sees gaps that do not mean loss, and `sequence` cannot tell it how many *loop iterations* have passed ([The protocol model §4.3](../model.md#43-sequence)). `tick` is the plant's own loop counter, and it is the one quantity that means the same thing on both sides of the loop.

---

## 6. Loss policy (future, not normative)

What a plant does when `loop.u` stops arriving, or arrives without a usable element for the current tick, is **not defined by this revision**. Candidate behaviours include:

* hold the last applied value;
* use the next element of the most recent plan;
* fall back to the plant's local controller;
* stop actuation;
* a watchdog that selects among these after a timeout.

A later revision may define the policy and the `source` values `≥ 3` that report it. Until then each plant documents its own behaviour, and consumers MUST NOT assume one.

As a conservative default, a plant SHOULD **stop actuation after a timeout** when `loop.u` stops arriving in `REMOTE` or `INJECT` mode, and SHOULD then require a new `loop.mode` request with `arm = true` before accepting remote actuation again.

---

## 7. Sizing

A STREAM sample is most useful when it crosses the link in a single frame. The frame limits of the transport ([Getting it across the link §4](../fragmentation-and-transports.md#4-transport-presets)) therefore bound both `M` and the number of measurement fields.

For `loop.u` with `n` inputs, each a `float32` variable-count array:

```text
COMMAND_REQUEST payload = 20 + parameters
parameters              = 4 + n × (2 + 4 × M)
```

`ref_tick` takes 4 octets; each array takes a 2-octet count plus at most `4 × M` octets of elements. The formula ignores a presence bitmap, which exists only when a field is nullable. The request payload must fit the maximum BTP payload of the transport, less the 16-octet authentication tag when the frame is encrypted. For `n = 2` and no encryption:

| Transport | Maximum BTP payload | Largest `M` in one frame |
| --------- | ------------------: | -----------------------: |
| ESP-NOW   |                 210 |                       22 |
| BLE       |                 472 |                       55 |
| Serial    |                4056 |                      503 |
| TCP       |                8152 |                     1015 |

USB HID, with a 22-octet payload, cannot carry a `loop.u` request in a single frame.

For `loop.state`, the body is 5 octets plus the measurements, after the 2-octet `schema_version` prefix ([Telemetry payloads §17](../telemetry.md#17-publishing-and-fragmentation)); on ESP-NOW that leaves 208 octets of body per frame. A larger sample is fragmented ([Getting it across the link §1](../fragmentation-and-transports.md#1-fragmentation)), and a fragmented message is discarded whole if any fragment is lost, so a producer whose loop rate matters keeps the per-tick samples within one frame.

---

## 8. Example

The manifest below describes a plant with two measurements (`y1`, `y2`) and two inputs (`u1`, `u2`). These names, and their units and ranges, are application-defined and appear only to make the example concrete. Identifiers are the producer's choice; a consumer binds by name.

### 8.1 Manifest

Topics:

| Topic         | Field       | Type      | Unit / range    | Notes                               |
| ------------- | ----------- | --------- | --------------- | ----------------------------------- |
| `loop.state`  | `tick`      | `uint32`  |                 |                                     |
|               | `mode`      | `enum8`   |                 | `OFF`, `LOCAL`, `REMOTE`, `INJECT`  |
|               | `y1`        | `float32` | per application |                                     |
|               | `y2`        | `float32` | per application |                                     |
| `loop.output` | `ref_tick`  | `uint32`  |                 |                                     |
|               | `source`    | `enum8`   |                 | `NONE`, `LOCAL`, `REMOTE`           |
|               | `u1`        | `float32` | per application | same name as the `loop.u` input     |
|               | `u2`        | `float32` | per application | same name as the `loop.u` input     |
| `loop.stats`  | `rx`        | `uint32`  |                 | optional topic, low rate            |
|               | `late`      | `uint32`  |                 |                                     |
|               | `lost`      | `uint32`  |                 |                                     |
|               | `age_last`  | `uint16`  | ticks           |                                     |
|               | `period_us` | `uint32`  | µs              |                                     |

Actions:

| Action        | Flags      | Parameter             | Type                                  |
| ------------- | ---------- | --------------------- | ------------------------------------- |
| `loop.u`      | STREAM     | `ref_tick`            | `uint32`                              |
|               |            | `u1`                  | `float32[]`, `max_element_count = 4`  |
|               |            | `u2`                  | `float32[]`, `max_element_count = 4`  |
| `loop.inject` | STREAM     | `tick`                | `uint32`                              |
|               |            | `y1`                  | `float32`                             |
|               |            | `y2`                  | `float32`                             |
| `loop.mode`   | DANGEROUS  | `mode`                | `enum8`                               |
|               |            | `arm`                 | `bool`                                |
| `loop.config` | IDEMPOTENT | *application-defined* |                                       |

### 8.2 Exchange

A predictive controller with `M = 3` drives a plant. One `loop.u` request is lost.

First, the controller sends `loop.mode` with `mode = REMOTE` and `arm = true`. The operator confirmed it, and the plant answers with a successful `COMMAND_RESULT`. The next `loop.state` carries `mode = REMOTE`.

Then, per plant tick:

| Plant tick | Plant side | Link and controller side |
| ---------: | ---------- | ------------------------ |
| 100 | Publishes `loop.state` (`tick = 100`). | The controller receives it, computes a plan and sends `loop.u` (`ref_tick = 100`, `u1 = [a0, a1, a2]`, `u2 = [b0, b1, b2]`). |
| 101 | Holds the plan of 100 and applies its element `101 − 100 − 1 = 0`. Publishes `loop.output` (`ref_tick = 100`, `source = REMOTE`) and `loop.state` (`tick = 101`). | The controller receives `loop.state` 101 and sends `loop.u` (`ref_tick = 101`). **This request is lost.** |
| 102 | The newest plan is still the one of 100. Applies element `102 − 100 − 1 = 1`. Publishes `loop.output` (`ref_tick = 100`, `source = REMOTE`): age 2. | The controller receives `loop.state` 102 and sends `loop.u` (`ref_tick = 102`). |
| 103 | The plan of 102 has arrived. Applies its element 0 and publishes `loop.output` (`ref_tick = 102`). | In `loop.stats`, `lost` is now 1: the accepted `ref_tick` values went from 100 to 102. |

Nothing was retransmitted and no clocks were compared. The lost sample was covered by the previous plan, and the plant, or anyone listening, reads the age (`102 − 100 = 2`) from the plant's own counters.

With `M = 1` the same loss would have left tick 102 without a usable element, which is the situation the loss policy of [§6](#6-loss-policy-future-not-normative) will address.

---

## 9. The optional controller interface

A controller that should run unchanged on a host and on an embedded target can use [`btp/profiles/loopctl.h`](../../include/btp/profiles/loopctl.h), a small portable C interface. `step` receives the measurements `y` of one tick and writes the plan `u[k*nu + i]`; `init`, `set_param`, `reset` and `destroy` complete the lifecycle. `y` follows the declaration order of the `loop.state` measurement fields and `u` the declaration order of the `loop.u` inputs. Nothing is allocated inside `step`.

Like the profile, it is an optional convention. It does not mention BTP, and nothing in the library depends on it.
