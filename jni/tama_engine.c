/* ============================================================================
 *  Tamagotchi L.i.f.e. — Motor nativo ARM64 (reimplementação limpa, Via 2)
 *  v0.52 — TEXTURAS REAIS: corrigir perda de contexto + diagnóstico de formato
 *  ----------------------------------------------------------------------------
 *  Diagnóstico da v0.51 (feedback do utilizador):
 *   (A) Xadrez preto/verde estático  -> textura carrega mas o FORMATO de pixel
 *       do .tgd está a ser mal interpretado no upload (provável RGBA4444/RGB565
 *       lido como LUMINANCE ou com stride/largura errada).
 *   (B) Verde contínuo após apagar/acender o ecrã -> o contexto EGL foi DESTRUÍDO
 *       e a v0.51 NÃO recarregava as texturas (tinha um latch de 1-só-vez).
 *
 *  Correções nesta v0.52:
 *   1. FIM DO LATCH: as texturas são recarregadas SEMPRE que o contexto GL é
 *      (re)criado. init() é chamado a partir de onSurfaceCreated; incrementamos
 *      uma "época" de contexto e invalidamos a cache de handles a cada época.
 *      => resolve (B): ao acordar o ecrã, as texturas voltam a ser carregadas.
 *   2. DIAGNÓSTICO GL: após cada OnLoadTexture consultamos glGetError() e
 *      registamos o handle e o erro, para isolar "decode errado" de "draw errado".
 *   3. GameGetPart() continua a reportar o estado de carregamento (sinal
 *      observável no renderer atual): 7/7 -> 1 estável; parcial -> pisca; 0 -> 0.
 *
 *  NOTA sobre (A): a descodificação real do .tgd acontece no lado Java
 *  (SampleGameNatives.OnLoadTexture). Para corrigir o xadrez é preciso confirmar
 *  o FORMATO com que o Java faz glTexImage2D (ver bloco DIAGNOSTICO no fim do
 *  ficheiro). Esta v0.52 instrumenta o nativo para te dar os números exatos.
 *
 *  Código 100% original, escrito de raiz para arm64-v8a.
 * ==========================================================================*/
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <android/log.h>
#include <GLES/gl.h>

#define LOG_TAG "TamaEngineARM64"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define NATIVES_CLASS "com/namcobandaigames/tamagotchilife/SampleGameNatives"

/* --------------------- Lista de texturas reais a carregar ------------------ */
static const char *const TEX_LIST[] = {
    "Tama2Movie_texture.tgd",
    "Tama2Movie_texture_c.tgd",
    "body_00000.tgd",
    "seg_00000.tgd",
    "seg_icon_00000.tgd",
    "font.tgd",
    "number_texture.tgd",
};
#define TEX_COUNT ((int)(sizeof(TEX_LIST)/sizeof(TEX_LIST[0])))

/* ------------------------------ Estado global ------------------------------ */
typedef struct {
    JavaVM   *vm;
    jclass    natives;
    jmethodID mLoadTexture;
    jmethodID mReadGameData;
    jmethodID mWriteGameData;
    jmethodID mPlaySound;
    jmethodID mGetSystemTime;

    int base_w, base_h;
    int real_w, real_h;
    int part;

    int  tex_id[TEX_COUNT];
    int  tex_loaded;
    long gl_epoch;        /* incrementa a cada (re)criação de contexto */
    long tex_epoch;       /* época em que a cache foi preenchida       */

    int  hunger, happy, sick, calling;
    long ticks, age_min;
    double last_step_ms;

    int   touch_active;
    float touch_x, touch_y;
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

static const char *gl_err_str(GLenum e) {
    switch (e) {
        case GL_NO_ERROR:          return "GL_NO_ERROR";
        case GL_INVALID_ENUM:      return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:     return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY:     return "GL_OUT_OF_MEMORY";
        default:                   return "GL_?";
    }
}

/* Chama OnLoadTexture(String) e devolve handle (>=0) ou -1, com diagnóstico */
static int call_load_texture(JNIEnv *env, const char *name) {
    if (!env || !G.natives || !G.mLoadTexture) return -1;
    while (glGetError() != GL_NO_ERROR) {}          /* limpa erros pendentes */
    jstring s = (*env)->NewStringUTF(env, name);
    if (!s) return -1;
    jint id = (*env)->CallStaticIntMethod(env, G.natives, G.mLoadTexture, s);
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionClear(env);
        (*env)->DeleteLocalRef(env, s);
        LOGE("OnLoadTexture('%s') lancou excecao", name);
        return -1;
    }
    (*env)->DeleteLocalRef(env, s);

    GLenum e = glGetError();
    GLboolean is_tex = (id >= 0) ? glIsTexture((GLuint)id) : GL_FALSE;
    LOGI("OnLoadTexture('%s') -> id=%d | glIsTexture=%d | glGetError=%s",
         name, (int)id, (int)is_tex, gl_err_str(e));
    return (int)id;
}

/* Carrega TODAS as texturas reais para a época de contexto ATUAL. */
static void load_all_textures(JNIEnv *env) {
    if (G.tex_epoch == G.gl_epoch && G.tex_loaded > 0) return; /* já feito nesta época */
    G.tex_loaded = 0;
    for (int i = 0; i < TEX_COUNT; ++i) {
        int id = call_load_texture(env, TEX_LIST[i]);
        G.tex_id[i] = id;
        if (id >= 0) G.tex_loaded++;
    }
    G.tex_epoch = G.gl_epoch;
    LOGI("=== Texturas reais (epoca %ld): %d/%d carregadas (callback %s) ===",
         G.gl_epoch, G.tex_loaded, TEX_COUNT, G.mLoadTexture ? "presente" : "AUSENTE");
}

/* Invalida a cache quando o contexto GL é (re)criado. */
static void on_context_created(void) {
    G.gl_epoch++;
    G.tex_loaded = 0;
    for (int i = 0; i < TEX_COUNT; ++i) G.tex_id[i] = -1;
    const GLubyte *ven = glGetString(GL_VENDOR);
    const GLubyte *ren = glGetString(GL_RENDERER);
    const GLubyte *ver = glGetString(GL_VERSION);
    LOGI("Contexto GL (re)criado. epoca=%ld | %s | %s | %s",
         G.gl_epoch,
         ven ? (const char*)ven : "?",
         ren ? (const char*)ren : "?",
         ver ? (const char*)ver : "?");
}

/* ============================================================================
 *  JNI_OnLoad
 * ==========================================================================*/
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved) {
    (void)reserved;
    memset(&G, 0, sizeof(G));
    for (int i = 0; i < TEX_COUNT; ++i) G.tex_id[i] = -1;
    G.gl_epoch = 0; G.tex_epoch = -1;
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
        G.mReadGameData  = (*env)->GetStaticMethodID(env, local, "OnReadGameData",  "(Ljava/lang/String;I)B");
        G.mWriteGameData = (*env)->GetStaticMethodID(env, local, "OnWriteGameData", "(Ljava/lang/String;B)Z");
        G.mPlaySound     = (*env)->GetStaticMethodID(env, local, "OnPlaySound",     "(I)V");
        G.mGetSystemTime = (*env)->GetStaticMethodID(env, local, "OnGetSystemTime", "()I");
        (*env)->ExceptionClear(env);
    } else {
        (*env)->ExceptionClear(env);
        LOGE("JNI_OnLoad: classe %s nao encontrada", NATIVES_CLASS);
    }
    LOGI("Motor ARM64 v0.52 carregado. OnLoadTexture=%s.",
         G.mLoadTexture ? "OK" : "nao encontrado");
    return JNI_VERSION_1_6;
}

/* ============================================================================
 *  13 funções exportadas
 * ==========================================================================*/
#define J(name) Java_com_namcobandaigames_tamagotchilife_SampleGameNatives_##name

/* init() é chamado a partir de onSurfaceCreated => aqui há SEMPRE novo contexto. */
JNIEXPORT void JNICALL J(init)(JNIEnv *env, jclass clazz, jint w, jint h) {
    (void)clazz;
    G.base_w = (w > 0) ? w : 480;
    G.base_h = (h > 0) ? h : 800;
    G.real_w = G.base_w;
    G.real_h = G.base_h;

    G.hunger = 80; G.happy = 80; G.sick = 0;
    recompute_calling();

    /* >>> contexto GL novo: invalida cache e RECARREGA texturas reais <<< */
    on_context_created();
    load_all_textures(env);

    LOGI("init(%d,%d): base=%dx%d, texturas=%d/%d (epoca %ld)",
         w, h, G.base_w, G.base_h, G.tex_loaded, TEX_COUNT, G.gl_epoch);
}

JNIEXPORT void JNICALL J(main)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    G.last_step_ms = now_ms();
    LOGI("main()");
}

JNIEXPORT void JNICALL J(step)(JNIEnv *env, jclass clazz) {
    (void)clazz;
    double t = now_ms();
    double dt = t - G.last_step_ms;
    if (dt < 0) dt = 0;
    G.last_step_ms = t;

    /* Rede de segurança: se a época de textura ficou atrás da época de contexto
     * (ex.: contexto recriado sem passar por init), recarrega. */
    if (G.tex_epoch != G.gl_epoch && G.mLoadTexture) {
        load_all_textures(env);
    }

    G.ticks++;
    G.age_min++;
    if ((G.ticks % 30) == 0) {
        G.hunger = clampi(G.hunger - 1, 0, 100);
        G.happy  = clampi(G.happy  - 1, 0, 100);
        recompute_calling();
    }
}

JNIEXPORT void JNICALL J(stop)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    LOGI("stop() @tick=%ld", G.ticks);
}

JNIEXPORT void JNICALL J(GameTerm)(JNIEnv *env, jclass clazz) {
    (void)clazz;
    if (G.natives && env) { (*env)->DeleteGlobalRef(env, G.natives); G.natives = NULL; }
    LOGI("GameTerm()");
}

JNIEXPORT jint JNICALL J(GameGetPart)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    if (G.tex_epoch != G.gl_epoch) return 0;             /* ainda por carregar */
    if (G.tex_loaded >= TEX_COUNT) return 1;             /* tudo carregou      */
    if (G.tex_loaded > 0) return ((G.ticks & 1) == 0) ? 1 : 0;
    return 0;
}

JNIEXPORT void JNICALL J(GameSetAppRequest)(JNIEnv *env, jclass clazz, jint req) {
    (void)env;
    switch (req) {
        case 1: G.hunger = clampi(G.hunger + 30, 0, 100); break;
        case 2: G.happy  = clampi(G.happy  + 30, 0, 100); break;
        case 3: G.sick = 0; break;
        case 0: J(init)(env, clazz, G.base_w, G.base_h); break;
        default: break;
    }
    recompute_calling();
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
    LOGI("ThreadCreate(%d,%d) (no-op)", a, b);
}
JNIEXPORT void JNICALL J(TMGCmakeAlarmInfoData)(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    LOGI("TMGCmakeAlarmInfoData()");
}

/* ============================================================================
 *  DIAGNOSTICO — para corrigir o XADREZ (formato do .tgd)
 *  ----------------------------------------------------------------------------
 *  O xadrez preto/verde vem do UPLOAD com formato errado. Precisamos de saber
 *  como o Java (SampleGameNatives.OnLoadTexture) faz o glTexImage2D. Duas
 *  hipóteses típicas e a respetiva correção:
 *
 *   H1) Usa GLUtils.texImage2D(target, 0, bitmap, 0) a partir de um Bitmap:
 *       -> nesse caso o .tgd tem de ser DESCODIFICADO para Bitmap ARGB_8888
 *          ANTES. Se o .tgd for passado a BitmapFactory como se fosse PNG/JPG,
 *          bitmap vem null e o resultado é lixo. Correção: escrever o decoder
 *          .tgd -> int[] ARGB e criar Bitmap.createBitmap(px,w,h,ARGB_8888).
 *
 *   H2) Faz glTexImage2D manual com GL_RGBA/GL_UNSIGNED_BYTE mas o .tgd guarda
 *       16 bits/pixel (RGBA4444 ou RGB565). Correção: usar o par correto:
 *          RGBA4444: format=GL_RGBA, type=GL_UNSIGNED_SHORT_4_4_4_4
 *          RGB565  : format=GL_RGB,  type=GL_UNSIGNED_SHORT_5_6_5
 *       e passar a largura/altura REAIS lidas do cabeçalho .tgd.
 *
 *  ENVIA-ME, do teu SampleGameNatives.java, o corpo de OnLoadTexture(String)
 *  e do parser do .tgd (ou o cabeçalho: bytes iniciais / campos width/height/
 *  format). Com isso, a v0.53 corrige o formato exato e o xadrez desaparece.
 * ==========================================================================*/
