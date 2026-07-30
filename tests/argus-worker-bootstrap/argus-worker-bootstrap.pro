QT += core
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = argus-worker-bootstrap-tests

INCLUDEPATH += ../../app

SOURCES += \
    main.cpp \
    ../../app/argus/workerbootstrap.cpp

HEADERS += \
    ../../app/argus/workerbootstrap.h

win32:LIBS += kernel32.lib
