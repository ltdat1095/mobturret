import 'package:flutter/material.dart';
import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:go_router/go_router.dart';

import '../../../core/router.dart';
import '../application/auth_notifier.dart';

class LoginScreen extends ConsumerStatefulWidget {
  const LoginScreen({super.key});

  @override
  ConsumerState<LoginScreen> createState() => _LoginScreenState();
}

class _LoginScreenState extends ConsumerState<LoginScreen> {
  final _formKey = GlobalKey<FormState>();
  final _emailCtrl = TextEditingController();
  final _pwCtrl = TextEditingController();

  @override
  void dispose() {
    _emailCtrl.dispose();
    _pwCtrl.dispose();
    super.dispose();
  }

  Future<void> _submit() async {
    if (!_formKey.currentState!.validate()) return;
    await ref
        .read(authNotifierProvider.notifier)
        .login(_emailCtrl.text.trim(), _pwCtrl.text);
  }

  @override
  Widget build(BuildContext context) {
    // Watch the notifier state so the form can show a spinner and
    // surface any error message from the previous attempt.
    final auth = ref.watch(authNotifierProvider).value;
    final busy = auth is AuthAuthenticating;
    final errorText = (auth is AuthUnauthenticated) ? auth.error : null;

    return Scaffold(
      appBar: AppBar(title: const Text('Sign in')),
      body: Center(
        child: SingleChildScrollView(
          padding: const EdgeInsets.all(24),
          child: Form(
            key: _formKey,
            child: Column(
              mainAxisSize: MainAxisSize.min,
              children: [
                TextFormField(
                  controller: _emailCtrl,
                  keyboardType: TextInputType.emailAddress,
                  textInputAction: TextInputAction.next,
                  enabled: !busy,
                  decoration: const InputDecoration(labelText: 'Email'),
                  validator: (v) {
                    final s = v?.trim() ?? '';
                    if (s.isEmpty) return 'Email is required';
                    if (!s.contains('@')) return 'Enter a valid email';
                    return null;
                  },
                  // Suppress the Material 3 floating selection toolbar
                  // that Android 16 shows over the IME on this AVD
                  // (hides the full QWERTY keyboard). Returning a
                  // zero-size box keeps the field editable but kills
                  // the floating magnifier / cut-copy-paste handle.
                  contextMenuBuilder: (
                    BuildContext context,
                    EditableTextState editableTextState,
                  ) =>
                      const SizedBox.shrink(),
                ),
                const SizedBox(height: 12),
                TextFormField(
                  controller: _pwCtrl,
                  obscureText: true,
                  enabled: !busy,
                  decoration: const InputDecoration(labelText: 'Password'),
                  validator: (v) =>
                      (v == null || v.isEmpty) ? 'Password is required' : null,
                  onFieldSubmitted: (_) => _submit(),
                  contextMenuBuilder: (
                    BuildContext context,
                    EditableTextState editableTextState,
                  ) =>
                      const SizedBox.shrink(),
                ),
                if (errorText != null) ...[
                  const SizedBox(height: 12),
                  Text(
                    errorText,
                    style: TextStyle(color: Theme.of(context).colorScheme.error),
                    textAlign: TextAlign.center,
                  ),
                ],
                const SizedBox(height: 24),
                FilledButton(
                  onPressed: busy ? null : _submit,
                  child: busy
                      ? const SizedBox(
                          height: 20,
                          width: 20,
                          child: CircularProgressIndicator(strokeWidth: 2),
                        )
                      : const Text('Sign in'),
                ),
                const SizedBox(height: 8),
                TextButton(
                  onPressed: busy ? null : () => context.go(AppRoutes.signup),
                  child: const Text('Create an account'),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}