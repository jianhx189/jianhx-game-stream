QT += widgets network
CONFIG += c++17 release
TEMPLATE = app
TARGET = MiniStreamHost
VERSION = 2.3.4
SOURCES += main.cpp
HEADERS += host_setup.h ui_components.h invitation_pairing.h
RC_ICONS = ministream.ico
QMAKE_TARGET_PRODUCT = MiniStream Host
QMAKE_TARGET_DESCRIPTION = MiniStream Host Console
LIBS += -liphlpapi -lshell32
QMAKE_CXXFLAGS += /utf-8
LIBS += -lws2_32

LIBS += -luser32
