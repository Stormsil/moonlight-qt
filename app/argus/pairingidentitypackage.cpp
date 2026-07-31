#include "pairingidentitypackage.h"
#include "startupchannel.h"

#include <QScopeGuard>
#include <QtEndian>

#include <algorithm>
#include <cstring>

namespace
{

constexpr int PackageVersion = 1;
constexpr int MagicLength = 8;
constexpr int HeaderLength =
    MagicLength + sizeof(qint32) + sizeof(quint16);
constexpr int FieldHeaderLength =
    sizeof(quint16) + sizeof(qint32);
constexpr quint16 UniqueIdField = 1;
constexpr quint16 CertificateField = 2;
constexpr quint16 PrivateKeyField = 3;
constexpr quint16 FieldCount = 3;
constexpr int MaximumUniqueIdBytes = 16;
constexpr int MaximumCertificateBytes = 64 * 1024;
constexpr int MaximumPrivateKeyBytes = 128 * 1024;
constexpr char Magic[] = "MLQTIDPK";

class PackageReader
{
public:
    explicit PackageReader(
        const QByteArray& package,
        qsizetype offset = 0)
        : m_package(package),
          m_offset(offset)
    {
    }

    bool readInt32(qint32& value)
    {
        if (!canRead(sizeof(value))) {
            return false;
        }

        value = qFromLittleEndian<qint32>(
            reinterpret_cast<const unsigned char*>(
                m_package.constData() + m_offset));
        m_offset += sizeof(value);
        return true;
    }

    bool readUInt16(quint16& value)
    {
        if (!canRead(sizeof(value))) {
            return false;
        }

        value = qFromLittleEndian<quint16>(
            reinterpret_cast<const unsigned char*>(
                m_package.constData() + m_offset));
        m_offset += sizeof(value);
        return true;
    }

    bool readField(
        quint16 expectedType,
        int maximumLength,
        QByteArray& value)
    {
        quint16 type;
        qint32 length;
        if (!readUInt16(type)
                || type != expectedType
                || !readInt32(length)
                || length < 1
                || length > maximumLength
                || !canRead(length)) {
            return false;
        }

        value = QByteArray(
            m_package.constData() + m_offset,
            length);
        m_offset += length;
        return true;
    }

    bool atEnd() const
    {
        return m_offset == m_package.size();
    }

private:
    bool canRead(qsizetype length) const
    {
        return length >= 0
            && m_offset <= m_package.size() - length;
    }

    const QByteArray& m_package;
    qsizetype m_offset = 0;
};

class ScopedPackageZero
{
public:
    explicit ScopedPackageZero(QByteArray& package)
        : m_package(package)
    {
    }

    ~ScopedPackageZero()
    {
        ArgusWorker::secureZero(
            m_package.data(),
            m_package.size());
        m_package.clear();
    }

private:
    QByteArray& m_package;
};

bool isCanonicalUniqueId(const QByteArray& uniqueId)
{
    if (uniqueId.size() < 1
            || uniqueId.size() > MaximumUniqueIdBytes) {
        return false;
    }

    return std::all_of(
        uniqueId.cbegin(),
        uniqueId.cend(),
        [](char value) {
            return (value >= '0' && value <= '9')
                || (value >= 'a' && value <= 'f');
        });
}

void appendInt32(QByteArray& package, qint32 value)
{
    const qint32 littleEndian = qToLittleEndian(value);
    package.append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendUInt16(QByteArray& package, quint16 value)
{
    const quint16 littleEndian = qToLittleEndian(value);
    package.append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendField(
    QByteArray& package,
    quint16 type,
    const QByteArray& value)
{
    appendUInt16(package, type);
    appendInt32(package, value.size());
    package.append(value);
}

}

namespace ArgusWorker
{

PairingIdentityPackageStatus PairingIdentityPackage::encode(
    IdentityManager& manager,
    QByteArray& package)
{
    secureZero(package.data(), package.size());
    package.clear();
    QByteArray uniqueId = manager.getUniqueId().toLatin1();
    QByteArray certificatePem = manager.getCertificate();
    QByteArray privateKeyPem = manager.getPrivateKey();
    const auto sourceZero = qScopeGuard(
        [&uniqueId, &certificatePem, &privateKeyPem]() {
            secureZero(uniqueId.data(), uniqueId.size());
            secureZero(certificatePem.data(), certificatePem.size());
            secureZero(privateKeyPem.data(), privateKeyPem.size());
        });
    if (!isCanonicalUniqueId(uniqueId)
            || certificatePem.isEmpty()
            || certificatePem.size() > MaximumCertificateBytes
            || privateKeyPem.isEmpty()
            || privateKeyPem.size() > MaximumPrivateKeyBytes) {
        return PairingIdentityPackageStatus::InvalidIdentity;
    }

    package.append(Magic, MagicLength);
    appendInt32(package, PackageVersion);
    appendUInt16(package, FieldCount);
    appendField(package, UniqueIdField, uniqueId);
    appendField(package, CertificateField, certificatePem);
    appendField(package, PrivateKeyField, privateKeyPem);
    if (package.size() > MaximumIdentityBytes) {
        secureZero(package.data(), package.size());
        package.clear();
        return PairingIdentityPackageStatus::InvalidPackage;
    }
    return PairingIdentityPackageStatus::Accepted;
}

PairingIdentityPackageStatus PairingIdentityPackage::decode(
    const QString& format,
    QByteArray package,
    IdentityManager::ProcessIdentity& identity)
{
    ScopedPackageZero packageZero(package);
    identity.clear();
    if (format != QLatin1String(PairingIdentityFormat)) {
        return PairingIdentityPackageStatus::UnsupportedFormat;
    }
    if (package.size() < HeaderLength
            || package.size() > MaximumIdentityBytes
            || std::memcmp(
                package.constData(),
                Magic,
                MagicLength) != 0) {
        return PairingIdentityPackageStatus::InvalidPackage;
    }

    PackageReader bodyReader(package, MagicLength);
    qint32 version;
    quint16 fieldCount;
    QByteArray uniqueId;
    QByteArray certificatePem;
    QByteArray privateKeyPem;
    if (!bodyReader.readInt32(version)
            || version != PackageVersion
            || !bodyReader.readUInt16(fieldCount)
            || fieldCount != FieldCount
            || !bodyReader.readField(
                UniqueIdField,
                MaximumUniqueIdBytes,
                uniqueId)
            || !bodyReader.readField(
                CertificateField,
                MaximumCertificateBytes,
                certificatePem)
            || !bodyReader.readField(
                PrivateKeyField,
                MaximumPrivateKeyBytes,
                privateKeyPem)
            || !bodyReader.atEnd()
            || !isCanonicalUniqueId(uniqueId)) {
        secureZero(uniqueId.data(), uniqueId.size());
        secureZero(
            certificatePem.data(),
            certificatePem.size());
        secureZero(
            privateKeyPem.data(),
            privateKeyPem.size());
        return PairingIdentityPackageStatus::InvalidPackage;
    }

    IdentityManager::ProcessIdentity candidate(
        QString::fromLatin1(uniqueId),
        std::move(certificatePem),
        std::move(privateKeyPem));
    secureZero(uniqueId.data(), uniqueId.size());
    if (!candidate.isValid()) {
        return PairingIdentityPackageStatus::InvalidIdentity;
    }

    identity = std::move(candidate);
    return PairingIdentityPackageStatus::Accepted;
}

}
