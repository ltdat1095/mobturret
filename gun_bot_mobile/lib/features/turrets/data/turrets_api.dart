import 'package:dio/dio.dart';

import '../domain/turret.dart';

/// Wraps the `/turrets/*` endpoints the server exposes in M3.
///
/// M3 stub: only `list()` is meaningful — it returns the current
/// user's turrets (empty for now). `get`, `register`, and `setMode`
/// are scaffolded for M3 proper but will fail against the current
/// server.
class TurretsApi {
  TurretsApi(this._dio);
  final Dio _dio;

  /// Server returns `data.data` (the envelope's `data` field) as a
  /// `ListOutput` with a `turrets` key (array of turret objects).
  Future<List<Turret>> list() async {
    final res = await _dio.get<Map<String, dynamic>>('/turrets');
    final outer = res.data!['data'] as Map<String, dynamic>;
    final raw = outer['turrets'] as List<dynamic>;
    return raw
        .cast<Map<String, dynamic>>()
        .map(Turret.fromJson)
        .toList(growable: false);
  }

  /// Stub for M3 proper. Will throw 404 against the current server
  /// because the endpoint doesn't exist yet.
  Future<Turret> get(String robotId) async {
    final res = await _dio.get<Map<String, dynamic>>('/turrets/$robotId');
    return Turret.fromJson(res.data!['data'] as Map<String, dynamic>);
  }
}
