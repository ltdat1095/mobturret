import 'package:dio/dio.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../../../core/dio_provider.dart';
import '../../../core/secure_jwt_storage.dart';
import '../domain/user.dart';
import 'auth_api.dart';

/// Translates server / network errors into a small set of strings the
/// UI can render directly. The server's `message` field is the source
/// of truth for HTTP errors; network / decode errors get a generic
/// fallback so we never leak stack traces to the user.
class AuthFailure implements Exception {
  AuthFailure(this.message);
  final String message;

  @override
  String toString() => message;
}

class AuthRepository {
  AuthRepository({
    required AuthApi api,
    required SecureJwtStorage storage,
  })  : _api = api,
        _storage = storage;

  final AuthApi _api;
  final SecureJwtStorage _storage;

  Future<User> signup({
    required String email,
    required String password,
  }) async {
    return _wrap(() => _api.signup(email: email, password: password));
  }

  /// Performs the login, then persists the resulting token. Callers
  /// (i.e. the [AuthNotifier]) should keep the token in memory as
  /// well so an immediate `/me` call doesn't need to round-trip the
  /// secure store.
  Future<LoginResult> login({
    required String email,
    required String password,
  }) async {
    final result = await _wrap(() => _api.login(email: email, password: password));
    await _storage.write(result.token);
    return result;
  }

  /// Used at app boot to rehydrate an existing session. Returns null
  /// when there's no stored token or the token is no longer valid.
  Future<({User user, String token})?> rehydrate() async {
    final token = await _storage.read();
    if (token == null || token.isEmpty) return null;
    try {
      final user = await _api.me(token);
      return (user: user, token: token);
    } on DioException {
      // 401 here means the server rejected the token. Wipe it so the
      // next boot doesn't waste another request on it.
      await _storage.delete();
      return null;
    }
  }

  Future<void> logout() async {
    await _storage.delete();
  }

  Future<T> _wrap<T>(Future<T> Function() op) async {
    try {
      return await op();
    } on DioException catch (e) {
      final body = e.response?.data;
      final msg = (body is Map && body['message'] is String)
          ? body['message'] as String
          : 'network error';
      throw AuthFailure(msg);
    }
  }
}

/// Self-building provider — constructs the [AuthRepository] from the
/// shared [Dio] and [SecureJwtStorage]. No override required; tests
/// that need a fake can override [dioProvider] or this one.
final authRepositoryProvider = Provider<AuthRepository>((ref) {
  final dio = ref.read(dioProvider);
  return AuthRepository(
    api: AuthApi(dio),
    storage: ref.read(secureStorageProvider),
  );
});