#include "RuntimeOracle.h"

#include "RendererConstructionHooks.h"

#undef SDL_CreateWindow
#undef SDL_CreateRenderer
#undef SDL_GL_CreateContext

#include "argus/decodedframesink.h"
#include "argus/streamrequestvalidation.h"
#include "streaming/streamutils.h"
#include "streaming/video/ffmpeg-renderers/sdlvid.h"
#include "streaming/video/ffmpeg.h"

#include <Limelight.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QString>
#include <QtEndian>
#include <qt_windows.h>

#include <atomic>
#include <cstring>

namespace
{

constexpr int HeaderBytes = 144;
constexpr int MaxPayloadBytes = 8 * 1024 * 1024;
std::atomic<bool> publicationArmed { false };
std::atomic<int> lastProbeStatus {
    static_cast<int>(ArgusWorker::FrameSlotPublishStatus::NotOpen) };

struct SharedSlotFixture
{
    HANDLE mutex = nullptr;
    HANDLE mapping = nullptr;
    unsigned char* view = nullptr;
    ArgusWorker::StartupSession session;
    ArgusWorker::StartupFrameSlotDescriptor descriptor;

    ~SharedSlotFixture()
    {
        if (view != nullptr) {
            SecureZeroMemory(view, HeaderBytes + MaxPayloadBytes);
            UnmapViewOfFile(view);
        }
        if (mapping != nullptr) {
            CloseHandle(mapping);
        }
        if (mutex != nullptr) {
            CloseHandle(mutex);
        }
    }

    bool create()
    {
        const QString suffix = QStringLiteral("979-%1")
            .arg(GetCurrentProcessId());
        descriptor.mapName =
            QStringLiteral("Local\\Argus.Stream.Frame.oracle-") + suffix;
        descriptor.mutexName =
            QStringLiteral("Local\\Argus.Stream.FrameLock.oracle-") + suffix;
        descriptor.protocolVersion = 2;
        descriptor.maxWidth = 1920;
        descriptor.maxHeight = 1080;
        descriptor.maxPayloadBytes = MaxPayloadBytes;

        for (int index = 0; index < 16; index++) {
            session.machineId[index] = static_cast<unsigned char>(index + 1);
            session.attemptId[index] = static_cast<unsigned char>(index + 17);
            session.sessionId[index] = static_cast<unsigned char>(index + 33);
        }

        mutex = CreateMutexW(
            nullptr,
            FALSE,
            reinterpret_cast<LPCWSTR>(descriptor.mutexName.utf16()));
        mapping = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            nullptr,
            PAGE_READWRITE,
            0,
            HeaderBytes + MaxPayloadBytes,
            reinterpret_cast<LPCWSTR>(descriptor.mapName.utf16()));
        if (mutex == nullptr || mapping == nullptr) {
            return false;
        }

        view = static_cast<unsigned char*>(MapViewOfFile(
            mapping,
            FILE_MAP_ALL_ACCESS,
            0,
            0,
            HeaderBytes + MaxPayloadBytes));
        if (view == nullptr) {
            return false;
        }

        std::memset(view, 0, HeaderBytes + MaxPayloadBytes);
        std::memcpy(view, "ARGFRM02", 8);
        const qint32 protocolVersion = qToLittleEndian<qint32>(2);
        std::memcpy(view + 8, &protocolVersion, sizeof(protocolVersion));
        std::memcpy(view + 24, session.machineId.data(), 16);
        std::memcpy(view + 40, session.attemptId.data(), 16);
        std::memcpy(view + 56, session.sessionId.data(), 16);
        return true;
    }
};

bool writeReceipt(const QString& path, const QJsonObject& receipt)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(QJsonDocument(receipt).toJson(QJsonDocument::Indented));
    return file.commit();
}

QJsonObject constructionJson(
    const ArgusRendererFreeOracle::RendererConstructionCounts& counts)
{
    return {
        { QStringLiteral("windowCalls"), counts.windowCalls },
        { QStringLiteral("rendererCalls"), counts.rendererCalls },
        { QStringLiteral("openGlContextCalls"), counts.openGlContextCalls },
    };
}

}

namespace ArgusRendererFreeOracle
{

bool isRequested(int argc, char* argv[])
{
    return argc > 1
        && QString::fromLocal8Bit(argv[1])
            == QStringLiteral("--argus-renderer-free-oracle");
}

void publishDecodedProbeFrame(AVFrame* frame)
{
    if (publicationArmed.load(std::memory_order_acquire)) {
        lastProbeStatus.store(
            static_cast<int>(ArgusWorker::publishDecodedFrame(frame)),
            std::memory_order_release);
    }
}

int run(int argc, char* argv[])
{
    QJsonArray failures;
    const auto require = [&failures](bool condition, const char* message) {
        if (!condition) {
            failures.append(QString::fromLatin1(message));
        }
        return condition;
    };

    const QString outputPath = argc >= 3
        ? QString::fromLocal8Bit(argv[2])
        : QString();
    require(!outputPath.isEmpty(), "output path is required");
    bool artifactLengthValid = false;
    bool oracleLengthValid = false;
    const QString artifactSha256 = argc == 13
        ? QString::fromLatin1(argv[3])
        : QString();
    const qint64 artifactLength = argc == 13
        ? QString::fromLatin1(argv[4]).toLongLong(&artifactLengthValid)
        : 0;
    const QString sourceCommit = argc == 13
        ? QString::fromLatin1(argv[5])
        : QString();
    const QString sourceTree = argc == 13
        ? QString::fromLatin1(argv[6])
        : QString();
    const QString oracleSha256 = argc == 13
        ? QString::fromLatin1(argv[7])
        : QString();
    const qint64 oracleLength = argc == 13
        ? QString::fromLatin1(argv[8]).toLongLong(&oracleLengthValid)
        : 0;
    const QString runnerPath = argc == 13
        ? QString::fromLocal8Bit(argv[9])
        : QString();
    const QString runnerWorkingTreeSha256 = argc == 13
        ? QString::fromLatin1(argv[10])
        : QString();
    const QString runnerGitBlobSha256 = argc == 13
        ? QString::fromLatin1(argv[11])
        : QString();
    const QString runnerCommit = argc == 13
        ? QString::fromLatin1(argv[12])
        : QString();
    require(
        argc == 13
            && artifactSha256.size() == 64
            && artifactLengthValid
            && artifactLength > 0
            && sourceCommit.size() == 40
            && sourceTree.size() == 40
            && oracleSha256.size() == 64
            && oracleLengthValid
            && oracleLength > 0
            && runnerPath
                == QStringLiteral(
                    "tests/argus-renderer-free-worker/"
                    "Run-RendererFreeWorkerOracle.ps1")
            && runnerWorkingTreeSha256.size() == 64
            && runnerGitBlobSha256.size() == 64
            && runnerCommit == sourceCommit,
        "exact artifact and oracle authority arguments are required");

    ArgusWorker::DecodedFrameSink invalidSink;
    const ArgusWorker::FrameSlotWriterStatus invalidOpenStatus =
        invalidSink.open({}, {});
    const qint32 invalidOpenCode =
        ArgusWorker::streamRequestValidationFailureCode(
            ArgusWorker::classifyFrameSlotOpen(invalidOpenStatus));
    require(invalidOpenCode == 1008, "sink open failure must map to 1008");
    invalidSink.close();

    SharedSlotFixture fixture;
    require(fixture.create(), "shared frame slot fixture creation failed");

    ArgusWorker::DecodedFrameSink sink;
    ArgusWorker::DecodedFrameSink competingSink;
    bool sinkOpened = false;
    bool sinkInstalled = false;
    bool competingInstallRejected = false;
    qint32 installFailureCode = 0;
    bool cleanupReinstallSucceeded = false;
    bool decoderInitialized = false;
    bool failureDecoderInitialized = false;
    qint32 publicationFailureCode = 0;
    bool firstFailureLatched = false;
    ArgusWorker::DecodedFrameMetadata publishedMetadata;

    if (fixture.view != nullptr) {
        sinkOpened = sink.open(fixture.descriptor, fixture.session)
            == ArgusWorker::FrameSlotWriterStatus::Opened;
        require(sinkOpened, "frame sink did not open");
    }
    if (sinkOpened) {
        sinkInstalled = ArgusWorker::installDecodedFrameSink(&sink);
        require(sinkInstalled, "frame sink did not install");
    }
    if (sinkInstalled) {
        competingInstallRejected =
            !ArgusWorker::installDecodedFrameSink(&competingSink);
        installFailureCode =
            ArgusWorker::streamRequestValidationFailureCode(
                ArgusWorker::classifyFrameSinkInstall(
                    !competingInstallRejected));
        require(
            competingInstallRejected && installFailureCode == 1009,
            "sink install conflict must map to 1009");

        resetRendererConstructionCounts();
        DECODER_PARAMETERS params {};
        params.window = nullptr;
        params.vds = StreamingPreferences::VDS_FORCE_SOFTWARE;
        params.renderer = StreamingPreferences::RS_AUTO;
        params.videoFormat = VIDEO_FORMAT_H264;
        params.width = 1280;
        params.height = 720;
        params.frameRate = 60;
        params.enableVsync = false;
        params.enableFramePacing = false;
        params.testOnly = true;
        {
            FFmpegVideoDecoder decoder(true);
            publicationArmed.store(true, std::memory_order_release);
            decoderInitialized = decoder.initializeRendererFree(&params);
            publicationArmed.store(false, std::memory_order_release);
            require(decoderInitialized, "renderer-free decoder did not initialize");
            publishedMetadata = ArgusWorker::activeDecodedFrameMetadata();
        }

        std::memset(fixture.view + 56, 0, 16);
        {
            FFmpegVideoDecoder decoder(true);
            publicationArmed.store(true, std::memory_order_release);
            failureDecoderInitialized =
                decoder.initializeRendererFree(&params);
            publicationArmed.store(false, std::memory_order_release);
        }
        const auto publicationStatus =
            static_cast<ArgusWorker::FrameSlotPublishStatus>(
                lastProbeStatus.load(std::memory_order_acquire));
        publicationFailureCode =
            ArgusWorker::frameSlotPublishFailureCode(publicationStatus);
        require(
            failureDecoderInitialized
                && publicationStatus
                    == ArgusWorker::FrameSlotPublishStatus::StaleSession
                && publicationFailureCode == 1103,
            "stale frame publication did not produce typed terminal failure");
        ArgusWorker::publishDecodedFrame(nullptr);
        firstFailureLatched =
            ArgusWorker::activeDecodedFrameFailureCode() == 1103;
        require(firstFailureLatched,
                "first frame publication failure was not latched");
    }

    const RendererConstructionCounts workerCounts =
        rendererConstructionCounts();
    require(
        workerCounts.windowCalls == 0
            && workerCounts.rendererCalls == 0
            && workerCounts.openGlContextCalls == 0,
        "worker constructed a window, renderer, or OpenGL context");
    require(
        publishedMetadata.frameCount == 1
            && publishedMetadata.sequence == 1
            && publishedMetadata.width > 0
            && publishedMetadata.height > 0,
        "decoded first frame was not published to the sink");

    if (sinkInstalled) {
        ArgusWorker::uninstallDecodedFrameSink(&sink);
    }
    sink.close();
    require(
        ArgusWorker::activeDecodedFrameMetadata().frameCount == 0,
        "active sink metadata remained after cleanup");
    require(
        ArgusWorker::activeDecodedFrameFailureCode() == 0,
        "active sink failure remained after cleanup");
    cleanupReinstallSucceeded =
        ArgusWorker::installDecodedFrameSink(&competingSink);
    require(cleanupReinstallSucceeded, "sink cleanup did not release ownership");
    if (cleanupReinstallSucceeded) {
        ArgusWorker::uninstallDecodedFrameSink(&competingSink);
    }
    competingSink.close();

    resetRendererConstructionCounts();
    bool interactiveWindowCreated = false;
    bool interactiveRendererInitialized = false;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO) == 0) {
        SDL_Window* window = StreamUtils::createTestWindow();
        interactiveWindowCreated = window != nullptr;
        require(interactiveWindowCreated, "interactive test window was not created");
        if (window != nullptr) {
            DECODER_PARAMETERS params {};
            params.window = window;
            params.vds = StreamingPreferences::VDS_FORCE_SOFTWARE;
            params.renderer = StreamingPreferences::RS_AUTO;
            params.videoFormat = VIDEO_FORMAT_H264;
            params.width = 1280;
            params.height = 720;
            params.frameRate = 60;
            params.testOnly = false;
            {
                SdlRenderer renderer;
                interactiveRendererInitialized = renderer.initialize(&params);
                require(
                    interactiveRendererInitialized,
                    "interactive SDL renderer was not initialized");
            }
            SDL_DestroyWindow(window);
        }
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
    }
    else {
        require(false, "SDL video initialization failed for interactive oracle");
    }
    const RendererConstructionCounts interactiveCounts =
        rendererConstructionCounts();
    require(
        interactiveCounts.windowCalls > 0
            && interactiveCounts.rendererCalls > 0,
        "interactive path did not construct its window and renderer");

    QJsonObject receipt {
        { QStringLiteral("schemaVersion"), 2 },
        { QStringLiteral("authority"), QJsonObject {
            { QStringLiteral("artifact"), QJsonObject {
                { QStringLiteral("sha256"), artifactSha256 },
                { QStringLiteral("length"), artifactLength },
                { QStringLiteral("sourceCommit"), sourceCommit },
                { QStringLiteral("sourceTree"), sourceTree },
            } },
            { QStringLiteral("oracle"), QJsonObject {
                { QStringLiteral("executableSha256"), oracleSha256 },
                { QStringLiteral("executableLength"), oracleLength },
                { QStringLiteral("configuration"), QStringLiteral(
                    "release-x64-argus_renderer_free_oracle") },
                { QStringLiteral("runnerPath"), runnerPath },
                { QStringLiteral("runnerWorkingTreeSha256"),
                    runnerWorkingTreeSha256 },
                { QStringLiteral("runnerGitBlobSha256"),
                    runnerGitBlobSha256 },
                { QStringLiteral("runnerCommit"), runnerCommit },
            } },
        } },
        { QStringLiteral("worker"), QJsonObject {
            { QStringLiteral("decoderInitialized"), decoderInitialized },
            { QStringLiteral("firstFramePublished"),
                publishedMetadata.frameCount == 1 },
            { QStringLiteral("frameCount"), publishedMetadata.frameCount },
            { QStringLiteral("width"), publishedMetadata.width },
            { QStringLiteral("height"), publishedMetadata.height },
            { QStringLiteral("constructionCalls"),
                constructionJson(workerCounts) },
        } },
        { QStringLiteral("sinkFailureAndCleanup"), QJsonObject {
            { QStringLiteral("openFailureCode"), invalidOpenCode },
            { QStringLiteral("installFailureCode"), installFailureCode },
            { QStringLiteral("publicationFailureCode"),
                publicationFailureCode },
            { QStringLiteral("firstFailureLatched"), firstFailureLatched },
            { QStringLiteral("conflictRejected"), competingInstallRejected },
            { QStringLiteral("cleanupReinstallSucceeded"),
                cleanupReinstallSucceeded },
            { QStringLiteral("activeAfterCleanup"),
                ArgusWorker::activeDecodedFrameMetadata().frameCount != 0 },
        } },
        { QStringLiteral("interactive"), QJsonObject {
            { QStringLiteral("windowCreated"), interactiveWindowCreated },
            { QStringLiteral("rendererInitialized"),
                interactiveRendererInitialized },
            { QStringLiteral("constructionCalls"),
                constructionJson(interactiveCounts) },
        } },
        { QStringLiteral("failures"), failures },
        { QStringLiteral("passed"), failures.isEmpty() },
    };

    if (outputPath.isEmpty() || !writeReceipt(outputPath, receipt)) {
        return 2;
    }
    return failures.isEmpty() ? 0 : 1;
}

}
