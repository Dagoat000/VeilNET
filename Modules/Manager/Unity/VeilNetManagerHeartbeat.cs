using System;
using System.Collections;
using System.Collections.Generic;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using UnityEngine;
using UnityEngine.Networking;

namespace VeilNet.Manager
{
    [DisallowMultipleComponent]
    public sealed class VeilNetManagerHeartbeat : MonoBehaviour
    {
        private const string GameTokenEnvironmentVariable = "VEILNET_GAME_TOKEN";
        private const string ServerIdEnvironmentVariable = "VEILNET_SERVER_ID";
        private const string ManagerUrlEnvironmentVariable = "VEILNET_MANAGER_URL";
        private const int MaximumPlayerIdLength = 50;
        private const int DefaultServerPort = 7777;

        [SerializeField] private string serviceBaseUrl = "http://127.0.0.1:8000";
        [SerializeField] private string serverId;
        [SerializeField, Min(10f)] private float heartbeatIntervalSeconds = 60f;

        private static VeilNetManagerHeartbeat instance;
        private readonly object playersLock = new object();
        private readonly HashSet<string> connectedPlayerIds = new HashSet<string>(StringComparer.Ordinal);
        private Coroutine heartbeatCoroutine;
        private string gameToken;

        public static VeilNetManagerHeartbeat Instance => instance;
        public int ServerPort { get; private set; }

        [RuntimeInitializeOnLoadMethod(RuntimeInitializeLoadType.BeforeSceneLoad)]
        private static void BootstrapManagedServer()
        {
            if (string.IsNullOrWhiteSpace(
                    Environment.GetEnvironmentVariable(ServerIdEnvironmentVariable)))
            {
                return;
            }

            GameObject managerObject = new GameObject(nameof(VeilNetManagerHeartbeat));
            DontDestroyOnLoad(managerObject);
            managerObject.AddComponent<VeilNetManagerHeartbeat>();
        }

        private void Awake()
        {
            if (instance != null && instance != this)
            {
                enabled = false;
                Destroy(this);
                return;
            }

            instance = this;
        }

        private void OnEnable()
        {
            serviceBaseUrl = Environment.GetEnvironmentVariable(ManagerUrlEnvironmentVariable)
                ?? serviceBaseUrl;
            serverId = Environment.GetEnvironmentVariable(ServerIdEnvironmentVariable)
                ?? serverId;

            gameToken = Environment.GetEnvironmentVariable(GameTokenEnvironmentVariable);
            if (string.IsNullOrWhiteSpace(gameToken))
            {
                Debug.LogError(
                    $"{GameTokenEnvironmentVariable} is missing from the game server process environment; Manager heartbeats are disabled.",
                    this);
                enabled = false;
                return;
            }

            if (string.IsNullOrWhiteSpace(serverId))
            {
                Debug.LogError("A VeilNet Manager server ID must be configured.", this);
                enabled = false;
                return;
            }

            if (!Uri.TryCreate(serviceBaseUrl, UriKind.Absolute, out Uri serviceUri)
                || (serviceUri.Scheme != Uri.UriSchemeHttp && serviceUri.Scheme != Uri.UriSchemeHttps)
                || !string.IsNullOrEmpty(serviceUri.Query)
                || !string.IsNullOrEmpty(serviceUri.Fragment)
                || (serviceUri.Scheme != Uri.UriSchemeHttps && !serviceUri.IsLoopback))
            {
                Debug.LogError(
                    "The VeilNet Manager URL must use HTTPS remotely, or HTTP on loopback, without a query or fragment.",
                    this);
                enabled = false;
                return;
            }

            serviceBaseUrl = serviceUri.GetLeftPart(UriPartial.Path).TrimEnd('/');
            ServerPort = ReadServerPort();
            Debug.Log(
                $"VeilNet Manager heartbeat component active for server '{serverId}' at {serviceBaseUrl}; game port {ServerPort}.",
                this);
            heartbeatCoroutine = StartCoroutine(SendHeartbeats(gameToken));
        }

        private void OnDisable()
        {
            if (heartbeatCoroutine != null)
            {
                StopCoroutine(heartbeatCoroutine);
                heartbeatCoroutine = null;
            }

            gameToken = null;
        }

        private void OnDestroy()
        {
            if (instance == this)
            {
                instance = null;
            }
        }

        public bool PlayerJoined(string playerId)
        {
            if (!IsValidPlayerId(playerId))
            {
                Debug.LogWarning("A VeilNet player ID must contain 1 to 50 non-whitespace characters.", this);
                return false;
            }

            lock (playersLock)
            {
                return connectedPlayerIds.Add(playerId);
            }
        }

        public bool PlayerLeft(string playerId)
        {
            if (string.IsNullOrWhiteSpace(playerId))
            {
                Debug.LogWarning("Cannot remove a player with an empty VeilNet player ID.", this);
                return false;
            }

            lock (playersLock)
            {
                return connectedPlayerIds.Remove(playerId);
            }
        }

        public static bool TryGetDeviceFingerprint(out string fingerprint)
        {
            fingerprint = null;
            string deviceIdentifier = SystemInfo.deviceUniqueIdentifier;
            if (string.IsNullOrWhiteSpace(deviceIdentifier)
                || string.Equals(deviceIdentifier, SystemInfo.unsupportedIdentifier, StringComparison.OrdinalIgnoreCase))
            {
                return false;
            }

            using (SHA256 sha256 = SHA256.Create())
            {
                byte[] digest = sha256.ComputeHash(Encoding.UTF8.GetBytes(deviceIdentifier));
                fingerprint = BitConverter.ToString(digest).Replace("-", "").ToLowerInvariant();
            }

            return true;
        }

        public void IdentifyPlayerByDeviceFingerprint(
            string deviceFingerprint,
            string displayName,
            Action<PlayerIdentityResult> onCompleted)
        {
            if (onCompleted == null)
            {
                throw new ArgumentNullException(nameof(onCompleted));
            }

            if (heartbeatCoroutine == null || !enabled)
            {
                onCompleted(new PlayerIdentityResult
                {
                    success = false,
                    error = "VeilNet Manager device identification is not available on this server."
                });
                return;
            }

            if (!IsValidDeviceFingerprintValue(deviceFingerprint))
            {
                onCompleted(new PlayerIdentityResult
                {
                    success = false,
                    error = "A valid Unity device fingerprint is required."
                });
                return;
            }

            string requestedUsername = string.IsNullOrWhiteSpace(displayName)
                ? "Pilot"
                : displayName.Trim();
            if (requestedUsername.Length > 20)
            {
                requestedUsername = requestedUsername.Substring(0, 20);
            }
            if (requestedUsername.Length < 3)
            {
                requestedUsername = "Pilot";
            }

            StartCoroutine(IdentifyPlayerRequest(
                deviceFingerprint,
                requestedUsername,
                onCompleted));
        }

        public void SetConnectedPlayers(IEnumerable<string> playerIds)
        {
            if (playerIds == null)
            {
                throw new ArgumentNullException(nameof(playerIds));
            }

            string[] snapshot = playerIds.Distinct(StringComparer.Ordinal).ToArray();
            if (snapshot.Any(playerId => !IsValidPlayerId(playerId)))
            {
                throw new ArgumentException(
                    "Every VeilNet player ID must contain 1 to 50 non-whitespace characters.",
                    nameof(playerIds));
            }

            lock (playersLock)
            {
                connectedPlayerIds.Clear();
                connectedPlayerIds.UnionWith(snapshot);
            }
        }

        public static int ReadPortArgument(string[] commandLineArguments, int fallbackPort)
        {
            if (commandLineArguments == null)
            {
                throw new ArgumentNullException(nameof(commandLineArguments));
            }

            for (int index = 0; index < commandLineArguments.Length; index++)
            {
                string argument = commandLineArguments[index];
                if (argument.StartsWith("-port=", StringComparison.OrdinalIgnoreCase)
                    && int.TryParse(argument.Substring("-port=".Length), out int port)
                    && port >= 1
                    && port <= 65535)
                {
                    return port;
                }
                if (string.Equals(argument, "-ngo-port", StringComparison.OrdinalIgnoreCase))
                {
                    if (index + 1 < commandLineArguments.Length
                        && int.TryParse(commandLineArguments[index + 1], out port)
                        && port >= 1
                        && port <= 65535)
                    {
                        return port;
                    }
                }
            }

            return fallbackPort;
        }

        private static int ReadServerPort()
        {
            string configuredPort = Environment.GetEnvironmentVariable("VEILNET_SERVER_PORT");
            if (int.TryParse(configuredPort, out int environmentPort)
                && environmentPort >= 1
                && environmentPort <= 65535)
            {
                return environmentPort;
            }

            if (!string.IsNullOrWhiteSpace(configuredPort))
            {
                Debug.LogWarning(
                    "VEILNET_SERVER_PORT is invalid; checking Unity server command-line arguments.");
            }

            int commandLinePort = ReadPortArgument(Environment.GetCommandLineArgs(), 0);
            if (commandLinePort > 0)
            {
                Debug.LogWarning("VEILNET_SERVER_PORT is not set; using the server command-line port.");
                return commandLinePort;
            }

            Debug.LogWarning(
                $"VEILNET_SERVER_PORT is not set; using the default Unity server port {DefaultServerPort}. "
                + "Configure VEILNET_SERVER_PORT if this server listens on a different port.");
            return DefaultServerPort;
        }

        private IEnumerator SendHeartbeats(string gameToken)
        {
            string escapedServerId = UnityWebRequest.EscapeURL(serverId);
            string heartbeatUrl = $"{serviceBaseUrl}/api/v1/game/servers/{escapedServerId}/heartbeat";

            while (enabled)
            {
                string[] playerSnapshot;
                lock (playersLock)
                {
                    playerSnapshot = connectedPlayerIds.ToArray();
                }

                string json = JsonUtility.ToJson(new HeartbeatPayload { player_ids = playerSnapshot });
                using (UnityWebRequest request = new UnityWebRequest(heartbeatUrl, UnityWebRequest.kHttpVerbPOST))
                {
                    request.uploadHandler = new UploadHandlerRaw(Encoding.UTF8.GetBytes(json));
                    request.downloadHandler = new DownloadHandlerBuffer();
                    request.timeout = 10;
                    request.SetRequestHeader("Content-Type", "application/json");
                    request.SetRequestHeader("Authorization", $"Bearer {gameToken}");

                    yield return request.SendWebRequest();

                    if (request.result == UnityWebRequest.Result.Success)
                    {
                        Debug.Log(
                            $"VeilNet Manager heartbeat sent for {serverId} with {playerSnapshot.Length} players.",
                            this);
                    }
                    else
                    {
                        Debug.LogError(
                            $"VeilNet Manager heartbeat failed for {serverId} (HTTP {request.responseCode}): {request.error}. {request.downloadHandler?.text}",
                            this);
                    }
                }

                yield return new WaitForSecondsRealtime(Mathf.Max(10f, heartbeatIntervalSeconds));
            }
        }

        private IEnumerator IdentifyPlayerRequest(
            string deviceFingerprint,
            string displayName,
            Action<PlayerIdentityResult> onCompleted)
        {
            string identityUrl = $"{serviceBaseUrl}/api/v1/game/auth/device";
            string json = JsonUtility.ToJson(new DeviceIdentityPayload
            {
                device_fingerprint = deviceFingerprint,
                username = displayName
            });

            using (UnityWebRequest request = new UnityWebRequest(identityUrl, UnityWebRequest.kHttpVerbPOST))
            {
                request.uploadHandler = new UploadHandlerRaw(Encoding.UTF8.GetBytes(json));
                request.downloadHandler = new DownloadHandlerBuffer();
                request.timeout = 10;
                request.SetRequestHeader("Content-Type", "application/json");
                request.SetRequestHeader("Authorization", $"Bearer {gameToken}");

                yield return request.SendWebRequest();

                if (request.result != UnityWebRequest.Result.Success)
                {
                    onCompleted(new PlayerIdentityResult
                    {
                        success = false,
                        error = $"Device identity lookup failed (HTTP {request.responseCode}): {request.error}. {request.downloadHandler?.text}"
                    });
                    yield break;
                }

                DeviceIdentityResponse response = JsonUtility.FromJson<DeviceIdentityResponse>(request.downloadHandler.text);
                if (response == null
                    || !IsValidPlayerId(response.user_id)
                    || string.IsNullOrWhiteSpace(response.username))
                {
                    onCompleted(new PlayerIdentityResult
                    {
                        success = false,
                        error = "Manager returned an invalid device identity response."
                    });
                    yield break;
                }

                onCompleted(new PlayerIdentityResult
                {
                    success = true,
                    userId = response.user_id,
                    username = response.username
                });
            }
        }

        public static bool IsValidDeviceFingerprintValue(string fingerprint)
        {
            return fingerprint != null
                && fingerprint.Length == 64
                && fingerprint.All(Uri.IsHexDigit);
        }

        private static bool IsValidPlayerId(string playerId)
        {
            return !string.IsNullOrWhiteSpace(playerId)
                && playerId.Length <= MaximumPlayerIdLength
                && playerId.Trim().Length == playerId.Length;
        }

        [Serializable]
        private sealed class HeartbeatPayload
        {
            public string[] player_ids;
        }

        [Serializable]
        private sealed class DeviceIdentityPayload
        {
            public string device_fingerprint;
            public string username;
        }

        [Serializable]
        private sealed class DeviceIdentityResponse
        {
            public string user_id;
            public string username;
        }

        public sealed class PlayerIdentityResult
        {
            public bool success;
            public string userId;
            public string username;
            public string error;
        }
    }
}
