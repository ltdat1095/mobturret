import 'package:dio/dio.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/dio_provider.dart';
import '../domain/turret.dart';
import 'turrets_api.dart';

/// Domain-shaped errors for the turrets feature. Mirrors the auth
/// feature's `AuthFailure` pattern: server message is the source of
/// truth, network / decode errors get a generic fallback.
class TurretsFailure implements Exception {
  TurretsFailure(this.message);
  final String message;

  @override
  String toString() => message;
}

class TurretsRepository {
  TurretsRepository({required TurretsApi api}) : _api = api;

  final TurretsApi _api;

  Future<List<Turret>> listOwned() async {
    return _wrap(() => _api.list());
  }

  Future<T> _wrap<T>(Future<T> Function() op) async {
    try {
      return await op();
    } on DioException catch (e) {
      final body = e.response?.data;
      final msg = (body is Map && body['message'] is String)
          ? body['message'] as String
          : 'network error';
      throw TurretsFailure(msg);
    }
  }
}

/// Self-building provider — constructs the [TurretsRepository] from
/// the shared [Dio]. No override required.
final turretsRepositoryProvider = Provider<TurretsRepository>((ref) {
  final dio = ref.read(dioProvider);
  return TurretsRepository(api: TurretsApi(dio));
});
