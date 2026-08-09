#include "argus/workerbootstrap.h"
#include "argus/frameslotwriter.h"
#include "argus/pairingclient.h"
#include "argus/pairingendpoint.h"
#include "argus/startupchannel.h"
#include "argus/pairingidentitypackage.h"
#include "argus/streamrequestvalidation.h"
#include "backend/identitymanager.h"
#include "backend/nvaddress.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtEndian>

#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <limits>
#include <cstdio>
#include <thread>
#include <vector>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace
{

int failures = 0;

void check(bool condition, const char* message)
{
    if (!condition) {
        qCritical().noquote() << message;
        failures++;
    }
}

bool selectsWorker(std::initializer_list<const char*> arguments)
{
    std::vector<QByteArray> storage;
    storage.reserve(arguments.size());
    for (const char* argument : arguments) {
        storage.emplace_back(argument);
    }

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (QByteArray& argument : storage) {
        argv.push_back(argument.data());
    }

    return ArgusWorker::isRequested(
        static_cast<int>(argv.size()),
        argv.data());
}

void checkStreamRequestValidationFailures()
{
    using Failure = ArgusWorker::StreamRequestValidationFailure;
    using ArgusWorker::streamRequestValidationFailureCode;

    const ArgusWorker::StartupFrameSlotDescriptor slot{
        QStringLiteral("Local\\Argus.Stream.Frame.test"),
        QStringLiteral("Local\\Argus.Stream.FrameLock.test"),
        1,
        1920,
        1080,
        1920 * 1080 * 4};
    ArgusWorker::StartupFrameSlotDescriptor differentSlot = slot;
    differentSlot.maxPayloadBytes--;

    const std::vector<std::pair<Failure, qint32>> expected{
        {Failure::InputIsolationUnavailable, 1001},
        {Failure::StartupEndpointMismatch, 1002},
        {Failure::StartupDisplayMissing, 1003},
        {Failure::StartupDisplayMismatch, 1004},
        {Failure::StartupFrameSlotMismatch, 1005},
        {Failure::EndpointInvalid, 1006},
        {Failure::ServerCertificateInvalid, 1007},
        {Failure::FrameSlotOpenFailed, 1008},
        {Failure::FrameSinkInstallFailed, 1009},
    };
    std::vector<qint32> codes;
    for (const auto& entry : expected) {
        const qint32 code = streamRequestValidationFailureCode(entry.first);
        check(code == entry.second,
              "RequestValidation failure code changed");
        check(code > 0 && code <= 4096,
              "RequestValidation failure code must remain bounded");
        check(ArgusWorker::isKnownStreamRequestValidationFailureCode(code),
              "Declared RequestValidation failure code must be known");
        codes.push_back(code);
    }
    std::sort(codes.begin(), codes.end());
    check(std::adjacent_find(codes.cbegin(), codes.cend()) == codes.cend(),
          "RequestValidation failure codes must be unique");
    check(!ArgusWorker::isKnownStreamRequestValidationFailureCode(0)
              && !ArgusWorker::isKnownStreamRequestValidationFailureCode(1000)
              && !ArgusWorker::isKnownStreamRequestValidationFailureCode(1010),
          "Unknown RequestValidation failure codes must fail closed");

    check(ArgusWorker::classifyInputIsolation(true) == Failure::None,
          "Supported input isolation must preserve request processing");
    check(ArgusWorker::classifyStartupRequest(
              QStringLiteral("http://startup"),
              QStringLiteral("DISPLAY"),
              slot,
              QStringLiteral("http://startup"),
              QStringLiteral("DISPLAY"),
              slot)
              == Failure::None,
          "Matching startup request must preserve request processing");
    check(ArgusWorker::classifyEndpointAndCertificate(true, true)
              == Failure::None,
          "Valid endpoint and certificate must preserve request processing");
    check(ArgusWorker::classifyFrameSlotOpen(
              ArgusWorker::FrameSlotWriterStatus::Opened)
              == Failure::None,
          "Opened frame slot must preserve request processing");
    check(ArgusWorker::classifyFrameSinkInstall(true) == Failure::None,
          "Installed frame sink must preserve request processing");

    check(ArgusWorker::classifyInputIsolation(false)
              == Failure::InputIsolationUnavailable,
          "Unavailable input isolation must have a distinct failure");
    check(ArgusWorker::classifyStartupRequest(
              QStringLiteral("http://startup"),
              QStringLiteral("DISPLAY"),
              slot,
              QStringLiteral("http://request"),
              QStringLiteral("DISPLAY"),
              slot)
              == Failure::StartupEndpointMismatch,
          "Startup endpoint mismatch must have a distinct failure");
    check(ArgusWorker::classifyStartupRequest(
              QStringLiteral("http://startup"),
              QString(),
              slot,
              QStringLiteral("http://startup"),
              QString(),
              slot)
              == Failure::StartupDisplayMissing,
          "Missing startup display must have a distinct failure");
    check(ArgusWorker::classifyStartupRequest(
              QStringLiteral("http://startup"),
              QStringLiteral("DISPLAY-A"),
              slot,
              QStringLiteral("http://startup"),
              QStringLiteral("DISPLAY-B"),
              slot)
              == Failure::StartupDisplayMismatch,
          "Startup display mismatch must have a distinct failure");
    check(ArgusWorker::classifyStartupRequest(
              QStringLiteral("http://startup"),
              QStringLiteral("DISPLAY"),
              slot,
              QStringLiteral("http://startup"),
              QStringLiteral("DISPLAY"),
              differentSlot)
              == Failure::StartupFrameSlotMismatch,
          "Startup frame-slot mismatch must have a distinct failure");
    check(ArgusWorker::classifyEndpointAndCertificate(false, true)
              == Failure::EndpointInvalid,
          "Invalid endpoint must have a distinct failure");
    check(ArgusWorker::classifyEndpointAndCertificate(true, false)
              == Failure::ServerCertificateInvalid,
          "Invalid certificate must have a distinct failure");
    check(ArgusWorker::classifyFrameSlotOpen(
              ArgusWorker::FrameSlotWriterStatus::Unavailable)
              == Failure::FrameSlotOpenFailed,
          "Frame-slot open failure must have a distinct failure");
    check(ArgusWorker::classifyFrameSinkInstall(false)
              == Failure::FrameSinkInstallFailed,
          "Frame-sink install failure must have a distinct failure");
}

void appendInt32(QByteArray& bytes, qint32 value)
{
    const qint32 littleEndian = qToLittleEndian(value);
    bytes.append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendUInt16(QByteArray& bytes, quint16 value)
{
    const quint16 littleEndian = qToLittleEndian(value);
    bytes.append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendInt64(QByteArray& bytes, qint64 value)
{
    const qint64 littleEndian = qToLittleEndian(value);
    bytes.append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendText(QByteArray& bytes, const QByteArray& text)
{
    appendInt32(bytes, text.size());
    bytes.append(text);
}

void appendIdentityField(
    QByteArray& package,
    quint16 type,
    const QByteArray& value)
{
    appendUInt16(package, type);
    appendInt32(package, value.size());
    package.append(value);
}

class TestIdentity
{
public:
    TestIdentity() = default;
    TestIdentity(const TestIdentity&) = delete;
    TestIdentity& operator=(const TestIdentity&) = delete;
    TestIdentity(TestIdentity&&) = default;
    TestIdentity& operator=(TestIdentity&&) = default;

    ~TestIdentity()
    {
        ArgusWorker::secureZero(
            certificatePem.data(),
            certificatePem.size());
        ArgusWorker::secureZero(
            privateKeyPem.data(),
            privateKeyPem.size());
    }

    QByteArray certificatePem;
    QByteArray privateKeyPem;
};

TestIdentity createTestIdentity()
{
    TestIdentity identity;
    X509* certificate = X509_new();
    EVP_PKEY* key = EVP_RSA_gen(2048);
    if (certificate == nullptr || key == nullptr) {
        X509_free(certificate);
        EVP_PKEY_free(key);
        return identity;
    }

    X509_set_version(certificate, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate), 577);
    X509_gmtime_adj(X509_getm_notBefore(certificate), -60);
    X509_gmtime_adj(
        X509_getm_notAfter(certificate),
        60 * 60 * 24);
    X509_set_pubkey(certificate, key);
    X509_NAME* name = X509_NAME_new();
    if (name != nullptr) {
        constexpr char CommonName[] = "Argus issue 577 test identity";
        X509_NAME_add_entry_by_txt(
            name,
            "CN",
            MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>(CommonName),
            -1,
            -1,
            0);
        X509_set_subject_name(certificate, name);
        X509_set_issuer_name(certificate, name);
        X509_NAME_free(name);
    }

    BIO* certificateBio = BIO_new(BIO_s_mem());
    BIO* keyBio = BIO_new(BIO_s_mem());
    if (name == nullptr
            || certificateBio == nullptr
            || keyBio == nullptr
            || X509_sign(certificate, key, EVP_sha256()) <= 0
            || PEM_write_bio_X509(certificateBio, certificate) != 1
            || PEM_write_bio_PrivateKey(
                keyBio,
                key,
                nullptr,
                nullptr,
                0,
                nullptr,
                nullptr) != 1) {
        BIO_free(certificateBio);
        BIO_free(keyBio);
        X509_free(certificate);
        EVP_PKEY_free(key);
        return identity;
    }

    BUF_MEM* memory = nullptr;
    BIO_get_mem_ptr(certificateBio, &memory);
    identity.certificatePem =
        QByteArray(memory->data, static_cast<int>(memory->length));
    BIO_get_mem_ptr(keyBio, &memory);
    identity.privateKeyPem =
        QByteArray(memory->data, static_cast<int>(memory->length));

    BIO_free(certificateBio);
    BIO_free(keyBio);
    X509_free(certificate);
    EVP_PKEY_free(key);
    return identity;
}

QByteArray encodeIdentityPackage(
    const QByteArray& uniqueId,
    const QByteArray& certificatePem,
    const QByteArray& privateKeyPem,
    qint32 version = 1,
    quint16 firstFieldType = 1,
    bool appendTrailingByte = false)
{
    QByteArray package("MLQTIDPK", 8);
    appendInt32(package, version);
    appendUInt16(package, 3);
    appendIdentityField(package, firstFieldType, uniqueId);
    appendIdentityField(package, 2, certificatePem);
    appendIdentityField(package, 3, privateKeyPem);
    if (appendTrailingByte) {
        package.append('\0');
    }
    return package;
}

ArgusWorker::StartupSession fixtureSession()
{
    ArgusWorker::StartupSession session;
    const auto copyGuid = [](
        const char* hex,
        ArgusWorker::StartupGuidBytes& destination) {
        const QByteArray bytes = QByteArray::fromHex(hex);
        std::copy(
            bytes.cbegin(),
            bytes.cend(),
            destination.begin());
    };
    copyGuid(
        "00aadff26a0815469d1a834cb9feb902",
        session.machineId);
    copyGuid(
        "688bb868163ab742a1ae7188e1d7ca51",
        session.attemptId);
    copyGuid(
        "77e7df85cfcdfe4da65f33ac70a44ad6",
        session.sessionId);
    return session;
}

QByteArray encodeSession(const ArgusWorker::StartupSession& session)
{
    QByteArray bytes;
    bytes.append(
        reinterpret_cast<const char*>(session.machineId.data()),
        static_cast<qsizetype>(session.machineId.size()));
    bytes.append(
        reinterpret_cast<const char*>(session.attemptId.data()),
        static_cast<qsizetype>(session.attemptId.size()));
    bytes.append(
        reinterpret_cast<const char*>(session.sessionId.data()),
        static_cast<qsizetype>(session.sessionId.size()));
    return bytes;
}

QByteArray encodeChallenge(
    const ArgusWorker::StartupSession& session,
    qint32 version = ArgusWorker::StartupProtocolVersion)
{
    const QByteArray nonce(32, '\x5a');
    QByteArray challenge;
    appendInt32(challenge, version);
    appendInt32(challenge, nonce.size());
    challenge.append(nonce);
    challenge.append(encodeSession(session));
    return challenge;
}

QByteArray encodePayload(
    const ArgusWorker::StartupSession& session,
    const QByteArray& endpoint =
        "https://127.0.0.1:47984",
    const QByteArray& identity = QByteArray::fromHex("0001feff"),
    bool includeFrameSlot = true,
    qint32 maxWidth = 1920)
{
    QByteArray payload;
    appendInt32(payload, ArgusWorker::StartupProtocolVersion);
    payload.append(encodeSession(session));
    appendText(payload, endpoint);
    payload.append('\x01');
    appendText(payload, "{17ad0e1c-9a35-4ae8-bc82-91f7e12d98f0}");
    appendText(payload, "moonlight-qt");
    appendInt32(payload, identity.size());
    payload.append(identity);
    payload.append(includeFrameSlot ? '\x01' : '\x00');
    if (includeFrameSlot) {
        appendText(payload, "Local\\Argus.Stream.Frame.test");
        appendText(payload, "Local\\Argus.Stream.FrameLock.test");
        appendInt32(payload, 2);
        appendInt32(payload, maxWidth);
        appendInt32(payload, 1080);
        appendInt32(payload, 8 * 1024 * 1024);
    }
    return payload;
}

QByteArray encodeStreamRequest(
    const ArgusWorker::StartupSession& session,
    const QByteArray& nonce,
    const QByteArray& endpoint = "http://127.0.0.1:48989",
    const QByteArray& serverCertificate = "server-certificate")
{
    QByteArray request;
    appendInt32(request, ArgusWorker::StartupProtocolVersion);
    appendInt32(request, 3);
    request.append(encodeSession(session));
    appendText(request, nonce);
    appendText(request, endpoint);
    appendText(request, serverCertificate);
    appendText(request, "{17ad0e1c-9a35-4ae8-bc82-91f7e12d98f0}");
    appendInt32(request, 1);
    appendInt32(request, 1);
    appendInt32(request, 1920);
    appendInt32(request, 1080);
    appendInt32(request, 60);
    appendInt32(request, 15000);
    appendText(request, "Local\\Argus.Stream.Frame.fixture");
    appendText(request, "Local\\Argus.Stream.FrameLock.fixture");
    appendInt32(request, 2);
    appendInt32(request, 1920);
    appendInt32(request, 1080);
    appendInt32(request, 8 * 1024 * 1024);
    return request;
}

void checkStreamControlCodec()
{
    const ArgusWorker::StartupSession session = fixtureSession();
    const QByteArray nonce(32, '\x5a');
    const QByteArray packet = encodeStreamRequest(session, nonce);
    ArgusWorker::StreamControlRequest request;
    check(ArgusWorker::StreamControlCodec::decodeRequest(
              packet,
              session,
              nonce,
              request)
              == ArgusWorker::StreamControlCodecStatus::Accepted,
          "Managed stream request fixture must decode");
    check(request.endpoint()
              == QStringLiteral("http://127.0.0.1:48989")
              && request.displayId()
                  == QStringLiteral(
                      "{17ad0e1c-9a35-4ae8-bc82-91f7e12d98f0}")
              && request.appId() == 1
              && request.codec()
                  == ArgusWorker::StreamVideoCodec::H264
              && request.width() == 1920
              && request.height() == 1080
              && request.framesPerSecond() == 60
              && request.firstFrameTimeoutMilliseconds() == 15000
              && request.frameSlot().protocolVersion == 2,
          "Stream request fields must retain exact managed bytes");

    QByteArray wrongNonce = nonce;
    wrongNonce[0] ^= static_cast<char>(0xff);
    check(ArgusWorker::StreamControlCodec::decodeRequest(
              packet,
              session,
              wrongNonce,
              request)
              == ArgusWorker::StreamControlCodecStatus::NonceMismatch,
          "Stream request wrong nonce must fail closed");
    wrongNonce.fill('\0');

    QByteArray trailing = packet;
    trailing.append('\0');
    check(ArgusWorker::StreamControlCodec::decodeRequest(
              trailing,
              session,
              nonce,
              request)
              == ArgusWorker::StreamControlCodecStatus::InvalidPacket,
          "Stream request trailing bytes must fail closed");

    ArgusWorker::StreamControlResponse response;
    response.setCompleted(
        session,
        3,
        1920,
        1080,
        7680,
        7,
        638895345678901234LL);
    QByteArray encodedResponse;
    check(ArgusWorker::StreamControlCodec::encodeResponse(
              response,
              encodedResponse)
              == ArgusWorker::StreamControlCodecStatus::Accepted,
          "Completed stream response must encode");
    QByteArray expectedResponse;
    appendInt32(expectedResponse, ArgusWorker::StartupProtocolVersion);
    appendInt32(expectedResponse, 4);
    expectedResponse.append(encodeSession(session));
    appendInt32(expectedResponse, 1);
    appendInt32(expectedResponse, 9);
    appendInt32(expectedResponse, 0);
    appendInt32(expectedResponse, 3);
    appendInt32(expectedResponse, 1920);
    appendInt32(expectedResponse, 1080);
    appendInt32(expectedResponse, 7680);
    appendInt32(expectedResponse, 0);
    appendInt64(expectedResponse, 7);
    appendInt64(expectedResponse, 638895345678901234LL);
    appendInt32(expectedResponse, 1);
    check(encodedResponse == expectedResponse,
          "Native stream response must match the managed byte contract");

    response.setOutcome(
        session,
        ArgusWorker::StreamControlOutcome::Unavailable,
        ArgusWorker::StreamControlPhase::ConnectionStart,
        -100,
        true);
    check(ArgusWorker::StreamControlCodec::encodeResponse(
              response,
              encodedResponse)
              == ArgusWorker::StreamControlCodecStatus::Accepted,
          "Failed stream response must encode privacy-safe diagnostics");
    expectedResponse.clear();
    appendInt32(expectedResponse, ArgusWorker::StartupProtocolVersion);
    appendInt32(expectedResponse, 4);
    expectedResponse.append(encodeSession(session));
    appendInt32(expectedResponse, 2);
    appendInt32(expectedResponse, 6);
    appendInt32(expectedResponse, -100);
    appendInt32(expectedResponse, 0);
    appendInt32(expectedResponse, 0);
    appendInt32(expectedResponse, 0);
    appendInt32(expectedResponse, 0);
    appendInt32(expectedResponse, 0);
    appendInt64(expectedResponse, 0);
    appendInt64(expectedResponse, 0);
    appendInt32(expectedResponse, 1);
    check(encodedResponse == expectedResponse,
          "Native stream failure diagnostics must match managed bytes");
    response.setOutcome(
        session,
        ArgusWorker::StreamControlOutcome::Unavailable,
        ArgusWorker::StreamControlPhase::Completed,
        0,
        true);
    check(ArgusWorker::StreamControlCodec::encodeResponse(
              response,
              encodedResponse)
              == ArgusWorker::StreamControlCodecStatus::InvalidPacket,
          "Failed stream response must not claim the completed phase");
}

QByteArray encodePairingRequest(
    const ArgusWorker::StartupSession& session,
    qint32 operation,
    const QByteArray& pin,
    const QByteArray& serverCertificate)
{
    QByteArray request;
    appendInt32(request, ArgusWorker::StartupProtocolVersion);
    appendInt32(request, 1);
    request.append(encodeSession(session));
    appendInt32(request, operation);
    appendText(request, pin);
    appendText(request, serverCertificate);
    return request;
}

void checkPairingControlCodec()
{
    NvAddress defaultPort;
    check(ArgusWorker::parsePairingEndpoint(
              "http://sunshine.invalid",
              defaultPort)
              && defaultPort.port() == 80,
          "Managed-normalized HTTP default port must be accepted");
    NvAddress explicitPort;
    check(ArgusWorker::parsePairingEndpoint(
              "http://sunshine.invalid:48989",
              explicitPort)
              && explicitPort.port() == 48989,
          "Explicit Sunshine HTTP port must be preserved");

    const ArgusWorker::StartupSession session = fixtureSession();
    const QByteArray pairPacket = encodePairingRequest(
        session,
        1,
        "4831",
        QByteArray());
    ArgusWorker::PairingControlRequest request;
    check(ArgusWorker::PairingControlCodec::decodeRequest(
              pairPacket,
              session,
              request)
              == ArgusWorker::PairingControlCodecStatus::Accepted,
          "Managed pairing request fixture must decode");
    check(request.operation()
              == ArgusWorker::PairingControlOperation::Pair
              && request.pin() == QByteArray("4831")
              && request.serverCertificate().isEmpty(),
          "Pairing request fields must retain exact managed bytes");

    ArgusWorker::PairingControlResponse response;
    response.setPaired(
        session,
        ArgusWorker::PairingIdentityFormat,
        QByteArray("identity-package"),
        QByteArray("server-certificate"));
    QByteArray responsePacket;
    check(ArgusWorker::PairingControlCodec::encodeResponse(
              response,
              responsePacket)
              == ArgusWorker::PairingControlCodecStatus::Accepted,
          "Paired response must encode");
    QByteArray expectedResponse;
    appendInt32(expectedResponse, ArgusWorker::StartupProtocolVersion);
    appendInt32(expectedResponse, 2);
    expectedResponse.append(encodeSession(session));
    appendInt32(expectedResponse, 1);
    appendText(expectedResponse, ArgusWorker::PairingIdentityFormat);
    appendText(expectedResponse, "identity-package");
    appendText(expectedResponse, "server-certificate");
    check(responsePacket == expectedResponse,
          "Worker pairing response must match managed byte contract");

    response.setPaired(
        session,
        ArgusWorker::PairingIdentityFormat,
        QByteArray(ArgusWorker::MaximumIdentityBytes, 'i'),
        QByteArray(ArgusWorker::MaximumServerCertificateBytes, 'c'));
    check(ArgusWorker::PairingControlCodec::encodeResponse(
              response,
              responsePacket)
              == ArgusWorker::PairingControlCodecStatus::Accepted,
          "Bounded identity and certificate must fit one response");

    QByteArray trailing = pairPacket;
    trailing.append('\0');
    check(ArgusWorker::PairingControlCodec::decodeRequest(
              trailing,
              session,
              request)
              == ArgusWorker::PairingControlCodecStatus::InvalidPacket,
          "Pairing request trailing bytes must fail closed");
    ArgusWorker::StartupSession wrongSession = fixtureSession();
    wrongSession.sessionId[0] ^= 0xff;
    check(ArgusWorker::PairingControlCodec::decodeRequest(
              pairPacket,
              wrongSession,
              request)
              == ArgusWorker::PairingControlCodecStatus::SessionMismatch,
          "Pairing request stale session must fail closed");
    check(ArgusWorker::PairingControlCodec::decodeRequest(
              encodePairingRequest(
                  session,
                  1,
                  "12345",
                  QByteArray()),
              session,
              request)
              == ArgusWorker::PairingControlCodecStatus::InvalidPacket,
          "Pairing request noncanonical PIN must fail closed");
}

void checkManagedCodecFixture()
{
    const ArgusWorker::StartupSession expectedSession = fixtureSession();
    const QByteArray nonce(32, '\x5a');
    const QByteArray challenge =
        encodeChallenge(expectedSession);

    ArgusWorker::StartupChallenge decodedChallenge;
    check(ArgusWorker::StartupCodec::decodeChallenge(
              challenge,
              decodedChallenge)
              == ArgusWorker::StartupCodecStatus::Accepted,
          "Managed challenge fixture must decode");
    check(decodedChallenge.session == expectedSession,
          ".NET Guid.ToByteArray session bytes must round-trip exactly");
    check(decodedChallenge.nonce == nonce,
          "Managed nonce must round-trip exactly");

    QByteArray hello;
    check(ArgusWorker::StartupCodec::encodeHello(
              decodedChallenge,
              7001,
              638894592000000000,
              hello)
              == ArgusWorker::StartupCodecStatus::Accepted,
          "Worker hello must encode");
    QByteArray expectedHello;
    appendInt32(expectedHello, ArgusWorker::StartupProtocolVersion);
    appendInt32(expectedHello, nonce.size());
    expectedHello.append(nonce);
    expectedHello.append(encodeSession(expectedSession));
    appendInt32(expectedHello, 7001);
    appendInt64(expectedHello, 638894592000000000);
    check(hello == expectedHello,
          "Worker hello must match the managed byte contract");

    const QByteArray identity = QByteArray::fromHex("0001feff");
    const QByteArray payload =
        encodePayload(expectedSession);

    ArgusWorker::StartupPayload decodedPayload;
    check(ArgusWorker::StartupCodec::decodePayload(
              payload,
              expectedSession,
              decodedPayload)
              == ArgusWorker::StartupCodecStatus::Accepted,
          "Managed startup payload fixture must decode");
    check(decodedPayload.endpoint()
              == QStringLiteral("https://127.0.0.1:47984"),
          "Endpoint must be retained in the typed in-memory payload");
    check(decodedPayload.identityFormat()
              == QStringLiteral("moonlight-qt"),
          "Identity format must be retained");
    check(decodedPayload.identity() == identity,
          "Identity bytes must be retained exactly");
    check(decodedPayload.frameSlot().maxPayloadBytes
              == 8 * 1024 * 1024,
          "Required frame-slot descriptor must decode");
}

void checkCodecFailures()
{
    const ArgusWorker::StartupSession session = fixtureSession();
    ArgusWorker::StartupChallenge challenge;

    QByteArray wrongVersion = encodeChallenge(session, 1);
    check(ArgusWorker::StartupCodec::decodeChallenge(
              wrongVersion,
              challenge)
              == ArgusWorker::StartupCodecStatus::InvalidChallenge,
          "Challenge version mismatch must fail closed");

    QByteArray truncated = encodeChallenge(session);
    truncated.chop(1);
    check(ArgusWorker::StartupCodec::decodeChallenge(
              truncated,
              challenge)
              == ArgusWorker::StartupCodecStatus::InvalidChallenge,
          "Truncated challenge must fail closed");

    QByteArray trailing = encodeChallenge(session);
    trailing.append('\0');
    check(ArgusWorker::StartupCodec::decodeChallenge(
              trailing,
              challenge)
              == ArgusWorker::StartupCodecStatus::InvalidChallenge,
          "Challenge trailing bytes must fail closed");

    ArgusWorker::StartupPayload payload;
    QByteArray validPayload = encodePayload(session);
    validPayload.append('\0');
    check(ArgusWorker::StartupCodec::decodePayload(
              validPayload,
              session,
              payload)
              == ArgusWorker::StartupCodecStatus::InvalidPayload,
          "Payload trailing bytes must fail closed");

    ArgusWorker::StartupSession wrongSession = fixtureSession();
    wrongSession.sessionId[0] ^= 0xff;
    check(ArgusWorker::StartupCodec::decodePayload(
              encodePayload(session),
              wrongSession,
              payload)
              == ArgusWorker::StartupCodecStatus::SessionMismatch,
          "Payload session mismatch must fail closed");

    check(ArgusWorker::StartupCodec::decodePayload(
              encodePayload(
                  session,
                  "https://127.0.0.1:47984?forbidden=1"),
              session,
              payload)
              == ArgusWorker::StartupCodecStatus::InvalidPayload,
          "Endpoint query material must fail closed");

    check(ArgusWorker::StartupCodec::decodePayload(
              encodePayload(
                  session,
                  "https://127.0.0.1:47984",
                  QByteArray(),
                  true),
              session,
              payload)
              == ArgusWorker::StartupCodecStatus::InvalidPayload,
          "Empty identity must fail closed");

    check(ArgusWorker::StartupCodec::decodePayload(
              encodePayload(
                  session,
                  "https://127.0.0.1:47984",
                  QByteArray(
                      ArgusWorker::MaximumIdentityBytes + 1,
                      '\x7f'),
                  true),
              session,
              payload)
              == ArgusWorker::StartupCodecStatus::InvalidPayload,
          "Oversized identity must fail closed");

    check(ArgusWorker::StartupCodec::decodePayload(
              encodePayload(
                  session,
                  "https://127.0.0.1:47984",
                  QByteArray::fromHex("0001feff"),
                  false),
              session,
              payload)
              == ArgusWorker::StartupCodecStatus::InvalidPayload,
          "Missing frame-slot capability must fail closed");

    check(ArgusWorker::StartupCodec::decodePayload(
              encodePayload(
                  session,
                  "https://127.0.0.1:47984",
                  QByteArray::fromHex("0001feff"),
                  true,
                  1921),
              session,
              payload)
              == ArgusWorker::StartupCodecStatus::InvalidPayload,
          "Frame-slot limits above v1 bounds must fail closed");
}

void checkIdentityPackageAndInstall()
{
    TestIdentity identity = createTestIdentity();
    TestIdentity otherIdentity = createTestIdentity();
    check(!identity.certificatePem.isEmpty()
              && !identity.privateKeyPem.isEmpty()
              && !otherIdentity.privateKeyPem.isEmpty(),
          "Task-owned RSA identity fixtures must be generated");
    if (identity.certificatePem.isEmpty()
            || identity.privateKeyPem.isEmpty()
            || otherIdentity.privateKeyPem.isEmpty()) {
        return;
    }

    const QByteArray uniqueId("0123456789abcdef");
    IdentityManager::ProcessIdentity decoded;
    check(ArgusWorker::PairingIdentityPackage::decode(
              ArgusWorker::PairingIdentityFormat,
              encodeIdentityPackage(
                  uniqueId,
                  identity.certificatePem,
                  identity.privateKeyPem),
              decoded)
              == ArgusWorker::PairingIdentityPackageStatus::Accepted,
          "Canonical identity-v1 package must decode");
    check(decoded.uniqueId() == QString::fromLatin1(uniqueId),
          "Decoded identity must preserve the canonical unique ID");
    check(decoded.certificatePem() == identity.certificatePem,
          "Decoded identity must preserve the certificate PEM");
    check(decoded.privateKeyPem() == identity.privateKeyPem,
          "Decoded identity must preserve the private-key PEM");

    const auto expectRejected = [](
        const QString& format,
        QByteArray package,
        const char* message) {
        IdentityManager::ProcessIdentity rejected;
        check(ArgusWorker::PairingIdentityPackage::decode(
                  format,
                  std::move(package),
                  rejected)
                  != ArgusWorker::PairingIdentityPackageStatus::Accepted,
              message);
    };
    expectRejected(
        QStringLiteral("moonlight-qt"),
        encodeIdentityPackage(
            uniqueId,
            identity.certificatePem,
            identity.privateKeyPem),
        "Unsupported identity format must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            identity.certificatePem,
            identity.privateKeyPem,
            2),
        "Unsupported identity package version must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            identity.certificatePem,
            identity.privateKeyPem,
            1,
            2),
        "Duplicate identity package field must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            QByteArray(),
            identity.certificatePem,
            identity.privateKeyPem),
        "Empty identity package field must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            "ABCDEF",
            identity.certificatePem,
            identity.privateKeyPem),
        "Noncanonical unique ID must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            QByteArray(17, 'a'),
            identity.certificatePem,
            identity.privateKeyPem),
        "Unique ID above the Moonlight bound must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            "not-a-certificate",
            identity.privateKeyPem),
        "Malformed certificate PEM must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            QByteArray(1, static_cast<char>(0xff)),
            identity.privateKeyPem),
        "Malformed UTF-8 certificate bytes must fail closed");
    QByteArray multipleCertificates =
        identity.certificatePem + '\n' + identity.certificatePem;
    QByteArray unsupportedAfterKey =
        identity.privateKeyPem + '\n' + identity.certificatePem;
    QByteArray trailingCertificateText =
        identity.certificatePem + "\nnot-whitespace";
    QByteArray nonAsciiWhitespace = identity.certificatePem;
    nonAsciiWhitespace.append("\xc2\xa0", 2);
    QByteArray prefixedCertificate =
        "not-whitespace\n" + identity.certificatePem;
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            multipleCertificates,
            identity.privateKeyPem),
        "Multiple certificate PEM objects must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            identity.certificatePem,
            unsupportedAfterKey),
        "Unsupported PEM object after private key must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            trailingCertificateText,
            identity.privateKeyPem),
        "Non-whitespace certificate suffix must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            nonAsciiWhitespace,
            identity.privateKeyPem),
        "Non-ASCII certificate suffix must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            prefixedCertificate,
            identity.privateKeyPem),
        "Non-whitespace certificate prefix must fail closed");
    multipleCertificates.fill('\0');
    unsupportedAfterKey.fill('\0');
    trailingCertificateText.fill('\0');
    nonAsciiWhitespace.fill('\0');
    prefixedCertificate.fill('\0');
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            identity.certificatePem,
            otherIdentity.privateKeyPem),
        "Certificate and private-key mismatch must fail closed");
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        encodeIdentityPackage(
            uniqueId,
            identity.certificatePem,
            identity.privateKeyPem,
            1,
            1,
            true),
        "Trailing identity package bytes must fail closed");
    QByteArray overflow = encodeIdentityPackage(
        uniqueId,
        identity.certificatePem,
        identity.privateKeyPem);
    const qint32 overflowLength = qToLittleEndian(
        std::numeric_limits<qint32>::max());
    std::memcpy(
        overflow.data() + 16,
        &overflowLength,
        sizeof(overflowLength));
    expectRejected(
        ArgusWorker::PairingIdentityFormat,
        std::move(overflow),
        "Overflowing identity field length must fail closed");

    QTemporaryDir settingsDirectory;
    check(settingsDirectory.isValid(),
          "Identity install test must own a temporary settings directory");
    if (!settingsDirectory.isValid()) {
        return;
    }
    QCoreApplication::setOrganizationName(
        QStringLiteral("ArgusIssue577Tests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("IdentityInstall"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::UserScope,
        settingsDirectory.path());
    QSettings::setPath(
        QSettings::IniFormat,
        QSettings::SystemScope,
        settingsDirectory.path());
    QString settingsFile;
    {
        QSettings settings;
        settingsFile = settings.fileName();
    }
    QFile::remove(settingsFile);

    check(IdentityManager::installProcessIdentity(std::move(decoded))
              == IdentityManager::ProcessIdentityInstallResult::Installed,
          "Validated identity must install before singleton initialization");
    IdentityManager::ProcessIdentity second;
    check(ArgusWorker::PairingIdentityPackage::decode(
              ArgusWorker::PairingIdentityFormat,
              encodeIdentityPackage(
                  uniqueId,
                  identity.certificatePem,
                  identity.privateKeyPem),
              second)
              == ArgusWorker::PairingIdentityPackageStatus::Accepted,
          "Second install fixture must decode");
    check(IdentityManager::installProcessIdentity(std::move(second))
              == IdentityManager::ProcessIdentityInstallResult::AlreadyInstalled,
          "Second process-local identity install must be rejected");

    IdentityManager* manager = IdentityManager::get();
    check(manager->getUniqueId() == QString::fromLatin1(uniqueId),
          "Existing unique-ID getter must return process-local identity");
    check(manager->getCertificate() == identity.certificatePem,
          "Existing certificate getter must return process-local identity");
    check(manager->getPrivateKey() == identity.privateKeyPem,
          "Existing private-key getter must return process-local identity");
    check(!manager->getSslConfig().localCertificate().isNull()
              && !manager->getSslConfig().privateKey().isNull(),
          "Existing SSL config getter must use process-local identity");

    IdentityManager::ProcessIdentity afterGet;
    check(ArgusWorker::PairingIdentityPackage::decode(
              ArgusWorker::PairingIdentityFormat,
              encodeIdentityPackage(
                  uniqueId,
                  identity.certificatePem,
                  identity.privateKeyPem),
              afterGet)
              == ArgusWorker::PairingIdentityPackageStatus::Accepted,
          "Post-get install fixture must decode");
    check(IdentityManager::installProcessIdentity(std::move(afterGet))
              == IdentityManager::ProcessIdentityInstallResult::AlreadyInitialized,
          "Identity install after singleton get must be rejected");
    check(!QFile::exists(settingsFile),
          "Process-local identity install must not read or write QSettings");
}

#if defined(Q_OS_WIN)
void checkFrameSlotWriter()
{
    const ArgusWorker::StartupSession session = fixtureSession();
    const QString suffix = QString::number(GetCurrentProcessId());
    const QString mapName =
        QStringLiteral("Local\\Argus.Stream.Frame.native-") + suffix;
    const QString mutexName =
        QStringLiteral("Local\\Argus.Stream.FrameLock.native-") + suffix;
    HANDLE mutex = CreateMutexW(
        nullptr,
        FALSE,
        reinterpret_cast<LPCWSTR>(mutexName.utf16()));
    HANDLE mapping = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        144 + 16,
        reinterpret_cast<LPCWSTR>(mapName.utf16()));
    unsigned char* view = static_cast<unsigned char*>(MapViewOfFile(
        mapping,
        FILE_MAP_ALL_ACCESS,
        0,
        0,
        144 + 16));
    check(mutex != nullptr && mapping != nullptr && view != nullptr,
          "Native frame-slot fixture must create task-owned handles");
    if (mutex == nullptr || mapping == nullptr || view == nullptr) {
        if (view != nullptr) UnmapViewOfFile(view);
        if (mapping != nullptr) CloseHandle(mapping);
        if (mutex != nullptr) CloseHandle(mutex);
        return;
    }

    std::memset(view, 0, 144 + 16);
    std::memcpy(view, "ARGFRM02", 8);
    const auto writeInt32 = [view](int offset, qint32 value) {
        const qint32 littleEndian = qToLittleEndian(value);
        std::memcpy(view + offset, &littleEndian, sizeof(littleEndian));
    };
    writeInt32(8, 2);
    std::memcpy(view + 24, session.machineId.data(), 16);
    std::memcpy(view + 40, session.attemptId.data(), 16);
    std::memcpy(view + 56, session.sessionId.data(), 16);

    ArgusWorker::StartupFrameSlotDescriptor descriptor;
    descriptor.mapName = mapName;
    descriptor.mutexName = mutexName;
    descriptor.protocolVersion = 2;
    descriptor.maxWidth = 2;
    descriptor.maxHeight = 2;
    descriptor.maxPayloadBytes = 16;
    ArgusWorker::FrameSlotWriter writer;
    check(writer.open(descriptor, session)
              == ArgusWorker::FrameSlotWriterStatus::Opened,
          "Native writer must open the managed-compatible slot");
    const QByteArray pixels(16, '\x4a');
    check(writer.publish(
              1,
              638895345678901234LL,
              2,
              2,
              8,
              pixels)
              == ArgusWorker::FrameSlotPublishStatus::Published,
          "Native writer must publish a bounded BGRA frame");
    qint32 state;
    qint64 timestamp;
    std::memcpy(&state, view + 12, sizeof(state));
    std::memcpy(&timestamp, view + 80, sizeof(timestamp));
    state = qFromLittleEndian(state);
    timestamp = qFromLittleEndian(timestamp);
    check(state == 2
              && timestamp == 638895345678901234LL
              && std::memcmp(view + 144, pixels.constData(), 16) == 0,
          "Native writer layout must match managed frame-slot v2");
    check(writer.publish(
              1,
              638895345678901235LL,
              2,
              2,
              8,
              pixels)
              == ArgusWorker::FrameSlotPublishStatus::OutOfOrder,
          "Native writer must reject stale sequence values");
    writer.close();

    SecureZeroMemory(view + 144, 16);
    UnmapViewOfFile(view);
    CloseHandle(mapping);
    CloseHandle(mutex);
}

void checkChannelTimeoutAndReuse()
{
    const QString pipeName =
        QStringLiteral("Argus.Stream.Startup.timeout-%1")
            .arg(GetCurrentProcessId());
    const QString pipePath =
        QStringLiteral("\\\\.\\pipe\\") + pipeName;
    HANDLE serverPipe = CreateNamedPipeW(
        reinterpret_cast<LPCWSTR>(pipePath.utf16()),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        4096,
        4096,
        0,
        nullptr);
    check(serverPipe != INVALID_HANDLE_VALUE,
          "Timeout test must create a task-owned startup pipe");
    if (serverPipe == INVALID_HANDLE_VALUE) {
        return;
    }

    std::thread server([serverPipe]() {
        const BOOL connected =
            ConnectNamedPipe(serverPipe, nullptr);
        if (connected
                || GetLastError() == ERROR_PIPE_CONNECTED) {
            Sleep(5500);
            DisconnectNamedPipe(serverPipe);
        }
        CloseHandle(serverPipe);
    });

    ArgusWorker::StartupChannel channel;
    ArgusWorker::StartupPayload payload;
    check(channel.receive(pipeName, payload)
              == ArgusWorker::StartupChannelStatus::TimedOut,
          "Silent startup server must hit the bounded timeout");
    check(channel.receive(pipeName, payload)
              == ArgusWorker::StartupChannelStatus::AlreadyUsed,
          "Startup channel must reject reuse without reconnecting");
    server.join();
}

void checkExpiredDeadlineDrain()
{
    const QString pipeName =
        QStringLiteral("Argus.Stream.Startup.deadline-%1")
            .arg(GetCurrentProcessId());
    const QString pipePath =
        QStringLiteral("\\\\.\\pipe\\") + pipeName;
    HANDLE serverPipe = CreateNamedPipeW(
        reinterpret_cast<LPCWSTR>(pipePath.utf16()),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        4096,
        4096,
        0,
        nullptr);
    check(serverPipe != INVALID_HANDLE_VALUE,
          "Deadline-edge test must create a task-owned startup pipe");
    if (serverPipe == INVALID_HANDLE_VALUE) {
        return;
    }

    HANDLE clientPipe = CreateFileW(
        reinterpret_cast<LPCWSTR>(pipePath.utf16()),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr);
    check(clientPipe != INVALID_HANDLE_VALUE,
          "Deadline-edge test must connect its task-owned client");
    if (clientPipe == INVALID_HANDLE_VALUE) {
        CloseHandle(serverPipe);
        return;
    }

    const BOOL connected = ConnectNamedPipe(serverPipe, nullptr);
    check(connected || GetLastError() == ERROR_PIPE_CONNECTED,
          "Deadline-edge test server must observe its client");
    check(ArgusWorker::exerciseExpiredDeadlineDrainForTest(
              clientPipe),
          "Expired deadline must cancel and drain pending I/O before return");

    CloseHandle(clientPipe);
    DisconnectNamedPipe(serverPipe);
    CloseHandle(serverPipe);
}
#endif

}

namespace ArgusWorker
{

PairingControlOutcome executePairingControl(
    const QString&,
    PairingControlRequest& request,
    PairingControlResponse& response)
{
    response.setOutcome(
        request.session(),
        PairingControlOutcome::Rejected);
    request.clear();
    return PairingControlOutcome::Rejected;
}

StreamControlOutcome executeStreamControl(
    const QString&,
    const QString&,
    const StartupFrameSlotDescriptor&,
    StreamControlRequest& request,
    StreamControlResponse& response)
{
    response.setOutcome(
        request.session(),
        StreamControlOutcome::Rejected,
        StreamControlPhase::RequestValidation,
        streamRequestValidationFailureCode(
            StreamRequestValidationFailure::EndpointInvalid),
        true);
    request.clear();
    return StreamControlOutcome::Rejected;
}

}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

    if (app.arguments().size() == 2
            && app.arguments().at(1)
                == QStringLiteral("--identity-package-stdin")) {
        QFile packageFile;
        check(packageFile.open(stdin, QIODevice::ReadOnly),
              "Managed identity fixture must be readable");
        IdentityManager::ProcessIdentity identity;
        check(ArgusWorker::PairingIdentityPackage::decode(
                  QLatin1String(ArgusWorker::PairingIdentityFormat),
                  packageFile.readAll(),
                  identity)
                  == ArgusWorker::PairingIdentityPackageStatus::Accepted,
              "Managed identity fixture must decode");
        check(IdentityManager::installProcessIdentity(std::move(identity))
                  == IdentityManager::ProcessIdentityInstallResult::Installed,
              "Managed identity fixture must install");
        IdentityManager* manager = IdentityManager::get();
        const QSslConfiguration ssl = manager->getSslConfig();
        check(!manager->getUniqueId().isEmpty()
                  && !ssl.localCertificate().isNull()
                  && !ssl.privateKey().isNull(),
              "Managed identity fixture must initialize TLS identity");
        return failures == 0 ? 0 : 1;
    }

    if (app.arguments().size() == 2
            && app.arguments().at(1)
                == QStringLiteral("--identity-only")) {
        checkIdentityPackageAndInstall();
        return failures == 0 ? 0 : 1;
    }

    check(!selectsWorker({"Moonlight.exe"}),
          "No-argument interactive startup must not select Argus worker mode");
    check(!selectsWorker(
              {"Moonlight.exe", "stream", "host", "Desktop"}),
          "Existing stream CLI startup must not select Argus worker mode");
    check(!selectsWorker(
              {"Moonlight.exe", "pair", "host"}),
          "Existing pair CLI startup must not select Argus worker mode");
    check(selectsWorker(
              {"Moonlight.exe", "--argus-worker", "--protocol", "1"}),
          "Explicit Argus worker flag must select worker mode");
    check(!selectsWorker(
              {"Moonlight.exe", "--argus-worker=true", "--protocol", "1"}),
          "Only the exact Argus worker flag may select worker mode");
    qputenv(ArgusWorker::StartupPipeEnvironmentVariable,
            "Argus.Stream.Startup.test-invalid-controls");
    check(ArgusWorker::runStartup(
              {"Moonlight.exe",
               "--argus-worker",
               "--protocol",
               "1",
               "--pairing-control",
               "--stream-control"})
              == ArgusWorker::ExitInvalidArguments,
          "Pairing and streaming control modes must be mutually exclusive");
    check(qEnvironmentVariableIsEmpty(
              ArgusWorker::StartupPipeEnvironmentVariable),
          "Invalid control mode must still remove the inherited capability");

    checkManagedCodecFixture();
    checkCodecFailures();
    checkPairingControlCodec();
    checkStreamControlCodec();
    checkStreamRequestValidationFailures();
#if defined(ARGUS_COMMON_C_NO_INPUT)
    check(ArgusWorker::isStreamInputIsolationSupported(),
          "Argus patched builds must expose the verified no-input capability");
#else
    check(!ArgusWorker::isStreamInputIsolationSupported(),
          "Argus pristine builds must remain fail-closed while upstream input is unconditional");
#endif

#if defined(Q_OS_WIN)
    checkFrameSlotWriter();
    checkChannelTimeoutAndReuse();
    checkExpiredDeadlineDrain();
#endif

    qunsetenv(ArgusWorker::StartupPipeEnvironmentVariable);
    check(ArgusWorker::runStartup(
              {"Moonlight.exe", "--argus-worker", "--protocol", "1"})
              == ArgusWorker::ExitStartupCapabilityUnavailable,
          "Worker startup must fail closed without inherited pipe capability");

    qputenv(ArgusWorker::StartupPipeEnvironmentVariable,
            "Argus.Stream.Startup.test-missing");
    check(ArgusWorker::runStartup(
              {"Moonlight.exe", "--argus-worker", "--protocol", "1"})
              == ArgusWorker::ExitHandshakeUnavailable,
          "Worker startup must fail closed when the handshake pipe is absent");
    check(qEnvironmentVariableIsEmpty(
              ArgusWorker::StartupPipeEnvironmentVariable),
          "Worker startup must remove the inherited pipe capability");

    qputenv(ArgusWorker::StartupPipeEnvironmentVariable,
            "Argus.Stream.Startup.test-invalid");
    check(ArgusWorker::runStartup(
              {"Moonlight.exe",
               "--argus-worker",
               "--protocol",
               "1",
               "--endpoint",
               "not-permitted"})
              == ArgusWorker::ExitInvalidArguments,
          "Endpoint material must be rejected from worker argv");
    check(qEnvironmentVariableIsEmpty(
              ArgusWorker::StartupPipeEnvironmentVariable),
          "Invalid worker argv must still remove the inherited capability");

#if defined(Q_OS_WIN)
    const QString pipeName =
        QStringLiteral("Argus.Stream.Startup.test-%1")
            .arg(GetCurrentProcessId());
    const QString pipePath =
        QStringLiteral("\\\\.\\pipe\\") + pipeName;
    HANDLE serverPipe = CreateNamedPipeW(
        reinterpret_cast<LPCWSTR>(pipePath.utf16()),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        4096,
        4096,
        0,
        nullptr);
    check(serverPipe != INVALID_HANDLE_VALUE,
          "Test must create a task-owned startup pipe");
    if (serverPipe != INVALID_HANDLE_VALUE) {
        std::thread server([serverPipe]() {
            const BOOL connected =
                ConnectNamedPipe(serverPipe, nullptr);
            if (connected
                    || GetLastError() == ERROR_PIPE_CONNECTED) {
                DisconnectNamedPipe(serverPipe);
            }
            CloseHandle(serverPipe);
        });
        qputenv(ArgusWorker::StartupPipeEnvironmentVariable,
                pipeName.toUtf8());
        check(ArgusWorker::runStartup(
                  {"Moonlight.exe", "--argus-worker", "--protocol", "1"})
                  == ArgusWorker::ExitHandshakeUnavailable,
              "Worker must fail closed when the server closes before challenge");
        check(qEnvironmentVariableIsEmpty(
                  ArgusWorker::StartupPipeEnvironmentVariable),
              "Connected worker startup must remove the inherited capability");
        server.join();
    }
#endif

    checkIdentityPackageAndInstall();

    return failures == 0 ? 0 : 1;
}
