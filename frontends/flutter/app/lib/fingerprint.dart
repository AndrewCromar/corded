// Unlocking with a fingerprint.
//
// The passphrase is kept in the phone's secure hardware, under a key that
// Android only releases after a fingerprint (or face) check. The passphrase
// still works by itself and is still what protects a copy of the vault file.
import 'package:biometric_storage/biometric_storage.dart';

class Fingerprint {
  /// Whether this phone has a fingerprint (or face) set up to use.
  static Future<bool> available() async {
    try {
      return await BiometricStorage().canAuthenticate() == CanAuthenticateResponse.success;
    } catch (_) {
      return false;
    }
  }

  static Future<BiometricStorageFile> _file() => BiometricStorage().getStorage(
        'vault_passphrase',
        options: StorageFileInitOptions(authenticationRequired: true),
        promptInfo: const PromptInfo(
          androidPromptInfo: AndroidPromptInfo(
            title: 'Unlock Corded',
            negativeButton: 'Use passphrase',
            confirmationRequired: false,
          ),
        ),
      );

  /// Asks for a fingerprint and puts the passphrase away behind it.
  static Future<void> save(String passphrase) async => (await _file()).write(passphrase);

  /// Asks for a fingerprint and gives the passphrase back; null if the
  /// person backed out or nothing is stored.
  static Future<String?> read() async {
    try {
      return await (await _file()).read();
    } on AuthException {
      return null;
    }
  }

  static Future<void> clear() async {
    try {
      await (await _file()).delete();
    } catch (_) {
      // Nothing was stored.
    }
  }
}
