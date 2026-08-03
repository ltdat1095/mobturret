import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../auth/application/auth_notifier.dart';
import '../application/turrets_notifier.dart';
import 'widgets/turret_card.dart';

/// Dashboard — the authenticated user's home screen.
///
/// Three states:
///   - loading: centered spinner
///   - error:   centered message + retry button
///   - data:    RefreshIndicator + ListView of TurretCard, or an
///              empty-state with a "coming soon" message and the
///              instructions for pairing a robot.
///
/// M3 proper will replace the empty state with a registration flow
/// (POST /turrets). Until then, registering a robot is server-side
/// only (a DDB admin task); the dashboard is honest about it.
class DashboardScreen extends ConsumerWidget {
  const DashboardScreen({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final asyncTurrets = ref.watch(turretsNotifierProvider);

    return Scaffold(
      appBar: AppBar(
        title: const Text('MobTurret'),
        actions: [
          IconButton(
            tooltip: 'Log out',
            icon: const Icon(Icons.logout),
            onPressed: () async {
              await ref.read(authNotifierProvider.notifier).logout();
            },
          ),
        ],
      ),
      body: asyncTurrets.when(
        loading: () => const Center(child: CircularProgressIndicator()),
        error: (err, _) => _ErrorView(
          message: err.toString(),
          onRetry: () => ref.read(turretsNotifierProvider.notifier).refresh(),
        ),
        data: (turrets) {
          if (turrets.isEmpty) {
            return const _EmptyState();
          }
          return RefreshIndicator(
            onRefresh: () =>
                ref.read(turretsNotifierProvider.notifier).refresh(),
            child: ListView.builder(
              itemCount: turrets.length,
              // The detail screen lands with M3 proper; until then the
              // card is tappable but no-op so the layout reads naturally.
              itemBuilder: (context, i) => TurretCard(turret: turrets[i]),
            ),
          );
        },
      ),
    );
  }
}

class _EmptyState extends StatelessWidget {
  const _EmptyState();

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(
              Icons.precision_manufacturing_outlined,
              size: 72,
              color: theme.colorScheme.outline,
            ),
            const SizedBox(height: 16),
            Text(
              'No robots yet',
              style: theme.textTheme.titleLarge,
              textAlign: TextAlign.center,
            ),
            const SizedBox(height: 8),
            Text(
              'When a robot is paired with your account it will appear '
              'here. M3 proper will add a registration flow — for now, '
              'robots are registered server-side.',
              style: theme.textTheme.bodyMedium,
              textAlign: TextAlign.center,
            ),
          ],
        ),
      ),
    );
  }
}

class _ErrorView extends StatelessWidget {
  const _ErrorView({required this.message, required this.onRetry});

  final String message;
  final VoidCallback onRetry;

  @override
  Widget build(BuildContext context) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            Icon(
              Icons.error_outline,
              size: 56,
              color: Theme.of(context).colorScheme.error,
            ),
            const SizedBox(height: 16),
            Text(
              message,
              textAlign: TextAlign.center,
              style: Theme.of(context).textTheme.bodyMedium,
            ),
            const SizedBox(height: 16),
            FilledButton.icon(
              onPressed: onRetry,
              icon: const Icon(Icons.refresh),
              label: const Text('Retry'),
            ),
          ],
        ),
      ),
    );
  }
}
