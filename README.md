# VeilNet

**A work-in-progress Unreal Engine plugin for server-authoritative visibility filtering and replicated shadow proxies, with an optional device-ban service.**

VeilNet packages reusable visibility sampling and shadow-proxy replication as a runtime plugin. The remaining `Source/TestingPlace` code is an example integration, not a requirement of the plugin. It needs multiplayer testing and feedback from other developers.

## Project status

This project is a **work in progress**. APIs and replication behavior may change. Contributions, questions, bug reports, and constructive feedback are welcome.

If you are unsure where to start, open an issue to discuss an idea before doing substantial work. Pull requests are welcome; focused changes with reproduction steps or multiplayer test results are especially helpful.

## Plugin contents

- `Plugins/VeilNet/Source/VeilNetRuntime` — reusable visibility and shadow-proxy runtime module.
- `Source/TestingPlace` — optional sample wiring for the original character and shooter classes.
- `Config/DefaultEngine.ini.fragment` — optional network relevancy timeout setting.

## Getting started

The original TestingPlace snapshot was built with Unreal Engine 5.8 for the `TestingPlaceEditor Win64 Development` target. The extracted plugin now builds for Win64 with Unreal Engine 5.8's `BuildPlugin` workflow; multiplayer runtime behavior has not yet been validated.

1. Copy both `Plugins/VeilNet` and `Plugins/EasyHWID` into your project's `Plugins` directory and enable VeilNet in the Unreal Editor or the `.uproject` file. VeilNet declares EasyHWID as a plugin dependency.
2. Add `VeilNetRuntime` to the module dependencies in your project's `Build.cs`.
3. Add `UVeilNetVisibilityComponent` to each player-controlled observer pawn. It samples visibility on the server. Configure `TargetTags` to include actors it should reveal; player-controlled pawns can also be included as targets with `bIncludePlayerControlledPawns`. With no target tags, only player-controlled pawns are considered by default. `RequiredTargetTag` can add an extra tag requirement. Configure `HorizontalFOV` and `VerticalFOV` to match the observer camera; both view angles are checked independently rather than treating horizontal FOV as a 3D cone.
4. On each actor whose network relevancy should depend on visibility, override `IsNetRelevantFor` and use the shared per-viewer policy:

   ```cpp
   #include "VeilNetVisibilityComponent.h"

   bool AMyCharacter::IsNetRelevantFor(
       const AActor* RealViewer,
       const AActor* ViewTarget,
       const FVector& SrcLocation) const
   {
       if (!Super::IsNetRelevantFor(RealViewer, ViewTarget, SrcLocation))
       {
           return false;
       }
       return UVeilNetVisibilityComponent::IsActorVisibleForViewer(
           this,
           RealViewer,
           ViewTarget);
   }
   ```
5. Add `UVeilNetShadowProxyComponent` to actors that need a shadow-only replicated representation. Assign its `SourceMesh` explicitly when an actor has multiple skeletal meshes; otherwise it uses the first skeletal mesh component it finds.
6. Regenerate project files if needed, close Unreal Editor, and perform a full editor build. Avoid Live Coding for reflected class changes.
7. Test with two-player networked PIE, including visibility, occluded shadows, animation, respawn, and movement.

If desired, merge the `IpNetDriver` section from `Config/DefaultEngine.ini.fragment` into your project's `Config/DefaultEngine.ini`; do not replace the destination config with the fragment. `Source/TestingPlace` is not required to use the plugin.

## Anti-wallhack visibility filtering

The anti-wallhack control runs in the Unreal game server, not the optional
.NET service. `UVeilNetVisibilityComponent` computes visibility on the server
using the observer's server-side view, field of view, range, and collision
traces. Use `IsNetRelevantFor` on every actor class carrying hidden-target
state to keep that state from being replicated to viewers who cannot see it.
The included character and shadow-proxy integrations show this pattern. The
visibility component by itself does not change replication relevancy.

For each observer, configure `TargetTags` to cover every actor type whose
position or state must be hidden. Ensure level geometry blocks the configured
visibility trace channel. Audit child and owned actors, replicated references,
RPCs, projectiles, audio, and UI for alternate leaks; a wallhack can use any
position or state already sent to the client. Do not use a client-reported
visibility result to authorize gameplay actions—validate hits and interactions
on the game server.

This is prevention by limiting replicated information, not detection of a
wallhack program. The device fingerprint service below is a separate,
spoofable moderation aid and cannot detect cheats or replace server-authoritative
gameplay validation. See the [multiplayer verification checklist](#multiplayer-verification-checklist).

For server-side visibility auditing, `UVeilNetVisibilityComponent` has
`bLogVisibilityTransitions` enabled by default. It logs an observer and target
when a target becomes visible or is no longer visible. These records describe
the server's visibility decision; they are not proof that a player used a
wallhack. Keep normal log access controls and retention policies in place.

The sample character also includes `UVeilNetMovementAuditComponent`. It checks
server-observed horizontal/vertical speed and position displacement across
movement modes, with separate limits for falling/jumping vertical movement.
Repeated speed excesses and large position discontinuities are logged as
anomalies, not proof of cheating. Call `ResetPositionBaseline` on the server
after an authorized teleport or respawn. The component does not kick or ban
players; tune its tolerances for legitimate game movement and abilities.
Weapon fire and hit validation are not covered by this sample audit. To retain
anomaly events in the optional moderator service, set `VEILNET_AUDIT_URL` on
the dedicated game server to the service base URL. The component authenticates
with the server-only `VEILNET_GAME_TOKEN`; events include a server player ID
and movement measurements, not a name or hardware fingerprint. The admin
page lists recent movement events for review, with retention controlled by
`VEILNET_EVENT_RETENTION_DAYS` (30 days by default). Telemetry is limited to
one event per character per second; use HTTPS when the service is remote.
The player ID is session-scoped, not a persistent account identity.

## Device-ban service and admin webpage

The optional Device Moderation service and its admin webpage are in
`Server`. Build and run the service with .NET 10:

```powershell
$env:VEILNET_GAME_TOKEN = "<unique game-server token>"
$env:VEILNET_ADMIN_TOKEN = "<different moderator token>"
$env:VEILNET_HASH_KEY = "<stable secret of at least 32 bytes>"
dotnet run --project .\Server\VeilNet.Server.csproj
```

Open `http://127.0.0.1:5080/` in a browser for the moderation page. The page accepts
the moderator token in-memory (it is not saved to local storage), lists
fingerprints, and lets authorized staff ban/unban devices. The service listens
on loopback by default. Use a trusted HTTPS reverse proxy for remote access;
set `VEILNET_LISTEN_URL` and `VEILNET_DB_PATH` as needed. Full setup and API
details are in `Server/README.md`. Never ship either token in a client build.
The game-server-only `VEILNET_GAME_TOKEN` authorizes device checks; it does not
authorize or implement wallhack visibility filtering.

For game integration, add `UVeilNetAntiCheatComponent` to the owning
`PlayerController` Blueprint/class, set its `ServiceBaseUrl`, and call
`SubmitDeviceForValidation` only after disclosing the collection and obtaining
any required consent. The game server reads `VEILNET_GAME_TOKEN` from its
process environment. A successful check sets `bDeviceValidated` on the
server; keep players out of gameplay until that flag is true. A banned device,
unsupported fingerprint, missing configuration, or unavailable API returns
the client to the main menu.

The API lists observed fingerprints at
`GET /api/v1/admin/devices?limit=50&offset=0`, bans via
`POST /api/v1/admin/bans`, and unbans via
`DELETE /api/v1/admin/bans/{fingerprint}`. All admin routes require
`VEILNET_ADMIN_TOKEN`.

EasyHWID derives a SHA-256 fingerprint from available SMBIOS system UUID and
motherboard serial values on Windows; raw hardware values are not sent to the
server. The server stores only a keyed HMAC of that fingerprint. This
transparent, user-mode identifier is still spoofable/resettable and is not
hardware attestation or a complete anti-cheat. Fingerprints remain sensitive
data: use HTTPS, restrict moderator access, keep the HMAC key stable to
preserve existing bans, and apply a retention policy. Do not rely on this
check instead of server-authoritative gameplay validation.

There is no general-purpose, unspoofable HWID available to a regular game
client. Strong device attestation requires a trusted platform or anti-cheat
provider, server-verifiable credentials, and appropriate user disclosure; it
cannot be achieved by hashing more client-readable identifiers.

## Contributing

There is no formal contribution process yet, so keep it simple:

- Open an issue for bugs, questions, or larger proposals.
- Submit a pull request with a clear description of the change and how you tested it.
- Keep changes focused and call out any project-specific assumptions or compatibility impact.
- Multiplayer PIE results, including known limitations, are valuable even when a change is still experimental.

Please be respectful and constructive in discussions. By contributing, you agree that your contributions are provided under the [MIT License](LICENSE).

## Replication privacy and limits

VeilNet applies a **server-side replication relevancy filter**: with the integration above, a target actor's replicated state is sent only while the observing player's server-maintained visibility cache says that target is visible. The shadow proxy uses the same per-viewer check and is not always relevant, so hidden targets do not continue streaming proxy position or pose. The server remains authoritative for gameplay.

This is a useful anti-ESP measure, not a guarantee against every information leak or a complete anti-cheat system:

- The filter must be wired into `IsNetRelevantFor` for **every replicated actor class that contains sensitive state**. Adding the visibility component alone does not filter the actor. The `TestingPlace` character example demonstrates this integration.
- Replication Graph rules, always-relevant settings, ownership, replicated references, RPCs, gameplay events, audio, or other actors can expose equivalent information unless those paths are reviewed too.
- A client cannot be made to forget state it already received. When a target leaves sight, VeilNet stops making it relevant after the configured visibility grace period; a client may retain previously received or otherwise disclosed information.
- A live shadow is itself a visual clue about position. With the security-first behavior, shadow proxies are not sent to a viewer while their source is not visible, so an unseen target has no live shadow for that viewer.
- This system does not protect secrets or replace server-side validation of movement, hits, damage, or other gameplay decisions.

### Multiplayer verification checklist

Use a dedicated or listen server and at least two clients. Confirm all of the following for each actor class you integrate:

1. A target hidden at spawn does not appear on a client and has no corresponding shadow-proxy actor on that client.
2. Once the server's visibility traces confirm the target is visible, the actor and its proxy become relevant to that viewer.
3. When visibility is lost, updates to both stop after `VisibilityGracePeriod` (33 ms by default); other viewers who can still see the target continue to receive updates.
4. A viewer without `UVeilNetVisibilityComponent` fails closed for filtered targets.
5. Inspect server/client actor lists and replicated references as well as the rendered scene; a visual test alone cannot establish that hidden state is not being replicated.

The plugin has been compiled for Win64 with Unreal Engine 5.8, and the visibility integration and 30 Hz shadow-pose update have been verified in multiplayer PIE. The separate horizontal/vertical FOV check builds, but still needs near-edge and above/below-target PIE verification.

## Next steps

Track follow-up work here. Check an item when its implementation and verification are complete:

**Initial audit:** the included character integration filters characters per viewer, and the shadow proxy follows its source actor's relevancy. Both use the plugin's shared per-viewer policy helper. The shooter weapon uses owner relevancy; NPC weapons are now spawned only by the server to avoid client-created copies. The projectile implementation is not included in this source snapshot, so its relevancy and replicated data still need review.

- [ ] **Audit all client-visible information.** Inventory replicated actors, components, replicated references, RPCs, gameplay events, audio, projectiles, and UI feedback that could reveal a hidden target. Add viewer-specific filtering or remove unnecessary data from each path.
- [ ] **Cover every relevant actor class.** Use the shared `UVeilNetVisibilityComponent::IsActorVisibleForViewer` policy in each actor carrying sensitive state, not just characters. Check child actors, attached actors, owned actors, and Replication Graph or Iris rules that may bypass normal relevancy. The included character and shadow proxy use the helper; other project actor classes still need integration.
- [ ] **Match visibility to the camera frustum.** Horizontal and vertical FOV limits are now tested independently to prevent the horizontal setting from admitting targets far above or below the view. Configure both to match the observer camera and verify near-edge targets in multiplayer PIE.
- [X] **Tune the visibility grace period.** The default is 33 ms (one visibility sampling interval), verified in multiplayer PIE. Increasing it can reduce flicker but allows longer continued relevancy after sight is lost.
- [X] **Minimize replicated data.** Hidden-target state remains server-side where possible, and the shadow proxy sends its pose at 30 Hz (down from 60 Hz); shadow smoothness and bandwidth were verified in multiplayer PIE. Continue to replicate only the minimum information permitted for each viewer.
- [ ] **Review indirect gameplay clues.** In the included weapon code, `MakeNoise` feeds server-side AI perception, and the weapon spawns a projectile; no client-facing sound or UI cue is implemented in the available files. The projectile class and presentation/audio paths are not included, so inspect and test those in the full project from the perspective of a client who cannot see the target.
- [ ] **Keep gameplay decisions server-authoritative.** The NPC damage/death authority guard has been verified in multiplayer PIE. Still audit the unavailable projectile implementation and all project-side movement, hits, damage, and interactions. Never trust a client-reported visibility result as proof that an action is allowed.
- [ ] **Make verification repeatable.** Use the multiplayer checklist above as a regression test for each supported actor type and network configuration. Consider automating dedicated-server/client PIE scenarios and running the Win64 plugin build in CI.

Replication relevancy reduces what a client is sent; it cannot erase information the client already received or guarantee protection against every side channel. Treat each new replicated feature as part of the information-flow audit.

## License

This project is licensed under the [MIT License](LICENSE).
