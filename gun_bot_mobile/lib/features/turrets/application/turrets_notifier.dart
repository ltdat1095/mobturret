import 'package:flutter/foundation.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../data/turrets_repository.dart';
import '../domain/turret.dart';

/// Source of truth for the dashboard's turret list.
///
/// `build()` runs once on first read and calls the repository. The
/// dashboard uses `AsyncValue.when` to render loading / data / error;
/// pull-to-refresh calls [refresh] which puts the state into loading
/// before re-fetching.
class TurretsNotifier extends AsyncNotifier<List<Turret>> {
  @override
  Future<List<Turret>> build() async {
    final repo = ref.read(turretsRepositoryProvider);
    return repo.listOwned();
  }

  /// Re-fetch from the server. Used for pull-to-refresh and after
  /// any mutation (registration, mode toggle) lands in M3 proper.
  Future<void> refresh() async {
    state = const AsyncValue<List<Turret>>.loading();
    state = await AsyncValue.guard(() async {
      final repo = ref.read(turretsRepositoryProvider);
      return repo.listOwned();
    });
  }
}

final turretsNotifierProvider =
    AsyncNotifierProvider<TurretsNotifier, List<Turret>>(TurretsNotifier.new);

/// Convenience: a debug-only listener that logs refresh attempts.
/// Drop this in once the dashboard's `RefreshIndicator` is wired so
/// the test logs show the cycle.
@visibleForTesting
const turretsNotifierDebugLabel = 'turrets-notifier';
