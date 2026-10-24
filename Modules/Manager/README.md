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
VEILNET_MANAGER_SERVER_EXECUTABLE=
VEILNET_MANAGER_SERVER_MAP=/Game/Maps/Lobby
```

The Manager loads `.ENV` at startup without overriding variables already set
in the process environment. `.ENV` is ignored by Git. Keep the two tokens
different and never put them in a game client. The executable setting is
intentionally blank until you package an Unreal dedicated server. Then set it
to that executable's absolute path; for example:

```dotenv
VEILNET_MANAGER_SERVER_EXECUTABLE=C:\Builds\VeilNetServer.exe
VEILNET_MANAGER_SERVER_MAP=/Game/Maps/Lobby
```

The map setting is passed as the first Unreal command-line argument; the
Manager adds `-Port=<registered server port>` and `-log`. Change the map to one
that exists in your packaged project. The Manager launches only this configured
executable; the browser cannot submit executable paths or commands.

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
- `POST /api/v1/admin/servers` — register a server with its ID, name, host,
  port, and player capacity.
- `GET /api/v1/admin/servers` and `GET /api/v1/admin/servers/{server_id}` —
  list or inspect servers, heartbeat status, and online player counts.
- `GET /api/v1/admin/processes` — inspect processes launched by this Manager
  instance and whether local launching is configured.
- `POST /api/v1/admin/servers/{server_id}/start` and
  `POST /api/v1/admin/servers/{server_id}/stop` — explicitly start or stop a
  registered local dedicated-server process.
- `GET /api/v1/admin/servers/{server_id}/logs` — read up to the latest 200
  output lines captured from a process launched by this Manager.
- `PATCH /api/v1/admin/servers/{server_id}` — update server details.
- `DELETE /api/v1/admin/servers/{server_id}` — remove a server and clear its
  player assignments.

### Game server

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
register, update, start, or stop game servers. A packaged Unreal dedicated
server executable must be configured before Start is enabled. The Manager
supports at most four active processes, does not restart them automatically,
and stops child processes during Manager shutdown. Process state is held in
memory, so run one Uvicorn worker and do not expect this Manager instance to
adopt processes after it restarts. Starting a process does not by itself make
it report a Redis heartbeat; the packaged game server still needs the Manager
heartbeat integration. The portal stores the admin token only in the current
browser tab's session storage and calls this Manager's API on the same origin;
it does not use the separate .NET moderation service.