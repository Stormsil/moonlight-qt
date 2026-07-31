#include "identitymanager.h"
#include "utils.h"

#include <QDebug>

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <utility>

#define SER_UNIQUEID "uniqueid"
#define SER_CERT "certificate"
#define SER_KEY "key"

IdentityManager* IdentityManager::s_Im = nullptr;
IdentityManager::ProcessIdentity*
    IdentityManager::s_ProcessIdentity = nullptr;
bool IdentityManager::s_ProcessIdentityInstallAttempted = false;

namespace
{

void clearByteArray(QByteArray& value)
{
    if (!value.isEmpty()) {
        OPENSSL_cleanse(value.data(), value.size());
    }
    value.clear();
}

bool isCanonicalUniqueId(const QString& uniqueId)
{
    if (uniqueId.size() < 1 || uniqueId.size() > 16) {
        return false;
    }

    return std::all_of(
        uniqueId.cbegin(),
        uniqueId.cend(),
        [](QChar value) {
            const ushort character = value.unicode();
            return (character >= '0' && character <= '9')
                || (character >= 'a' && character <= 'f');
        });
}

bool remainingBioIsWhitespace(BIO* bio)
{
    char buffer[256];
    int length;
    while ((length = BIO_read(
                bio,
                buffer,
                sizeof(buffer))) > 0) {
        for (int index = 0; index < length; index++) {
            if (!std::isspace(
                    static_cast<unsigned char>(buffer[index]))) {
                return false;
            }
        }
    }

    return length == 0;
}

template<std::size_t Size>
bool startsWithLiteral(
    const QByteArray& value,
    int offset,
    const char (&prefix)[Size])
{
    constexpr int prefixLength = static_cast<int>(Size) - 1;
    return offset <= value.size() - prefixLength
        && std::equal(
            prefix,
            prefix + prefixLength,
            value.constData() + offset);
}

int firstNonWhitespaceOffset(const QByteArray& value)
{
    int offset = 0;
    while (offset < value.size()
            && std::isspace(
                static_cast<unsigned char>(value[offset]))) {
        offset++;
    }

    return offset;
}

bool hasSupportedCertificatePrefix(const QByteArray& value)
{
    const int offset = firstNonWhitespaceOffset(value);
    return startsWithLiteral(
        value,
        offset,
        "-----BEGIN CERTIFICATE-----");
}

bool hasSupportedPrivateKeyPrefix(const QByteArray& value)
{
    const int offset = firstNonWhitespaceOffset(value);
    return startsWithLiteral(
               value,
               offset,
               "-----BEGIN PRIVATE KEY-----")
        || startsWithLiteral(
               value,
               offset,
               "-----BEGIN RSA PRIVATE KEY-----");
}

bool isValidRsaIdentity(
    const QByteArray& certificatePem,
    const QByteArray& privateKeyPem)
{
    if (certificatePem.isEmpty()
            || privateKeyPem.isEmpty()
            || !hasSupportedCertificatePrefix(certificatePem)
            || !hasSupportedPrivateKeyPrefix(privateKeyPem)) {
        return false;
    }

    QSslCertificate sslCertificate(
        certificatePem,
        QSsl::Pem);
    QSslKey sslKey(
        privateKeyPem,
        QSsl::Rsa,
        QSsl::Pem,
        QSsl::PrivateKey);
    if (sslCertificate.isNull() || sslKey.isNull()) {
        return false;
    }

    BIO* certificateBio = BIO_new_mem_buf(
        certificatePem.constData(),
        certificatePem.size());
    BIO* privateKeyBio = BIO_new_mem_buf(
        privateKeyPem.constData(),
        privateKeyPem.size());
    if (certificateBio == nullptr || privateKeyBio == nullptr) {
        BIO_free(certificateBio);
        BIO_free(privateKeyBio);
        return false;
    }

    X509* certificate = PEM_read_bio_X509(
        certificateBio,
        nullptr,
        nullptr,
        nullptr);
    EVP_PKEY* privateKey = PEM_read_bio_PrivateKey(
        privateKeyBio,
        nullptr,
        nullptr,
        nullptr);
    EVP_PKEY* certificateKey = certificate == nullptr
        ? nullptr
        : X509_get_pubkey(certificate);
    bool valid = certificate != nullptr
        && privateKey != nullptr
        && certificateKey != nullptr
        && remainingBioIsWhitespace(certificateBio)
        && remainingBioIsWhitespace(privateKeyBio);
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    valid = valid
        && EVP_PKEY_is_a(certificateKey, "RSA") == 1
        && EVP_PKEY_is_a(privateKey, "RSA") == 1
        && EVP_PKEY_eq(certificateKey, privateKey) == 1;
#else
    valid = valid
        && EVP_PKEY_base_id(certificateKey) == EVP_PKEY_RSA
        && EVP_PKEY_base_id(privateKey) == EVP_PKEY_RSA
        && EVP_PKEY_cmp(certificateKey, privateKey) == 1;
#endif

    EVP_PKEY_free(certificateKey);
    EVP_PKEY_free(privateKey);
    X509_free(certificate);
    BIO_free(certificateBio);
    BIO_free(privateKeyBio);
    return valid;
}

}

IdentityManager::ProcessIdentity::ProcessIdentity(
    QString uniqueId,
    QByteArray certificatePem,
    QByteArray privateKeyPem)
    : m_UniqueId(std::move(uniqueId)),
      m_CertificatePem(std::move(certificatePem)),
      m_PrivateKeyPem(std::move(privateKeyPem))
{
}

IdentityManager::ProcessIdentity::ProcessIdentity(
    ProcessIdentity&& other) noexcept
    : m_UniqueId(std::move(other.m_UniqueId)),
      m_CertificatePem(std::move(other.m_CertificatePem)),
      m_PrivateKeyPem(std::move(other.m_PrivateKeyPem))
{
    other.clear();
}

IdentityManager::ProcessIdentity&
IdentityManager::ProcessIdentity::operator=(
    ProcessIdentity&& other) noexcept
{
    if (this != &other) {
        clear();
        m_UniqueId = std::move(other.m_UniqueId);
        m_CertificatePem = std::move(other.m_CertificatePem);
        m_PrivateKeyPem = std::move(other.m_PrivateKeyPem);
        other.clear();
    }
    return *this;
}

IdentityManager::ProcessIdentity::~ProcessIdentity()
{
    clear();
}

const QString&
IdentityManager::ProcessIdentity::uniqueId() const
{
    return m_UniqueId;
}

const QByteArray&
IdentityManager::ProcessIdentity::certificatePem() const
{
    return m_CertificatePem;
}

const QByteArray&
IdentityManager::ProcessIdentity::privateKeyPem() const
{
    return m_PrivateKeyPem;
}

bool IdentityManager::ProcessIdentity::isValid() const
{
    return isCanonicalUniqueId(m_UniqueId)
        && isValidRsaIdentity(
            m_CertificatePem,
            m_PrivateKeyPem);
}

void IdentityManager::ProcessIdentity::clear()
{
    m_UniqueId.fill(QChar('\0'));
    m_UniqueId.clear();
    clearByteArray(m_CertificatePem);
    clearByteArray(m_PrivateKeyPem);
}

IdentityManager*
IdentityManager::get()
{
    // This will always be called first on the main thread,
    // so it's safe to initialize without locks.
    if (s_Im == nullptr) {
        if (s_ProcessIdentity != nullptr) {
            s_Im = new IdentityManager(
                std::move(*s_ProcessIdentity));
            delete s_ProcessIdentity;
            s_ProcessIdentity = nullptr;
        }
        else {
            s_Im = new IdentityManager();
        }
    }

    return s_Im;
}

IdentityManager::ProcessIdentityInstallResult
IdentityManager::installProcessIdentity(ProcessIdentity&& identity)
{
    if (s_Im != nullptr) {
        return ProcessIdentityInstallResult::AlreadyInitialized;
    }
    if (s_ProcessIdentityInstallAttempted) {
        return ProcessIdentityInstallResult::AlreadyInstalled;
    }

    s_ProcessIdentityInstallAttempted = true;
    if (!identity.isValid()) {
        return ProcessIdentityInstallResult::InvalidIdentity;
    }

    s_ProcessIdentity =
        new ProcessIdentity(std::move(identity));
    return ProcessIdentityInstallResult::Installed;
}

void IdentityManager::createCredentials(QSettings& settings)
{
    X509* cert = X509_new();
    THROW_BAD_ALLOC_IF_NULL(cert);

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    EVP_PKEY* pk = EVP_RSA_gen(2048);
    THROW_BAD_ALLOC_IF_NULL(pk);
#else
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL);
    THROW_BAD_ALLOC_IF_NULL(ctx);

    EVP_PKEY_keygen_init(ctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048);

    // pk must be initialized on input
    EVP_PKEY* pk = NULL;
    EVP_PKEY_keygen(ctx, &pk);

    EVP_PKEY_CTX_free(ctx);
    THROW_BAD_ALLOC_IF_NULL(pk);
#endif

    X509_set_version(cert, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 0);
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), 60 * 60 * 24 * 365 * 20); // 20 yrs
#else
    X509_gmtime_adj(X509_getm_notBefore(cert), 0);
    X509_gmtime_adj(X509_getm_notAfter(cert), 60 * 60 * 24 * 365 * 20); // 20 yrs
#endif

    X509_set_pubkey(cert, pk);

    X509_NAME* name = X509_NAME_new();
    THROW_BAD_ALLOC_IF_NULL(name);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<unsigned char *>(const_cast<char*>("NVIDIA GameStream Client")),
                               -1, -1, 0);
    X509_set_subject_name(cert, name);
    X509_set_issuer_name(cert, name);
    X509_NAME_free(name);

    X509_sign(cert, pk, EVP_sha256());

    BIO* biokey = BIO_new(BIO_s_mem());
    THROW_BAD_ALLOC_IF_NULL(biokey);
    PEM_write_bio_PrivateKey(biokey, pk, NULL, NULL, 0, NULL, NULL);

    BIO* biocert = BIO_new(BIO_s_mem());
    THROW_BAD_ALLOC_IF_NULL(biocert);
    PEM_write_bio_X509(biocert, cert);

    BUF_MEM* mem;
    BIO_get_mem_ptr(biokey, &mem);
    m_CachedPrivateKey = QByteArray(mem->data, (int)mem->length);

    BIO_get_mem_ptr(biocert, &mem);
    m_CachedPemCert = QByteArray(mem->data, (int)mem->length);

    X509_free(cert);
    EVP_PKEY_free(pk);
    BIO_free(biokey);
    BIO_free(biocert);

    // Check that the new keypair is valid before persisting it
    if (getSslCertificate().isNull()) {
        qFatal("Newly generated certificate is unreadable");
    }
    if (getSslKey().isNull()) {
        qFatal("Newly generated private key is unreadable");
    }

    settings.setValue(SER_CERT, m_CachedPemCert);
    settings.setValue(SER_KEY, m_CachedPrivateKey);

    qInfo() << "Wrote new identity credentials to settings";
}

IdentityManager::IdentityManager()
{
    QSettings settings;

    m_CachedPemCert = settings.value(SER_CERT).toByteArray();
    m_CachedPrivateKey = settings.value(SER_KEY).toByteArray();

    if (m_CachedPemCert.isEmpty() || m_CachedPrivateKey.isEmpty()) {
        qInfo() << "No existing credentials found";
        createCredentials(settings);
    }
    else if (getSslCertificate().isNull()) {
        qWarning() << "Certificate is unreadable";
        createCredentials(settings);
    }
    else if (getSslKey().isNull()) {
        qWarning() << "Private key is unreadable";
        createCredentials(settings);
    }

    // We should have valid credentials now. If not, we're screwed
    if (getSslCertificate().isNull()) {
        qFatal("Certificate is unreadable");
    }
    if (getSslKey().isNull()) {
        qFatal("Private key is unreadable");
    }

    // Load the unique ID from settings
    m_CachedUniqueId = settings.value(SER_UNIQUEID).toString();
    if (!m_CachedUniqueId.isEmpty()) {
        qInfo() << "Loaded unique ID from settings:" << m_CachedUniqueId;
    }
    else {
        // Generate a new unique ID in base 16
        uint64_t uid;
        RAND_bytes(reinterpret_cast<unsigned char*>(&uid), sizeof(uid));
        m_CachedUniqueId = QString::number(uid, 16);

        qInfo() << "Generated new unique ID:" << m_CachedUniqueId;

        settings.setValue(SER_UNIQUEID, m_CachedUniqueId);
    }
}

IdentityManager::IdentityManager(ProcessIdentity&& identity)
    : m_CachedPrivateKey(
          std::move(identity.m_PrivateKeyPem)),
      m_CachedPemCert(
          std::move(identity.m_CertificatePem)),
      m_CachedUniqueId(
          std::move(identity.m_UniqueId))
{
    identity.clear();
}

QSslCertificate
IdentityManager::getSslCertificate()
{
    if (m_CachedSslCert.isNull()) {
        m_CachedSslCert = QSslCertificate(m_CachedPemCert);
    }
    return m_CachedSslCert;
}

QSslKey
IdentityManager::getSslKey()
{
    if (m_CachedSslKey.isNull()) {
        // This seemingly useless const_cast is required for old OpenSSL headers
        // where BIO_new_mem_buf's parameter is not declared const like those on
        // the Steam Link hardware.
        BIO* bio = BIO_new_mem_buf(const_cast<char*>(m_CachedPrivateKey.constData()), -1);
        THROW_BAD_ALLOC_IF_NULL(bio);

        EVP_PKEY* pk = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
        BIO_free(bio);

        bio = BIO_new(BIO_s_mem());
        THROW_BAD_ALLOC_IF_NULL(bio);

        // We must write out our PEM in the old PKCS1 format for SecureTransport
        // on macOS/iOS to be able to read it.
#ifdef Q_OS_DARWIN
        PEM_write_bio_PrivateKey_traditional(bio, pk, nullptr, nullptr, 0, nullptr, 0);
#else
        PEM_write_bio_PrivateKey(bio, pk, nullptr, nullptr, 0, nullptr, 0);
#endif

        BUF_MEM* mem;
        BIO_get_mem_ptr(bio, &mem);
        m_CachedSslKey = QSslKey(QByteArray(mem->data, (int)mem->length), QSsl::Rsa);

        BIO_free(bio);
        EVP_PKEY_free(pk);
    }
    return m_CachedSslKey;
}

QSslConfiguration
IdentityManager::getSslConfig()
{
    QSslConfiguration sslConfig(QSslConfiguration::defaultConfiguration());
    sslConfig.setLocalCertificate(getSslCertificate());
    sslConfig.setPrivateKey(getSslKey());
    return sslConfig;
}

QString
IdentityManager::getUniqueId()
{
    return m_CachedUniqueId;
}

QByteArray
IdentityManager::getCertificate()
{
    return m_CachedPemCert;
}

QByteArray
IdentityManager::getPrivateKey()
{
    return m_CachedPrivateKey;
}
