/// Wire-side status of a robot. The server reports `ONLINE` / `OFFLINE`
/// from the cloud_bridge; `unknown` is reserved for cases where the
/// field is missing or the row was migrated from an older schema.
enum TurretStatus {
  online,
  offline,
  unknown;

  static TurretStatus fromString(String? raw) {
    switch (raw) {
      case 'ONLINE':
        return TurretStatus.online;
      case 'OFFLINE':
        return TurretStatus.offline;
      default:
        return TurretStatus.unknown;
    }
  }

  String get display {
    switch (this) {
      case TurretStatus.online:
        return 'Online';
      case TurretStatus.offline:
        return 'Offline';
      case TurretStatus.unknown:
        return 'Unknown';
    }
  }
}

/// Wire-side mode. Maps server's uppercase `MANUAL` / `AUTO` / `SAFE`
/// to lowercase Dart-style identifiers.
enum TurretMode {
  manual,
  auto,
  safe;

  static TurretMode fromString(String? raw) {
    switch (raw) {
      case 'MANUAL':
        return TurretMode.manual;
      case 'AUTO':
        return TurretMode.auto;
      case 'SAFE':
        return TurretMode.safe;
      default:
        return TurretMode.safe; // SAFE is the safe default for unknown
    }
  }

  /// The wire string the server expects in a `mode` command payload.
  String get wire {
    switch (this) {
      case TurretMode.manual:
        return 'MANUAL';
      case TurretMode.auto:
        return 'AUTO';
      case TurretMode.safe:
        return 'SAFE';
    }
  }

  String get display {
    switch (this) {
      case TurretMode.manual:
        return 'Manual';
      case TurretMode.auto:
        return 'Auto';
      case TurretMode.safe:
        return 'Safe';
    }
  }
}
