#include "decodedframesink.h"

#include <QDateTime>
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
    return m_writer.open(descriptor, session);
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
}

bool DecodedFrameSink::publish(AVFrame* frame)
{
    if (frame == nullptr
            || frame->width < 1
            || frame->height < 1
            || frame->width > 1920
            || frame->height > 1080) {
        return false;
    }

    QMutexLocker locker(&m_mutex);
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
            return false;
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
        return false;
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
        return false;
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
        return false;
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
        return false;
    }

    m_metadata.sequence = sequence;
    m_metadata.timestampUtcTicks = timestampUtcTicks;
    m_metadata.width = sourceWidth;
    m_metadata.height = sourceHeight;
    m_metadata.stride = stride;
    m_metadata.frameCount++;
    return true;
}

DecodedFrameMetadata DecodedFrameSink::metadata() const
{
    QMutexLocker locker(&m_mutex);
    return m_metadata;
}

bool installDecodedFrameSink(DecodedFrameSink* sink)
{
    DecodedFrameSink* expected = nullptr;
    return activeSink.compare_exchange_strong(expected, sink);
}

void uninstallDecodedFrameSink(DecodedFrameSink* sink)
{
    activeSink.compare_exchange_strong(sink, nullptr);
}

void publishDecodedFrame(AVFrame* frame)
{
    DecodedFrameSink* sink = activeSink.load(std::memory_order_acquire);
    if (sink != nullptr) {
        sink->publish(frame);
    }
}

DecodedFrameMetadata activeDecodedFrameMetadata()
{
    DecodedFrameSink* sink = activeSink.load(std::memory_order_acquire);
    return sink != nullptr
        ? sink->metadata()
        : DecodedFrameMetadata {};
}

}
