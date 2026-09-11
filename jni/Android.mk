LOCAL_PATH := $(call my-dir)
include $(CLEAR_VARS)
LOCAL_MODULE    := AndroidGame001JNI            # -> libAndroidGame001JNI.so
LOCAL_SRC_FILES := tama_engine.c
LOCAL_LDLIBS    := -llog -lGLESv1_CM -lEGL -lm
LOCAL_CFLAGS    := -O2 -Wall -Wextra -fvisibility=hidden \
                   -DJNIEXPORT='__attribute__((visibility("default")))'
include $(BUILD_SHARED_LIBRARY)
