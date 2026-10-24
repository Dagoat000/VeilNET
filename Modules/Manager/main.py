import asyncio
import logging
import os
import secrets
import sqlite3
import subprocess
import time
from collections import deque
from collections.abc import Generator
from contextlib import asynccontextmanager, contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Annotated
import sys
import redis
from dotenv import load_dotenv
from fastapi import Depends, FastAPI, HTTPException, Query, Request, Response
from fastapi.responses import FileResponse
from fastapi.security import HTTPAuthorizationCredentials, HTTPBearer
from pydantic import BaseModel, Field
import uvicorn

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)

if getattr(sys, "frozen", False):
    APP_DIR = Path(sys.executable).resolve().parent
else:
    APP_DIR = Path(__file__).resolve().parent

ENV_FILE = APP_DIR / ".ENV"

if not ENV_FILE.is_file():
    raise RuntimeError(f"Environment file not found: {ENV_FILE}")

load_dotenv(ENV_FILE, override=True)

DATABASE_PATH = Path(
    os.environ.get(
        "VEILNET_MANAGER_DB_PATH",
        str(Path(__file__).with_name("manager.db")),
    )
)
PRESENCE_TTL_SECONDS = 90
MANAGED_SERVER_LIMIT = 4
DEFAULT_SERVER_MAP = os.environ.get(
    "VEILNET_MANAGER_SERVER_MAP",
    "/Game/Maps/Lobby",
)
redis_client = redis.Redis.from_url(
    os.environ.get("VEILNET_MANAGER_REDIS_URL", "redis://localhost:6379/0"),
    decode_responses=True,
    socket_connect_timeout=2,
    socket_timeout=2,
)
bearer_auth = HTTPBearer(auto_error=False)
managed_processes: dict[str, "ManagedServerProcess"] = {}
process_lock = asyncio.Lock()


@dataclass
class ManagedServerProcess:
    server_id: str
    server_port: int
    process: asyncio.subprocess.Process
    started_at: int
    output: deque[str] = field(default_factory=lambda: deque(maxlen=200))
    monitor_task: asyncio.Task[None] | None = None
    exit_code: int | None = None
    status: str = "starting"
    stop_requested: bool = False


def configured_server_executable() -> Path | None:
    configured_path = os.environ.get("VEILNET_MANAGER_SERVER_EXECUTABLE", "").strip()
    return Path(configured_path).expanduser() if configured_path else None


def server_executable_available() -> bool:
    executable = configured_server_executable()
    try:
        return executable is not None and executable.is_file()
    except (OSError, ValueError):
        return False


def managed_process_response(server_id: str) -> dict[str, object]:
    record = managed_processes.get(server_id)
    if record is None:
        return {
            "status": "not_started",
            "pid": None,
            "started_at": None,
            "exit_code": None,
        }
    return {
        "status": record.status,
        "pid": record.process.pid,
        "started_at": record.started_at,
        "exit_code": record.exit_code,
    }


async def monitor_server_process(record: ManagedServerProcess) -> None:
    stream = record.process.stdout
    if stream is not None:
        while line := await stream.readline():
            record.output.append(line.decode("utf-8", errors="replace").rstrip())
    exit_code = await record.process.wait()
    record.exit_code = exit_code
    record.status = (
        "stopped"
        if record.stop_requested or exit_code == 0
        else "failed"
    )
    logger.info(
        "Managed game server %s exited with code %s",
        record.server_id,
        exit_code,
    )


async def stop_managed_process(record: ManagedServerProcess) -> None:
    if record.process.returncode is None:
        record.stop_requested = True
        record.status = "stopping"
        try:
            record.process.terminate()
            await asyncio.wait_for(record.process.wait(), timeout=10)
        except ProcessLookupError:
            await record.process.wait()
        except TimeoutError:
            logger.warning(
                "Managed game server %s did not stop; killing it",
                record.server_id,
            )
            record.process.kill()
            await record.process.wait()
    if record.monitor_task is not None:
        await record.monitor_task


class PlayerCreate(BaseModel):
    username: str = Field(min_length=3, max_length=20)
    user_id: str = Field(min_length=1, max_length=50)


class PlayerUpdate(BaseModel):
    username: str | None = Field(default=None, min_length=3, max_length=20)


class BanRequest(BaseModel):
    reason: str = Field(min_length=1, max_length=500)


class ServerCreate(BaseModel):
    server_id: str = Field(min_length=1, max_length=50)
    server_name: str = Field(min_length=3, max_length=50)
    server_max_players: int = Field(default=100, ge=1, le=10000)
    server_ip: str = Field(min_length=1, max_length=253)
    server_port: int = Field(default=7777, ge=1, le=65535)


class ServerUpdate(BaseModel):
    server_name: str | None = Field(default=None, min_length=3, max_length=50)
    server_max_players: int | None = Field(default=None, ge=1, le=10000)
    server_ip: str | None = Field(default=None, min_length=1, max_length=253)
    server_port: int | None = Field(default=None, ge=1, le=65535)


class HeartbeatRequest(BaseModel):
    player_ids: list[str] = Field(default_factory=list, max_length=10000)


@contextmanager
def database() -> Generator[sqlite3.Connection, None, None]:
    connection = sqlite3.connect(DATABASE_PATH, timeout=10)
    try:
        connection.row_factory = sqlite3.Row
        connection.execute("PRAGMA foreign_keys = ON")
        with connection:
            yield connection
    finally:
        connection.close()


def initialize_database() -> None:
    DATABASE_PATH.parent.mkdir(parents=True, exist_ok=True)
    with database() as connection:
        connection.executescript(
            """
            CREATE TABLE IF NOT EXISTS servers (
                server_id TEXT PRIMARY KEY,
                server_name TEXT NOT NULL,
                server_max_players INTEGER NOT NULL CHECK (server_max_players > 0),
                server_ip TEXT NOT NULL,
                server_port INTEGER NOT NULL CHECK (server_port BETWEEN 1 AND 65535),
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            );

            CREATE TABLE IF NOT EXISTS players (
                user_id TEXT PRIMARY KEY,
                username TEXT NOT NULL,
                is_banned INTEGER NOT NULL DEFAULT 0 CHECK (is_banned IN (0, 1)),
                ban_reason TEXT NOT NULL DEFAULT '',
                ban_timestamp INTEGER NOT NULL DEFAULT 0,
                which_server TEXT REFERENCES servers(server_id) ON DELETE SET NULL,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            );

            CREATE INDEX IF NOT EXISTS ix_players_which_server
                ON players(which_server);
            """
        )


@asynccontextmanager
async def lifespan(_: FastAPI):
    initialize_database()
    try:
        yield
    finally:
        for record in managed_processes.values():
            if record.process.returncode is None:
                await stop_managed_process(record)


app = FastAPI(
    title="VeilNet Server Manager",
    description="Authenticated player and game-server management API.",
    version="1.0.0",
    lifespan=lifespan,
)
PORTAL_FILE = Path(__file__).with_name("static") / "index.html"


def require_token(
    credentials: Annotated[
        HTTPAuthorizationCredentials | None,
        Depends(bearer_auth),
    ],
    environment_variable: str,
) -> None:
    expected_token = os.environ.get(environment_variable)
    if not expected_token:
        raise HTTPException(
            status_code=503,
            detail=f"{environment_variable} is not configured.",
        )
    if (
        credentials is None
        or credentials.scheme.lower() != "bearer"
        or not secrets.compare_digest(credentials.credentials, expected_token)
    ):
        raise HTTPException(status_code=401, detail="Invalid or missing bearer token.")


def require_admin(
    credentials: Annotated[
        HTTPAuthorizationCredentials | None,
        Depends(bearer_auth),
    ],
) -> None:
    require_token(credentials, "VEILNET_ADMIN_TOKEN")


def require_game_server(
    credentials: Annotated[
        HTTPAuthorizationCredentials | None,
        Depends(bearer_auth),
    ],
) -> None:
    require_token(credentials, "VEILNET_GAME_TOKEN")


def player_presence_key(user_id: str) -> str:
    return f"veilnet:manager:player:{user_id}"


def server_heartbeat_key(server_id: str) -> str:
    return f"veilnet:manager:server:{server_id}:heartbeat"


def decode_redis_text(value: str | bytes | None) -> str | None:
    return value.decode("utf-8") if isinstance(value, bytes) else value


def ensure_redis_available() -> None:
    try:
        redis_client.ping()
    except redis.RedisError as error:
        logger.exception("Redis is unavailable")
        raise HTTPException(
            status_code=503,
            detail="Redis is unavailable; presence operations cannot be completed.",
        ) from error


def player_response(
    player: sqlite3.Row,
    presence_server_id: str | None,
) -> dict[str, object]:
    is_online = (
        player["which_server"] is not None
        and presence_server_id == player["which_server"]
    )
    return {
        "user_id": player["user_id"],
        "username": player["username"],
        "is_banned": bool(player["is_banned"]),
        "ban_reason": player["ban_reason"],
        "ban_timestamp": player["ban_timestamp"],
        "is_online": is_online,
        "is_in_server": is_online,
        "which_server": player["which_server"] if is_online else None,
        "created_at": player["created_at"],
        "updated_at": player["updated_at"],
    }


def get_player_response(player: sqlite3.Row) -> dict[str, object]:
    try:
        presence_server_id = decode_redis_text(
            redis_client.get(player_presence_key(player["user_id"]))
        )
        if presence_server_id and not redis_client.exists(
            server_heartbeat_key(presence_server_id)
        ):
            presence_server_id = None
    except redis.RedisError as error:
        logger.exception("Unable to read player presence from Redis")
        raise HTTPException(
            status_code=503, detail="Player presence is unavailable."
        ) from error
    return player_response(player, presence_server_id)


def server_response(
    server: sqlite3.Row,
    presence_by_player: list[str | None],
    online: bool,
) -> dict[str, object]:
    online_count = sum(
        1
        for presence_server_id in presence_by_player
        if online and presence_server_id == server["server_id"]
    )
    return {
        "server_id": server["server_id"],
        "server_name": server["server_name"],
        "server_max_players": server["server_max_players"],
        "server_ip": server["server_ip"],
        "server_port": server["server_port"],
        "is_online": online,
        "online_players": online_count,
        "process": managed_process_response(server["server_id"]),
        "created_at": server["created_at"],
        "updated_at": server["updated_at"],
    }


def get_server_response(server: sqlite3.Row) -> dict[str, object]:
    with database() as connection:
        players = connection.execute(
            "SELECT user_id, which_server FROM players WHERE which_server = ?",
            (server["server_id"],),
        ).fetchall()
    try:
        online = bool(redis_client.exists(server_heartbeat_key(server["server_id"])))
        presence = (
            [
                decode_redis_text(value)
                for value in redis_client.mget(
                    [player_presence_key(player["user_id"]) for player in players]
                )
            ]
            if players
            else []
        )
    except redis.RedisError as error:
        logger.exception("Unable to read server presence from Redis")
        raise HTTPException(
            status_code=503, detail="Server presence is unavailable."
        ) from error
    return server_response(server, presence, online)


def get_required_player(
    connection: sqlite3.Connection,
    user_id: str,
) -> sqlite3.Row:
    player = connection.execute(
        "SELECT * FROM players WHERE user_id = ?",
        (user_id,),
    ).fetchone()
    if player is None:
        raise HTTPException(status_code=404, detail="Player not found.")
    return player


def get_required_server(
    connection: sqlite3.Connection,
    server_id: str,
) -> sqlite3.Row:
    server = connection.execute(
        "SELECT * FROM servers WHERE server_id = ?",
        (server_id,),
    ).fetchone()
    if server is None:
        raise HTTPException(status_code=404, detail="Server not found.")
    return server


@app.middleware("http")
async def add_latency_header(request: Request, call_next):
    start_time = time.perf_counter()
    response = await call_next(request)
    process_time = time.perf_counter() - start_time
    response.headers["X-Process-Time"] = f"{process_time:.4f}s"
    logger.info("Path: %s | Latency: %.4fs", request.url.path, process_time)
    return response


@app.get("/status")
def status() -> dict[str, str]:
    with database() as connection:
        connection.execute("SELECT 1").fetchone()
    try:
        redis_client.ping()
    except redis.RedisError as error:
        logger.warning("Health check: Redis is unavailable: %s", error)
        raise HTTPException(
            status_code=503,
            detail={"status": "degraded", "database": "ok", "redis": "unavailable"},
        ) from error
    return {"status": "ok", "database": "ok", "redis": "ok"}


@app.get("/", include_in_schema=False)
def management_portal() -> FileResponse:
    return FileResponse(PORTAL_FILE)


@app.get("/api/v1/admin/overview", dependencies=[Depends(require_admin)])
def admin_overview() -> dict[str, int]:
    with database() as connection:
        player_count = connection.execute("SELECT COUNT(*) FROM players").fetchone()[0]
        banned_count = connection.execute(
            "SELECT COUNT(*) FROM players WHERE is_banned = 1"
        ).fetchone()[0]
        assigned_players = connection.execute(
            "SELECT user_id, which_server FROM players WHERE which_server IS NOT NULL"
        ).fetchall()
        servers = connection.execute("SELECT server_id FROM servers").fetchall()
    try:
        player_presence = (
            [
                decode_redis_text(value)
                for value in redis_client.mget(
                    [
                        player_presence_key(player["user_id"])
                        for player in assigned_players
                    ]
                )
            ]
            if assigned_players
            else []
        )
        heartbeats = (
            [
                decode_redis_text(value)
                for value in redis_client.mget(
                    [server_heartbeat_key(server["server_id"]) for server in servers]
                )
            ]
            if servers
            else []
        )
    except redis.RedisError as error:
        logger.exception("Unable to load admin overview from Redis")
        raise HTTPException(
            status_code=503, detail="Manager overview is unavailable."
        ) from error
    online_server_ids = {
        server["server_id"]
        for server, heartbeat in zip(servers, heartbeats)
        if heartbeat is not None
    }
    online_players = sum(
        1
        for player, presence_server_id in zip(assigned_players, player_presence)
        if presence_server_id == player["which_server"]
        and presence_server_id in online_server_ids
    )
    return {
        "players": player_count,
        "players_online": online_players,
        "players_banned": banned_count,
        "servers": len(servers),
        "servers_online": sum(heartbeat is not None for heartbeat in heartbeats),
    }


@app.post(
    "/api/v1/admin/players", status_code=201, dependencies=[Depends(require_admin)]
)
def create_player(body: PlayerCreate) -> dict[str, object]:
    now = int(time.time())
    try:
        with database() as connection:
            connection.execute(
                """
                INSERT INTO players (
                    user_id, username, created_at, updated_at
                ) VALUES (?, ?, ?, ?)
                """,
                (body.user_id, body.username, now, now),
            )
            player = get_required_player(connection, body.user_id)
    except sqlite3.IntegrityError as error:
        raise HTTPException(
            status_code=409, detail="A player with that user_id already exists."
        ) from error
    return get_player_response(player)


@app.get("/api/v1/admin/players", dependencies=[Depends(require_admin)])
def list_players(
    limit: int = Query(default=50, ge=1, le=200),
    offset: int = Query(default=0, ge=0),
    search: str | None = Query(default=None, max_length=100),
) -> dict[str, object]:
    search_term = search.strip() if search else ""
    with database() as connection:
        if search_term:
            pattern = f"%{search_term}%"
            total = connection.execute(
                """
                SELECT COUNT(*) FROM players
                WHERE user_id LIKE ? OR username LIKE ?
                """,
                (pattern, pattern),
            ).fetchone()[0]
            players = connection.execute(
                """
                SELECT * FROM players
                WHERE user_id LIKE ? OR username LIKE ?
                ORDER BY created_at DESC, user_id
                LIMIT ? OFFSET ?
                """,
                (pattern, pattern, limit, offset),
            ).fetchall()
        else:
            total = connection.execute("SELECT COUNT(*) FROM players").fetchone()[0]
            players = connection.execute(
                "SELECT * FROM players ORDER BY created_at DESC, user_id LIMIT ? OFFSET ?",
                (limit, offset),
            ).fetchall()
        servers = connection.execute("SELECT server_id FROM servers").fetchall()
    try:
        presence = (
            [
                decode_redis_text(value)
                for value in redis_client.mget(
                    [player_presence_key(player["user_id"]) for player in players]
                )
            ]
            if players
            else []
        )
        heartbeats = (
            redis_client.mget(
                [server_heartbeat_key(server["server_id"]) for server in servers]
            )
            if players and servers
            else []
        )
        active_server_ids = {
            server["server_id"]
            for server, heartbeat in zip(servers, heartbeats)
            if heartbeat is not None
        }
    except redis.RedisError as error:
        logger.exception("Unable to list player presence from Redis")
        raise HTTPException(
            status_code=503, detail="Player presence is unavailable."
        ) from error
    return {
        "items": [
            player_response(
                player,
                presence_server_id if presence_server_id in active_server_ids else None,
            )
            for player, presence_server_id in zip(players, presence)
        ],
        "total": total,
        "limit": limit,
        "offset": offset,
    }


@app.get(
    "/api/v1/admin/players/{user_id}",
    dependencies=[Depends(require_admin)],
)
def read_player(user_id: str) -> dict[str, object]:
    with database() as connection:
        player = get_required_player(connection, user_id)
    return get_player_response(player)


@app.patch(
    "/api/v1/admin/players/{user_id}",
    dependencies=[Depends(require_admin)],
)
def update_player(user_id: str, body: PlayerUpdate) -> dict[str, object]:
    changes = body.model_dump(exclude_unset=True)
    if not changes:
        raise HTTPException(
            status_code=400, detail="Provide at least one field to update."
        )
    if changes.get("username") is None:
        raise HTTPException(status_code=400, detail="username cannot be null.")
    with database() as connection:
        player = get_required_player(connection, user_id)
        connection.execute(
            "UPDATE players SET username = ?, updated_at = ? WHERE user_id = ?",
            (changes["username"], int(time.time()), user_id),
        )
        player = get_required_player(connection, user_id)
    return get_player_response(player)


@app.delete(
    "/api/v1/admin/players/{user_id}",
    status_code=204,
    dependencies=[Depends(require_admin)],
)
def delete_player(user_id: str) -> Response:
    ensure_redis_available()
    with database() as connection:
        player = get_required_player(connection, user_id)
        if player["which_server"] is not None:
            try:
                is_online = (
                    decode_redis_text(redis_client.get(player_presence_key(user_id)))
                    == player["which_server"]
                )
            except redis.RedisError as error:
                raise HTTPException(
                    status_code=503, detail="Player presence is unavailable."
                ) from error
            if is_online:
                raise HTTPException(
                    status_code=409,
                    detail="Player must leave the server before deletion.",
                )
        connection.execute("DELETE FROM players WHERE user_id = ?", (user_id,))
    try:
        redis_client.delete(player_presence_key(user_id))
    except redis.RedisError as error:
        logger.exception("Unable to clear deleted player's Redis presence")
        raise HTTPException(
            status_code=503, detail="Player was deleted, but presence cleanup failed."
        ) from error
    return Response(status_code=204)


@app.post(
    "/api/v1/admin/players/{user_id}/ban",
    dependencies=[Depends(require_admin)],
)
def ban_player(user_id: str, body: BanRequest) -> dict[str, object]:
    reason = body.reason.strip()
    if not reason:
        raise HTTPException(status_code=422, detail="Ban reason cannot be blank.")
    now = int(time.time())
    with database() as connection:
        get_required_player(connection, user_id)
        connection.execute(
            """
            UPDATE players
            SET is_banned = 1, ban_reason = ?, ban_timestamp = ?,
                which_server = NULL, updated_at = ?
            WHERE user_id = ?
            """,
            (reason, now, now, user_id),
        )
        player = get_required_player(connection, user_id)
    try:
        redis_client.delete(player_presence_key(user_id))
    except redis.RedisError as error:
        logger.exception("Player was banned, but Redis presence cleanup failed")
        raise HTTPException(
            status_code=503, detail="Player was banned, but presence cleanup failed."
        ) from error
    return player_response(player, None)


@app.delete(
    "/api/v1/admin/players/{user_id}/ban",
    dependencies=[Depends(require_admin)],
)
def unban_player(user_id: str) -> dict[str, object]:
    now = int(time.time())
    with database() as connection:
        get_required_player(connection, user_id)
        connection.execute(
            """
            UPDATE players
            SET is_banned = 0, ban_reason = '', ban_timestamp = 0, updated_at = ?
            WHERE user_id = ?
            """,
            (now, user_id),
        )
        player = get_required_player(connection, user_id)
    return get_player_response(player)


@app.post(
    "/api/v1/admin/servers", status_code=201, dependencies=[Depends(require_admin)]
)
def create_server(body: ServerCreate) -> dict[str, object]:
    now = int(time.time())
    try:
        with database() as connection:
            connection.execute(
                """
                INSERT INTO servers (
                    server_id, server_name, server_max_players, server_ip,
                    server_port, created_at, updated_at
                ) VALUES (?, ?, ?, ?, ?, ?, ?)
                """,
                (
                    body.server_id,
                    body.server_name,
                    body.server_max_players,
                    body.server_ip,
                    body.server_port,
                    now,
                    now,
                ),
            )
            server = get_required_server(connection, body.server_id)
    except sqlite3.IntegrityError as error:
        raise HTTPException(
            status_code=409, detail="A server with that server_id already exists."
        ) from error
    return get_server_response(server)


@app.get("/api/v1/admin/servers", dependencies=[Depends(require_admin)])
def list_servers() -> dict[str, object]:
    with database() as connection:
        servers = connection.execute(
            "SELECT * FROM servers ORDER BY created_at DESC, server_id"
        ).fetchall()
        assigned_players = connection.execute(
            "SELECT user_id, which_server FROM players WHERE which_server IS NOT NULL"
        ).fetchall()
    try:
        heartbeats = (
            [
                decode_redis_text(value)
                for value in redis_client.mget(
                    [server_heartbeat_key(server["server_id"]) for server in servers]
                )
            ]
            if servers
            else []
        )
        presence = (
            [
                decode_redis_text(value)
                for value in redis_client.mget(
                    [
                        player_presence_key(player["user_id"])
                        for player in assigned_players
                    ]
                )
            ]
            if assigned_players
            else []
        )
    except redis.RedisError as error:
        logger.exception("Unable to list server presence from Redis")
        raise HTTPException(
            status_code=503, detail="Server presence is unavailable."
        ) from error
    presence_by_server: dict[str, list[str | None]] = {}
    for player, player_presence in zip(assigned_players, presence):
        server_id = player["which_server"]
        presence_by_server.setdefault(server_id, []).append(player_presence)
    return {
        "items": [
            server_response(
                server,
                presence_by_server.get(server["server_id"], []),
                heartbeat is not None,
            )
            for server, heartbeat in zip(servers, heartbeats)
        ],
        "total": len(servers),
        "launch_available": server_executable_available(),
    }


@app.get("/api/v1/admin/processes", dependencies=[Depends(require_admin)])
async def list_managed_processes() -> dict[str, object]:
    async with process_lock:
        items = [
            {"server_id": server_id, **managed_process_response(server_id)}
            for server_id in managed_processes
        ]
    return {
        "items": items,
        "running": sum(item["status"] in {"starting", "running", "stopping"} for item in items),
        "limit": MANAGED_SERVER_LIMIT,
        "launch_available": server_executable_available(),
        "map": DEFAULT_SERVER_MAP,
    }


@app.post(
    "/api/v1/admin/servers/{server_id}/start",
    status_code=202,
    dependencies=[Depends(require_admin)],
)
async def start_server_process(server_id: str) -> dict[str, object]:
    executable_path = configured_server_executable()
    if executable_path is None:
        raise HTTPException(
            status_code=503,
            detail=(
                "Dedicated server executable is not configured. Set "
                "VEILNET_MANAGER_SERVER_EXECUTABLE in the Manager .ENV after "
                "packaging the Unreal server."
            ),
        )
    try:
        executable = executable_path.resolve(strict=True)
    except (OSError, RuntimeError) as error:
        logger.error("Configured dedicated server executable is unavailable: %s", error)
        raise HTTPException(
            status_code=503,
            detail="Configured dedicated server executable was not found or is inaccessible.",
        ) from error
    if not executable.is_file():
        raise HTTPException(
            status_code=503,
            detail="Configured dedicated server executable is not a file.",
        )
    if not DEFAULT_SERVER_MAP.startswith("/") or any(
        character in DEFAULT_SERVER_MAP for character in ("\0", "\r", "\n")
    ):
        raise HTTPException(
            status_code=503,
            detail="VEILNET_MANAGER_SERVER_MAP must be an Unreal map path beginning with '/'.",
        )

    async with process_lock:
        with database() as connection:
            server = get_required_server(connection, server_id)
            server_port = server["server_port"]
        existing = managed_processes.get(server_id)
        if existing is not None and existing.process.returncode is None:
            raise HTTPException(
                status_code=409,
                detail="This server already has a managed process running.",
            )
        active_records = [
            record
            for record in managed_processes.values()
            if record.process.returncode is None
        ]
        if len(active_records) >= MANAGED_SERVER_LIMIT:
            raise HTTPException(
                status_code=409,
                detail=f"The Manager limit of {MANAGED_SERVER_LIMIT} active servers has been reached.",
            )
        if any(record.server_port == server_port for record in active_records):
            raise HTTPException(
                status_code=409,
                detail=f"Port {server_port} is already assigned to a managed process.",
            )
        try:
            process = await asyncio.create_subprocess_exec(
                str(executable),
                DEFAULT_SERVER_MAP,
                f"-Port={server_port}",
                "-log",
                stdin=asyncio.subprocess.DEVNULL,
                stdout=asyncio.subprocess.PIPE,
                stderr=subprocess.STDOUT,
                cwd=executable.parent,
            )
        except OSError as error:
            logger.exception("Unable to launch dedicated server %s", server_id)
            raise HTTPException(
                status_code=503,
                detail="The dedicated server process could not be started.",
            ) from error
        record = ManagedServerProcess(
            server_id=server_id,
            server_port=server_port,
            process=process,
            started_at=int(time.time()),
        )
        managed_processes[server_id] = record
        record.monitor_task = asyncio.create_task(monitor_server_process(record))
        logger.info(
            "Started managed game server %s with PID %s on port %s",
            server_id,
            process.pid,
            server_port,
        )
    return {"server_id": server_id, **managed_process_response(server_id)}


@app.post(
    "/api/v1/admin/servers/{server_id}/stop",
    dependencies=[Depends(require_admin)],
)
async def stop_server_process(server_id: str) -> dict[str, object]:
    async with process_lock:
        record = managed_processes.get(server_id)
        if record is None or record.process.returncode is not None:
            raise HTTPException(
                status_code=409,
                detail="This server has no active process managed by this Manager.",
            )
        await stop_managed_process(record)
    return {"server_id": server_id, **managed_process_response(server_id)}


@app.get(
    "/api/v1/admin/servers/{server_id}/logs",
    dependencies=[Depends(require_admin)],
)
async def read_server_process_logs(
    server_id: str,
    limit: int = Query(default=100, ge=1, le=200),
) -> dict[str, object]:
    with database() as connection:
        get_required_server(connection, server_id)
    async with process_lock:
        record = managed_processes.get(server_id)
        if record is None:
            raise HTTPException(
                status_code=404,
                detail="No process launched by this Manager is available for this server.",
            )
        output = list(record.output)
    return {"server_id": server_id, "items": output[-limit:]}


@app.get(
    "/api/v1/admin/servers/{server_id}",
    dependencies=[Depends(require_admin)],
)
def read_server(server_id: str) -> dict[str, object]:
    with database() as connection:
        server = get_required_server(connection, server_id)
    return get_server_response(server)


@app.patch(
    "/api/v1/admin/servers/{server_id}",
    dependencies=[Depends(require_admin)],
)
async def update_server(server_id: str, body: ServerUpdate) -> dict[str, object]:
    changes = body.model_dump(exclude_unset=True)
    if not changes:
        raise HTTPException(
            status_code=400, detail="Provide at least one field to update."
        )
    if any(value is None for value in changes.values()):
        raise HTTPException(
            status_code=400, detail="Updated server fields cannot be null."
        )
    columns = ", ".join(f"{column} = ?" for column in changes)
    values = [*changes.values(), int(time.time()), server_id]
    async with process_lock:
        with database() as connection:
            get_required_server(connection, server_id)
            process = managed_processes.get(server_id)
            if (
                process is not None
                and process.process.returncode is None
                and changes.get("server_port", process.server_port) != process.server_port
            ):
                raise HTTPException(
                    status_code=409,
                    detail="Stop the managed process before changing its port.",
                )
            connection.execute(
                f"UPDATE servers SET {columns}, updated_at = ? WHERE server_id = ?",
                values,
            )
            server = get_required_server(connection, server_id)
    return get_server_response(server)


@app.delete(
    "/api/v1/admin/servers/{server_id}",
    status_code=204,
    dependencies=[Depends(require_admin)],
)
async def delete_server(server_id: str) -> Response:
    async with process_lock:
        process = managed_processes.get(server_id)
        if process is not None and process.process.returncode is None:
            raise HTTPException(
                status_code=409,
                detail="Stop the managed process before removing this server.",
            )
        with database() as connection:
            get_required_server(connection, server_id)
            players = connection.execute(
                "SELECT user_id FROM players WHERE which_server = ?",
                (server_id,),
            ).fetchall()
            connection.execute("DELETE FROM servers WHERE server_id = ?", (server_id,))
        managed_processes.pop(server_id, None)
    try:
        pipeline = redis_client.pipeline()
        pipeline.delete(server_heartbeat_key(server_id))
        for player in players:
            pipeline.delete(player_presence_key(player["user_id"]))
        pipeline.execute()
    except redis.RedisError as error:
        logger.exception("Server was deleted, but Redis presence cleanup failed")
        raise HTTPException(
            status_code=503, detail="Server was deleted, but presence cleanup failed."
        ) from error
    return Response(status_code=204)


@app.post(
    "/api/v1/game/servers/{server_id}/heartbeat",
    dependencies=[Depends(require_game_server)],
)
def server_heartbeat(
    server_id: str,
    body: HeartbeatRequest,
) -> dict[str, object]:
    if len(set(body.player_ids)) != len(body.player_ids):
        raise HTTPException(
            status_code=422, detail="player_ids must not contain duplicates."
        )
    ensure_redis_available()
    now = int(time.time())
    with database() as connection:
        connection.execute("BEGIN IMMEDIATE")
        server = get_required_server(connection, server_id)
        if len(body.player_ids) > server["server_max_players"]:
            raise HTTPException(
                status_code=409,
                detail="Heartbeat exceeds the server's player capacity.",
            )
        players_by_id: dict[str, sqlite3.Row] = {}
        for user_id in body.player_ids:
            player = get_required_player(connection, user_id)
            if player["is_banned"]:
                raise HTTPException(
                    status_code=409, detail=f"Player {user_id} is banned."
                )
            players_by_id[user_id] = player
        for user_id, player in players_by_id.items():
            current_server = player["which_server"]
            if current_server and current_server != server_id:
                current_presence = decode_redis_text(
                    redis_client.get(player_presence_key(user_id))
                )
                if current_presence == current_server and redis_client.exists(
                    server_heartbeat_key(current_server)
                ):
                    raise HTTPException(
                        status_code=409,
                        detail=f"Player {user_id} is active on another server.",
                    )
        previous = connection.execute(
            "SELECT user_id FROM players WHERE which_server = ?",
            (server_id,),
        ).fetchall()
        absent_ids = [
            player["user_id"]
            for player in previous
            if player["user_id"] not in players_by_id
        ]
        if absent_ids:
            placeholders = ",".join("?" for _ in absent_ids)
            connection.execute(
                f"""
                UPDATE players
                SET which_server = NULL, updated_at = ?
                WHERE user_id IN ({placeholders})
                """,
                [now, *absent_ids],
            )
        for user_id in body.player_ids:
            connection.execute(
                "UPDATE players SET which_server = ?, updated_at = ? WHERE user_id = ?",
                (server_id, now, user_id),
            )
    try:
        pipeline = redis_client.pipeline()
        pipeline.set(
            server_heartbeat_key(server_id),
            "1",
            ex=PRESENCE_TTL_SECONDS,
        )
        for user_id in absent_ids:
            pipeline.delete(player_presence_key(user_id))
        for user_id in body.player_ids:
            pipeline.set(
                player_presence_key(user_id),
                server_id,
                ex=PRESENCE_TTL_SECONDS,
            )
        pipeline.execute()
    except redis.RedisError as error:
        logger.exception("Heartbeat was stored, but Redis presence update failed")
        raise HTTPException(
            status_code=503, detail="Heartbeat was stored, but presence update failed."
        ) from error
    return {
        "server_id": server_id,
        "is_online": True,
        "online_players": len(body.player_ids),
        "heartbeat_ttl_seconds": PRESENCE_TTL_SECONDS,
    }


@app.post(
    "/api/v1/game/servers/{server_id}/players/{user_id}/join",
    dependencies=[Depends(require_game_server)],
)
def player_joined(server_id: str, user_id: str) -> dict[str, object]:
    ensure_redis_available()
    now = int(time.time())
    with database() as connection:
        connection.execute("BEGIN IMMEDIATE")
        server = get_required_server(connection, server_id)
        player = get_required_player(connection, user_id)
        if player["is_banned"]:
            raise HTTPException(
                status_code=403, detail="Banned players cannot join a server."
            )
        if not redis_client.exists(server_heartbeat_key(server_id)):
            raise HTTPException(
                status_code=409,
                detail="Server must send a heartbeat before players join.",
            )
        current_server = player["which_server"]
        if current_server == server_id:
            redis_client.set(
                player_presence_key(user_id),
                server_id,
                ex=PRESENCE_TTL_SECONDS,
            )
            return player_response(player, server_id)
        if current_server:
            current_presence = decode_redis_text(
                redis_client.get(player_presence_key(user_id))
            )
            if current_presence == current_server and redis_client.exists(
                server_heartbeat_key(current_server)
            ):
                raise HTTPException(
                    status_code=409, detail="Player is active on another server."
                )
        assigned = connection.execute(
            "SELECT user_id FROM players WHERE which_server = ?",
            (server_id,),
        ).fetchall()
        assigned_presence = (
            [
                decode_redis_text(value)
                for value in redis_client.mget(
                    [
                        player_presence_key(assigned_player["user_id"])
                        for assigned_player in assigned
                    ]
                )
            ]
            if assigned
            else []
        )
        online_count = sum(1 for value in assigned_presence if value == server_id)
        if online_count >= server["server_max_players"]:
            raise HTTPException(status_code=409, detail="Server is full.")
        connection.execute(
            "UPDATE players SET which_server = ?, updated_at = ? WHERE user_id = ?",
            (server_id, now, user_id),
        )
        player = get_required_player(connection, user_id)
    try:
        redis_client.set(
            player_presence_key(user_id),
            server_id,
            ex=PRESENCE_TTL_SECONDS,
        )
    except redis.RedisError as error:
        logger.exception("Player joined, but Redis presence update failed")
        raise HTTPException(
            status_code=503, detail="Player joined, but presence update failed."
        ) from error
    return player_response(player, server_id)


@app.post(
    "/api/v1/game/servers/{server_id}/players/{user_id}/leave",
    dependencies=[Depends(require_game_server)],
)
def player_left(server_id: str, user_id: str) -> dict[str, object]:
    with database() as connection:
        player = get_required_player(connection, user_id)
        if player["which_server"] == server_id:
            connection.execute(
                "UPDATE players SET which_server = NULL, updated_at = ? WHERE user_id = ?",
                (int(time.time()), user_id),
            )
            player = get_required_player(connection, user_id)
    try:
        presence_server_id = decode_redis_text(
            redis_client.get(player_presence_key(user_id))
        )
        if presence_server_id == server_id:
            redis_client.delete(player_presence_key(user_id))
    except redis.RedisError as error:
        logger.exception("Player left, but Redis presence cleanup failed")
        raise HTTPException(
            status_code=503, detail="Player left, but presence cleanup failed."
        ) from error
    return get_player_response(player)


if __name__ == "__main__":
    uvicorn.run(
        "main:app",
        host="127.0.0.1",
        port=8000,
        workers=1,
        log_level="info",
    )
    