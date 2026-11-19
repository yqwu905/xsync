#include "core/SecretStore.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QStandardPaths>

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <sys/stat.h>
#endif

namespace xsync {

#ifdef _WIN32

QString SecretStore::protect(const QString& plaintext) {
    if (plaintext.isEmpty()) return {};
    const QByteArray utf8 = plaintext.toUtf8();
    DATA_BLOB inBlob{};
    inBlob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(utf8.constData()));
    inBlob.cbData = static_cast<DWORD>(utf8.size());

    DATA_BLOB outBlob{};
    if (!CryptProtectData(&inBlob, L"xsync-cred", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        return {};
    }
    QByteArray enc(reinterpret_cast<const char*>(outBlob.pbData),
                   static_cast<qsizetype>(outBlob.cbData));
    SecureZeroMemory(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return QStringLiteral("dpapi:") + QString::fromLatin1(enc.toBase64());
}

QString SecretStore::unprotect(const QString& ciphertext) {
    if (ciphertext.isEmpty()) return {};
    if (!ciphertext.startsWith(QLatin1String("dpapi:"))) {
        return {};
    }
    const QByteArray enc = QByteArray::fromBase64(ciphertext.mid(6).toLatin1());
    if (enc.isEmpty()) return {};

    DATA_BLOB inBlob{};
    inBlob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(enc.constData()));
    inBlob.cbData = static_cast<DWORD>(enc.size());

    DATA_BLOB outBlob{};
    if (!CryptUnprotectData(&inBlob, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &outBlob)) {
        return {};
    }
    QByteArray dec(reinterpret_cast<const char*>(outBlob.pbData),
                   static_cast<qsizetype>(outBlob.cbData));
    SecureZeroMemory(outBlob.pbData, outBlob.cbData);
    LocalFree(outBlob.pbData);
    return QString::fromUtf8(dec);
}

#else

namespace {

QByteArray loadOrCreateMasterKey() {
    const QString configDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(configDir);
    const QString keyPath = configDir + QStringLiteral("/.xsync.key");

    QFile f(keyPath);
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        QByteArray key = f.readAll();
        f.close();
        if (key.size() == 32) {
            return key;
        }
    }

    QByteArray key(32, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(key.data()), 32) != 1) {
        return {};
    }
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(key);
        f.close();
        ::chmod(keyPath.toLocal8Bit().constData(), 0600);
    }
    return key;
}

}  // namespace

QString SecretStore::protect(const QString& plaintext) {
    if (plaintext.isEmpty()) return {};
    const QByteArray key = loadOrCreateMasterKey();
    if (key.size() != 32) return {};

    const QByteArray plain = plaintext.toUtf8();
    unsigned char iv[12];
    if (RAND_bytes(iv, sizeof(iv)) != 1) return {};

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    QByteArray cipher(plain.size() + 16, '\0');
    int outLen1 = 0;
    int outLen2 = 0;
    unsigned char tag[16];
    bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, sizeof(iv), nullptr) == 1 &&
              EVP_EncryptInit_ex(ctx, nullptr, nullptr,
                                 reinterpret_cast<const unsigned char*>(key.constData()), iv) == 1 &&
              EVP_EncryptUpdate(ctx, reinterpret_cast<unsigned char*>(cipher.data()), &outLen1,
                                reinterpret_cast<const unsigned char*>(plain.constData()),
                                static_cast<int>(plain.size())) == 1 &&
              EVP_EncryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(cipher.data()) + outLen1,
                                  &outLen2) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, sizeof(tag), tag) == 1;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) return {};
    cipher.resize(outLen1 + outLen2);

    QByteArray blob;
    blob.reserve(sizeof(iv) + sizeof(tag) + cipher.size());
    blob.append(reinterpret_cast<const char*>(iv), sizeof(iv));
    blob.append(reinterpret_cast<const char*>(tag), sizeof(tag));
    blob.append(cipher);
    return QStringLiteral("aesgcm:") + QString::fromLatin1(blob.toBase64());
}

QString SecretStore::unprotect(const QString& ciphertext) {
    if (ciphertext.isEmpty()) return {};
    if (!ciphertext.startsWith(QLatin1String("aesgcm:"))) return {};
    const QByteArray key = loadOrCreateMasterKey();
    if (key.size() != 32) return {};

    const QByteArray blob = QByteArray::fromBase64(ciphertext.mid(7).toLatin1());
    if (blob.size() < 28) return {};

    const unsigned char* iv = reinterpret_cast<const unsigned char*>(blob.constData());
    const unsigned char* tag = reinterpret_cast<const unsigned char*>(blob.constData() + 12);
    const unsigned char* enc = reinterpret_cast<const unsigned char*>(blob.constData() + 28);
    const int encLen = static_cast<int>(blob.size() - 28);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return {};

    QByteArray plain(encLen + 16, '\0');
    int outLen1 = 0;
    int outLen2 = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
              EVP_DecryptInit_ex(ctx, nullptr, nullptr,
                                 reinterpret_cast<const unsigned char*>(key.constData()), iv) == 1 &&
              EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(plain.data()), &outLen1,
                                enc, encLen) == 1 &&
              EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16,
                                  const_cast<unsigned char*>(tag)) == 1 &&
              EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(plain.data()) + outLen1,
                                  &outLen2) == 1;
    EVP_CIPHER_CTX_free(ctx);

    if (!ok) return {};
    plain.resize(outLen1 + outLen2);
    return QString::fromUtf8(plain);
}

#endif

}  // namespace xsync
