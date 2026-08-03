// API base URL — overridable via `--dart-define=API_BASE_URL=...`.
//
// Android emulator reaches the host machine at `10.0.2.2`, which maps
// to `127.0.0.1` on the dev laptop. iOS simulator / physical devices
// need the laptop's LAN IP — pass it explicitly:
//
//   flutter run --dart-define=API_BASE_URL=http://192.168.1.42:8182
//
// The default port (8182) is the same as `PORT` in server/.env. It
// is NOT 8080 because other dev servers on this box already use 8080.
// HTTPS only in prod; dev runs plain HTTP and Android needs a
// `usesCleartextTraffic` opt-in for the loopback hosts — see
// android/app/src/main/AndroidManifest.xml.
const String kApiBaseUrl = String.fromEnvironment(
  'API_BASE_URL',
  defaultValue: 'http://10.0.2.2:8182',
);