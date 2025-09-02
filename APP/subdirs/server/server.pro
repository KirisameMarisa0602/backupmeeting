QT += core network gui sql
CONFIG += c++11 console
CONFIG -= app_bundle
TEMPLATE = app
TARGET = server

INCLUDEPATH += $$PWD
INCLUDEPATH += $$PWD/common
INCLUDEPATH += $$PWD/src

SOURCES += \
    src/main.cpp \
    src/roomhub.cpp \
    src/udprelay.cpp \
    src/udpmedia_client.cpp \
    src/recorder.cpp \
    common/protocol.cpp \
    common/annot.cpp

HEADERS += \
    src/roomhub.h \
    src/udprelay.h \
    src/udpmedia_client.h \
    src/recorder.h \
    common/protocol.h \
    common/annot.h

qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
