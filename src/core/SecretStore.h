#pragma once

#include <QString>

namespace xsync {

// Encrypts/decrypts saved passwords & key passphrases at rest.
// - On Windows: uses native OS Windows DPAPI (CryptProtectData / CryptUnprotectData),
//   scoped to the current Windows user account.
// - On Linux/macOS: uses AES-256-GCM (OpenSSL) keyed with a per-user machine key
//   stored with 0600 permissions under the user's config directory.
class SecretStore {
public:
    static QString protect(const QString& plaintext);
    static QString unprotect(const QString& ciphertext);
};

}  // namespace xsync
