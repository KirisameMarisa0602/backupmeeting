# 使用前，包含方必须设置 COMMON_DIR 指向 common 目录
isEmpty(COMMON_DIR) {
    error("common.pri requires COMMON_DIR to be set by includer")
}

INCLUDEPATH += $$COMMON_DIR
HEADERS += $$COMMON_DIR/protocol.h
SOURCES += $$COMMON_DIR/protocol.cpp