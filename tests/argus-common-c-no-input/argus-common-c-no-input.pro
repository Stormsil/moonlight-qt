QT -= core gui

TEMPLATE = app
TARGET = argus-common-c-no-input-tests
CONFIG += console c++17
CONFIG -= app_bundle
DEFINES += LC_TEST_INPUT_ISOLATION

isEmpty(ARGUS_COMMON_C_DIR) {
    error("ARGUS_COMMON_C_DIR is required for Argus common-c tests")
}

COMMON_C_DIR = $$clean_path($$ARGUS_COMMON_C_DIR)
INCLUDEPATH += $$COMMON_C_DIR/src

win32:CONFIG(release, debug|release): LIBS += -L$$OUT_PWD/../../moonlight-common-c/release/ -lmoonlight-common-c
else:win32:CONFIG(debug, debug|release): LIBS += -L$$OUT_PWD/../../moonlight-common-c/debug/ -lmoonlight-common-c
else:unix: LIBS += -L$$OUT_PWD/../../moonlight-common-c/ -lmoonlight-common-c

win32 {
    contains(QT_ARCH, x86_64) {
        LIBS += -L$$PWD/../../libs/windows/lib/x64
    }
    LIBS += -llibssl -llibcrypto -lws2_32 -lwinmm
}

SOURCES += main.cpp
