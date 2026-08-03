import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../features/auth/application/auth_notifier.dart';
import '../features/auth/presentation/login_screen.dart';
import '../features/auth/presentation/signup_screen.dart';
import '../features/turrets/presentation/dashboard_screen.dart';

/// Top-level app routes. M6 ships /login, /signup; M7 adds /dashboard
/// as the authenticated destination. /dashboard is reached once the
/// JWT is valid and bounces the user back to /login on expiry.
abstract class AppRoutes {
  static const dashboard = '/dashboard';
  static const login = '/login';
  static const signup = '/signup';
}

/// Builds the GoRouter. The redirect reads the AuthState and:
///   - if booting, shows a splash (no redirect).
///   - if unauthenticated and trying to reach /dashboard, send to /login.
///   - if authenticated and on /login or /signup, send to /dashboard.
GoRouter buildRouter(Ref ref) {
  return GoRouter(
    initialLocation: AppRoutes.login,
    refreshListenable: _AuthStateListenable(ref),
    redirect: (context, state) {
      final auth = ref.read(authNotifierProvider).value;
      final goingTo = state.matchedLocation;
      final isAuthRoute =
          goingTo == AppRoutes.login || goingTo == AppRoutes.signup;
      final isDashboard = goingTo == AppRoutes.dashboard;

      // Boot in progress — don't bounce yet.
      if (auth == null || auth is AuthBooting) return null;

      final authed = auth is AuthAuthenticated;

      if (!authed && isDashboard) return AppRoutes.login;
      if (authed && isAuthRoute) return AppRoutes.dashboard;
      return null;
    },
    routes: [
      GoRoute(
        path: AppRoutes.login,
        builder: (context, state) => const LoginScreen(),
      ),
      GoRoute(
        path: AppRoutes.signup,
        builder: (context, state) => const SignupScreen(),
      ),
      GoRoute(
        path: AppRoutes.dashboard,
        builder: (context, state) => const DashboardScreen(),
      ),
    ],
  );
}

/// Listenable wrapper around the Riverpod auth state. GoRouter needs
/// something it can `addListener` on to know when to re-run `redirect`;
/// AsyncNotifier exposes `value`, not a Listenable, so we bridge.
class _AuthStateListenable extends ChangeNotifier {
  _AuthStateListenable(this._ref) {
    _sub = _ref.listen<AsyncValue<AuthState>>(
      authNotifierProvider,
      (_, _) => notifyListeners(),
    );
  }
  final Ref _ref;
  late final ProviderSubscription<AsyncValue<AuthState>> _sub;

  @override
  void dispose() {
    _sub.close();
    super.dispose();
  }
}

final routerProvider = Provider<GoRouter>(buildRouter);