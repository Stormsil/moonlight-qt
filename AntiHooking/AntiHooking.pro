QT       -= core gui

TARGET = AntiHooking
TEMPLATE = lib

win32:*-msvc:CONFIG(release, debug|release) {
    # Keep the CodeView path independent of the task-local build root so the
    # separately deployed DLL is reproducible alongside Moonlight.exe.
    QMAKE_LFLAGS += /PDBALTPATH:AntiHooking.pdb
}

include(../globaldefs.pri)

INCLUDEPATH += $$PWD/../libs/windows/include
contains(QT_ARCH, i386) {
    LIBS += -L$$PWD/../libs/windows/lib/x86
}
contains(QT_ARCH, x86_64) {
    LIBS += -L$$PWD/../libs/windows/lib/x64
}
contains(QT_ARCH, arm64) {
    LIBS += -L$$PWD/../libs/windows/lib/arm64
}

LIBS += -ldetours
DEFINES += ANTIHOOKING_LIBRARY
SOURCES += antihookingprotection.cpp
HEADERS += antihookingprotection.h
