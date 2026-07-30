#include "argus/workerbootstrap.h"
#include "argus/startupchannel.h"

#include <QCoreApplication>
#include <QDebug>
#include <QtEndian>

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

void appendInt32(QByteArray& bytes, qint32 value)
{
    const qint32 littleEndian = qToLittleEndian(value);
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
    qint32 version = 1)
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
    appendInt32(payload, 1);
    payload.append(encodeSession(session));
    appendText(payload, endpoint);
    appendText(payload, "moonlight-qt");
    appendInt32(payload, identity.size());
    payload.append(identity);
    payload.append(includeFrameSlot ? '\x01' : '\x00');
    if (includeFrameSlot) {
        appendText(payload, "Local\\Argus.Stream.Frame.test");
        appendText(payload, "Local\\Argus.Stream.FrameLock.test");
        appendInt32(payload, 1);
        appendInt32(payload, maxWidth);
        appendInt32(payload, 1080);
        appendInt32(payload, 8 * 1024 * 1024);
    }
    return payload;
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
    appendInt32(expectedHello, 1);
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

    QByteArray wrongVersion = encodeChallenge(session, 2);
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

#if defined(Q_OS_WIN)
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

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);

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

    checkManagedCodecFixture();
    checkCodecFailures();

#if defined(Q_OS_WIN)
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

    return failures == 0 ? 0 : 1;
}
