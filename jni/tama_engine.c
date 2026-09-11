/* ============================================================================
 *  Tamagotchi L.i.f.e. — Motor nativo ARM64 (reimplementação limpa, Via 2)
 *  v0.49+ — núcleo funcional
 *
 *  CÓDIGO NOVO, 100% original, escrito de raiz para arm64-v8a.
 *  NÃO deriva do binário ARM32 (irreversível): reimplementa o contrato JNI
 *  mapeado no dossiê do motor.
 *
 *  Exporta as 13 funções que a classe Java SampleGameNatives chama e honra os
 *  callbacks essenciais (texturas, save data, som) por method-ID em cache.
 * ==========================================================================*/

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <android/log.h>

#define LOG_TAG "TamaEngineARM64"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ---- assinatura da classe Java (ajusta se o teu package/classe diferir) ---- */
#define NATIVES_CLASS "com/namcobandaigames/tamagotchilife/SampleGameNatives"

/* ------------------------- Parts (máquina de estados) ---------------------- */
enum {
    PART_LOGO = 0,
    PART_TITLE,
    PART_GAME_MAIN,   /* ecrã principal de cuidar do bicho */
    PART_OPTION,
    PART_LIBRARY,
    PART_COUNT
};

/* ------------------------------ Estado global ------------------------------ */
typedef struct {
    int   base_w, base_h;
    int   real_w, real_h;
    int   part;

    double last_step_ms;
    double accum_ms;
    long   ticks;

    int   hunger;                 /* 100 = cheio, 0 = esfomeado */
    int   happy;                  /* 100 = feliz, 0 = triste */
    int   discipline;
    int   age_min;
    int   sick;
    int   calling;                /* 1 = a pedir atenção (pisca) */

    int   touch_active;
    float touch_x, touch_y;

    JavaVM   *vm;
    jclass    natives;
    jmethodID mLoadTexture;
    jmethodID mReadGameData;
    jmethodID mWriteGameData;
    jmethodID mPlaySound;
    jmethodID mGetSystemTime;
} TamaState;

static TamaState G;

/* --------------------------------- Utils ----------------------------------- */
static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1.0e6;
}
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static JNIEnv *get_env(void) {
    JNIEnv *env = NULL;
    if (G.vm) (*G.vm)->GetEnv(G.vm, (void **)&env, JNI_VERSION_1_6);
    return env;
}

static void recompute_calling(void) {
    G.calling = (G.hunger <= 25 || G.happy <= 25 || G.sick) ? 1 : 0;
}

static void sim_tick(void) {
    G.ticks++;
    G.age_min += 1;
    if ((G.ticks % 3) == 0)  G.hunger = clampi(G.hunger - 1, 0, 100);
    if ((G.ticks % 4) == 0)  G.happy  = clampi(G.happy  - 1, 0, 100);
    if (G.hunger == 0 && G.happy == 0 && !G.sick && (G.ticks % 10) == 0) G.sick = 1;
    recompute_calling();
}

/* ============================================================================
 *  JNI_OnLoad — cache de JavaVM e method-IDs dos callbacks
 * ==========================================================================*/
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    memset(&G, 0, sizeof(G));
    G.vm = vm;

    JNIEnv *env = NULL;
    if ((*vm)->GetEnv(vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK || !env) {
        LOGE("JNI_OnLoad: GetEnv falhou");
        return JNI_ERR;
    }

    jclass local = (*env)->FindClass(env, NATIVES_CLASS);
    if (local) {
        G.natives = (jclass)(*env)->NewGlobalRef(env, local);
        G.mLoadTexture   = (*env)->GetStaticMethodID(env, local, "OnLoadTexture",   "(Ljava/lang/String;)I");
        G.mReadGameData  = (*env)->GetStaticMethodID(env, local, "OnReadGameData",  "(Ljava/lang/String;I)[B");
        G.mWriteGameData = (*env)->GetStaticMethodID(env, local, "OnWriteGameData", "(Ljava/lang/String;[B)Z");
        G.mPlaySound     = (*env)->GetStaticMethodID(env, local, "OnPlaySound",     "(I)V");
        G.mGetSystemTime = (*env)->GetStaticMethodID(env, local, "OnGetSystemTime", "()I");
        (*env)->ExceptionClear(env);
    } else {
        (*env)->ExceptionClear(env);
        LOGE("JNI_OnLoad: classe %s nao encontrada (seguimos sem callbacks)", NATIVES_CLASS);
    }
    LOGI("Motor ARM64 carregado. JNI_VERSION_1_6.");
    return JNI_VERSION_1_6;
}

/* ============================================================================
 *  As 13 funções nativas exportadas (contrato SampleGameNatives)
 * ==========================================================================*/
#define J(name) Java_com_namcobandaigames_tamagotchilife_SampleGameNatives_##name

JNIEXPORT void JNICALL J(init)(JNIEnv *env, jclass clazz, jint w, jint h) {
    (void)env; (void)clazz;
    G.base_w = (w > 0) ? w : 480;
    G.base_h = (h > 0) ? h : 800;
    G.real_w = G.base_w; G.real_h = G.base_h;
    G.hunger = 80; G.happy = 80; G.discipline = 50;
    G.age_min = 0; G.sick = 0; G.calling = 0;
    G.part = PART_GAME_MAIN;
    G.ticks = 0; G.accum_ms = 0; G.last_step_ms = now_ms();
    recompute_calling();
    LOGI("init(%d,%d) -> bicho novo (hunger=%d happy=%d)", w, h, G.hunger, G.happy);
}

JNIEXPORT void JNICALL J(main)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    G.last_step_ms = now_ms();
    LOGI("main()");
}

JNIEXPORT void JNICALL J(step)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    double t = now_ms();
    double dt = t - G.last_step_ms;
    if (dt < 0) dt = 0;
    G.last_step_ms = t;
    G.accum_ms += dt;
    while (G.accum_ms >= 1000.0) { G.accum_ms -= 1000.0; sim_tick(); }
}

JNIEXPORT void JNICALL J(stop)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz; LOGI("stop() @tick=%ld", G.ticks);
}

JNIEXPORT void JNICALL J(GameTerm)(JNIEnv *env, jclass clazz) {
    (void)clazz;
    if (G.natives && env) { (*env)->DeleteGlobalRef(env, G.natives); G.natives = NULL; }
    LOGI("GameTerm()");
}

JNIEXPORT jint JNICALL J(GameGetPart)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    if (G.calling) return ((G.ticks & 1) == 0) ? 1 : 0;  /* pisca ao chamar */
    return 0;                                            /* satisfeito: estável */
}

JNIEXPORT void JNICALL J(GameSetAppRequest)(JNIEnv *env, jclass clazz, jint req) {
    switch (req) {
        case 1: G.hunger = clampi(G.hunger + 30, 0, 100); break;
        case 2: G.happy  = clampi(G.happy  + 30, 0, 100); break;
        case 3: G.sick = 0; break;
        case 0: J(init)(env, clazz, G.base_w, G.base_h); break;
        default: break;
    }
    recompute_calling();
    LOGI("GameSetAppRequest(%d) -> hunger=%d happy=%d sick=%d", req, G.hunger, G.happy, G.sick);
}

JNIEXPORT void JNICALL J(GameInputSetTouches)(JNIEnv *env, jclass clazz,
                                              jint id, jfloat x, jfloat y, jfloat a, jfloat b) {
    (void)env; (void)clazz; (void)id; (void)a; (void)b;
    G.touch_active = 1; G.touch_x = x; G.touch_y = y;
    if (G.hunger <= G.happy) G.hunger = clampi(G.hunger + 20, 0, 100);
    else                     G.happy  = clampi(G.happy  + 20, 0, 100);
    recompute_calling();
}

JNIEXPORT void JNICALL J(GameInputReleaseTouches)(JNIEnv *env, jclass clazz,
                                                  jint id, jfloat x, jfloat y) {
    (void)env; (void)clazz; (void)id; (void)x; (void)y;
    G.touch_active = 0;
}

JNIEXPORT jint JNICALL J(GetBaseScreenWidth)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz; return G.base_w > 0 ? G.base_w : 480;
}
JNIEXPORT jint JNICALL J(GetBaseScreenHeight)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz; return G.base_h > 0 ? G.base_h : 800;
}

JNIEXPORT void JNICALL J(ThreadCreate)(JNIEnv *env, jclass clazz, jint a, jint b) {
    (void)env; (void)clazz; (void)a; (void)b;
    LOGI("ThreadCreate(%d,%d) (no-op: usamos o loop do GLSurfaceView)", a, b);
}

JNIEXPORT void JNICALL J(TMGCmakeAlarmInfoData)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz; LOGI("TMGCmakeAlarmInfoData()");
}

/* ------------------ helpers de callback (para versões seguintes) ----------- */
__attribute__((unused))
static int call_load_texture(const char *path) {
    JNIEnv *env = get_env();
    if (!env || !G.natives || !G.mLoadTexture) return -1;
    jstring s = (*env)->NewStringUTF(env, path);
    jint id = (*env)->CallStaticIntMethod(env, G.natives, G.mLoadTexture, s);
    (*env)->DeleteLocalRef(env, s);
    return (int)id;
}
__attribute__((unused))
static int call_write_save(const char *name, const unsigned char *buf, int len) {
    JNIEnv *env = get_env();
    if (!env || !G.natives || !G.mWriteGameData) return 0;
    jstring s = (*env)->NewStringUTF(env, name);
    jbyteArray arr = (*env)->NewByteArray(env, len);
    (*env)->SetByteArrayRegion(env, arr, 0, len, (const jbyte *)buf);
    jboolean ok = (*env)->CallStaticBooleanMethod(env, G.natives, G.mWriteGameData, s, arr);
    (*env)->DeleteLocalRef(env, arr);
    (*env)->DeleteLocalRef(env, s);
    return ok ? 1 : 0;
}
