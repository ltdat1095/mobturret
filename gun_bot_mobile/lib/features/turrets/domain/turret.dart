import 'enums.dart';

/// A registered MobTurret robot, as it appears in the dashboard list.
///
/// The wire format is fixed by `server/internal/turrets/usecase/dto.go`
/// (when M3 proper lands). For the M3 stub the server returns:
///   { "robot_id", "owner_id", "name", "status", "mode",
///     "firmware_version", "video_capture_enabled", "created_at",
///     "last_seen" }
///
/// `lastSeen` is nullable: the server may not have a state yet for
/// freshly-registered turrets.
class Turret {
  const Turret({
    required this.robotId,
    required this.ownerId,
    required this.name,
    required this.status,
    required this.mode,
    required this.firmwareVersion,
    required this.videoCaptureEnabled,
    required this.createdAt,
    this.lastSeen,
  });

  final String robotId;
  final String ownerId;
  final String name;
  final TurretStatus status;
  final TurretMode mode;
  final String firmwareVersion;
  final bool videoCaptureEnabled;
  final DateTime createdAt;
  final DateTime? lastSeen;

  factory Turret.fromJson(Map<String, dynamic> json) {
    return Turret(
      robotId: json['robot_id'] as String,
      ownerId: json['owner_id'] as String,
      name: (json['name'] as String?) ?? (json['robot_id'] as String),
      status: TurretStatus.fromString(json['status'] as String?),
      mode: TurretMode.fromString(json['mode'] as String?),
      firmwareVersion: (json['firmware_version'] as String?) ?? '',
      videoCaptureEnabled:
          (json['video_capture_enabled'] as bool?) ?? false,
      createdAt: DateTime.parse(json['created_at'] as String),
      lastSeen: json['last_seen'] is String
          ? DateTime.tryParse(json['last_seen'] as String)
          : null,
    );
  }
}
