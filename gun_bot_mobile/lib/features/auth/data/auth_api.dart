import 'package:dio/dio.dart';

import '../domain/user.dart';

/// Wraps the three `/auth/*` endpoints the server exposes in M2.
///
/// The wire format is fixed by the Go handler: the response body is
/// the standard `{code, message, data}` envelope, so each method
/// pulls `data` off the envelope and parses it into a typed object.
class AuthApi {
  AuthApi(this._dio);
  final Dio _dio;

  /// Server returns `data.data` for signup with `{user_id, email, created_at}`.
  Future<User> signup({
    required String email,
    required String password,
  }) async {
    final res = await _dio.post<Map<String, dynamic>>(
      '/auth/signup',
      data: {'email': email, 'password': password},
    );
    return User.fromJson(res.data!['data'] as Map<String, dynamic>);
  }

  /// Server returns `data.data` for login with
  /// `{token, token_type, expires_in, user_id, email}`.
  Future<LoginResult> login({
    required String email,
    required String password,
  }) async {
    final res = await _dio.post<Map<String, dynamic>>(
      '/auth/login',
      data: {'email': email, 'password': password},
    );
    final body = res.data!['data'] as Map<String, dynamic>;
    return LoginResult(
      token: body['token'] as String,
      tokenType: body['token_type'] as String,
      expiresIn: body['expires_in'] as int,
      userId: body['user_id'] as String,
      email: body['email'] as String,
    );
  }

  /// Server returns `data.data` for /me with `{user_id, email, created_at}`.
  Future<User> me(String token) async {
    final res = await _dio.get<Map<String, dynamic>>(
      '/auth/me',
      options: Options(headers: {'Authorization': 'Bearer $token'}),
    );
    return User.fromJson(res.data!['data'] as Map<String, dynamic>);
  }
}

class LoginResult {
  const LoginResult({
    required this.token,
    required this.tokenType,
    required this.expiresIn,
    required this.userId,
    required this.email,
  });

  final String token;
  final String tokenType;
  final int expiresIn;
  final String userId;
  final String email;
}