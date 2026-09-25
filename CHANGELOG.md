# Changelog

Selected changes in the BTP 2.x line, migrated from the README. This is a
summary of milestones, not a complete list of commits or releases.

See [versioning and branches](docs/library.md#10-versioning-and-branches)
for the release policy and [the README](README.md) to get started.

## 2.x history

`2.2` `btp::messages` · `2.3` verbatim manifest relay · `2.4` `btp::telemetry` ·
`2.5` body-only sample mode · `2.6` `btp::DedupCache` · `2.7` `btp::Endpoint` ·
`2.8` `btp::Receiver` · `2.9` `btp::Session` · `2.10` `priority_class()` ·
`2.11` `btp::Node` (endpoint + receiver + session, one object) ·
`2.12` `btp::Catalog` (consumer-side discovery) ·
`2.13` telemetry schema-declaration helpers, one line per field ·
`2.14` `connect()` (`SessionInitiator`) + `publish_named()` ·
`2.15` subscriptions (`SubscriptionTable` / `SubscriptionClient`) ·
`2.16` commands (`DedupCache` / `CommandClient`) + `STATUS` reporting ·
`2.21` `on_publish()` + `publish_subscribed_topics()` ·
`2.22` producer/consumer setup + loop boilerplate folded into `Node` ·
`2.23` `on_terminal()`; `StaticNode<>` bundles commands ·
`2.24` `NodeTerminalFn` gets `Node&` / `now_ms` ·
`2.25` `NodeConfig.terminal` / `.command` wire at construction ·
`2.26` `routine()` — one call covers a whole loop pass ·
`2.27` `reply_seal` — per-reply seal selection ·
`2.28` a catalogue field's unit and description ·
`2.29` `Node::reconfigure()` (removed again in 2.34 — see below) ·
`2.30` `publish_with()` / `publish_named_with()` ·
`2.31` `TransportLimits` — generic, replaces the closed `TransportProfile` enum ·
`2.32` `TransportLimits` drops `max_payload_size` (derived, not set) ·
`2.33` `SizedNode<NodeSize>` — Low / Medium / High memory tiers ·
`2.34` `NodeConfig` becomes an abstract class, replacing `reconfigure()`
(set transport before node construction, identity before `begin()`; see
[configuration lifetime](docs/library.md#161-the-contract)); `HelloBuilder`.

Later changes:

* `2.35`: decoded-frame receive entry point and catalogue `source_info`.
* `2.36`: subscription outcomes include peer, topic, rate and status.
* `2.37`: `SessionInitiator::peer_config_revision()`.
* `2.38`: public headers tolerate Qt's `slots` macro.
* `2.39`: catalogue supports body-only topics with no fields.
* `2.40`: subscription rate policy, reboot eviction and introspection.
* `2.41`: built-in STATUS v2 topic reporting.
* `2.42`: asynchronous command completion.
* `2.43`: `Node::receive_outcome()`.
* `2.44`: manifest format 3 field ranges (`min_value` / `max_value`).
* `2.45`: format-3 ranges become optional per field, selected by `HAS_RANGE`.
* `2.46`: a keyed `Node` rejects cleartext messages by default
  (`NodeConfig::accept_cleartext()`, `Node::Stats::dropped_cleartext` /
  `dropped_open_failed`); TCP and BLE transport profiles (`kTcpTransport`,
  `kBleTransport`); `Endpoint::send_logical()` (and every `Node` send)
  fragments to at most 250-octet frames on any transport, so a payload past
  ~200 octets on a TCP / Serial / BLE node no longer fails to send;
  `btp::TxQueue` / `StaticTxQueue` (`btp/txqueue.hpp`), a send-side priority
  queue that sheds telemetry first.
* `2.47`: several links on one `Node` -- `NodeLink` (the per-link half of
  `NodeConfig`), `attach_link()` / `receive_on()` / `enable_session_on()` /
  `reset_link()`, `LinkRef` epochs for replies that outlive a connection,
  `StaticNode<..., Links>`, `NodeConfig::terminal_on()` / `lock()`. Source
  compatible: a single-link node needs no change.

2.46 changes receive behavior for any `Node` whose `NodeConfig::has_open()` is
true: a message that arrives without `ENCRYPTED` is now dropped instead of
routed, so a peer can no longer reach a terminal or command handler by simply
not sealing. The session handshake is unaffected. A consumer that legitimately
receives some traffic in the clear on a keyed link overrides
`accept_cleartext()`; its peers must seal everything else.

The format-3 field layout changed between 2.44 and 2.45. Upgrade producers
and consumers that exchange format-3 manifests together; 2.45 still reads
manifest formats 1 and 2. The frame header and its wire-version byte are
unchanged.
