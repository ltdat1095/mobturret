import 'dart:async';

import 'package:dio/dio.dart';

import 'config.dart';

/// Callback the interceptor uses to drop an invalidated token. Decouples
/// the Dio plumbing from the auth state machine so this file doesn't
/// need to know about Riverpod notifiers.
typedef TokenInvalidator = FutureOr<void> Function();

/// Callback that supplies the current access token for the
/// `Authorization: Bearer` header. Returning null means "no token" and
/// the request goes out without one — same as a logged-out user
/// hitting a public endpoint.
typedef TokenProvider = FutureOr<String?> Function();

/// Builds the app's [Dio] instance with the auth interceptor attached.
///
/// The interceptor does two things:
///   1. On every outbound request, ask [tokenProvider] for a token and
///      stamp the `Authorization` header if there is one.
///   2. On any 401 response, call [onUnauthorized] to clear the token
///      from memory and storage — the router will then bounce the user
///      back to /login.
///
/// Memory-first ordering for the 401 case: state is cleared before
/// storage so any in-flight retry that re-reads the token sees nothing.
Dio buildDioClient({
  required TokenProvider tokenProvider,
  required TokenInvalidator onUnauthorized,
}) {
  final dio = Dio(
    BaseOptions(
      baseUrl: kApiBaseUrl,
      connectTimeout: const Duration(seconds: 10),
      receiveTimeout: const Duration(seconds: 10),
      contentType: 'application/json',
      responseType: ResponseType.json,
      // We handle non-2xx explicitly via `errors: false` so the
      // interceptor sees the raw 401 and the caller's `try/catch` on
      // `DioException` still works.
      validateStatus: (status) => status != null && status < 400,
    ),
  );
  dio.interceptors.add(_AuthInterceptor(
    tokenProvider: tokenProvider,
    onUnauthorized: onUnauthorized,
  ));
  return dio;
}

class _AuthInterceptor extends Interceptor {
  _AuthInterceptor({
    required this.tokenProvider,
    required this.onUnauthorized,
  });

  final TokenProvider tokenProvider;
  final TokenInvalidator onUnauthorized;

  @override
  Future<void> onRequest(
    RequestOptions options,
    RequestInterceptorHandler handler,
  ) async {
    final token = await tokenProvider();
    if (token != null && token.isNotEmpty) {
      options.headers['Authorization'] = 'Bearer $token';
    }
    handler.next(options);
  }

  @override
  Future<void> onError(
    DioException err,
    ErrorInterceptorHandler handler,
  ) async {
    if (err.response?.statusCode == 401) {
      await onUnauthorized();
    }
    handler.next(err);
  }
}