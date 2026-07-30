#include "argus/workerbootstrap.h"

#include <QCoreApplication>
#include <QDebug>

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
        qputenv(ArgusWorker::StartupPipeEnvironmentVariable,
                pipeName.toUtf8());
        check(ArgusWorker::runStartup(
                  {"Moonlight.exe", "--argus-worker", "--protocol", "1"})
                  == ArgusWorker::ExitHandshakeProtocolPending,
              "Worker must connect only through the inherited pipe capability");
        check(qEnvironmentVariableIsEmpty(
                  ArgusWorker::StartupPipeEnvironmentVariable),
              "Connected worker startup must remove the inherited capability");
        CloseHandle(serverPipe);
    }
#endif

    return failures == 0 ? 0 : 1;
}
