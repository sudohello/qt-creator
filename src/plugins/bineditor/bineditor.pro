TEMPLATE = lib
TARGET = BinEditor
include(../../qtcreatorplugin.pri)

HEADERS += bineditorplugin.h \
        bineditor.h \
        bineditorconstants.h \
        markup.h

SOURCES += bineditorplugin.cpp \
        bineditor.cpp
