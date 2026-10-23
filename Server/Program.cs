using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Microsoft.Data.Sqlite;

var gameToken = RequiredEnvironmentVariable("VEILNET_GAME_TOKEN");
var adminToken = RequiredEnvironmentVariable("VEILNET_ADMIN_TOKEN");
var hashKey = RequiredEnvironmentVariable("VEILNET_HASH_KEY");

if (gameToken == adminToken)
{
	throw new InvalidOperationException(
		"VEILNET_GAME_TOKEN and VEILNET_ADMIN_TOKEN must be different.");
}

var hashKeyBytes = Encoding.UTF8.GetBytes(hashKey);
if (hashKeyBytes.Length < 32)
{
	throw new InvalidOperationException(
		"VEILNET_HASH_KEY must contain at least 32 UTF-8 bytes.");
}

var eventRetentionDays = ReadEventRetentionDays();
var databasePath = Environment.GetEnvironmentVariable("VEILNET_DB_PATH");
if (string.IsNullOrWhiteSpace(databasePath))
{
	databasePath = Path.Combine(AppContext.BaseDirectory, "veilnet-moderation.db");
}

var fullDatabasePath = Path.GetFullPath(databasePath);
Directory.CreateDirectory(Path.GetDirectoryName(fullDatabasePath)!);
var connectionString = new SqliteConnectionStringBuilder
{
	DataSource = fullDatabasePath,
	Mode = SqliteOpenMode.ReadWriteCreate,
	Cache = SqliteCacheMode.Shared
}.ToString();

await InitializeDatabaseAsync(connectionString);
await PruneExpiredGameEventsAsync(
	connectionString,
	eventRetentionDays,
	CancellationToken.None);

var builder = WebApplication.CreateBuilder(args);
builder.WebHost.UseUrls(
	Environment.GetEnvironmentVariable("VEILNET_LISTEN_URL")
		?? "http://127.0.0.1:5080");

var app = builder.Build();
app.UseDefaultFiles();
app.UseStaticFiles();

app.MapPost("/api/v1/game/check", async (
	HttpRequest request,
	DeviceCheckRequest body,
	CancellationToken cancellationToken) =>
{
	if (!IsAuthorized(request, gameToken))
	{
		return Results.Unauthorized();
	}

	if (!IsValidDeviceFingerprint(body.DeviceFingerprint))
	{
		return Results.BadRequest(new
		{
			error = "deviceFingerprint must be 64 hexadecimal characters."
		});
	}

	var fingerprint = CreateFingerprint(
		body.DeviceFingerprint,
		hashKeyBytes);
	var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();

	await using var connection = await OpenConnectionAsync(
		connectionString,
		cancellationToken);
	await using (var upsert = connection.CreateCommand())
	{
		upsert.CommandText = """
			INSERT INTO devices (fingerprint, first_seen, last_seen)
			VALUES ($fingerprint, $now, $now)
			ON CONFLICT(fingerprint) DO UPDATE SET last_seen = excluded.last_seen;
			""";
		upsert.Parameters.AddWithValue("$fingerprint", fingerprint);
		upsert.Parameters.AddWithValue("$now", now);
		await upsert.ExecuteNonQueryAsync(cancellationToken);
	}

	await using var banQuery = connection.CreateCommand();
	banQuery.CommandText = """
		SELECT reason
		FROM bans
		WHERE fingerprint = $fingerprint
			AND (expires_at IS NULL OR expires_at > $now)
		LIMIT 1;
		""";
	banQuery.Parameters.AddWithValue("$fingerprint", fingerprint);
	banQuery.Parameters.AddWithValue("$now", now);
	var reason = await banQuery.ExecuteScalarAsync(cancellationToken) as string;

	return Results.Ok(new
	{
		fingerprint,
		banned = reason is not null,
		reason
	});
});

app.MapPost("/api/v1/game/events", async (
	HttpRequest request,
	GameAuditEventRequest body,
	CancellationToken cancellationToken) =>
{
	if (!IsAuthorized(request, gameToken))
	{
		return Results.Unauthorized();
	}

	if (body.PlayerId < 0
		|| body.Kind is not ("movement_speed" or "movement_discontinuity")
		|| !double.IsFinite(body.Observed)
		|| !double.IsFinite(body.Allowed)
		|| body.Observed < 0
		|| body.Allowed < 0
		|| body.Observed > 10_000_000
		|| body.Allowed > 10_000_000
		|| body.MovementMode is < 0 or > 7
		|| !double.IsFinite(body.ServerTime)
		|| body.ServerTime < 0
		|| body.ServerTime > 1_000_000_000)
	{
		return Results.BadRequest(new
		{
			error = "Provide a supported event kind, valid player and movement IDs, and finite non-negative observed/allowed values."
		});
	}

	var receivedAt = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
	await using var connection = await OpenConnectionAsync(
		connectionString,
		cancellationToken);
	await using var command = connection.CreateCommand();
	command.CommandText = """
		DELETE FROM game_events
		WHERE received_at < $retentionCutoff;
		INSERT INTO game_events (
			kind, player_id, observed, allowed, movement_mode,
			server_time, received_at
		)
		VALUES (
			$kind, $playerId, $observed, $allowed, $movementMode,
			$serverTime, $receivedAt
		);
		SELECT last_insert_rowid();
		""";
	command.Parameters.AddWithValue("$kind", body.Kind);
	command.Parameters.AddWithValue("$playerId", body.PlayerId);
	command.Parameters.AddWithValue("$observed", body.Observed);
	command.Parameters.AddWithValue("$allowed", body.Allowed);
	command.Parameters.AddWithValue("$movementMode", body.MovementMode);
	command.Parameters.AddWithValue("$serverTime", body.ServerTime);
	command.Parameters.AddWithValue("$receivedAt", receivedAt);
	command.Parameters.AddWithValue(
		"$retentionCutoff",
		receivedAt - eventRetentionDays * 86400L);
	var eventId = (long)(await command.ExecuteScalarAsync(cancellationToken)
		?? throw new InvalidOperationException(
			"Failed to store the game audit event."));

	return Results.Ok(new { id = eventId });
});

app.MapGet("/api/v1/admin/events", async (
	HttpRequest request,
	int? limit,
	int? offset,
	CancellationToken cancellationToken) =>
{
	if (!IsAuthorized(request, adminToken))
	{
		return Results.Unauthorized();
	}

	var pageSize = Math.Clamp(limit ?? 50, 1, 200);
	var pageOffset = Math.Max(offset ?? 0, 0);
	var events = new List<GameAuditEventSummary>();
	await PruneExpiredGameEventsAsync(
		connectionString,
		eventRetentionDays,
		cancellationToken);

	await using var connection = await OpenConnectionAsync(
		connectionString,
		cancellationToken);
	await using var query = connection.CreateCommand();
	query.CommandText = """
		SELECT id, kind, player_id, observed, allowed, movement_mode,
			server_time, received_at
		FROM game_events
		ORDER BY id DESC
		LIMIT $limit OFFSET $offset;
		""";
	query.Parameters.AddWithValue("$limit", pageSize);
	query.Parameters.AddWithValue("$offset", pageOffset);

	await using var reader = await query.ExecuteReaderAsync(cancellationToken);
	while (await reader.ReadAsync(cancellationToken))
	{
		events.Add(new GameAuditEventSummary(
			reader.GetInt64(0),
			reader.GetString(1),
			reader.GetInt32(2),
			reader.GetDouble(3),
			reader.GetDouble(4),
			reader.GetInt32(5),
			reader.GetDouble(6),
			DateTimeOffset.FromUnixTimeSeconds(reader.GetInt64(7))));
	}

	return Results.Ok(events);
});

app.MapGet("/api/v1/admin/devices", async (
	HttpRequest request,
	int? limit,
	int? offset,
	CancellationToken cancellationToken) =>
{
	if (!IsAuthorized(request, adminToken))
	{
		return Results.Unauthorized();
	}

	var pageSize = Math.Clamp(limit ?? 50, 1, 200);
	var pageOffset = Math.Max(offset ?? 0, 0);
	var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
	var devices = new List<DeviceSummary>();

	await using var connection = await OpenConnectionAsync(
		connectionString,
		cancellationToken);
	await using var query = connection.CreateCommand();
	query.CommandText = """
		SELECT d.fingerprint, d.first_seen, d.last_seen,
			CASE WHEN b.fingerprint IS NULL THEN 0 ELSE 1 END,
			b.reason, b.expires_at
		FROM devices d
		LEFT JOIN bans b ON b.fingerprint = d.fingerprint
			AND (b.expires_at IS NULL OR b.expires_at > $now)
		ORDER BY d.last_seen DESC
		LIMIT $limit OFFSET $offset;
		""";
	query.Parameters.AddWithValue("$now", now);
	query.Parameters.AddWithValue("$limit", pageSize);
	query.Parameters.AddWithValue("$offset", pageOffset);

	await using var reader = await query.ExecuteReaderAsync(cancellationToken);
	while (await reader.ReadAsync(cancellationToken))
	{
		devices.Add(new DeviceSummary(
			reader.GetString(0),
			DateTimeOffset.FromUnixTimeSeconds(reader.GetInt64(1)),
			DateTimeOffset.FromUnixTimeSeconds(reader.GetInt64(2)),
			reader.GetInt64(3) == 1,
			reader.IsDBNull(4) ? null : reader.GetString(4),
			reader.IsDBNull(5)
				? null
				: DateTimeOffset.FromUnixTimeSeconds(reader.GetInt64(5))));
	}

	return Results.Ok(devices);
});

app.MapPost("/api/v1/admin/bans", async (
	HttpRequest request,
	CreateBanRequest body,
	CancellationToken cancellationToken) =>
{
	if (!IsAuthorized(request, adminToken))
	{
		return Results.Unauthorized();
	}

	if (!IsValidFingerprint(body.Fingerprint)
		|| string.IsNullOrWhiteSpace(body.Reason)
		|| body.Reason.Length > 500
		|| string.IsNullOrWhiteSpace(body.BannedBy)
		|| body.BannedBy.Length > 100)
	{
		return Results.BadRequest(new
		{
			error = "Provide a 64-character fingerprint, a reason (1-500 characters), and bannedBy (1-100 characters)."
		});
	}

	long? expiresAt = null;
	if (body.ExpiresAt is not null)
	{
		if (!DateTimeOffset.TryParse(
				body.ExpiresAt,
				out var parsedExpiry)
			|| parsedExpiry <= DateTimeOffset.UtcNow)
		{
			return Results.BadRequest(new
			{
				error = "expiresAt must be a valid future date/time."
			});
		}

		expiresAt = parsedExpiry.ToUniversalTime().ToUnixTimeSeconds();
	}

	var fingerprint = body.Fingerprint.ToLowerInvariant();
	var now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
	await using var connection = await OpenConnectionAsync(
		connectionString,
		cancellationToken);
	await using var command = connection.CreateCommand();
	command.CommandText = """
		INSERT INTO bans (fingerprint, reason, banned_by, banned_at, expires_at)
		VALUES ($fingerprint, $reason, $bannedBy, $now, $expiresAt)
		ON CONFLICT(fingerprint) DO UPDATE SET
			reason = excluded.reason,
			banned_by = excluded.banned_by,
			banned_at = excluded.banned_at,
			expires_at = excluded.expires_at;
		""";
	command.Parameters.AddWithValue("$fingerprint", fingerprint);
	command.Parameters.AddWithValue("$reason", body.Reason.Trim());
	command.Parameters.AddWithValue("$bannedBy", body.BannedBy.Trim());
	command.Parameters.AddWithValue("$now", now);
	command.Parameters.AddWithValue(
		"$expiresAt",
		expiresAt is null ? DBNull.Value : expiresAt.Value);
	await command.ExecuteNonQueryAsync(cancellationToken);

	DateTimeOffset? expiryDate = expiresAt is null
		? null
		: DateTimeOffset.FromUnixTimeSeconds(expiresAt.Value);
	return Results.Ok(new
	{
		fingerprint,
		banned = true,
		bannedAt = DateTimeOffset.FromUnixTimeSeconds(now),
		expiresAt = expiryDate
	});
});

app.MapDelete("/api/v1/admin/bans/{fingerprint}", async (
	HttpRequest request,
	string fingerprint,
	CancellationToken cancellationToken) =>
{
	if (!IsAuthorized(request, adminToken))
	{
		return Results.Unauthorized();
	}

	if (!IsValidFingerprint(fingerprint))
	{
		return Results.BadRequest(new
		{
			error = "fingerprint must be 64 hexadecimal characters."
		});
	}

	await using var connection = await OpenConnectionAsync(
		connectionString,
		cancellationToken);
	await using var command = connection.CreateCommand();
	command.CommandText = "DELETE FROM bans WHERE fingerprint = $fingerprint;";
	command.Parameters.AddWithValue(
		"$fingerprint",
		fingerprint.ToLowerInvariant());
	var removed = await command.ExecuteNonQueryAsync(cancellationToken);

	return removed == 0
		? Results.NotFound()
		: Results.NoContent();
});

app.Run();

static string RequiredEnvironmentVariable(string name)
{
	var value = Environment.GetEnvironmentVariable(name);
	if (string.IsNullOrWhiteSpace(value))
	{
		throw new InvalidOperationException(
			$"{name} must be configured before the service starts.");
	}

	return value;
}

static int ReadEventRetentionDays()
{
	var value = Environment.GetEnvironmentVariable(
		"VEILNET_EVENT_RETENTION_DAYS");
	if (string.IsNullOrWhiteSpace(value))
	{
		return 30;
	}

	if (!int.TryParse(value, out var days) || days is < 1 or > 3650)
	{
		throw new InvalidOperationException(
			"VEILNET_EVENT_RETENTION_DAYS must be an integer from 1 to 3650.");
	}

	return days;
}

static bool IsAuthorized(HttpRequest request, string expectedToken)
{
	var authorization = request.Headers.Authorization.ToString();
	const string prefix = "Bearer ";
	if (!authorization.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
	{
		return false;
	}

	var provided = Encoding.UTF8.GetBytes(authorization[prefix.Length..].Trim());
	var expected = Encoding.UTF8.GetBytes(expectedToken);
	return provided.Length == expected.Length
		&& CryptographicOperations.FixedTimeEquals(provided, expected);
}

static bool IsValidDeviceFingerprint(string? fingerprint)
{
	return IsValidFingerprint(fingerprint);
}

static bool IsValidFingerprint(string? fingerprint)
{
	return fingerprint is not null
		&& Regex.IsMatch(fingerprint, "^[0-9a-fA-F]{64}$");
}

static string CreateFingerprint(string deviceFingerprint, byte[] key)
{
	var hash = HMACSHA256.HashData(
		key,
		Encoding.UTF8.GetBytes(deviceFingerprint.ToLowerInvariant()));
	return Convert.ToHexString(hash).ToLowerInvariant();
}

static async Task<SqliteConnection> OpenConnectionAsync(
	string connectionString,
	CancellationToken cancellationToken)
{
	var connection = new SqliteConnection(connectionString);
	await connection.OpenAsync(cancellationToken);
	await using var command = connection.CreateCommand();
	command.CommandText = "PRAGMA foreign_keys = ON; PRAGMA busy_timeout = 5000;";
	await command.ExecuteNonQueryAsync(cancellationToken);
	return connection;
}

static async Task InitializeDatabaseAsync(string connectionString)
{
	await using var connection = new SqliteConnection(connectionString);
	await connection.OpenAsync();
	await using var command = connection.CreateCommand();
	command.CommandText = """
		PRAGMA journal_mode = WAL;
		CREATE TABLE IF NOT EXISTS devices (
			fingerprint TEXT PRIMARY KEY,
			first_seen INTEGER NOT NULL,
			last_seen INTEGER NOT NULL
		);
		CREATE TABLE IF NOT EXISTS bans (
			fingerprint TEXT PRIMARY KEY,
			reason TEXT NOT NULL,
			banned_by TEXT NOT NULL,
			banned_at INTEGER NOT NULL,
			expires_at INTEGER NULL,
			FOREIGN KEY (fingerprint) REFERENCES devices(fingerprint)
		);
		CREATE TABLE IF NOT EXISTS game_events (
			id INTEGER PRIMARY KEY AUTOINCREMENT,
			kind TEXT NOT NULL,
			player_id INTEGER NOT NULL,
			observed REAL NOT NULL,
			allowed REAL NOT NULL,
			movement_mode INTEGER NOT NULL,
			server_time REAL NOT NULL,
			received_at INTEGER NOT NULL
		);
		CREATE INDEX IF NOT EXISTS ix_devices_last_seen
			ON devices(last_seen DESC);
		CREATE INDEX IF NOT EXISTS ix_game_events_received_at
			ON game_events(received_at DESC);
		""";
	await command.ExecuteNonQueryAsync();

	await using var columns = connection.CreateCommand();
	columns.CommandText = "PRAGMA table_info(game_events);";
	var hasServerTime = false;
	await using (var reader = await columns.ExecuteReaderAsync())
	{
		while (await reader.ReadAsync())
		{
			if (reader.GetString(1) == "server_time")
			{
				hasServerTime = true;
				break;
			}
		}
	}

	if (!hasServerTime)
	{
		await using var migration = connection.CreateCommand();
		migration.CommandText = """
			ALTER TABLE game_events
			ADD COLUMN server_time REAL NOT NULL DEFAULT 0;
			""";
		await migration.ExecuteNonQueryAsync();
	}
}

static async Task PruneExpiredGameEventsAsync(
	string connectionString,
	int retentionDays,
	CancellationToken cancellationToken)
{
	var cutoff = DateTimeOffset.UtcNow.ToUnixTimeSeconds()
		- retentionDays * 86400L;
	await using var connection = await OpenConnectionAsync(
		connectionString,
		cancellationToken);
	await using var command = connection.CreateCommand();
	command.CommandText =
		"DELETE FROM game_events WHERE received_at < $cutoff;";
	command.Parameters.AddWithValue("$cutoff", cutoff);
	await command.ExecuteNonQueryAsync(cancellationToken);
}

record DeviceCheckRequest(string DeviceFingerprint);
record GameAuditEventRequest(
	string Kind,
	int PlayerId,
	double Observed,
	double Allowed,
	int MovementMode,
	double ServerTime);
record CreateBanRequest(
	string Fingerprint,
	string Reason,
	string BannedBy,
	string? ExpiresAt);
record DeviceSummary(
	string Fingerprint,
	DateTimeOffset FirstSeen,
	DateTimeOffset LastSeen,
	bool Banned,
	string? BanReason,
	DateTimeOffset? BanExpiresAt);
record GameAuditEventSummary(
	long Id,
	string Kind,
	int PlayerId,
	double Observed,
	double Allowed,
	int MovementMode,
	double ServerTime,
	DateTimeOffset ReceivedAt);
