#include "startupchannel.h"

#include <QRegularExpression>
#include <QUrl>
#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace
{

constexpr int StartupIoTimeoutMilliseconds = 5000;
constexpr int MaximumFrameWidth = 1920;
constexpr int MaximumFrameHeight = 1080;
constexpr int MaximumFramePayloadBytes = 8 * 1024 * 1024;
constexpr qint64 DotNetFileTimeOffsetTicks = 504911232000000000LL;
constexpr int PairingPinBytes = 4;
constexpr qint32 PairingRequestMessageType = 1;
constexpr qint32 PairingResponseMessageType = 2;
constexpr qint32 StreamRequestMessageType = 3;
constexpr qint32 StreamResponseMessageType = 4;
constexpr qint32 FrameReadyMessageType = 5;
constexpr qint32 FrameConsumedMessageType = 6;
constexpr qint32 TerminalMessageType = 7;
constexpr qint32 DisconnectMessageType = 8;

class PacketReader
{
public:
    explicit PacketReader(const QByteArray& packet)
        : m_packet(packet)
    {
    }

    bool readInt32(qint32& value)
    {
        if (!canRead(sizeof(qint32))) {
            return false;
        }

        value = qFromLittleEndian<qint32>(
            reinterpret_cast<const unsigned char*>(
                m_packet.constData() + m_offset));
        m_offset += sizeof(qint32);
        return true;
    }

    bool readInt64(qint64& value)
    {
        if (!canRead(sizeof(qint64))) {
            return false;
        }

        value = qFromLittleEndian<qint64>(
            reinterpret_cast<const unsigned char*>(
                m_packet.constData() + m_offset));
        m_offset += sizeof(qint64);
        return true;
    }

    bool readByte(unsigned char& value)
    {
        if (!canRead(1)) {
            return false;
        }

        value = static_cast<unsigned char>(
            m_packet.at(m_offset++));
        return true;
    }

    bool readBytes(qsizetype length, QByteArray& value)
    {
        if (length < 0 || !canRead(length)) {
            return false;
        }

        value = QByteArray(
            m_packet.constData() + m_offset,
            length);
        m_offset += length;
        return true;
    }

    bool readGuid(ArgusWorker::StartupGuidBytes& value)
    {
        if (!canRead(static_cast<qsizetype>(value.size()))) {
            return false;
        }

        std::memcpy(
            value.data(),
            m_packet.constData() + m_offset,
            value.size());
        m_offset += static_cast<qsizetype>(value.size());
        return true;
    }

    bool atEnd() const
    {
        return m_offset == m_packet.size();
    }

private:
    bool canRead(qsizetype length) const
    {
        return length >= 0
            && m_offset <= m_packet.size() - length;
    }

    const QByteArray& m_packet;
    qsizetype m_offset = 0;
};

void appendInt32(QByteArray& packet, qint32 value)
{
    const qint32 littleEndian = qToLittleEndian(value);
    packet.append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendInt64(QByteArray& packet, qint64 value)
{
    const qint64 littleEndian = qToLittleEndian(value);
    packet.append(
        reinterpret_cast<const char*>(&littleEndian),
        sizeof(littleEndian));
}

void appendSession(
    QByteArray& packet,
    const ArgusWorker::StartupSession& session)
{
    packet.append(
        reinterpret_cast<const char*>(session.machineId.data()),
        static_cast<qsizetype>(session.machineId.size()));
    packet.append(
        reinterpret_cast<const char*>(session.attemptId.data()),
        static_cast<qsizetype>(session.attemptId.size()));
    packet.append(
        reinterpret_cast<const char*>(session.sessionId.data()),
        static_cast<qsizetype>(session.sessionId.size()));
}

bool readSession(
    PacketReader& reader,
    ArgusWorker::StartupSession& session)
{
    return reader.readGuid(session.machineId)
        && reader.readGuid(session.attemptId)
        && reader.readGuid(session.sessionId);
}

bool readBoundedBytes(
    PacketReader& reader,
    int maximumLength,
    QByteArray& value)
{
    qint32 length;
    return reader.readInt32(length)
        && length >= 1
        && length <= maximumLength
        && reader.readBytes(length, value);
}

bool readBoundedOptionalBytes(
    PacketReader& reader,
    int maximumLength,
    QByteArray& value)
{
    qint32 length;
    return reader.readInt32(length)
        && length >= 0
        && length <= maximumLength
        && reader.readBytes(length, value);
}

bool isCanonicalPairingPin(const QByteArray& pin)
{
    return pin.size() == PairingPinBytes
        && std::all_of(
            pin.cbegin(),
            pin.cend(),
            [](char value) {
                return value >= '0' && value <= '9';
            });
}

bool isValidIdentityFormat(const QByteArray& format)
{
    return std::all_of(
        format.cbegin(),
        format.cend(),
        [](char value) {
            const unsigned char character =
                static_cast<unsigned char>(value);
            return (character >= 'a' && character <= 'z')
                || (character >= 'A' && character <= 'Z')
                || (character >= '0' && character <= '9')
                || character == '.'
                || character == '_'
                || character == '-';
        });
}

bool isValidEndpoint(
    const QByteArray& endpointBytes,
    QString& endpointText)
{
    const QUrl endpoint =
        QUrl::fromEncoded(endpointBytes, QUrl::StrictMode);
    if (!endpoint.isValid()
            || endpoint.isRelative()
            || endpoint.scheme().isEmpty()
            || !endpoint.userInfo().isEmpty()
            || !endpoint.query().isEmpty()
            || !endpoint.fragment().isEmpty()) {
        return false;
    }

    endpointText = QString::fromUtf8(endpointBytes);
    return !endpointText.isEmpty();
}

bool isValidFrameSlot(
    const ArgusWorker::StartupFrameSlotDescriptor& slot)
{
    return !slot.mapName.isEmpty()
        && !slot.mutexName.isEmpty()
        && std::any_of(
            slot.frameSlotId.cbegin(),
            slot.frameSlotId.cend(),
            [](unsigned char value) { return value != 0; })
        && slot.protocolVersion == 3
        && slot.maxWidth >= 1
        && slot.maxWidth <= MaximumFrameWidth
        && slot.maxHeight >= 1
        && slot.maxHeight <= MaximumFrameHeight
        && slot.maxPayloadBytes >= 1
        && slot.maxPayloadBytes <= MaximumFramePayloadBytes;
}

bool isValidStreamEndpoint(
    const QByteArray& endpointBytes,
    QString& endpointText)
{
    const QUrl endpoint =
        QUrl::fromEncoded(endpointBytes, QUrl::StrictMode);
    if (!endpoint.isValid()
            || endpoint.isRelative()
            || endpoint.scheme().compare(
                QStringLiteral("http"),
                Qt::CaseInsensitive) != 0
            || endpoint.host().isEmpty()
            || !endpoint.userInfo().isEmpty()
            || !endpoint.query().isEmpty()
            || !endpoint.fragment().isEmpty()
            || (!endpoint.path().isEmpty()
                && endpoint.path() != QStringLiteral("/"))) {
        return false;
    }

    endpointText = QString::fromUtf8(endpointBytes);
    return !endpointText.isEmpty()
        && endpointText.toUtf8() == endpointBytes;
}

bool fixedTimeEquals(
    const QByteArray& left,
    const QByteArray& right)
{
    if (left.size() != right.size()) {
        return false;
    }

    unsigned char difference = 0;
    for (qsizetype index = 0; index < left.size(); index++) {
        difference |= static_cast<unsigned char>(left.at(index))
            ^ static_cast<unsigned char>(right.at(index));
    }
    return difference == 0;
}

#if defined(Q_OS_WIN)
class ScopedHandle
{
public:
    explicit ScopedHandle(HANDLE handle = INVALID_HANDLE_VALUE)
        : m_handle(handle)
    {
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    ScopedHandle(ScopedHandle&& other) noexcept
        : m_handle(std::exchange(
            other.m_handle,
            INVALID_HANDLE_VALUE))
    {
    }

    ScopedHandle& operator=(ScopedHandle&& other) noexcept
    {
        if (this != &other) {
            if (m_handle != INVALID_HANDLE_VALUE
                    && m_handle != nullptr) {
                CloseHandle(m_handle);
            }
            m_handle = std::exchange(
                other.m_handle,
                INVALID_HANDLE_VALUE);
        }
        return *this;
    }

    ~ScopedHandle()
    {
        if (m_handle != INVALID_HANDLE_VALUE
                && m_handle != nullptr) {
            CloseHandle(m_handle);
        }
    }

    HANDLE get() const
    {
        return m_handle;
    }

    bool isValid() const
    {
        return m_handle != INVALID_HANDLE_VALUE
            && m_handle != nullptr;
    }

    HANDLE release()
    {
        return std::exchange(m_handle, INVALID_HANDLE_VALUE);
    }

private:
    HANDLE m_handle;
};

DWORD remainingMilliseconds(ULONGLONG deadline)
{
    const ULONGLONG now = GetTickCount64();
    if (now >= deadline) {
        return 0;
    }

    const ULONGLONG remaining = deadline - now;
    return static_cast<DWORD>(
        std::min<ULONGLONG>(
            remaining,
            std::numeric_limits<DWORD>::max()));
}

DWORD cancelAndDrainIo(
    HANDLE pipe,
    OVERLAPPED& overlapped,
    DWORD& transferred)
{
    if (!CancelIoEx(pipe, &overlapped)) {
        const DWORD cancelError = GetLastError();
        if (cancelError != ERROR_NOT_FOUND) {
            CancelIo(pipe);
        }
    }

    if (GetOverlappedResult(
            pipe,
            &overlapped,
            &transferred,
            TRUE)) {
        return ERROR_SUCCESS;
    }

    return GetLastError();
}

ArgusWorker::StartupChannelStatus waitForIo(
    HANDLE pipe,
    OVERLAPPED& overlapped,
    ULONGLONG deadline,
    DWORD& transferred,
    bool* terminalCompletionObserved = nullptr)
{
    const DWORD remaining = remainingMilliseconds(deadline);
    if (remaining == 0) {
        cancelAndDrainIo(pipe, overlapped, transferred);
        if (terminalCompletionObserved != nullptr) {
            *terminalCompletionObserved = true;
        }
        return ArgusWorker::StartupChannelStatus::TimedOut;
    }

    const DWORD wait = WaitForSingleObject(
        overlapped.hEvent,
        remaining);
    if (wait == WAIT_TIMEOUT) {
        cancelAndDrainIo(pipe, overlapped, transferred);
        if (terminalCompletionObserved != nullptr) {
            *terminalCompletionObserved = true;
        }
        return ArgusWorker::StartupChannelStatus::TimedOut;
    }

    if (wait != WAIT_OBJECT_0) {
        const DWORD error =
            cancelAndDrainIo(pipe, overlapped, transferred);
        if (terminalCompletionObserved != nullptr) {
            *terminalCompletionObserved = true;
        }
        return error == ERROR_BROKEN_PIPE
                || error == ERROR_PIPE_NOT_CONNECTED
            ? ArgusWorker::StartupChannelStatus::ChannelClosed
            : ArgusWorker::StartupChannelStatus::IoFailure;
    }

    if (!GetOverlappedResult(
            pipe,
            &overlapped,
            &transferred,
            FALSE)) {
        const DWORD error = GetLastError();
        if (terminalCompletionObserved != nullptr) {
            *terminalCompletionObserved = true;
        }
        return error == ERROR_BROKEN_PIPE
                || error == ERROR_PIPE_NOT_CONNECTED
            ? ArgusWorker::StartupChannelStatus::ChannelClosed
            : ArgusWorker::StartupChannelStatus::IoFailure;
    }

    if (terminalCompletionObserved != nullptr) {
        *terminalCompletionObserved = true;
    }
    return ArgusWorker::StartupChannelStatus::Accepted;
}

ArgusWorker::StartupChannelStatus readSome(
    HANDLE pipe,
    void* buffer,
    DWORD length,
    ULONGLONG deadline,
    DWORD& transferred)
{
    ScopedHandle event(CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr));
    if (!event.isValid()) {
        return ArgusWorker::StartupChannelStatus::IoFailure;
    }

    OVERLAPPED overlapped {};
    overlapped.hEvent = event.get();
    transferred = 0;
    if (ReadFile(
            pipe,
            buffer,
            length,
            &transferred,
            &overlapped)) {
        return transferred == 0
            ? ArgusWorker::StartupChannelStatus::ChannelClosed
            : ArgusWorker::StartupChannelStatus::Accepted;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_BROKEN_PIPE
            || error == ERROR_PIPE_NOT_CONNECTED) {
        return ArgusWorker::StartupChannelStatus::ChannelClosed;
    }
    if (error != ERROR_IO_PENDING) {
        return ArgusWorker::StartupChannelStatus::IoFailure;
    }

    const ArgusWorker::StartupChannelStatus result =
        waitForIo(
            pipe,
            overlapped,
            deadline,
            transferred);
    if (result == ArgusWorker::StartupChannelStatus::Accepted
            && transferred == 0) {
        return ArgusWorker::StartupChannelStatus::ChannelClosed;
    }

    return result;
}

ArgusWorker::StartupChannelStatus readExact(
    HANDLE pipe,
    void* buffer,
    DWORD length,
    ULONGLONG deadline)
{
    auto* bytes = static_cast<unsigned char*>(buffer);
    DWORD offset = 0;
    while (offset < length) {
        DWORD transferred;
        const ArgusWorker::StartupChannelStatus result =
            readSome(
                pipe,
                bytes + offset,
                length - offset,
                deadline,
                transferred);
        if (result != ArgusWorker::StartupChannelStatus::Accepted) {
            return result;
        }

        offset += transferred;
    }

    return ArgusWorker::StartupChannelStatus::Accepted;
}

ArgusWorker::StartupChannelStatus writeSome(
    HANDLE pipe,
    const void* buffer,
    DWORD length,
    ULONGLONG deadline,
    DWORD& transferred)
{
    ScopedHandle event(CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr));
    if (!event.isValid()) {
        return ArgusWorker::StartupChannelStatus::IoFailure;
    }

    OVERLAPPED overlapped {};
    overlapped.hEvent = event.get();
    transferred = 0;
    if (WriteFile(
            pipe,
            buffer,
            length,
            &transferred,
            &overlapped)) {
        return ArgusWorker::StartupChannelStatus::Accepted;
    }

    const DWORD error = GetLastError();
    if (error == ERROR_BROKEN_PIPE
            || error == ERROR_PIPE_NOT_CONNECTED) {
        return ArgusWorker::StartupChannelStatus::ChannelClosed;
    }
    if (error != ERROR_IO_PENDING) {
        return ArgusWorker::StartupChannelStatus::IoFailure;
    }

    return waitForIo(
        pipe,
        overlapped,
        deadline,
        transferred);
}

ArgusWorker::StartupChannelStatus writeExact(
    HANDLE pipe,
    const void* buffer,
    DWORD length,
    ULONGLONG deadline)
{
    const auto* bytes =
        static_cast<const unsigned char*>(buffer);
    DWORD offset = 0;
    while (offset < length) {
        DWORD transferred;
        const ArgusWorker::StartupChannelStatus result =
            writeSome(
                pipe,
                bytes + offset,
                length - offset,
                deadline,
                transferred);
        if (result != ArgusWorker::StartupChannelStatus::Accepted) {
            return result;
        }
        if (transferred == 0) {
            return ArgusWorker::StartupChannelStatus::ChannelClosed;
        }

        offset += transferred;
    }

    return ArgusWorker::StartupChannelStatus::Accepted;
}

ArgusWorker::StartupChannelStatus readPacket(
    HANDLE pipe,
    ULONGLONG deadline,
    QByteArray& packet)
{
    qint32 lengthBytes;
    ArgusWorker::StartupChannelStatus result =
        readExact(
            pipe,
            &lengthBytes,
            sizeof(lengthBytes),
            deadline);
    if (result != ArgusWorker::StartupChannelStatus::Accepted) {
        return result;
    }

    const qint32 length = qFromLittleEndian(lengthBytes);
    if (length < 1
            || length > ArgusWorker::MaximumStartupPacketBytes) {
        return ArgusWorker::StartupChannelStatus::PacketOutOfBounds;
    }

    packet.resize(length);
    result = readExact(
        pipe,
        packet.data(),
        length,
        deadline);
    if (result != ArgusWorker::StartupChannelStatus::Accepted) {
        ArgusWorker::secureZero(packet.data(), packet.size());
        packet.clear();
    }
    return result;
}

ArgusWorker::StartupChannelStatus writePacket(
    HANDLE pipe,
    ULONGLONG deadline,
    const QByteArray& packet)
{
    if (packet.isEmpty()
            || packet.size()
                > ArgusWorker::MaximumStartupPacketBytes) {
        return ArgusWorker::StartupChannelStatus::PacketOutOfBounds;
    }

    const qint32 length =
        qToLittleEndian(static_cast<qint32>(packet.size()));
    ArgusWorker::StartupChannelStatus result =
        writeExact(
            pipe,
            &length,
            sizeof(length),
            deadline);
    if (result != ArgusWorker::StartupChannelStatus::Accepted) {
        return result;
    }

    return writeExact(
        pipe,
        packet.constData(),
        static_cast<DWORD>(packet.size()),
        deadline);
}

ArgusWorker::StartupChannelStatus connectPipe(
    const QString& pipeName,
    ULONGLONG deadline,
    ScopedHandle& pipe)
{
    const QString path =
        QStringLiteral("\\\\.\\pipe\\") + pipeName;
    const DWORD remaining = remainingMilliseconds(deadline);
    if (remaining == 0) {
        return ArgusWorker::StartupChannelStatus::TimedOut;
    }

    if (!WaitNamedPipeW(
            reinterpret_cast<LPCWSTR>(path.utf16()),
            remaining)) {
        return GetLastError() == ERROR_SEM_TIMEOUT
            ? ArgusWorker::StartupChannelStatus::TimedOut
            : ArgusWorker::StartupChannelStatus::PipeUnavailable;
    }

    ScopedHandle connected(CreateFileW(
        reinterpret_cast<LPCWSTR>(path.utf16()),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL
            | FILE_FLAG_OVERLAPPED
            | SECURITY_SQOS_PRESENT
            | SECURITY_IDENTIFICATION,
        nullptr));
    if (!connected.isValid()) {
        return ArgusWorker::StartupChannelStatus::PipeUnavailable;
    }

    pipe = std::move(connected);
    return ArgusWorker::StartupChannelStatus::Accepted;
}

bool currentProcessMetadata(qint32& processId, qint64& creationTicks)
{
    FILETIME creation;
    FILETIME exit;
    FILETIME kernel;
    FILETIME user;
    if (!GetProcessTimes(
            GetCurrentProcess(),
            &creation,
            &exit,
            &kernel,
            &user)) {
        return false;
    }

    ULARGE_INTEGER fileTime;
    fileTime.LowPart = creation.dwLowDateTime;
    fileTime.HighPart = creation.dwHighDateTime;
    if (fileTime.QuadPart
            > static_cast<ULONGLONG>(
                std::numeric_limits<qint64>::max()
                - DotNetFileTimeOffsetTicks)) {
        return false;
    }

    const DWORD currentProcessId = GetCurrentProcessId();
    if (currentProcessId
            > static_cast<DWORD>(
                std::numeric_limits<qint32>::max())) {
        return false;
    }

    processId = static_cast<qint32>(currentProcessId);
    creationTicks =
        static_cast<qint64>(fileTime.QuadPart)
        + DotNetFileTimeOffsetTicks;
    return true;
}
#endif

ArgusWorker::StartupChannelStatus mapCodecStatus(
    ArgusWorker::StartupCodecStatus status)
{
    using ArgusWorker::StartupChannelStatus;
    using ArgusWorker::StartupCodecStatus;
    switch (status) {
    case StartupCodecStatus::Accepted:
        return StartupChannelStatus::Accepted;
    case StartupCodecStatus::PacketOutOfBounds:
        return StartupChannelStatus::PacketOutOfBounds;
    case StartupCodecStatus::InvalidChallenge:
        return StartupChannelStatus::InvalidChallenge;
    case StartupCodecStatus::InvalidPayload:
        return StartupChannelStatus::InvalidPayload;
    case StartupCodecStatus::SessionMismatch:
        return StartupChannelStatus::SessionMismatch;
    }

    return StartupChannelStatus::IoFailure;
}

ArgusWorker::StartupChannelStatus mapPairingCodecStatus(
    ArgusWorker::PairingControlCodecStatus status)
{
    switch (status) {
    case ArgusWorker::PairingControlCodecStatus::Accepted:
        return ArgusWorker::StartupChannelStatus::Accepted;
    case ArgusWorker::PairingControlCodecStatus::PacketOutOfBounds:
        return ArgusWorker::StartupChannelStatus::PacketOutOfBounds;
    case ArgusWorker::PairingControlCodecStatus::SessionMismatch:
        return ArgusWorker::StartupChannelStatus::SessionMismatch;
    case ArgusWorker::PairingControlCodecStatus::InvalidPacket:
        return ArgusWorker::StartupChannelStatus::InvalidPayload;
    }

    return ArgusWorker::StartupChannelStatus::IoFailure;
}

ArgusWorker::StartupChannelStatus mapStreamCodecStatus(
    ArgusWorker::StreamControlCodecStatus status)
{
    switch (status) {
    case ArgusWorker::StreamControlCodecStatus::Accepted:
        return ArgusWorker::StartupChannelStatus::Accepted;
    case ArgusWorker::StreamControlCodecStatus::PacketOutOfBounds:
        return ArgusWorker::StartupChannelStatus::PacketOutOfBounds;
    case ArgusWorker::StreamControlCodecStatus::SessionMismatch:
    case ArgusWorker::StreamControlCodecStatus::NonceMismatch:
        return ArgusWorker::StartupChannelStatus::SessionMismatch;
    case ArgusWorker::StreamControlCodecStatus::InvalidPacket:
        return ArgusWorker::StartupChannelStatus::InvalidPayload;
    }
    return ArgusWorker::StartupChannelStatus::IoFailure;
}

}

namespace ArgusWorker
{

void secureZero(void* data, qsizetype length)
{
    volatile unsigned char* cursor =
        static_cast<volatile unsigned char*>(data);
    while (length-- > 0) {
        *cursor++ = 0;
    }
}

StartupSession::~StartupSession()
{
    clear();
}

void StartupSession::clear()
{
    secureZero(machineId.data(), machineId.size());
    secureZero(attemptId.data(), attemptId.size());
    secureZero(sessionId.data(), sessionId.size());
}

bool StartupSession::operator==(
    const StartupSession& other) const
{
    return machineId == other.machineId
        && attemptId == other.attemptId
        && sessionId == other.sessionId;
}

StartupChallenge::~StartupChallenge()
{
    clear();
}

void StartupChallenge::clear()
{
    secureZero(nonce.data(), nonce.size());
    nonce.clear();
    session.clear();
}

StartupPayload::~StartupPayload()
{
    clear();
}

const StartupSession& StartupPayload::session() const
{
    return m_session;
}

const QString& StartupPayload::endpoint() const
{
    return m_endpoint;
}

const QString& StartupPayload::displayId() const
{
    return m_displayId;
}

const QString& StartupPayload::identityFormat() const
{
    return m_identityFormat;
}

const QByteArray& StartupPayload::identity() const
{
    return m_identity;
}

QByteArray StartupPayload::takeIdentity()
{
    return std::move(m_identity);
}

const StartupFrameSlotDescriptor&
StartupPayload::frameSlot() const
{
    return m_frameSlot;
}

void StartupPayload::clear()
{
    m_session.clear();
    m_endpoint.fill(QChar('\0'));
    m_endpoint.clear();
    m_displayId.fill(QChar('\0'));
    m_displayId.clear();
    m_identityFormat.fill(QChar('\0'));
    m_identityFormat.clear();
    secureZero(m_identity.data(), m_identity.size());
    m_identity.clear();
    m_frameSlot.mapName.fill(QChar('\0'));
    m_frameSlot.mapName.clear();
    m_frameSlot.mutexName.fill(QChar('\0'));
    m_frameSlot.mutexName.clear();
    m_frameSlot.frameSlotId.fill(0);
    m_frameSlot.protocolVersion = 0;
    m_frameSlot.maxWidth = 0;
    m_frameSlot.maxHeight = 0;
    m_frameSlot.maxPayloadBytes = 0;
}

StartupCodecStatus StartupCodec::decodeChallenge(
    const QByteArray& packet,
    StartupChallenge& challenge)
{
    challenge.clear();
    if (packet.isEmpty()
            || packet.size() > MaximumStartupPacketBytes) {
        return StartupCodecStatus::PacketOutOfBounds;
    }

    PacketReader reader(packet);
    qint32 version;
    qint32 nonceLength;
    if (!reader.readInt32(version)
            || version != StartupProtocolVersion
            || !reader.readInt32(nonceLength)
            || nonceLength != StartupNonceBytes
            || !reader.readBytes(
                nonceLength,
                challenge.nonce)
            || !readSession(reader, challenge.session)
            || !reader.atEnd()) {
        challenge.clear();
        return StartupCodecStatus::InvalidChallenge;
    }

    return StartupCodecStatus::Accepted;
}

StartupCodecStatus StartupCodec::encodeHello(
    const StartupChallenge& challenge,
    qint32 processId,
    qint64 creationTimeUtcTicks,
    QByteArray& packet)
{
    secureZero(packet.data(), packet.size());
    packet.clear();
    if (challenge.nonce.size() != StartupNonceBytes
            || processId <= 0
            || creationTimeUtcTicks <= 0) {
        return StartupCodecStatus::InvalidChallenge;
    }

    appendInt32(packet, StartupProtocolVersion);
    appendInt32(packet, challenge.nonce.size());
    packet.append(challenge.nonce);
    appendSession(packet, challenge.session);
    appendInt32(packet, processId);
    appendInt64(packet, creationTimeUtcTicks);
    return StartupCodecStatus::Accepted;
}

StartupCodecStatus StartupCodec::decodePayload(
    const QByteArray& packet,
    const StartupSession& expectedSession,
    StartupPayload& payload)
{
    payload.clear();
    if (packet.isEmpty()
            || packet.size() > MaximumStartupPacketBytes) {
        return StartupCodecStatus::PacketOutOfBounds;
    }

    PacketReader reader(packet);
    qint32 version;
    StartupSession session;
    QByteArray endpointBytes;
    QByteArray displayIdBytes;
    unsigned char hasDisplayId;
    QByteArray formatBytes;
    QByteArray identity;
    unsigned char hasFrameSlot;
    QByteArray mapNameBytes;
    QByteArray mutexNameBytes;
    StartupFrameSlotDescriptor frameSlot;
    if (!reader.readInt32(version)
            || version != StartupProtocolVersion
            || !readSession(reader, session)
            || !readBoundedBytes(
                reader,
                MaximumEndpointBytes,
                endpointBytes)
            || !reader.readByte(hasDisplayId)
            || hasDisplayId > 1
            || (hasDisplayId == 1
                && !readBoundedBytes(
                    reader,
                    MaximumDisplayIdBytes,
                    displayIdBytes))
            || !readBoundedBytes(
                reader,
                MaximumIdentityFormatBytes,
                formatBytes)
            || !isValidIdentityFormat(formatBytes)) {
        secureZero(endpointBytes.data(), endpointBytes.size());
        secureZero(displayIdBytes.data(), displayIdBytes.size());
        secureZero(formatBytes.data(), formatBytes.size());
        return StartupCodecStatus::InvalidPayload;
    }

    QString endpoint;
    const QString displayId = QString::fromUtf8(displayIdBytes);
    if (!isValidEndpoint(endpointBytes, endpoint)) {
        secureZero(endpointBytes.data(), endpointBytes.size());
        secureZero(displayIdBytes.data(), displayIdBytes.size());
        secureZero(formatBytes.data(), formatBytes.size());
        return StartupCodecStatus::InvalidPayload;
    }
    secureZero(endpointBytes.data(), endpointBytes.size());
    const bool displayIdValid = displayId.toUtf8() == displayIdBytes
        && std::none_of(
            displayId.cbegin(),
            displayId.cend(),
            [](QChar character) { return !character.isPrint(); });
    secureZero(displayIdBytes.data(), displayIdBytes.size());
    if (!displayIdValid) {
        secureZero(formatBytes.data(), formatBytes.size());
        return StartupCodecStatus::InvalidPayload;
    }

    qint32 identityLength;
    if (!reader.readInt32(identityLength)
            || identityLength < 1
            || identityLength > MaximumIdentityBytes
            || !reader.readBytes(identityLength, identity)
            || !reader.readByte(hasFrameSlot)
            || hasFrameSlot == 0
            || !readBoundedBytes(
                reader,
                MaximumCapabilityNameBytes,
                mapNameBytes)
            || !readBoundedBytes(
                reader,
                MaximumCapabilityNameBytes,
                mutexNameBytes)
            || !reader.readGuid(frameSlot.frameSlotId)
            || !reader.readInt32(frameSlot.protocolVersion)
            || !reader.readInt32(frameSlot.maxWidth)
            || !reader.readInt32(frameSlot.maxHeight)
            || !reader.readInt32(frameSlot.maxPayloadBytes)
            || !reader.atEnd()) {
        secureZero(formatBytes.data(), formatBytes.size());
        secureZero(identity.data(), identity.size());
        secureZero(mapNameBytes.data(), mapNameBytes.size());
        secureZero(mutexNameBytes.data(), mutexNameBytes.size());
        endpoint.fill(QChar('\0'));
        return StartupCodecStatus::InvalidPayload;
    }

    frameSlot.mapName = QString::fromUtf8(mapNameBytes);
    frameSlot.mutexName = QString::fromUtf8(mutexNameBytes);
    secureZero(mapNameBytes.data(), mapNameBytes.size());
    secureZero(mutexNameBytes.data(), mutexNameBytes.size());
    if (!isValidFrameSlot(frameSlot)) {
        secureZero(formatBytes.data(), formatBytes.size());
        secureZero(identity.data(), identity.size());
        endpoint.fill(QChar('\0'));
        frameSlot.mapName.fill(QChar('\0'));
        frameSlot.mutexName.fill(QChar('\0'));
        return StartupCodecStatus::InvalidPayload;
    }

    if (!(session == expectedSession)) {
        secureZero(formatBytes.data(), formatBytes.size());
        secureZero(identity.data(), identity.size());
        endpoint.fill(QChar('\0'));
        frameSlot.mapName.fill(QChar('\0'));
        frameSlot.mutexName.fill(QChar('\0'));
        return StartupCodecStatus::SessionMismatch;
    }

    payload.m_session = session;
    payload.m_endpoint = endpoint;
    payload.m_displayId = displayId;
    payload.m_identityFormat = QString::fromLatin1(formatBytes);
    payload.m_identity = identity;
    payload.m_frameSlot = frameSlot;

    secureZero(formatBytes.data(), formatBytes.size());
    secureZero(identity.data(), identity.size());
    endpoint.fill(QChar('\0'));
    frameSlot.mapName.fill(QChar('\0'));
    frameSlot.mutexName.fill(QChar('\0'));
    return StartupCodecStatus::Accepted;
}

PairingControlRequest::~PairingControlRequest()
{
    clear();
}

const StartupSession& PairingControlRequest::session() const
{
    return m_session;
}

PairingControlOperation PairingControlRequest::operation() const
{
    return m_operation;
}

const QByteArray& PairingControlRequest::pin() const
{
    return m_pin;
}

const QByteArray& PairingControlRequest::serverCertificate() const
{
    return m_serverCertificate;
}

QByteArray PairingControlRequest::takePin()
{
    return std::move(m_pin);
}

void PairingControlRequest::clear()
{
    m_session.clear();
    secureZero(m_pin.data(), m_pin.size());
    m_pin.clear();
    secureZero(
        m_serverCertificate.data(),
        m_serverCertificate.size());
    m_serverCertificate.clear();
}

PairingControlResponse::~PairingControlResponse()
{
    clear();
}

const StartupSession& PairingControlResponse::session() const
{
    return m_session;
}

PairingControlOutcome PairingControlResponse::outcome() const
{
    return m_outcome;
}

const QByteArray& PairingControlResponse::identityFormat() const
{
    return m_identityFormat;
}

const QByteArray& PairingControlResponse::identity() const
{
    return m_identity;
}

const QByteArray& PairingControlResponse::serverCertificate() const
{
    return m_serverCertificate;
}

void PairingControlResponse::setPaired(
    const StartupSession& session,
    const QByteArray& identityFormat,
    QByteArray identity,
    QByteArray serverCertificate)
{
    clear();
    m_session = session;
    m_outcome = PairingControlOutcome::Paired;
    m_identityFormat = identityFormat;
    m_identity = std::move(identity);
    m_serverCertificate = std::move(serverCertificate);
}

void PairingControlResponse::setOutcome(
    const StartupSession& session,
    PairingControlOutcome outcome)
{
    clear();
    m_session = session;
    m_outcome = outcome;
}

void PairingControlResponse::clear()
{
    m_session.clear();
    secureZero(m_identityFormat.data(), m_identityFormat.size());
    m_identityFormat.clear();
    secureZero(m_identity.data(), m_identity.size());
    m_identity.clear();
    secureZero(
        m_serverCertificate.data(),
        m_serverCertificate.size());
    m_serverCertificate.clear();
    m_outcome = PairingControlOutcome::Rejected;
}

PairingControlCodecStatus PairingControlCodec::decodeRequest(
    const QByteArray& packet,
    const StartupSession& expectedSession,
    PairingControlRequest& request)
{
    request.clear();
    if (packet.size() < 68
            || packet.size() > MaximumStartupPacketBytes) {
        return PairingControlCodecStatus::PacketOutOfBounds;
    }

    PacketReader reader(packet);
    qint32 version;
    qint32 messageType;
    StartupSession session;
    qint32 operationValue;
    QByteArray pin;
    QByteArray serverCertificate;
    if (!reader.readInt32(version)
            || version != StartupProtocolVersion
            || !reader.readInt32(messageType)
            || messageType != PairingRequestMessageType
            || !readSession(reader, session)
            || !reader.readInt32(operationValue)
            || !readBoundedOptionalBytes(
                reader,
                PairingPinBytes,
                pin)
            || !readBoundedOptionalBytes(
                reader,
                MaximumServerCertificateBytes,
                serverCertificate)
            || !reader.atEnd()) {
        secureZero(pin.data(), pin.size());
        secureZero(
            serverCertificate.data(),
            serverCertificate.size());
        return PairingControlCodecStatus::InvalidPacket;
    }

    if (!(session == expectedSession)) {
        secureZero(pin.data(), pin.size());
        secureZero(
            serverCertificate.data(),
            serverCertificate.size());
        return PairingControlCodecStatus::SessionMismatch;
    }

    const auto operation =
        static_cast<PairingControlOperation>(operationValue);
    const bool fieldsValid =
        (operation == PairingControlOperation::Pair
         && isCanonicalPairingPin(pin)
         && serverCertificate.isEmpty())
        || (operation == PairingControlOperation::Verify
            && pin.isEmpty()
            && !serverCertificate.isEmpty());
    if (!fieldsValid) {
        secureZero(pin.data(), pin.size());
        secureZero(
            serverCertificate.data(),
            serverCertificate.size());
        return PairingControlCodecStatus::InvalidPacket;
    }

    request.m_session = session;
    request.m_operation = operation;
    request.m_pin = std::move(pin);
    request.m_serverCertificate =
        std::move(serverCertificate);
    return PairingControlCodecStatus::Accepted;
}

PairingControlCodecStatus PairingControlCodec::encodeResponse(
    const PairingControlResponse& response,
    QByteArray& packet)
{
    secureZero(packet.data(), packet.size());
    packet.clear();
    const bool paired =
        response.outcome() == PairingControlOutcome::Paired;
    const bool fieldsValid = paired
        ? !response.identityFormat().isEmpty()
            && response.identityFormat().size()
                <= MaximumIdentityFormatBytes
            && isValidIdentityFormat(response.identityFormat())
            && !response.identity().isEmpty()
            && response.identity().size() <= MaximumIdentityBytes
            && !response.serverCertificate().isEmpty()
            && response.serverCertificate().size()
                <= MaximumServerCertificateBytes
        : (response.outcome()
                == PairingControlOutcome::AlreadyPaired
            || response.outcome()
                == PairingControlOutcome::WrongPin
            || response.outcome()
                == PairingControlOutcome::Unavailable
            || response.outcome()
                == PairingControlOutcome::Rejected)
            && response.identityFormat().isEmpty()
            && response.identity().isEmpty()
            && response.serverCertificate().isEmpty();
    if (!fieldsValid) {
        return PairingControlCodecStatus::InvalidPacket;
    }

    appendInt32(packet, StartupProtocolVersion);
    appendInt32(packet, PairingResponseMessageType);
    appendSession(packet, response.session());
    appendInt32(
        packet,
        static_cast<qint32>(response.outcome()));
    appendInt32(packet, response.identityFormat().size());
    packet.append(response.identityFormat());
    appendInt32(packet, response.identity().size());
    packet.append(response.identity());
    appendInt32(packet, response.serverCertificate().size());
    packet.append(response.serverCertificate());
    if (packet.size() > MaximumStartupPacketBytes) {
        secureZero(packet.data(), packet.size());
        packet.clear();
        return PairingControlCodecStatus::PacketOutOfBounds;
    }
    return PairingControlCodecStatus::Accepted;
}

StreamControlRequest::~StreamControlRequest()
{
    clear();
}

const StartupSession& StreamControlRequest::session() const
{
    return m_session;
}

const QString& StreamControlRequest::endpoint() const
{
    return m_endpoint;
}

const QByteArray& StreamControlRequest::serverCertificate() const
{
    return m_serverCertificate;
}

const QString& StreamControlRequest::displayId() const
{
    return m_displayId;
}

qint32 StreamControlRequest::appId() const
{
    return m_appId;
}

StreamVideoCodec StreamControlRequest::codec() const
{
    return m_codec;
}

qint32 StreamControlRequest::width() const
{
    return m_width;
}

qint32 StreamControlRequest::height() const
{
    return m_height;
}

qint32 StreamControlRequest::framesPerSecond() const
{
    return m_framesPerSecond;
}

qint32 StreamControlRequest::firstFrameTimeoutMilliseconds() const
{
    return m_firstFrameTimeoutMilliseconds;
}

const StartupFrameSlotDescriptor& StreamControlRequest::frameSlot() const
{
    return m_frameSlot;
}

void StreamControlRequest::clear()
{
    m_session.clear();
    m_endpoint.fill(QChar('\0'));
    m_endpoint.clear();
    secureZero(
        m_serverCertificate.data(),
        m_serverCertificate.size());
    m_serverCertificate.clear();
    m_displayId.fill(QChar('\0'));
    m_displayId.clear();
    m_appId = 0;
    m_codec = StreamVideoCodec::H264;
    m_width = 0;
    m_height = 0;
    m_framesPerSecond = 0;
    m_firstFrameTimeoutMilliseconds = 0;
    m_frameSlot.mapName.fill(QChar('\0'));
    m_frameSlot.mapName.clear();
    m_frameSlot.mutexName.fill(QChar('\0'));
    m_frameSlot.mutexName.clear();
    m_frameSlot.frameSlotId.fill(0);
    m_frameSlot.protocolVersion = 0;
    m_frameSlot.maxWidth = 0;
    m_frameSlot.maxHeight = 0;
    m_frameSlot.maxPayloadBytes = 0;
}

StreamControlCodecStatus StreamControlCodec::decodeRequest(
    const QByteArray& packet,
    const StartupSession& expectedSession,
    const QByteArray& expectedNonce,
    StreamControlRequest& request)
{
    request.clear();
    if (packet.size() < 100
            || packet.size() > MaximumStartupPacketBytes) {
        return StreamControlCodecStatus::PacketOutOfBounds;
    }

    PacketReader reader(packet);
    qint32 version;
    qint32 messageType;
    StartupSession session;
    QByteArray nonce;
    QByteArray endpointBytes;
    QByteArray serverCertificate;
    QByteArray displayIdBytes;
    qint32 appId;
    qint32 codecValue;
    qint32 width;
    qint32 height;
    qint32 framesPerSecond;
    qint32 firstFrameTimeoutMilliseconds;
    QByteArray mapNameBytes;
    QByteArray mutexNameBytes;
    StartupFrameSlotDescriptor slot;
    const auto clearTemporaries = [&]() {
        secureZero(nonce.data(), nonce.size());
        nonce.clear();
        secureZero(endpointBytes.data(), endpointBytes.size());
        endpointBytes.clear();
        secureZero(
            serverCertificate.data(),
            serverCertificate.size());
        serverCertificate.clear();
        secureZero(displayIdBytes.data(), displayIdBytes.size());
        displayIdBytes.clear();
        secureZero(mapNameBytes.data(), mapNameBytes.size());
        mapNameBytes.clear();
        secureZero(mutexNameBytes.data(), mutexNameBytes.size());
        mutexNameBytes.clear();
    };
    if (!reader.readInt32(version)
            || version != StreamProtocolVersion
            || !reader.readInt32(messageType)
            || messageType != StreamRequestMessageType
            || !readSession(reader, session)
            || !readBoundedBytes(reader, StartupNonceBytes, nonce)
            || nonce.size() != StartupNonceBytes
            || !readBoundedBytes(
                reader,
                MaximumEndpointBytes,
                endpointBytes)
            || !readBoundedBytes(
                reader,
                MaximumServerCertificateBytes,
                serverCertificate)
            || !readBoundedBytes(
                reader,
                MaximumDisplayIdBytes,
                displayIdBytes)
            || !reader.readInt32(appId)
            || !reader.readInt32(codecValue)
            || !reader.readInt32(width)
            || !reader.readInt32(height)
            || !reader.readInt32(framesPerSecond)
            || !reader.readInt32(firstFrameTimeoutMilliseconds)
            || !readBoundedBytes(
                reader,
                MaximumCapabilityNameBytes,
                mapNameBytes)
            || !readBoundedBytes(
                reader,
                MaximumCapabilityNameBytes,
                mutexNameBytes)
            || !reader.readGuid(slot.frameSlotId)
            || !reader.readInt32(slot.protocolVersion)
            || !reader.readInt32(slot.maxWidth)
            || !reader.readInt32(slot.maxHeight)
            || !reader.readInt32(slot.maxPayloadBytes)
            || !reader.atEnd()) {
        clearTemporaries();
        return StreamControlCodecStatus::InvalidPacket;
    }

    if (!(session == expectedSession)) {
        clearTemporaries();
        return StreamControlCodecStatus::SessionMismatch;
    }
    if (expectedNonce.size() != StartupNonceBytes
            || !fixedTimeEquals(nonce, expectedNonce)) {
        clearTemporaries();
        return StreamControlCodecStatus::NonceMismatch;
    }

    QString endpoint;
    const QString displayId = QString::fromUtf8(displayIdBytes);
    slot.mapName = QString::fromUtf8(mapNameBytes);
    slot.mutexName = QString::fromUtf8(mutexNameBytes);
    const qint64 requiredPayload =
        static_cast<qint64>(width) * 4 * height;
    const bool valid =
        isValidStreamEndpoint(endpointBytes, endpoint)
        && !displayId.isEmpty()
        && displayId.toUtf8() == displayIdBytes
        && std::none_of(
            displayId.cbegin(),
            displayId.cend(),
            [](QChar character) { return !character.isPrint(); })
        && appId >= 1
        && static_cast<StreamVideoCodec>(codecValue)
            == StreamVideoCodec::H264
        && width >= 1
        && width <= MaximumFrameWidth
        && height >= 1
        && height <= MaximumFrameHeight
        && framesPerSecond >= 1
        && framesPerSecond <= 120
        && firstFrameTimeoutMilliseconds >= 1000
        && firstFrameTimeoutMilliseconds <= 30000
        && isValidFrameSlot(slot)
        && slot.maxWidth >= width
        && slot.maxHeight >= height
        && slot.maxPayloadBytes >= requiredPayload
        && slot.mapName.startsWith(
            QStringLiteral("Local\\Argus.Stream.Frame."))
        && slot.mutexName.startsWith(
            QStringLiteral("Local\\Argus.Stream.FrameLock."))
        && slot.mapName.toUtf8() == mapNameBytes
        && slot.mutexName.toUtf8() == mutexNameBytes;
    if (!valid) {
        clearTemporaries();
        return StreamControlCodecStatus::InvalidPacket;
    }

    request.m_session = session;
    request.m_endpoint = endpoint;
    request.m_serverCertificate = std::move(serverCertificate);
    request.m_displayId = displayId;
    request.m_appId = appId;
    request.m_codec = static_cast<StreamVideoCodec>(codecValue);
    request.m_width = width;
    request.m_height = height;
    request.m_framesPerSecond = framesPerSecond;
    request.m_firstFrameTimeoutMilliseconds =
        firstFrameTimeoutMilliseconds;
    request.m_frameSlot = slot;
    clearTemporaries();
    return StreamControlCodecStatus::Accepted;
}

StreamControlTransition StreamControlSequenceFence::acceptFrameReady(
    qint64 sequence)
{
    if (m_terminal) {
        return StreamControlTransition::Terminal;
    }
    if (m_outstandingSequence != 0) {
        return StreamControlTransition::OutstandingFrame;
    }
    if (sequence != m_lastConsumedSequence + 1) {
        return sequence <= m_lastConsumedSequence
            ? StreamControlTransition::Duplicate
            : StreamControlTransition::WrongSequence;
    }

    m_outstandingSequence = sequence;
    return StreamControlTransition::Accepted;
}

StreamControlTransition StreamControlSequenceFence::acceptFrameConsumed(
    qint64 sequence)
{
    if (m_terminal) {
        return StreamControlTransition::Terminal;
    }
    if (m_outstandingSequence == 0) {
        return sequence <= m_lastConsumedSequence
            ? StreamControlTransition::Duplicate
            : StreamControlTransition::WrongSequence;
    }
    if (sequence != m_outstandingSequence) {
        return StreamControlTransition::WrongSequence;
    }

    m_lastConsumedSequence = sequence;
    m_outstandingSequence = 0;
    return StreamControlTransition::Accepted;
}

StreamControlTransition StreamControlSequenceFence::acceptTerminal()
{
    if (m_terminal) {
        return StreamControlTransition::Terminal;
    }
    if (m_outstandingSequence != 0) {
        return StreamControlTransition::OutstandingFrame;
    }

    m_terminal = true;
    return StreamControlTransition::Accepted;
}

const StartupSession& StreamControlResponse::session() const
{
    return m_session;
}

StreamControlOutcome StreamControlResponse::outcome() const
{
    return m_outcome;
}

StreamControlPhase StreamControlResponse::phase() const { return m_phase; }
qint32 StreamControlResponse::failureCode() const { return m_failureCode; }

qint32 StreamControlResponse::frameCount() const { return m_frameCount; }
qint32 StreamControlResponse::width() const { return m_width; }
qint32 StreamControlResponse::height() const { return m_height; }
qint32 StreamControlResponse::stride() const { return m_stride; }
qint64 StreamControlResponse::sequence() const { return m_sequence; }
qint64 StreamControlResponse::timestampUtcTicks() const
{
    return m_timestampUtcTicks;
}
bool StreamControlResponse::disconnectClean() const
{
    return m_disconnectClean;
}

void StreamControlResponse::setCompleted(
    const StartupSession& session,
    qint32 frameCount,
    qint32 width,
    qint32 height,
    qint32 stride,
    qint64 sequence,
    qint64 timestampUtcTicks)
{
    clear();
    m_session = session;
    m_outcome = StreamControlOutcome::Completed;
    m_phase = StreamControlPhase::Completed;
    m_failureCode = 0;
    m_frameCount = frameCount;
    m_width = width;
    m_height = height;
    m_stride = stride;
    m_sequence = sequence;
    m_timestampUtcTicks = timestampUtcTicks;
    m_disconnectClean = true;
}

void StreamControlResponse::setOutcome(
    const StartupSession& session,
    StreamControlOutcome outcome,
    StreamControlPhase phase,
    qint32 failureCode,
    bool disconnectClean)
{
    clear();
    m_session = session;
    m_outcome = outcome;
    m_phase = phase;
    m_failureCode = failureCode;
    m_disconnectClean = disconnectClean;
}

void StreamControlResponse::clear()
{
    m_session.clear();
    m_outcome = StreamControlOutcome::Rejected;
    m_phase = StreamControlPhase::RequestValidation;
    m_failureCode = 0;
    m_frameCount = 0;
    m_width = 0;
    m_height = 0;
    m_stride = 0;
    m_sequence = 0;
    m_timestampUtcTicks = 0;
    m_disconnectClean = false;
}

StreamControlCodecStatus StreamControlCodec::encodeResponse(
    const StreamControlResponse& response,
    QByteArray& packet)
{
    secureZero(packet.data(), packet.size());
    packet.clear();
    const bool completed =
        response.outcome() == StreamControlOutcome::Completed;
    const bool valid = completed
        ? response.phase() == StreamControlPhase::Completed
            && response.failureCode() == 0
            && response.frameCount() >= 1
            && response.width() >= 1
            && response.width() <= MaximumFrameWidth
            && response.height() >= 1
            && response.height() <= MaximumFrameHeight
            && response.stride() >= response.width() * 4
            && static_cast<qint64>(response.stride())
                * response.height() <= MaximumFramePayloadBytes
            && response.sequence() >= 1
            && response.timestampUtcTicks()
                >= 621355968000000000LL
            && response.disconnectClean()
        : response.phase() != StreamControlPhase::Completed
            && (response.outcome() == StreamControlOutcome::Unavailable
            || response.outcome() == StreamControlOutcome::Rejected
            || response.outcome() == StreamControlOutcome::DecodeTimedOut)
            && response.frameCount() == 0
            && response.width() == 0
            && response.height() == 0
            && response.stride() == 0
            && response.sequence() == 0
            && response.timestampUtcTicks() == 0;
    if (!valid) {
        return StreamControlCodecStatus::InvalidPacket;
    }

    appendInt32(packet, StartupProtocolVersion);
    appendInt32(packet, StreamResponseMessageType);
    appendSession(packet, response.session());
    appendInt32(packet, static_cast<qint32>(response.outcome()));
    appendInt32(packet, static_cast<qint32>(response.phase()));
    appendInt32(packet, response.failureCode());
    appendInt32(packet, response.frameCount());
    appendInt32(packet, response.width());
    appendInt32(packet, response.height());
    appendInt32(packet, response.stride());
    appendInt32(packet, 0);
    appendInt64(packet, response.sequence());
    appendInt64(packet, response.timestampUtcTicks());
    appendInt32(packet, response.disconnectClean() ? 1 : 0);
    return StreamControlCodecStatus::Accepted;
}

StreamControlCodecStatus StreamControlCodec::encodeFrameReady(
    const StartupSession& session,
    const QByteArray& nonce,
    const StartupGuidBytes& frameSlotId,
    qint64 sequence,
    qint32 width,
    qint32 height,
    qint32 stride,
    qint64 timestampUtcTicks,
    QByteArray& packet)
{
    secureZero(packet.data(), packet.size());
    packet.clear();
    const bool valid = nonce.size() == StartupNonceBytes
        && std::any_of(
            frameSlotId.cbegin(),
            frameSlotId.cend(),
            [](unsigned char value) { return value != 0; })
        && sequence >= 1
        && width >= 1
        && width <= MaximumFrameWidth
        && height >= 1
        && height <= MaximumFrameHeight
        && stride >= width * 4
        && static_cast<qint64>(stride) * height
            <= MaximumFramePayloadBytes
        && timestampUtcTicks >= 621355968000000000LL
        && timestampUtcTicks <= 3155378975999999999LL;
    if (!valid) {
        return StreamControlCodecStatus::InvalidPacket;
    }

    appendInt32(packet, StreamProtocolVersion);
    appendInt32(packet, FrameReadyMessageType);
    appendSession(packet, session);
    appendInt32(packet, nonce.size());
    packet.append(nonce);
    packet.append(
        reinterpret_cast<const char*>(frameSlotId.data()),
        static_cast<qsizetype>(frameSlotId.size()));
    appendInt64(packet, sequence);
    appendInt32(packet, width);
    appendInt32(packet, height);
    appendInt32(packet, stride);
    appendInt32(packet, 0);
    appendInt64(packet, timestampUtcTicks);
    return StreamControlCodecStatus::Accepted;
}

StreamControlCodecStatus StreamControlCodec::decodeCommand(
    const QByteArray& packet,
    const StartupSession& expectedSession,
    const QByteArray& expectedNonce,
    const StartupGuidBytes& expectedFrameSlotId,
    StreamControlCommand& command)
{
    command = {};
    if (packet.size() < 100
            || packet.size() > MaximumStartupPacketBytes) {
        return StreamControlCodecStatus::PacketOutOfBounds;
    }

    PacketReader reader(packet);
    qint32 version;
    qint32 messageType;
    StartupSession session;
    QByteArray nonce;
    StartupGuidBytes frameSlotId {};
    qint64 sequence = 0;
    if (!reader.readInt32(version)
            || version != StreamProtocolVersion
            || !reader.readInt32(messageType)
            || (messageType != FrameConsumedMessageType
                && messageType != DisconnectMessageType)
            || !readSession(reader, session)
            || !readBoundedBytes(reader, StartupNonceBytes, nonce)
            || nonce.size() != StartupNonceBytes
            || !reader.readGuid(frameSlotId)
            || (messageType == FrameConsumedMessageType
                && !reader.readInt64(sequence))
            || !reader.atEnd()) {
        secureZero(nonce.data(), nonce.size());
        return StreamControlCodecStatus::InvalidPacket;
    }
    if (!(session == expectedSession)) {
        secureZero(nonce.data(), nonce.size());
        return StreamControlCodecStatus::SessionMismatch;
    }
    if (expectedNonce.size() != StartupNonceBytes
            || !fixedTimeEquals(nonce, expectedNonce)) {
        secureZero(nonce.data(), nonce.size());
        return StreamControlCodecStatus::NonceMismatch;
    }
    secureZero(nonce.data(), nonce.size());
    if (!std::equal(
            frameSlotId.cbegin(),
            frameSlotId.cend(),
            expectedFrameSlotId.cbegin())
            || (messageType == FrameConsumedMessageType
                && sequence < 1)) {
        return StreamControlCodecStatus::InvalidPacket;
    }

    command.kind = messageType == FrameConsumedMessageType
        ? StreamControlCommandKind::FrameConsumed
        : StreamControlCommandKind::Disconnect;
    command.sequence = sequence;
    return StreamControlCodecStatus::Accepted;
}

StreamControlCodecStatus StreamControlCodec::encodeTerminal(
    const StreamControlResponse& response,
    const QByteArray& nonce,
    const StartupGuidBytes& frameSlotId,
    QByteArray& packet)
{
    secureZero(packet.data(), packet.size());
    packet.clear();
    const bool completed =
        response.outcome() == StreamControlOutcome::Completed;
    const bool valid = nonce.size() == StartupNonceBytes
        && std::any_of(
            frameSlotId.cbegin(),
            frameSlotId.cend(),
            [](unsigned char value) { return value != 0; })
        && (completed
            ? response.phase() == StreamControlPhase::Completed
                && response.failureCode() == 0
                && response.frameCount() >= 1
                && response.disconnectClean()
            : response.phase() != StreamControlPhase::Completed
                && (response.outcome() == StreamControlOutcome::Unavailable
                    || response.outcome() == StreamControlOutcome::Rejected
                    || response.outcome()
                        == StreamControlOutcome::DecodeTimedOut)
                && response.frameCount() == 0);
    if (!valid) {
        return StreamControlCodecStatus::InvalidPacket;
    }

    appendInt32(packet, StreamProtocolVersion);
    appendInt32(packet, TerminalMessageType);
    appendSession(packet, response.session());
    appendInt32(packet, nonce.size());
    packet.append(nonce);
    packet.append(
        reinterpret_cast<const char*>(frameSlotId.data()),
        static_cast<qsizetype>(frameSlotId.size()));
    appendInt32(packet, static_cast<qint32>(response.outcome()));
    appendInt32(packet, static_cast<qint32>(response.phase()));
    appendInt32(packet, response.failureCode());
    appendInt32(packet, response.frameCount());
    appendInt32(packet, response.disconnectClean() ? 1 : 0);
    return StreamControlCodecStatus::Accepted;
}

StartupChannelStatus StartupChannel::receive(
    const QString& pipeName,
    StartupPayload& payload)
{
    if (m_used) {
        return StartupChannelStatus::AlreadyUsed;
    }
    m_used = true;
    payload.clear();

#if defined(Q_OS_WIN)
    const ULONGLONG deadline =
        GetTickCount64() + StartupIoTimeoutMilliseconds;
    ScopedHandle pipe;
    StartupChannelStatus status =
        connectPipe(pipeName, deadline, pipe);
    if (status != StartupChannelStatus::Accepted) {
        return status;
    }

    QByteArray packet;
    status = readPacket(pipe.get(), deadline, packet);
    if (status != StartupChannelStatus::Accepted) {
        return status;
    }

    StartupChallenge challenge;
    status = mapCodecStatus(
        StartupCodec::decodeChallenge(packet, challenge));
    secureZero(packet.data(), packet.size());
    packet.clear();
    if (status != StartupChannelStatus::Accepted) {
        return status;
    }

    qint32 processId;
    qint64 creationTimeUtcTicks;
    if (!currentProcessMetadata(
            processId,
            creationTimeUtcTicks)) {
        return StartupChannelStatus::ProcessMetadataUnavailable;
    }

    status = mapCodecStatus(
        StartupCodec::encodeHello(
            challenge,
            processId,
            creationTimeUtcTicks,
            packet));
    if (status == StartupChannelStatus::Accepted) {
        status = writePacket(
            pipe.get(),
            deadline,
            packet);
    }
    secureZero(packet.data(), packet.size());
    packet.clear();
    if (status != StartupChannelStatus::Accepted) {
        return status;
    }

    status = readPacket(pipe.get(), deadline, packet);
    if (status != StartupChannelStatus::Accepted) {
        return status;
    }

    status = mapCodecStatus(
        StartupCodec::decodePayload(
            packet,
            challenge.session,
            payload));
    secureZero(packet.data(), packet.size());
    packet.clear();
    if (status == StartupChannelStatus::Accepted) {
        secureZero(m_channelNonce.data(), m_channelNonce.size());
        m_channelNonce = challenge.nonce;
        m_pipeHandle = pipe.release();
    }
    return status;
#else
    Q_UNUSED(pipeName);
    return StartupChannelStatus::PipeUnavailable;
#endif
}

StartupChannel::~StartupChannel()
{
    secureZero(m_channelNonce.data(), m_channelNonce.size());
    m_channelNonce.clear();
#if defined(Q_OS_WIN)
    HANDLE pipe = static_cast<HANDLE>(m_pipeHandle);
    if (pipe != nullptr && pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe);
    }
#endif
    m_pipeHandle = nullptr;
}

StartupChannelStatus StartupChannel::receiveStreamRequest(
    const StartupSession& expectedSession,
    StreamControlRequest& request)
{
    request.clear();
    if (m_streamUsed) {
        return StartupChannelStatus::AlreadyUsed;
    }
    m_streamUsed = true;
#if defined(Q_OS_WIN)
    HANDLE pipe = static_cast<HANDLE>(m_pipeHandle);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return StartupChannelStatus::PipeUnavailable;
    }
    const ULONGLONG deadline =
        GetTickCount64() + StartupIoTimeoutMilliseconds;
    QByteArray packet;
    StartupChannelStatus status = readPacket(pipe, deadline, packet);
    if (status == StartupChannelStatus::Accepted) {
        status = mapStreamCodecStatus(
            StreamControlCodec::decodeRequest(
                packet,
                expectedSession,
                m_channelNonce,
                request));
    }
    secureZero(packet.data(), packet.size());
    packet.clear();
    m_streamResponsePending =
        status == StartupChannelStatus::Accepted;
    m_streamActive = status == StartupChannelStatus::Accepted;
    return status;
#else
    Q_UNUSED(expectedSession);
    return StartupChannelStatus::PipeUnavailable;
#endif
}

StartupChannelStatus StartupChannel::sendStreamResponse(
    const StreamControlResponse& response)
{
    if (!m_streamResponsePending) {
        return StartupChannelStatus::AlreadyUsed;
    }
    m_streamResponsePending = false;
    m_streamActive = false;
#if defined(Q_OS_WIN)
    HANDLE pipe = static_cast<HANDLE>(m_pipeHandle);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return StartupChannelStatus::PipeUnavailable;
    }
    QByteArray packet;
    StartupChannelStatus status = mapStreamCodecStatus(
        StreamControlCodec::encodeResponse(response, packet));
    if (status == StartupChannelStatus::Accepted) {
        const ULONGLONG deadline =
            GetTickCount64() + StartupIoTimeoutMilliseconds;
        status = writePacket(pipe, deadline, packet);
    }
    secureZero(packet.data(), packet.size());
    packet.clear();
    return status;
#else
    Q_UNUSED(response);
    return StartupChannelStatus::PipeUnavailable;
#endif
}

StartupChannelStatus StartupChannel::sendFrameReady(
    const StartupSession& session,
    const StartupFrameSlotDescriptor& frameSlot,
    qint64 sequence,
    qint32 width,
    qint32 height,
    qint32 stride,
    qint64 timestampUtcTicks)
{
    if (!m_streamActive) {
        return StartupChannelStatus::AlreadyUsed;
    }
#if defined(Q_OS_WIN)
    HANDLE pipe = static_cast<HANDLE>(m_pipeHandle);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return StartupChannelStatus::PipeUnavailable;
    }
    QByteArray packet;
    StartupChannelStatus status = mapStreamCodecStatus(
        StreamControlCodec::encodeFrameReady(
            session,
            m_channelNonce,
            frameSlot.frameSlotId,
            sequence,
            width,
            height,
            stride,
            timestampUtcTicks,
            packet));
    if (status == StartupChannelStatus::Accepted) {
        status = writePacket(
            pipe,
            GetTickCount64() + StartupIoTimeoutMilliseconds,
            packet);
    }
    secureZero(packet.data(), packet.size());
    return status;
#else
    Q_UNUSED(session);
    Q_UNUSED(frameSlot);
    Q_UNUSED(sequence);
    Q_UNUSED(width);
    Q_UNUSED(height);
    Q_UNUSED(stride);
    Q_UNUSED(timestampUtcTicks);
    return StartupChannelStatus::PipeUnavailable;
#endif
}

StartupChannelStatus StartupChannel::receiveStreamCommand(
    const StartupSession& expectedSession,
    const StartupFrameSlotDescriptor& frameSlot,
    StreamControlCommand& command,
    qint32 timeoutMilliseconds)
{
    command = {};
    if (!m_streamActive) {
        return StartupChannelStatus::AlreadyUsed;
    }
    if (timeoutMilliseconds < 1 || timeoutMilliseconds > 30000) {
        return StartupChannelStatus::InvalidPayload;
    }
#if defined(Q_OS_WIN)
    HANDLE pipe = static_cast<HANDLE>(m_pipeHandle);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return StartupChannelStatus::PipeUnavailable;
    }
    QByteArray packet;
    StartupChannelStatus status = readPacket(
        pipe,
        GetTickCount64() + static_cast<ULONGLONG>(timeoutMilliseconds),
        packet);
    if (status == StartupChannelStatus::Accepted) {
        status = mapStreamCodecStatus(
            StreamControlCodec::decodeCommand(
                packet,
                expectedSession,
                m_channelNonce,
                frameSlot.frameSlotId,
                command));
    }
    secureZero(packet.data(), packet.size());
    return status;
#else
    Q_UNUSED(expectedSession);
    Q_UNUSED(frameSlot);
    Q_UNUSED(timeoutMilliseconds);
    return StartupChannelStatus::PipeUnavailable;
#endif
}

StartupChannelStatus StartupChannel::sendStreamTerminal(
    const StreamControlResponse& response,
    const StartupFrameSlotDescriptor& frameSlot)
{
    if (!m_streamActive) {
        return StartupChannelStatus::AlreadyUsed;
    }
    m_streamActive = false;
    m_streamResponsePending = false;
#if defined(Q_OS_WIN)
    HANDLE pipe = static_cast<HANDLE>(m_pipeHandle);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return StartupChannelStatus::PipeUnavailable;
    }
    QByteArray packet;
    StartupChannelStatus status = mapStreamCodecStatus(
        StreamControlCodec::encodeTerminal(
            response,
            m_channelNonce,
            frameSlot.frameSlotId,
            packet));
    if (status == StartupChannelStatus::Accepted) {
        status = writePacket(
            pipe,
            GetTickCount64() + StartupIoTimeoutMilliseconds,
            packet);
    }
    secureZero(packet.data(), packet.size());
    return status;
#else
    Q_UNUSED(response);
    Q_UNUSED(frameSlot);
    return StartupChannelStatus::PipeUnavailable;
#endif
}

StartupChannelStatus StartupChannel::receivePairingRequest(
    const StartupSession& expectedSession,
    PairingControlRequest& request)
{
    request.clear();
    if (m_pairingUsed) {
        return StartupChannelStatus::AlreadyUsed;
    }
    m_pairingUsed = true;

#if defined(Q_OS_WIN)
    HANDLE pipe = static_cast<HANDLE>(m_pipeHandle);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return StartupChannelStatus::PipeUnavailable;
    }

    const ULONGLONG deadline =
        GetTickCount64() + StartupIoTimeoutMilliseconds;
    QByteArray packet;
    StartupChannelStatus status = readPacket(
        pipe,
        deadline,
        packet);
    if (status == StartupChannelStatus::Accepted) {
        status = mapPairingCodecStatus(
            PairingControlCodec::decodeRequest(
                packet,
                expectedSession,
                request));
    }
    secureZero(packet.data(), packet.size());
    packet.clear();
    m_pairingResponsePending =
        status == StartupChannelStatus::Accepted;
    return status;
#else
    Q_UNUSED(expectedSession);
    return StartupChannelStatus::PipeUnavailable;
#endif
}

StartupChannelStatus StartupChannel::sendPairingResponse(
    const PairingControlResponse& response)
{
    if (!m_pairingResponsePending) {
        return StartupChannelStatus::AlreadyUsed;
    }
    m_pairingResponsePending = false;

#if defined(Q_OS_WIN)
    HANDLE pipe = static_cast<HANDLE>(m_pipeHandle);
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return StartupChannelStatus::PipeUnavailable;
    }

    QByteArray packet;
    StartupChannelStatus status = mapPairingCodecStatus(
        PairingControlCodec::encodeResponse(response, packet));
    if (status == StartupChannelStatus::Accepted) {
        const ULONGLONG deadline =
            GetTickCount64() + StartupIoTimeoutMilliseconds;
        status = writePacket(pipe, deadline, packet);
    }
    secureZero(packet.data(), packet.size());
    packet.clear();
    return status;
#else
    Q_UNUSED(response);
    return StartupChannelStatus::PipeUnavailable;
#endif
}

#if defined(ARGUS_STARTUP_CHANNEL_TESTS) && defined(Q_OS_WIN)
bool exerciseExpiredDeadlineDrainForTest(void* pipeHandle)
{
    HANDLE pipe = static_cast<HANDLE>(pipeHandle);
    ScopedHandle event(CreateEventW(
        nullptr,
        TRUE,
        FALSE,
        nullptr));
    if (!event.isValid()) {
        return false;
    }

    unsigned char byte = 0;
    DWORD transferred = 0;
    OVERLAPPED overlapped {};
    overlapped.hEvent = event.get();
    if (ReadFile(
            pipe,
            &byte,
            sizeof(byte),
            &transferred,
            &overlapped)
            || GetLastError() != ERROR_IO_PENDING) {
        return false;
    }

    bool terminalCompletionObserved = false;
    const StartupChannelStatus status = waitForIo(
        pipe,
        overlapped,
        GetTickCount64(),
        transferred,
        &terminalCompletionObserved);
    secureZero(&byte, sizeof(byte));
    return status == StartupChannelStatus::TimedOut
        && terminalCompletionObserved;
}
#endif

}
