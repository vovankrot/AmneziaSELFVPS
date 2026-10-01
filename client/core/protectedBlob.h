#pragma once
#include <QByteArray>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <memory>

namespace ProtectedBlob {
inline QByteArray seal(const QByteArray &plain, const QByteArray &key)
{
    if (key.size() != 32 || plain.isEmpty()) return {};
    QByteArray nonce(12, '\0'), tag(16, '\0'), cipher(plain.size() + 16, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char *>(nonce.data()), nonce.size()) != 1) return {};
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int n = 0, total = 0;
    if (!ctx || EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(nonce.constData())) != 1
        || EVP_EncryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(cipher.data()), &n,
            reinterpret_cast<const unsigned char *>(plain.constData()), plain.size()) != 1) return {};
    total = n;
    if (EVP_EncryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(cipher.data()) + total, &n) != 1
        || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) != 1) return {};
    cipher.resize(total + n);
    return QByteArray("SVPSGCM1") + nonce + tag + cipher;
}
inline QByteArray open(const QByteArray &sealed, const QByteArray &key)
{
    if (key.size() != 32 || sealed.size() <= 36 || !sealed.startsWith("SVPSGCM1")) return {};
    QByteArray tag = sealed.mid(20, 16), plain(sealed.size() - 36 + 16, '\0');
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    int n = 0, total = 0;
    if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr,
        reinterpret_cast<const unsigned char *>(key.constData()), reinterpret_cast<const unsigned char *>(sealed.constData() + 8)) != 1
        || EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char *>(plain.data()), &n,
            reinterpret_cast<const unsigned char *>(sealed.constData() + 36), sealed.size() - 36) != 1) return {};
    total = n;
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, tag.size(), tag.data()) != 1
        || EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char *>(plain.data()) + total, &n) != 1) return {};
    plain.resize(total + n);
    return plain;
}
}
