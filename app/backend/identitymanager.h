#pragma once

#include <QSslConfiguration>
#include <QSslCertificate>
#include <QSslKey>
#include <QSettings>

class IdentityManager
{
public:
    class ProcessIdentity
    {
    public:
        ProcessIdentity() = default;
        ProcessIdentity(
            QString uniqueId,
            QByteArray certificatePem,
            QByteArray privateKeyPem);
        ProcessIdentity(const ProcessIdentity&) = delete;
        ProcessIdentity& operator=(const ProcessIdentity&) = delete;
        ProcessIdentity(ProcessIdentity&& other) noexcept;
        ProcessIdentity& operator=(ProcessIdentity&& other) noexcept;
        ~ProcessIdentity();

        const QString& uniqueId() const;
        const QByteArray& certificatePem() const;
        const QByteArray& privateKeyPem() const;
        bool isValid() const;
        void clear();

    private:
        QString m_UniqueId;
        QByteArray m_CertificatePem;
        QByteArray m_PrivateKeyPem;

        friend class IdentityManager;
    };

    enum class ProcessIdentityInstallResult
    {
        Installed,
        InvalidIdentity,
        AlreadyInstalled,
        AlreadyInitialized,
    };

    QString
    getUniqueId();

    QByteArray
    getCertificate();

    QByteArray
    getPrivateKey();

    QSslConfiguration
    getSslConfig();

    static
    IdentityManager*
    get();

    static
    ProcessIdentityInstallResult
    installProcessIdentity(ProcessIdentity&& identity);

private:
    IdentityManager();
    explicit IdentityManager(ProcessIdentity&& identity);

    QSslCertificate
    getSslCertificate();

    QSslKey
    getSslKey();

    void
    createCredentials(QSettings& settings);

    // Initialized in constructor
    QByteArray m_CachedPrivateKey;
    QByteArray m_CachedPemCert;

    // Lazy initialized
    QString m_CachedUniqueId;
    QSslCertificate m_CachedSslCert;
    QSslKey m_CachedSslKey;

    static IdentityManager* s_Im;
    static ProcessIdentity* s_ProcessIdentity;
    static bool s_ProcessIdentityInstallAttempted;
};
