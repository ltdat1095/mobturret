// Model-layer unit tests for the turrets feature. The dashboard
// widget tree is small enough to verify on the emulator; what
// matters here is the wire-format mapping (server → Dart) and the
// enum coercions that handle missing / malformed fields.
import 'package:flutter_test/flutter_test.dart';

import 'package:mobturret_mobile/features/turrets/domain/enums.dart';
import 'package:mobturret_mobile/features/turrets/domain/turret.dart';

void main() {
  group('TurretStatus', () {
    test('parses ONLINE', () {
      expect(TurretStatus.fromString('ONLINE'), TurretStatus.online);
    });

    test('parses OFFLINE', () {
      expect(TurretStatus.fromString('OFFLINE'), TurretStatus.offline);
    });

    test('null maps to unknown', () {
      expect(TurretStatus.fromString(null), TurretStatus.unknown);
    });

    test('unexpected value maps to unknown', () {
      expect(TurretStatus.fromString('BANANA'), TurretStatus.unknown);
    });
  });

  group('TurretMode', () {
    test('round-trips through the wire string', () {
      for (final m in TurretMode.values) {
        expect(TurretMode.fromString(m.wire), m);
      }
    });

    test('null falls back to SAFE', () {
      // SAFE is the safe default for unknown — the turret will
      // refuse to fire on a misparsed mode.
      expect(TurretMode.fromString(null), TurretMode.safe);
    });
  });

  group('Turret.fromJson', () {
    test('parses a fully-populated M3 stub record', () {
      final json = {
        'robot_id': 'bridge-001',
        'owner_id': 'owner-uuid',
        'name': 'Bridge 001',
        'status': 'ONLINE',
        'mode': 'AUTO',
        'firmware_version': '0.1.0',
        'video_capture_enabled': true,
        'created_at': '2026-08-01T00:00:00Z',
        'last_seen': '2026-08-03T10:30:00Z',
      };
      final t = Turret.fromJson(json);
      expect(t.robotId, 'bridge-001');
      expect(t.ownerId, 'owner-uuid');
      expect(t.name, 'Bridge 001');
      expect(t.status, TurretStatus.online);
      expect(t.mode, TurretMode.auto);
      expect(t.firmwareVersion, '0.1.0');
      expect(t.videoCaptureEnabled, isTrue);
      expect(t.createdAt.toUtc(), DateTime.utc(2026, 8, 1));
      expect(t.lastSeen, isNotNull);
      expect(t.lastSeen!.toUtc(), DateTime.utc(2026, 8, 3, 10, 30));
    });

    test('falls back to robot_id when name is absent', () {
      // design.md lets `name` be optional; the M3 stub omits it for
      // never-registered rows. The card must still have a title.
      final t = Turret.fromJson({
        'robot_id': 'bridge-002',
        'owner_id': 'owner-uuid',
        'status': 'OFFLINE',
        'mode': 'SAFE',
        'video_capture_enabled': false,
        'created_at': '2026-08-01T00:00:00Z',
      });
      expect(t.name, 'bridge-002');
      expect(t.firmwareVersion, '');
      expect(t.lastSeen, isNull);
      expect(t.status, TurretStatus.offline);
      expect(t.mode, TurretMode.safe);
    });

    test('handles malformed last_seen as null', () {
      final t = Turret.fromJson({
        'robot_id': 'x',
        'owner_id': 'y',
        'status': 'OFFLINE',
        'mode': 'SAFE',
        'video_capture_enabled': false,
        'created_at': '2026-08-01T00:00:00Z',
        'last_seen': 'not-a-timestamp',
      });
      expect(t.lastSeen, isNull);
    });
  });
}
