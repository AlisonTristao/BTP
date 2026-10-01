# Profiles

BTP is a generic protocol. It defines how telemetry, commands, logs and a manifest are framed, identified and carried; it does not say what a particular topic, field or action *means*. A **profile** is a published agreement about exactly that: a set of names, and the behaviour expected behind them, for one kind of application.

!!! note "Profiles are conventions, not protocol"
    A profile is a naming and semantic **convention** layered on top of BTP. It is **not part of the protocol**.

    * An endpoint does not have to follow any profile, and a profile never makes an otherwise valid BTP endpoint invalid.
    * BTP can be used in any other way. The wire format, the manifest and the library are the same with or without a profile.
    * Nothing in the core library depends on a profile. The optional headers under `include/btp/profiles/` only spell the profile's strings and values; the library neither includes nor tests them.

Profiles exist so that two independently written endpoints, one producing and one consuming, can interoperate without a private agreement for every pair. They are optional vocabulary, not a layer a deployment must adopt.

Available profiles:

| Profile | Prefix  | Purpose                                              | Page                 |
| ------- | ------- | ---------------------------------------------------- | -------------------- |
| `loop`  | `loop.` | Closed-loop control between a plant and a controller | [The `loop` profile](loop.md) |

---

## 1. Rules common to every profile

### 1.1 Names live under a reserved dotted prefix

Each profile owns one prefix ending in a dot, for example `loop.`. Every topic, action, field or parameter name that the profile defines is the profile prefix followed by a short name:

```text
loop.state        topic
loop.u            action
```

The prefix applies to topic and action names. Field and parameter names inside them are scoped by their topic or action and are not prefixed: `tick` in `loop.state`, `ref_tick` in `loop.u`.

An endpoint that exposes a name under a profile's prefix is claiming to follow that profile for that name. Names outside every profile prefix belong to the application and are never affected by a profile.

### 1.2 Consumers bind by name, never by numeric id

Numeric identifiers are the producer's choice. BTP requires them to be stable *within one producer* ([Commands and discovery §3.11](../commands.md#311-stable-identifiers-and-revisions)), but it does not make them portable between producers: two producers may number the same logical topic differently.

A profile therefore identifies things by:

```text
(topic name, field name)
(action name, parameter name)
```

A consumer reads the manifest, locates the topic or action by name, locates the field or parameter by name inside it, and only then uses the identifiers the manifest gave it. It does not hard-code `topic_id`, `field_id` or `action_id`.

This does not contradict the identity rules of [Telemetry payloads §18](../telemetry.md#18-client-binding): *after* the name has been resolved through the manifest, the numeric identity is what a client holds on to for the lifetime of that schema.

### 1.3 Evolution is additive

Within one prefix, a revision of a profile may only:

* add new **optional** topics, actions and fields;
* add new enum values, where the profile reserves room for them.

It may not rename, remove, retype or change the meaning of anything it already defined. To make a breaking change, the profile takes a **new prefix**, for example `loop2.`, and the old prefix keeps its meaning. Both can be exposed side by side.

Consumers follow three rules so that this works:

* **Unknown fields, topics and actions are ignored.** Something under the prefix that the consumer does not know is not an error.
* **A missing required field means the feature is unavailable.** The consumer does not guess, substitute a default or fall back to another field.
* **Unknown enum values are not an error** either; [Telemetry payloads §14.3](../telemetry.md#143-unknown-enum-values) already describes how they are handled.

### 1.4 Units and ranges come from the manifest

A profile fixes the *role* of a field, not its engineering unit or range. Where a field carries an application quantity, the unit, scale, offset and minimum/maximum are whatever the manifest declares ([Commands and discovery §3.7](../commands.md#37-field-records)). A profile page may say "measurement fields, unit and range in the manifest"; it never says "metres" or "0 to 255".

### 1.5 Presentation stays with the client

A profile defines what the data *is*, not how to show it. Labels, colours, axes, panel layout and chart types are client configuration and are not carried by profile names ([Telemetry payloads §18.3](../telemetry.md#183-presentation-is-not-telemetry-metadata)). A client may offer a ready-made view for a profile it recognises, but that view is the client's own.

---

## 2. Using a profile

A producer that wants to follow a profile publishes the profile's names in its manifest and implements the behaviour described. A consumer that understands a profile looks for the profile's required topics and actions by name; if they are present it enables the corresponding feature, and if they are not it does nothing special. Endpoints that do not know the profile see only ordinary topics and actions, and can use them exactly as any other BTP data.

The reference library ships a header per profile with the names as constants, for example [`btp/profiles/loop.hpp`](../../include/btp/profiles/loop.hpp). They are conveniences. Spelling the strings by hand is equally valid.
