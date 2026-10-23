# VeilNet device-moderation server

This .NET 10 service provides the device-ban API and a small admin webpage. It
does not inspect gameplay, detect wallhack software, or decide actor visibility.
Anti-wallhack prevention is implemented in the Unreal game server by
`UVeilNetVisibilityComponent` and per-actor replication relevancy checks.
It stores only a keyed HMAC of the client-provided SHA-256 hardware
fingerprint in SQLite.

## Run locally

```powershell
$env:VEILNET_GAME_TOKEN = "<unique game-server token>"
$env:VEILNET_ADMIN_TOKEN = "<different moderator token>"
$env:VEILNET_HASH_KEY = "<stable secret of at least 32 bytes>"
dotnet run --project .\VeilNet.Server.csproj
```

Open `http://127.0.0.1:5080/` and enter the admin token to list devices and
ban/unban them, or review recent movement-audit events. The page keeps the
token only in memory. Configure `VEILNET_DB_PATH` to move the SQLite database,
`VEILNET_LISTEN_URL` to change the bind address, and
`VEILNET_EVENT_RETENTION_DAYS` (1-3650, default 30) to set audit-event
retention. The default bind is loopback; use a trusted HTTPS reverse-proxy for
remote access. Do not expose the moderator token to the game client.

The HMAC key must remain stable or previously stored fingerprints will no
longer match. Back up the database and key securely together.

## API

- `POST /api/v1/game/check` — `VEILNET_GAME_TOKEN`; body
  `{"deviceFingerprint":"64 hexadecimal characters"}`. Registers a device
  and returns its server fingerprint and active-ban status.
- `POST /api/v1/game/events` — `VEILNET_GAME_TOKEN`; accepts server-generated
  movement anomaly events (`movement_speed` or `movement_discontinuity`).
  Events expire according to `VEILNET_EVENT_RETENTION_DAYS`.
- `GET /api/v1/admin/events?limit=50&offset=0` — `VEILNET_ADMIN_TOKEN`;
  returns recent movement events for moderator review.
- `GET /api/v1/admin/devices?limit=50&offset=0` — `VEILNET_ADMIN_TOKEN`;
  returns the observed devices, last-seen times, and active ban status.
- `POST /api/v1/admin/bans` — `VEILNET_ADMIN_TOKEN`; body
  `{"fingerprint":"64 hex","reason":"...","bannedBy":"...","expiresAt":null}`.
  Set `expiresAt` to an ISO-8601 future timestamp for a timed ban.
- `DELETE /api/v1/admin/bans/{fingerprint}` — `VEILNET_ADMIN_TOKEN`.

The `deviceFingerprint` comes from the Windows-only EasyHWID plugin. The
server cannot prove a client-provided fingerprint is genuine; modified
clients can spoof it. This mechanism is a moderation aid, not a full
anti-cheat or hardware attestation.

## Anti-wallhack integration

The game server must compute visibility and withhold hidden actors' replicated
state. Add `UVeilNetVisibilityComponent` to each observer pawn, configure its
target tags, and use
`UVeilNetVisibilityComponent::IsActorVisibleForViewer` from the
`IsNetRelevantFor` override of every actor type that carries sensitive target
state. Apply the same policy to proxies and related actors. Configure the
visibility trace channel so world geometry blocks traces, then test with
multiple clients and inspect replicated actor state—not only rendered output.

The sample integration and additional leakage checks are documented in the
repository's [anti-wallhack section](../README.md#anti-wallhack-visibility-filtering).
Device validation is independent of this filter. Keep the game token on the
game server, and do not treat a successful device check as proof that a client
is cheat-free.

The sample character also has a server-side movement audit. It checks
horizontal/vertical speed and position displacement in supported movement
modes, with separate vertical limits while falling. Large position changes
can also occur during legitimate server teleports; call `ResetPositionBaseline`
for authorized teleports and respawns. These are anomaly logs only; they do
not auto-ban or disconnect players. Tune the audit for game modes with
legitimate movement abilities. Weapon fire and hit validation are not covered.
When `VEILNET_AUDIT_URL` is set in the dedicated game server environment, the
component sends anomaly records to the service using `VEILNET_GAME_TOKEN`;
without it, the existing Unreal server log remains the only record. The event
contains a server player ID, event kind, measurements, movement mode, and
server time—not a hardware fingerprint or player name. Telemetry submission is
limited to one event per character per second by the component. The numeric
player ID is session-scoped, not a stable account identity. Use HTTPS for
remote service connections. Treat logs as investigation leads, not automated
cheat determinations.

There is no general-purpose, unspoofable HWID that a regular game client can
report. EasyHWID fingerprints client-readable SMBIOS values, so a modified
client can forge them. Stronger device attestation requires an integrated
trusted platform or anti-cheat provider and server-verifiable credentials.
