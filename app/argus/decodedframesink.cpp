#include "decodedframesink.h"

#include <QDateTime>
#include <QDeadlineTimer>
#include <QMutexLocker>

#include <atomic>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif

namespace
{

std::atomic<ArgusWorker::DecodedFrameSink*> activeSink { nullptr };
std::atomic<qint32> activeSinkFailureCode { 0 };
constexpr qint64 DotNetFileTimeOffsetTicks = 504911232000000000LL;

qint64 currentUtcTicks()
{
#if defined(Q_OS_WIN)
    FILETIME time;
    GetSystemTimePreciseAsFileTime(&time);
    ULARGE_INTEGER encoded;
    encoded.LowPart = time.dwLowDateTime;
    encoded.HighPart = time.dwHighDateTime;
    return static_cast<qint64>(encoded.QuadPart)
        + DotNetFileTimeOffsetTicks;
#else
    return QDateTime::currentDateTimeUtc().toMSecsSinceEpoch()
            * 10000
        + 621355968000000000LL;
#endif
}

}

namespace ArgusWorker
{

DecodedFrameSink::~DecodedFrameSink()
{
    close();
}

FrameSlotWriterStatus DecodedFrameSink::open(
    const StartupFrameSlotDescriptor& descriptor,
    const StartupSession& session)
{
    QMutexLocker locker(&m_mutex);
    m_metadata = {};
    m_outstandingSequence = 0;
    m_failureCode = 0;
    m_controlNotified = false;
    const FrameSlotWriterStatus status = m_writer.open(descriptor, session);
    m_closed = status != FrameSlotWriterStatus::Opened;
    return status;
}

void DecodedFrameSink::close()
{
    QMutexLocker locker(&m_mutex);
    m_writer.close();
    if (m_swsContext != nullptr) {
        sws_freeContext(m_swsContext);
        m_swsContext = nullptr;
    }
    m_metadata = {};
    m_outstandingSequence = 0;
    m_failureCode = 0;
    m_controlNotified = false;
    m_closed = true;
    m_changed.wakeAll();
}

FrameSlotPublishStatus DecodedFrameSink::publish(AVFrame* frame)
{
    if (frame == nullptr
            || frame->width < 1
            || frame->height < 1) {
        QMutexLocker locker(&m_mutex);
        m_failureCode = frameSlotPublishFailureCode(
            FrameSlotPublishStatus::InvalidFrame);
        m_changed.wakeAll();
        return FrameSlotPublishStatus::InvalidFrame;
    }
    if (frame->width > 1920 || frame->height > 1080) {
        QMutexLocker locker(&m_mutex);
        m_failureCode = frameSlotPublishFailureCode(
            FrameSlotPublishStatus::ExceedsBounds);
        m_changed.wakeAll();
        return FrameSlotPublishStatus::ExceedsBounds;
    }

    QMutexLocker locker(&m_mutex);
    if (m_closed) {
        return FrameSlotPublishStatus::NotOpen;
    }
    if (m_outstandingSequence != 0) {
        return FrameSlotPublishStatus::OutstandingFrame;
    }
    const auto fail = [this](FrameSlotPublishStatus status) {
        m_failureCode = frameSlotPublishFailureCode(status);
        m_changed.wakeAll();
        return status;
    };
    AVFrame* transferred = nullptr;
    AVFrame* source = frame;
    const AVPixFmtDescriptor* descriptor =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    if (descriptor != nullptr
            && (descriptor->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0) {
        transferred = av_frame_alloc();
        if (transferred == nullptr
                || av_hwframe_transfer_data(
                    transferred,
                    frame,
                    0) < 0) {
            av_frame_free(&transferred);
            return fail(FrameSlotPublishStatus::IoFailure);
        }
        source = transferred;
    }

    const qint32 stride = source->width * 4;
    const qint32 sourceWidth = source->width;
    const qint32 sourceHeight = source->height;
    const qint64 payloadLength =
        static_cast<qint64>(stride) * source->height;
    if (payloadLength < 1
            || payloadLength > 8 * 1024 * 1024) {
        av_frame_free(&transferred);
        return fail(FrameSlotPublishStatus::ExceedsBounds);
    }

    m_swsContext = sws_getCachedContext(
        m_swsContext,
        source->width,
        source->height,
        static_cast<AVPixelFormat>(source->format),
        source->width,
        source->height,
        AV_PIX_FMT_BGRA,
        SWS_FAST_BILINEAR,
        nullptr,
        nullptr,
        nullptr);
    if (m_swsContext == nullptr) {
        av_frame_free(&transferred);
        return fail(FrameSlotPublishStatus::InvalidFrame);
    }

    QByteArray pixels(
        static_cast<qsizetype>(payloadLength),
        Qt::Uninitialized);
    uint8_t* destination[] = {
        reinterpret_cast<uint8_t*>(pixels.data()),
        nullptr,
        nullptr,
        nullptr,
    };
    int destinationLines[] = { stride, 0, 0, 0 };
    const int converted = sws_scale(
        m_swsContext,
        source->data,
        source->linesize,
        0,
        source->height,
        destination,
        destinationLines);
    av_frame_free(&transferred);
    if (converted != sourceHeight) {
        secureZero(pixels.data(), pixels.size());
        return fail(FrameSlotPublishStatus::IoFailure);
    }

    const qint64 sequence = m_metadata.sequence + 1;
    const qint64 timestampUtcTicks = currentUtcTicks();
    const FrameSlotPublishStatus status = m_writer.publish(
        sequence,
        timestampUtcTicks,
        sourceWidth,
        sourceHeight,
        stride,
        pixels);
    secureZero(pixels.data(), pixels.size());
    if (status != FrameSlotPublishStatus::Published) {
        return status == FrameSlotPublishStatus::OutstandingFrame
            ? status
            : fail(status);
    }

    m_metadata.sequence = sequence;
    m_metadata.timestampUtcTicks = timestampUtcTicks;
    m_metadata.width = sourceWidth;
    m_metadata.height = sourceHeight;
    m_metadata.stride = stride;
    m_metadata.frameCount++;
    m_outstandingSequence = sequence;
    m_changed.wakeAll();
    return FrameSlotPublishStatus::Published;
}

DecodedFrameMetadata DecodedFrameSink::metadata() const
{
    QMutexLocker locker(&m_mutex);
    return m_metadata;
}

DecodedFrameWaitOutcome DecodedFrameSink::waitForFrameOrControl(
    qint64 afterSequence,
    qint32 timeoutMilliseconds,
    DecodedFrameMetadata& metadata)
{
    metadata = {};
    if (afterSequence < 0
            || timeoutMilliseconds < 1
            || timeoutMilliseconds > 30000) {
        return DecodedFrameWaitOutcome::Failed;
    }

    QMutexLocker locker(&m_mutex);
    QDeadlineTimer deadline(timeoutMilliseconds);
    while (true) {
        if (m_controlNotified) {
            return DecodedFrameWaitOutcome::Control;
        }
        if (m_metadata.sequence > afterSequence) {
            metadata = m_metadata;
            return DecodedFrameWaitOutcome::FrameReady;
        }
        if (m_failureCode != 0) {
            return DecodedFrameWaitOutcome::Failed;
        }
        if (m_closed) {
            return DecodedFrameWaitOutcome::Closed;
        }
        if (!m_changed.wait(&m_mutex, deadline)) {
            return DecodedFrameWaitOutcome::TimedOut;
        }
    }
}

void DecodedFrameSink::notifyControl()
{
    QMutexLocker locker(&m_mutex);
    m_controlNotified = true;
    m_changed.wakeAll();
}

void DecodedFrameSink::clearControlNotification()
{
    QMutexLocker locker(&m_mutex);
    m_controlNotified = false;
}

bool DecodedFrameSink::acknowledgeConsumed(qint64 sequence)
{
    QMutexLocker locker(&m_mutex);
    if (sequence < 1
            || sequence != m_outstandingSequence
            || !m_writer.confirmConsumed(sequence)) {
        return false;
    }
    m_outstandingSequence = 0;
    return true;
}

bool installDecodedFrameSink(DecodedFrameSink* sink)
{
    DecodedFrameSink* expected = nullptr;
    if (!activeSink.compare_exchange_strong(expected, sink)) {
        return false;
    }
    activeSinkFailureCode.store(0, std::memory_order_release);
    return true;
}

void uninstallDecodedFrameSink(DecodedFrameSink* sink)
{
    if (activeSink.compare_exchange_strong(sink, nullptr)) {
        activeSinkFailureCode.store(0, std::memory_order_release);
    }
}

FrameSlotPublishStatus publishDecodedFrame(AVFrame* frame)
{
    DecodedFrameSink* sink = activeSink.load(std::memory_order_acquire);
    const FrameSlotPublishStatus status = sink != nullptr
        ? sink->publish(frame)
        : FrameSlotPublishStatus::NotOpen;
    if (sink != nullptr
            && status != FrameSlotPublishStatus::Published
            && status != FrameSlotPublishStatus::OutstandingFrame) {
        qint32 expected = 0;
        activeSinkFailureCode.compare_exchange_strong(
            expected,
            frameSlotPublishFailureCode(status));
    }
    return status;
}

DecodedFrameMetadata activeDecodedFrameMetadata()
{
    DecodedFrameSink* sink = activeSink.load(std::memory_order_acquire);
    return sink != nullptr
        ? sink->metadata()
        : DecodedFrameMetadata {};
}

qint32 activeDecodedFrameFailureCode()
{
    return activeSinkFailureCode.load(std::memory_order_acquire);
}

DecodedFrameWaitOutcome waitForActiveDecodedFrameAfter(
    qint64 afterSequence,
    qint32 timeoutMilliseconds,
    DecodedFrameMetadata& metadata)
{
    DecodedFrameSink* sink = activeSink.load(std::memory_order_acquire);
    return sink != nullptr
        ? sink->waitForFrameOrControl(
            afterSequence,
            timeoutMilliseconds,
            metadata)
        : DecodedFrameWaitOutcome::Closed;
}

}
