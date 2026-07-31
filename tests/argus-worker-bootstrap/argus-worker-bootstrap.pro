QT += core network
CONFIG += console testcase c++17
CONFIG -= app_bundle
DEFINES += ARGUS_STARTUP_CHANNEL_TESTS
TEMPLATE = app
TARGET = argus-worker-bootstrap-tests

INCLUDEPATH += ../../app

SOURCES += \
    main.cpp \
    ../../app/argus/frameslotwriter.cpp \
    ../../app/argus/pairingendpoint.cpp \
    ../../app/argus/pairingidentitypackage.cpp \
    ../../app/argus/startupchannel.cpp \
    ../../app/argus/workerbootstrap.cpp \
    ../../app/backend/identitymanager.cpp \
    ../../app/backend/nvaddress.cpp

HEADERS += \
    ../../app/argus/frameslotwriter.h \
    ../../app/argus/pairingidentitypackage.h \
    ../../app/argus/pairingendpoint.h \
    ../../app/argus/startupchannel.h \
    ../../app/argus/workerbootstrap.h \
    ../../app/backend/identitymanager.h

win32 {
    INCLUDEPATH += \
        $$PWD/../../libs/windows/include \
        $$PWD/../../libs/windows/include/x64
    LIBS += \
        -L$$PWD/../../libs/windows/lib/x64 \
        -llibssl \
        -llibcrypto \
        kernel32.lib
}
