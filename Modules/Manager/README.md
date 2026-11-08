# VeilNet server manager

This FastAPI service manages player records, bans, game-server records, and
live server/player presence. SQLite is the source of truth for records; Redis
holds expiring heartbeats and presence. Redis must be available for health
checks and presence operations.

## Run locally

Start Redis and create a local `.ENV` file next to `main.py`:

```dotenv
VEILNET_GAME_TOKEN=replace-with-a-game-server-token
VEILNET_ADMIN_TOKEN=replace-with-a-different-admin-token
VEILNET_MANAGER_REDIS_URL=redis://localhost:6379/0
VEILNET_MANAGER_DB_PATH=manager.db
VEILNET_MANAGER_UNREAL_SERVER_EXECUTABLE=
VEILNET_MANAGER_UNREAL_SERVER_MAP=/Game/Maps/Lobby
VEILNET_MANAGER_UNITY_SERVER_EXECUTABLE=
VEILNET_MANAGER_GAME_API_URL=http://127.0.0.1:8000
VEILNET_MANAGER_IDENTITY_KEY=replace-with-a-stable-random-secret-of-at-least-32-bytes
```

The Manager loads `.ENV` at startup without overriding variables already set
in the process environment. `.ENV` is ignored by Git. Keep the two tokens
different and never put them in a game client. Generate and keep
`VEILNET_MANAGER_IDENTITY_KEY` secret and stable; it is used to derive private
player IDs from device fingerprints, and changing it will make existing device
accounts appear new. Generate a value with
`python -c "import secrets; print(secrets.token_urlsafe(32))"` and keep it
private. Set the executable path for each engine you want to launch. For
example:

```dotenv
VEILNET_MANAGER_UNREAL_SERVER_EXECUTABLE=C:\Builds\VeilNetServer.exe
VEILNET_MANAGER_UNREAL_SERVER_MAP=/Game/Maps/Lobby
VEILNET_MANAGER_UNITY_SERVER_EXECUTABLE=C:\Builds\UnityDedicatedServer.exe
VEILNET_MANAGER_IDENTITY_KEY=use-a-unique-random-secret-at-least-32-bytes-long
```

Choose Unity or Unreal when registering or editing each server in the Manager.
The Unreal executable receives the configured map followed by
`-Port=<registered server port>` and `-log`. For Unity, the Manager sets
`VEILNET_SERVER_PORT` in the dedicated-server process environment instead of
passing Unity command-line flags; the dedicated server build must use that
value for its game transport's listening port. Build the Unity player with
Dedicated Server optimizations for headless operation. Set the map to one that
exists in your packaged Unreal project. For existing installations,
`VEILNET_MANAGER_SERVER_EXECUTABLE` and `VEILNET_MANAGER_SERVER_MAP` remain
supported as Unreal-only fallbacks. The Manager launches only executables
configured in its `.ENV`; the browser cannot submit executable paths or
commands.

## Unity server integration

Copy [`Unity/VeilNetManagerHeartbeat.cs`](./Unity/VeilNetManagerHeartbeat.cs)
into your Unity project's `Assets` folder and rebuild the Unity server. When
the Manager starts the Unity executable, it sets `VEILNET_SERVER_ID`,
`VEILNET_SERVER_PORT`, and `VEILNET_MANAGER_URL` in the process environment.
The script automatically creates its component before scene load when
`VEILNET_SERVER_ID` is set, so it does not depend on a scene object reference.
For servers launched outside the Manager, set `VEILNET_SERVER_ID`,
`VEILNET_SERVER_PORT`, `VEILNET_MANAGER_URL`, and `VEILNET_GAME_TOKEN`
yourself. Set `VEILNET_MANAGER_GAME_API_URL` to the reachable API base URL if
the default `http://127.0.0.1:8000` is not right for the server host. Do not
put the game token in a client build. The component does not require Unity's
`-batchmode` flag. If `VEILNET_SERVER_PORT` is omitted, the component uses
`-ngo-port <port>`, legacy `-port=<port>`, or 7777 in that order. Set the
environment variable when the Manager's registered port is not 7777. Use HTTPS
when the Manager is remote.

On startup, the Unity log should contain `VeilNet Manager heartbeat component
active`; after the first request it should report that a heartbeat was sent.
If neither message appears, the component is not active in the running build
or scene. If the request fails, its log includes the HTTP status and response
body to help identify a URL, token, or server-ID configuration issue.

The Unity Netcode integration can use VeilNet device identity directly; EOS is
not required. Clients send a device fingerprint in NGO connection approval,
and the server approves them only after the Manager creates or finds their
player record. The server then includes connected player IDs in its heartbeat.

The client can get its Unity device fingerprint by calling
`VeilNetManagerHeartbeat.TryGetDeviceFingerprint(out var fingerprint)`.
Send that fingerprint to the dedicated server as part of the Netcode for GameObjects
connection approval payload; clients must not call the Manager. The server calls
`IdentifyPlayerByDeviceFingerprint(fingerprint, displayName, callback)`. For example:

```csharp
managerHeartbeat.IdentifyPlayerByDeviceFingerprint(fingerprint, displayName, result =>
{
    if (!result.success)
    {
        RejectConnection(result.error);
        return;
    }

    AcceptConnection(result.userId, result.username);
    managerHeartbeat.PlayerJoined(result.userId);
});
```

The Manager automatically creates a player record the first time it sees a
device fingerprint and returns the same generated player ID on later
connections. The Manager stores only a keyed HMAC-derived player ID, not the
fingerprint or raw Unity device identifier. This is convenience identity, not
strong authentication: `SystemInfo.deviceUniqueIdentifier` is
platform-dependent and can be spoofed or reset, and a modified client can send
another fingerprint. Do not use it to protect purchases or other valuable
accounts; use a trusted account/anti-cheat provider for that. Disclose device
identification to players and use HTTPS remotely. Connect authoritative
networking callbacks to `PlayerJoined(playerId)` and
`PlayerLeft(playerId)`, or periodically call `SetConnectedPlayers` with the
complete current player ID list. Banned or over-capacity player snapshots are
rejected. The Manager sets `VEILNET_SERVER_PORT` for Unity launches and the
component exposes its value as `ServerPort`; configure your networking
transport to listen on that port. Legacy `-port=<port>` parsing remains as a
fallback.

Run from `Modules/Manager`:

```powershell
uv sync
uv run uvicorn main:app --host 127.0.0.1 --port 8000
```

The API defaults to `127.0.0.1:8000` when started with the command above.
Use HTTPS and a trusted reverse proxy before allowing remote access. Keep both
tokens on trusted servers; never embed them in a game client. Start/stop
controls let an admin token launch a process with the Manager's operating
system permissions. Keep the Manager bound to loopback unless remote access is
required, and run it under a restricted operating-system account.

## API

All admin endpoints require `Authorization: Bearer <VEILNET_ADMIN_TOKEN>`.
All game-server endpoints require `Authorization: Bearer <VEILNET_GAME_TOKEN>`.
The unauthenticated `GET /status` checks both SQLite and Redis.

### Admin

- `POST /api/v1/admin/players` — create `{"user_id":"...","username":"..."}`.
- `GET /api/v1/admin/players?limit=50&offset=0` — list players and presence.
- Add `search=<text>` to filter by username or player ID.
- `GET /api/v1/admin/overview` — get player, ban, and online server counts for
  the management portal.
- `GET /api/v1/admin/players/{user_id}` — get a player.
- `PATCH /api/v1/admin/players/{user_id}` — update `username`.
- `DELETE /api/v1/admin/players/{user_id}` — delete an offline player.
- `POST /api/v1/admin/players/{user_id}/ban` — ban with `{"reason":"..."}`.
- `DELETE /api/v1/admin/players/{user_id}/ban` — unban.
- `POST /api/v1/admin/servers` — register a server with its ID, name, engine
  (`"unreal"` or `"unity"`; defaults to `"unreal"`), host, port, and player
  capacity.
- `GET /api/v1/admin/servers` and `GET /api/v1/admin/servers/{server_id}` —
  list or inspect servers, heartbeat status, and online player counts.
- `GET /api/v1/admin/processes` — inspect processes launched by this Manager
  instance and whether local launching is configured.
- `POST /api/v1/admin/servers/{server_id}/start` and
  `POST /api/v1/admin/servers/{server_id}/stop` — explicitly start or stop a
  registered local dedicated-server process.
- `GET /api/v1/admin/servers/{server_id}/logs` — read up to the latest 200
  output lines captured from a process launched by this Manager.
- `PATCH /api/v1/admin/servers/{server_id}` — update server details, including
  `server_engine` (`"unreal"` or `"unity"`). Stop a managed process before
  changing its engine.
- `DELETE /api/v1/admin/servers/{server_id}` — remove a server and clear its
  player assignments.

### Game server

- `POST /api/v1/game/auth/device` — identify or automatically create a player
  from `{"device_fingerprint":"<64 hex characters>","username":"Pilot"}` using
  the game-server token. `username` is optional; the response contains the
  stable player ID and registered username. The Manager uses
  `VEILNET_MANAGER_IDENTITY_KEY` to derive IDs without storing the fingerprint.
  Forward client device fingerprints only through your authenticated game
  server; use HTTPS when it connects remotely.
- `POST /api/v1/game/servers/{server_id}/heartbeat` — renew the server
  heartbeat and reconcile its connected players using
  `{"player_ids":["player-1","player-2"]}`. Send the complete current player
  list at least once every 90 seconds; an empty list means no players are
  connected.
- `POST /api/v1/game/servers/{server_id}/players/{user_id}/join` — record a
  player joining. The server must be registered and have sent a heartbeat.
- `POST /api/v1/game/servers/{server_id}/players/{user_id}/leave` — record a
  player leaving.

The heartbeat snapshot is authoritative: it removes players omitted from that
server's next heartbeat and rejects banned players or players still active on
another server. Redis keys expire after 90 seconds without a heartbeat. Player
IDs must come from a trusted server-side identity system; these endpoints do
not authenticate individual players.

Interactive API documentation is available at `/docs` while the service is
running. The Manager management portal is served at
`http://127.0.0.1:8000/`; sign in with
`VEILNET_ADMIN_TOKEN` to review the overview, search and manage players, and
register, update, start, or stop game servers. The matching engine's packaged
dedicated-server executable must be configured before Start is enabled. The
Manager supports at most four active processes, does not restart them automatically,
and stops child processes during Manager shutdown. Process state is held in
memory, so run one Uvicorn worker and do not expect this Manager instance to
adopt processes after it restarts. Starting a process does not by itself make
it report a Redis heartbeat; the packaged game server still needs the Manager
heartbeat integration. The portal stores the admin token only in the current
browser tab's session storage and calls this Manager's API on the same origin;
it does not use the separate .NET moderation service.