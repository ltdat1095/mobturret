import 'package:dio/dio.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../features/auth/application/auth_notifier.dart';
import 'dio_client.dart';

/// Source-of-truth [Dio] for the app. Built lazily on first read,
/// disposed when the provider is torn down.
///
/// Wiring the auth interceptor in the provider body (rather than in
/// `app.dart`) fixes a pre-existing Riverpod 2.x quirk where
/// `Provider.overrideWithValue` overrides from a nested
/// `ProviderScope` weren't visible to the auth notifier's read of
/// `authRepositoryProvider`. The new pattern is self-building:
/// the providers construct their own dependencies, and tests
/// override the leaf providers (e.g. `dioProvider`).
final dioProvider = Provider<Dio>((ref) {
  final dio = buildDioClient(
    tokenProvider: () => ref.read(currentTokenProvider),
    onUnauthorized: () async {
      await ref.read(authNotifierProvider.notifier).logout(
            message: 'Session expired. Please sign in again.',
          );
    },
  );
  ref.onDispose(() => dio.close(force: true));
  return dio;
});
