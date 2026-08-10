#include "frameslotwriter.h"

#include <QtEndian>

#include <cstring>
#include <limits>

#include <openssl/sha.h>

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace
{

constexpr int PayloadOffset = 144;
constexpr int StateOffset = 12;
constexpr int GenerationOffset = 16;
constexpr int MachineOffset = 24;
constexpr int AttemptOffset = 40;
constexpr int SessionOffset = 56;
constexpr int SequenceOffset = 72;
constexpr int TimestampOffset = 80;
constexpr int WidthOffset = 88;
constexpr int HeightOffset = 92;
constexpr int StrideOffset = 96;
constexpr int FormatOffset = 100;
constexpr int PayloadLengthOffset = 104;
constexpr int ChecksumOffset = 112;
constexpr int WritingState = 1;
constexpr int CommittedState = 2;
constexpr int Bgra32Format = 0;
constexpr qint64 DotNetUnixEpochTicks = 621355968000000000LL;
constexpr qint64 DotNetMaximumTicks = 3155378975999999999LL;
constexpr unsigned long MutexTimeoutMilliseconds = 5000;

template<typename T>
T readLittleEndian(const unsigned char* view, int offset)
{
    T encoded;
    std::memcpy(&encoded, view + offset, sizeof(encoded));
    return qFromLittleEndian(encoded);
}

template<typename T>
void writeLittleEndian(unsigned char* view, int offset, T value)
{
    const T encoded = qToLittleEndian(value);
    std::memcpy(view + offset, &encoded, sizeof(encoded));
}

}

namespace ArgusWorker
{

qint32 frameSlotPublishFailureCode(FrameSlotPublishStatus status)
{
    switch (status) {
    case FrameSlotPublishStatus::Published:
        return 0;
    case FrameSlotPublishStatus::NotOpen:
        return 1101;
    case FrameSlotPublishStatus::TimedOut:
        return 1102;
    case FrameSlotPublishStatus::StaleSession:
        return 1103;
    case FrameSlotPublishStatus::OutOfOrder:
        return 1104;
    case FrameSlotPublishStatus::ExceedsBounds:
        return 1105;
    case FrameSlotPublishStatus::InvalidFrame:
        return 1106;
    case FrameSlotPublishStatus::IoFailure:
        return 1107;
    }
    return 1107;
}

bool isKnownFrameSlotPublishFailureCode(qint32 code)
{
    return code >= 1101 && code <= 1107;
}

FrameSlotWriter::~FrameSlotWriter()
{
    close();
}

FrameSlotWriterStatus FrameSlotWriter::open(
    const StartupFrameSlotDescriptor& descriptor,
    const StartupSession& session)
{
    close();
    if (descriptor.protocolVersion != 2
            || descriptor.maxWidth < 1
            || descriptor.maxWidth > 1920
            || descriptor.maxHeight < 1
            || descriptor.maxHeight > 1080
            || descriptor.maxPayloadBytes < 1
            || descriptor.maxPayloadBytes > 8 * 1024 * 1024
            || !descriptor.mapName.startsWith(
                QStringLiteral("Local\\Argus.Stream.Frame."))
            || !descriptor.mutexName.startsWith(
                QStringLiteral("Local\\Argus.Stream.FrameLock."))) {
        return FrameSlotWriterStatus::InvalidDescriptor;
    }

#if defined(Q_OS_WIN)
    HANDLE mapping = OpenFileMappingW(
        FILE_MAP_READ | FILE_MAP_WRITE,
        FALSE,
        reinterpret_cast<LPCWSTR>(descriptor.mapName.utf16()));
    if (mapping == nullptr) {
        return FrameSlotWriterStatus::Unavailable;
    }

    HANDLE mutex = OpenMutexW(
        SYNCHRONIZE | MUTEX_MODIFY_STATE,
        FALSE,
        reinterpret_cast<LPCWSTR>(descriptor.mutexName.utf16()));
    if (mutex == nullptr) {
        CloseHandle(mapping);
        return FrameSlotWriterStatus::Unavailable;
    }

    const SIZE_T mappingBytes = static_cast<SIZE_T>(
        PayloadOffset + descriptor.maxPayloadBytes);
    unsigned char* view = static_cast<unsigned char*>(MapViewOfFile(
        mapping,
        FILE_MAP_READ | FILE_MAP_WRITE,
        0,
        0,
        mappingBytes));
    if (view == nullptr) {
        CloseHandle(mutex);
        CloseHandle(mapping);
        return FrameSlotWriterStatus::Unavailable;
    }

    m_descriptor = descriptor;
    m_session = session;
    m_mappingHandle = mapping;
    m_mutexHandle = mutex;
    m_view = view;
    if (!lock()) {
        close();
        return FrameSlotWriterStatus::Unavailable;
    }
    const bool valid = headerMatches();
    unlock();
    if (!valid) {
        close();
        return FrameSlotWriterStatus::StaleSession;
    }
    return FrameSlotWriterStatus::Opened;
#else
    Q_UNUSED(session);
    return FrameSlotWriterStatus::Unsupported;
#endif
}

FrameSlotPublishStatus FrameSlotWriter::publish(
    qint64 sequence,
    qint64 timestampUtcTicks,
    qint32 width,
    qint32 height,
    qint32 stride,
    const QByteArray& bgraPixels)
{
    if (m_view == nullptr) {
        return FrameSlotPublishStatus::NotOpen;
    }
    if (sequence < 1
            || timestampUtcTicks < DotNetUnixEpochTicks
            || timestampUtcTicks > DotNetMaximumTicks
            || width < 1
            || height < 1
            || stride < width * 4
            || static_cast<qint64>(stride) * height
                != bgraPixels.size()) {
        return FrameSlotPublishStatus::InvalidFrame;
    }
    if (width > m_descriptor.maxWidth
            || height > m_descriptor.maxHeight
            || bgraPixels.size() > m_descriptor.maxPayloadBytes) {
        return FrameSlotPublishStatus::ExceedsBounds;
    }
    if (!lock()) {
        return FrameSlotPublishStatus::TimedOut;
    }

    FrameSlotPublishStatus result = FrameSlotPublishStatus::IoFailure;
    do {
        if (!headerMatches()) {
            result = FrameSlotPublishStatus::StaleSession;
            break;
        }
        const qint32 state = readLittleEndian<qint32>(
            m_view,
            StateOffset);
        const qint64 currentSequence = state == CommittedState
            ? readLittleEndian<qint64>(m_view, SequenceOffset)
            : 0;
        if (sequence <= currentSequence) {
            result = FrameSlotPublishStatus::OutOfOrder;
            break;
        }

        unsigned char checksum[SHA256_DIGEST_LENGTH] {};
        SHA256(
            reinterpret_cast<const unsigned char*>(
                bgraPixels.constData()),
            static_cast<size_t>(bgraPixels.size()),
            checksum);
        writeLittleEndian<qint32>(m_view, StateOffset, WritingState);
#if defined(Q_OS_WIN)
        MemoryBarrier();
#endif
        writeLittleEndian<qint64>(
            m_view,
            GenerationOffset,
            readLittleEndian<qint64>(m_view, GenerationOffset) + 1);
        writeLittleEndian<qint64>(m_view, SequenceOffset, sequence);
        writeLittleEndian<qint64>(m_view, TimestampOffset, timestampUtcTicks);
        writeLittleEndian<qint32>(m_view, WidthOffset, width);
        writeLittleEndian<qint32>(m_view, HeightOffset, height);
        writeLittleEndian<qint32>(m_view, StrideOffset, stride);
        writeLittleEndian<qint32>(m_view, FormatOffset, Bgra32Format);
        writeLittleEndian<qint32>(
            m_view,
            PayloadLengthOffset,
            bgraPixels.size());
        std::memcpy(
            m_view + ChecksumOffset,
            checksum,
            sizeof(checksum));
        std::memcpy(
            m_view + PayloadOffset,
            bgraPixels.constData(),
            static_cast<size_t>(bgraPixels.size()));
#if defined(Q_OS_WIN)
        const bool flushedPayload = FlushViewOfFile(
            m_view,
            static_cast<SIZE_T>(
                PayloadOffset + bgraPixels.size())) != FALSE;
        MemoryBarrier();
#else
        const bool flushedPayload = true;
#endif
        secureZero(checksum, sizeof(checksum));
        if (!flushedPayload) {
            break;
        }
        writeLittleEndian<qint32>(
            m_view,
            StateOffset,
            CommittedState);
#if defined(Q_OS_WIN)
        MemoryBarrier();
        if (!FlushViewOfFile(m_view + StateOffset, sizeof(qint32))) {
            break;
        }
#endif
        result = FrameSlotPublishStatus::Published;
    } while (false);

    unlock();
    return result;
}

void FrameSlotWriter::close()
{
#if defined(Q_OS_WIN)
    if (m_locked) {
        unlock();
    }
    if (m_view != nullptr) {
        UnmapViewOfFile(m_view);
    }
    if (m_mutexHandle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_mutexHandle));
    }
    if (m_mappingHandle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_mappingHandle));
    }
#endif
    m_view = nullptr;
    m_mutexHandle = nullptr;
    m_mappingHandle = nullptr;
    m_descriptor.mapName.fill(QChar('\0'));
    m_descriptor.mapName.clear();
    m_descriptor.mutexName.fill(QChar('\0'));
    m_descriptor.mutexName.clear();
    m_descriptor.protocolVersion = 0;
    m_descriptor.maxWidth = 0;
    m_descriptor.maxHeight = 0;
    m_descriptor.maxPayloadBytes = 0;
    m_session.clear();
    m_locked = false;
}

bool FrameSlotWriter::headerMatches() const
{
    return m_view != nullptr
        && std::memcmp(m_view, "ARGFRM02", 8) == 0
        && readLittleEndian<qint32>(m_view, 8) == 2
        && std::memcmp(m_view + MachineOffset, m_session.machineId.data(), 16) == 0
        && std::memcmp(m_view + AttemptOffset, m_session.attemptId.data(), 16) == 0
        && std::memcmp(m_view + SessionOffset, m_session.sessionId.data(), 16) == 0;
}

bool FrameSlotWriter::lock()
{
#if defined(Q_OS_WIN)
    if (m_mutexHandle == nullptr || m_locked) {
        return false;
    }
    const DWORD result = WaitForSingleObject(
        static_cast<HANDLE>(m_mutexHandle),
        MutexTimeoutMilliseconds);
    m_locked = result == WAIT_OBJECT_0
        || result == WAIT_ABANDONED;
    return m_locked;
#else
    return false;
#endif
}

void FrameSlotWriter::unlock()
{
#if defined(Q_OS_WIN)
    if (m_locked) {
        ReleaseMutex(static_cast<HANDLE>(m_mutexHandle));
    }
#endif
    m_locked = false;
}

}
