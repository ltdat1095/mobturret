import 'package:flutter/foundation.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';

import '../data/auth_repository.dart';
import '../domain/user.dart';

/// Discriminated union for auth state. Sealed so the router can
/// exhaustively match on it for redirects.
sealed class AuthState {
  const AuthState();
}

/// We don't yet know whether a stored token is valid. Shows a splash.
class AuthBooting extends AuthState {
  const AuthBooting();
}

/// Logged out — either never signed in, or logged out, or the
/// rehydrate probe failed. [error] is the most recent failure, if
/// any, so the UI can display it on the login screen.
class AuthUnauthenticated extends AuthState {
  const AuthUnauthenticated({this.error});
  final String? error;
}

/// Mid-login / mid-signup. The button should show a spinner.
class AuthAuthenticating extends AuthState {
  const AuthAuthenticating();
}

/// Logged in. [token] is in memory so /me can be retried without
/// re-reading secure storage; storage is still the source of truth
/// across restarts.
class AuthAuthenticated extends AuthState {
  const AuthAuthenticated({
    required this.user,
    required this.token,
  });
  final User user;
  final String token;
}

/// Source of truth for auth state.
///
/// `build()` runs once at startup and:
///   1. Reads the JWT from secure storage.
///   2. If present, calls /me to confirm it still works.
///   3. If /me fails, drops the token and lands in Unauthenticated.
///
/// Beyond boot, the notifier exposes [login], [signup], and
/// [logout]. On a 401 from anywhere, the auth interceptor in
/// `core/dio_client.dart` calls [logout] directly so the state stays
/// in sync.
class AuthNotifier extends AsyncNotifier<AuthState> {
  @override
  Future<AuthState> build() async {
    final repo = ref.read(authRepositoryProvider);
    final session = await repo.rehydrate();
    if (session == null) return const AuthUnauthenticated();
    return AuthAuthenticated(user: session.user, token: session.token);
  }

  Future<void> login(String email, String password) async {
    state = const AsyncValue.data(AuthAuthenticating());
    final repo = ref.read(authRepositoryProvider);
    try {
      final result = await repo.login(email: email, password: password);
      state = AsyncValue.data(AuthAuthenticated(
        user: User(
          userId: result.userId,
          email: result.email,
          // The login response doesn't include created_at; the
          // /me bootstrap call will fill it in on the next refresh.
          createdAt: DateTime.now().toUtc(),
        ),
        token: result.token,
      ));
    } catch (e, st) {
      debugPrint('AuthNotifier.login failed: $e');
      state = AsyncValue.data(AuthUnauthenticated(error: e.toString()));
      // Surface the error to listeners via AsyncValue.error too so
      // tests / error overlays can see it.
      AsyncValue.error(e, st);
    }
  }

  Future<void> signup(String email, String password) async {
    state = const AsyncValue.data(AuthAuthenticating());
    final repo = ref.read(authRepositoryProvider);
    try {
      await repo.signup(email: email, password: password);
      // Signup succeeded; immediately log the user in. This avoids a
      // second round-trip to fetch a token, but we still need a real
      // token in memory — so we run login() through the repo.
      final result = await repo.login(email: email, password: password);
      state = AsyncValue.data(AuthAuthenticated(
        user: User(
          userId: result.userId,
          email: result.email,
          createdAt: DateTime.now().toUtc(),
        ),
        token: result.token,
      ));
    } catch (e, st) {
      debugPrint('AuthNotifier.signup failed: $e');
      state = AsyncValue.data(AuthUnauthenticated(error: e.toString()));
      AsyncValue.error(e, st);
    }
  }

  /// Called by the 401 interceptor. Drops the token and goes back to
  /// the unauthenticated state with the supplied message so the UI
  /// can show "Session expired, please log in again."
  Future<void> logout({String? message}) async {
    final repo = ref.read(authRepositoryProvider);
    await repo.logout();
    state = AsyncValue.data(AuthUnauthenticated(error: message));
  }
}

final authNotifierProvider =
    AsyncNotifierProvider<AuthNotifier, AuthState>(AuthNotifier.new);

/// Convenience selector used by the Dio interceptor: read the current
/// token without forcing the whole AuthState. Returns null when not
/// authenticated so the interceptor leaves the header off.
final currentTokenProvider = Provider<String?>((ref) {
  final state = ref.watch(authNotifierProvider).value;
  return state is AuthAuthenticated ? state.token : null;
});