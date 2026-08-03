import 'package:flutter/material.dart';

import '../../domain/enums.dart';
import '../../domain/turret.dart';

/// One turret in the dashboard list. Renders name, status dot,
/// mode chip, and last-seen relative time. Tapping is the parent's
/// responsibility (the M3 detail screen handler isn't wired yet).
///
/// This widget is unused on the dashboard until M3 proper lands and
/// the list is non-empty; it's authored now so the M3 follow-up
/// doesn't need to touch the dashboard.
class TurretCard extends StatelessWidget {
  const TurretCard({
    super.key,
    required this.turret,
    this.onTap,
  });

  final Turret turret;
  final VoidCallback? onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final dotColor = switch (turret.status) {
      TurretStatus.online => Colors.green,
      TurretStatus.offline => Colors.grey,
      TurretStatus.unknown => Colors.amber,
    };
    final lastSeenText = turret.lastSeen == null
        ? 'never'
        : _formatRelative(turret.lastSeen!);

    return Card(
      margin: const EdgeInsets.symmetric(horizontal: 16, vertical: 6),
      child: ListTile(
        leading: _StatusDot(color: dotColor),
        title: Text(turret.name, style: theme.textTheme.titleMedium),
        subtitle: Text('${turret.robotId} • $lastSeenText'),
        trailing: Chip(
          label: Text(turret.mode.display),
          visualDensity: VisualDensity.compact,
        ),
        onTap: onTap,
      ),
    );
  }
}

class _StatusDot extends StatelessWidget {
  const _StatusDot({required this.color});
  final Color color;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 12,
      height: 12,
      decoration: BoxDecoration(
        color: color,
        shape: BoxShape.circle,
      ),
    );
  }
}

String _formatRelative(DateTime then) {
  final now = DateTime.now().toUtc();
  final diff = now.difference(then.toUtc());
  if (diff.inSeconds < 60) return '${diff.inSeconds}s ago';
  if (diff.inMinutes < 60) return '${diff.inMinutes}m ago';
  if (diff.inHours < 24) return '${diff.inHours}h ago';
  return '${diff.inDays}d ago';
}
