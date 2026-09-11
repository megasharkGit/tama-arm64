LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

# O nome do módulo TEM de coincidir com o .so esperado pela app:
#   libAndroidGame001JNI.so  ->  LOCAL_MODULE := AndroidGame001JNI
LOCAL_MODULE    := AndroidGame001JNI
LOCAL_SRC_FILES := tama_engine.c

LOCAL_LDLIBS    := -llog -lGLESv1_CM -lm
LOCAL_CFLAGS    := -O2 -Wall -Wextra

include $(BUILD_SHARED_LIBRARY)
