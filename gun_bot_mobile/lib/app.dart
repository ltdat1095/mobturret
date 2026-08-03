import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import 'core/router.dart';

/// App root. Builds the [GoRouter] (via [routerProvider]) and hands
/// it to [MaterialApp.router]. No provider overrides here — the
/// feature providers (`authRepositoryProvider`, `turretsRepositoryProvider`,
/// `dioProvider`) are self-building. Tests override the leaf
/// provider they need to swap.
class MobTurretApp extends ConsumerWidget {
  const MobTurretApp({super.key});

  @override
  Widget build(BuildContext context, WidgetRef ref) {
    final router = ref.watch(routerProvider);
    return MaterialApp.router(
      title: 'MobTurret',
      debugShowCheckedModeBanner: false,
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(seedColor: Colors.indigo),
        useMaterial3: true,
      ),
      routerConfig: router,
    );
  }
}
