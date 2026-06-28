# gun_bot_mobile — Flutter app subproject

**Status: planned.** This folder is a placeholder; nothing is built
yet.

The Flutter mobile app is the user-facing command center for the
turret: authentication, telemetry dashboard, manual control, media
gallery, and push alert reception.

## Responsibilities (per `design.md` §6)

- **Auth** — username / password, exchange for JWT via AWS API
  Gateway, store securely in Keychain / Keystore.
- **Dashboard** — list turrets owned by the current user, show
  real-time telemetry from the MQTT bridge.
- **Control Center (Manual mode)** — RTSP video overlay, dual-axis
  virtual joystick, direct TCP / WebSocket to the A55 for sub-50 ms
  control. Stream pan / tilt / fire over the local channel; cloud
  MQTT is bypassed.
- **Media Gallery** — list and play back saved alert videos from
  S3 (pre-signed download URLs).
- **Push alerts** — receive FCM (Android) / APNs (iOS) notifications
  when the cloud router fires.

## Planned layout (not yet committed)

**Feature-first layout** (`lib/features/<feature>/`), state via
**Riverpod 2.x**:

```
gun_bot_mobile/
├── lib/
│   ├── main.dart
│   ├── app.dart
│   ├── core/                 # shared: JWT storage, dio interceptor, FCM token, base URL config
│   ├── features/
│   │   ├── auth/             # login + signup screens, auth notifier, API client
│   │   ├── dashboard/        # robot list, telemetry widgets
│   │   ├── control/          # joystick + RTSP overlay + TCP socket (Phase 3)
│   │   └── gallery/          # media list + playback (Phase 2)
│   └── shared/               # reusable widgets, theme, utilities
├── android/
├── ios/
└── pubspec.yaml
```

## Planned dependencies

- `dio` — REST client to API Gateway
- `mqtt_client` — WSS MQTT to AWS IoT Core (Custom JWT auth)
- `flutter_vlc_player` (or native media pipeline) — RTSP decoding (Phase 3)
- `flutter_riverpod` (^2.x) — state management (locked)
- `flutter_secure_storage` — JWT / FCM token storage
- `firebase_messaging` — push notifications (Phase 2)

## Open questions still to settle

- RTSP decoder: VLC, ExoPlayer, libmpv? Trade-off is battery vs
  latency on each platform — **Phase 3**.
- Direct TCP socket: raw `dart:io` Socket vs WebSocket — depends on
  whether the A55 serves WebSocket natively — **Phase 3**.
- Joystick library: roll-our-own vs `flutter_joystick` package —
  **Phase 3**.

## Build (target, not yet implemented)

```bash
flutter pub get
flutter run                       # debug on connected device / emulator
flutter build apk --release       # Android
flutter build ios --release       # iOS
```

**Base URL config (dev):** Android emulator reaches the host
machine at `http://10.0.2.2:8080`. iOS simulator and physical
devices use the laptop's LAN IP. Configurable via
`lib/core/config.dart` and overridable with `--dart-define`.

## Related docs

- [`../CLAUDE.md`](../CLAUDE.md) — MobTurret root
- [`../design.md`](../design.md) §6 (mobile architecture) —
  authoritative spec
