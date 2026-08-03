import 'package:flutter_riverpod/flutter_riverpod.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';

/// Key the access token is stored under in the platform secure store.
const String _kJwtKey = 'jwt_token';

/// Thin wrapper over [FlutterSecureStorage] so the rest of the app
/// talks in terms of `read()` / `write()` / `delete()` without seeing
/// the platform options.
///
/// Android uses EncryptedSharedPreferences (AES + Android Keystore).
/// iOS uses the default Keychain entry (first_unlock_this_device).
class SecureJwtStorage {
  SecureJwtStorage([FlutterSecureStorage? storage])
      : _storage = storage ??
            const FlutterSecureStorage(
              aOptions: AndroidOptions(encryptedSharedPreferences: true),
              iOptions: IOSOptions(
                accessibility: KeychainAccessibility.first_unlock_this_device,
              ),
            );

  final FlutterSecureStorage _storage;

  Future<String?> read() => _storage.read(key: _kJwtKey);

  Future<void> write(String token) => _storage.write(
        key: _kJwtKey,
        value: token,
      );

  Future<void> delete() => _storage.delete(key: _kJwtKey);
}

/// Riverpod entry point — kept tiny on purpose; the [SecureJwtStorage]
/// itself has no state worth hiding behind a Notifier.
final secureStorageProvider = Provider<SecureJwtStorage>((ref) {
  return SecureJwtStorage();
});