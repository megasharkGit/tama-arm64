/* ============================================================================
 *  Tamagotchi L.i.f.e. — Motor nativo ARM64 (Via 2)
 *  v0.52 — PRIMEIRA TEXTURA: desenho GLES1 dentro de step()
 *
 *  Descoberta que torna isto possível (confirmada por desmontagem do teu APK):
 *  o renderer Java (SampleGameGLView / classe 'ao') faz, a cada frame:
 *      1) GameGetPart()           (nativo)
 *      2) glClearColor(1,0,valor) + glClear()
 *      3) step()                  (nativo)   <-- corre na GL thread, contexto ativo
 *      4) glFlush()
 *  Logo, qualquer comando OpenGL ES 1.x emitido dentro de step() desenha POR CIMA
 *  do fundo e fica visível. É exatamente isso que fazemos aqui.
 *
 *  Nesta versão o motor:
 *   - cria (uma vez, já na GL thread) uma textura RGBA 32x32 gerada por código
 *     (padrão axadrezado) — prova o pipeline de texturas SEM depender ainda dos
 *     callbacks Java de carregamento (OnLoadTexture) que ligamos na próxima versão;
 *   - desenha um quad de ecrã inteiro com essa textura;
 *   - modula a cor conforme o estado do bicho: VERDE quando calmo, VERMELHO a
 *     piscar quando "chama" (fome/feliz baixos ou doente).
 *
 *  Resultado no ecrã: em vez do pulso vermelho/magenta, passas a ver um padrão
 *  axadrezado (a textura!) que fica verde quando o bicho está bem e pisca
 *  vermelho quando precisa de atenção.
 * ==========================================================================*/

#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <android/log.h>
#include <GLES/gl.h>          /* OpenGL ES 1.x (a app usa GL10) */

#define LOG_TAG "TamaEngineARM64"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define NATIVES_CLASS "com/namcobandaigames/tamagotchilife/SampleGameNatives"

enum { PART_LOGO=0, PART_TITLE, PART_GAME_MAIN, PART_OPTION, PART_LIBRARY, PART_COUNT };

typedef struct {
    int   base_w, base_h;
    int   real_w, real_h;
    int   part;

    double last_step_ms;
    double accum_ms;
    long   ticks;

    int   hunger, happy, discipline, age_min, sick, calling;

    int   touch_active;
    float touch_x, touch_y;

    /* --- GL --- */
    GLuint tex;                /* nome da textura gerada */
    int    tex_ready;          /* 1 depois de criada (na GL thread) */

    JavaVM   *vm;
    jclass    natives;
    jmethodID mLoadTexture, mGetTextureID, mGetTextureW, mGetTextureH;
    jmethodID mReadGameData, mWriteGameData, mPlaySound, mGetSystemTime;
} TamaState;

static TamaState G;

/* --------------------------------- Utils ----------------------------------- */
static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec*1000.0 + (double)ts.tv_nsec/1.0e6;
}
static int clampi(int v,int lo,int hi){return v<lo?lo:(v>hi?hi:v);}
static JNIEnv *get_env(void){JNIEnv*e=NULL;if(G.vm)(*G.vm)->GetEnv(G.vm,(void**)&e,JNI_VERSION_1_6);return e;}
static void recompute_calling(void){G.calling=(G.hunger<=25||G.happy<=25||G.sick)?1:0;}
static void sim_tick(void){
    G.ticks++; G.age_min++;
    if((G.ticks%3)==0) G.hunger=clampi(G.hunger-1,0,100);
    if((G.ticks%4)==0) G.happy =clampi(G.happy -1,0,100);
    if(G.hunger==0&&G.happy==0&&!G.sick&&(G.ticks%10)==0) G.sick=1;
    recompute_calling();
}

/* ============================ GL: textura + desenho ======================== */
#define TW 32
#define TH 32
static void gl_make_texture(void){          /* corre na GL thread (via step) */
    if (G.tex_ready) return;
    static unsigned char px[TW*TH*4];
    for (int y=0;y<TH;y++)
        for (int x=0;x<TW;x++){
            int i=(y*TW+x)*4;
            int checker=((x>>2)+(y>>2))&1;
            unsigned char c = checker?0x30:0xF0;
            px[i+0]=c; px[i+1]=c; px[i+2]=c; px[i+3]=0xFF;
        }
    glGenTextures(1,&G.tex);
    glBindTexture(GL_TEXTURE_2D,G.tex);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_REPEAT);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,TW,TH,0,GL_RGBA,GL_UNSIGNED_BYTE,px);
    G.tex_ready=1;
    LOGI("textura criada (id=%u, %dx%d)",G.tex,TW,TH);
}

static void gl_draw(void){
    int W = G.base_w>0?G.base_w:480;
    int H = G.base_h>0?G.base_h:800;

    gl_make_texture();

    /* projeção em pixels, origem no canto superior esquerdo */
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrthof(0.0f,(GLfloat)W,(GLfloat)H,0.0f,-1.0f,1.0f);
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    /* cor de modulação = estado do bicho */
    if (G.calling) {
        float on = ((G.ticks & 1)==0) ? 1.0f : 0.35f;  /* pisca vermelho */
        glColor4f(on,0.15f,0.15f,1.0f);
    } else {
        glColor4f(0.25f,0.9f,0.35f,1.0f);               /* verde calmo */
    }

    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D,G.tex);

    /* quad de ecrã inteiro; repetimos a textura algumas vezes (UV 0..6) */
    GLfloat v[] = { 0,0,  (GLfloat)W,0,  0,(GLfloat)H,  (GLfloat)W,(GLfloat)H };
    GLfloat t[] = { 0,0,  6,0,          0,10,          6,10 };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2,GL_FLOAT,0,v);
    glTexCoordPointer(2,GL_FLOAT,0,t);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    glDisable(GL_TEXTURE_2D);
    glColor4f(1,1,1,1);          /* repõe cor por defeito */
}

/* ================================ JNI_OnLoad =============================== */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *reserved){
    (void)reserved; memset(&G,0,sizeof(G)); G.vm=vm;
    JNIEnv*env=NULL;
    if((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK||!env){LOGE("GetEnv falhou");return JNI_ERR;}
    jclass local=(*env)->FindClass(env,NATIVES_CLASS);
    if(local){
        G.natives=(jclass)(*env)->NewGlobalRef(env,local);
        G.mLoadTexture =(*env)->GetStaticMethodID(env,local,"OnLoadTexture","(Ljava/lang/String;)I");
        G.mGetTextureID=(*env)->GetStaticMethodID(env,local,"OnGetTextureID","(I)I");
        G.mGetTextureW =(*env)->GetStaticMethodID(env,local,"OnGetTextureWidth","(I)I");
        G.mGetTextureH =(*env)->GetStaticMethodID(env,local,"OnGetTextureHeight","(I)I");
        G.mReadGameData=(*env)->GetStaticMethodID(env,local,"OnReadGameData","(Ljava/lang/String;I)[B");
        G.mWriteGameData=(*env)->GetStaticMethodID(env,local,"OnWriteGameData","(Ljava/lang/String;[B)Z");
        G.mPlaySound   =(*env)->GetStaticMethodID(env,local,"OnPlaySound","(I)V");
        G.mGetSystemTime=(*env)->GetStaticMethodID(env,local,"OnGetSystemTime","()I");
        (*env)->ExceptionClear(env);
    } else {(*env)->ExceptionClear(env); LOGE("classe %s nao encontrada",NATIVES_CLASS);}
    LOGI("Motor ARM64 v0.52 carregado.");
    return JNI_VERSION_1_6;
}

/* ============================== 13 funcoes JNI ============================= */
#define J(name) Java_com_namcobandaigames_tamagotchilife_SampleGameNatives_##name

JNIEXPORT void JNICALL J(init)(JNIEnv*env,jclass c,jint w,jint h){
    (void)env;(void)c;
    G.base_w=(w>0)?w:480; G.base_h=(h>0)?h:800; G.real_w=G.base_w; G.real_h=G.base_h;
    G.hunger=80; G.happy=80; G.discipline=50; G.age_min=0; G.sick=0; G.calling=0;
    G.part=PART_GAME_MAIN; G.ticks=0; G.accum_ms=0; G.last_step_ms=now_ms();
    G.tex_ready=0;   /* recriar textura na proxima GL thread */
    recompute_calling();
    LOGI("init(%d,%d)",w,h);
}
JNIEXPORT void JNICALL J(main)(JNIEnv*env,jclass c){(void)env;(void)c;G.last_step_ms=now_ms();LOGI("main()");}

JNIEXPORT void JNICALL J(step)(JNIEnv*env,jclass c){
    (void)env;(void)c;
    double t=now_ms(),dt=t-G.last_step_ms; if(dt<0)dt=0; G.last_step_ms=t;
    G.accum_ms+=dt; while(G.accum_ms>=1000.0){G.accum_ms-=1000.0; sim_tick();}
    gl_draw();     /* <<< desenha a textura por cima do fundo (na GL thread) */
}

JNIEXPORT void JNICALL J(stop)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("stop()");}
JNIEXPORT void JNICALL J(GameTerm)(JNIEnv*env,jclass c){
    (void)c; if(G.natives&&env){(*env)->DeleteGlobalRef(env,G.natives);G.natives=NULL;} LOGI("GameTerm()");
}

/* IMPORTANTE: agora o desenho e' feito por gl_draw().
 * Mantemos GameGetPart a devolver 0 para o fundo (glClear) ser vermelho estavel;
 * a textura tapa-o quase todo e a cor do bicho vem da modulacao em gl_draw(). */
JNIEXPORT jint JNICALL J(GameGetPart)(JNIEnv*env,jclass c){(void)env;(void)c;return 0;}

JNIEXPORT void JNICALL J(GameSetAppRequest)(JNIEnv*env,jclass c,jint req){
    switch(req){
        case 1: G.hunger=clampi(G.hunger+30,0,100); break;
        case 2: G.happy =clampi(G.happy +30,0,100); break;
        case 3: G.sick=0; break;
        case 0: J(init)(env,c,G.base_w,G.base_h); break;
        default: break;
    }
    recompute_calling();
}
JNIEXPORT void JNICALL J(GameInputSetTouches)(JNIEnv*env,jclass c,jint id,jfloat x,jfloat y,jfloat a,jfloat b){
    (void)env;(void)c;(void)id;(void)a;(void)b; G.touch_active=1; G.touch_x=x; G.touch_y=y;
    if(G.hunger<=G.happy) G.hunger=clampi(G.hunger+20,0,100); else G.happy=clampi(G.happy+20,0,100);
    recompute_calling();
}
JNIEXPORT void JNICALL J(GameInputReleaseTouches)(JNIEnv*env,jclass c,jint id,jfloat x,jfloat y){
    (void)env;(void)c;(void)id;(void)x;(void)y; G.touch_active=0;
}
JNIEXPORT jint JNICALL J(GetBaseScreenWidth)(JNIEnv*env,jclass c){(void)env;(void)c;return G.base_w>0?G.base_w:480;}
JNIEXPORT jint JNICALL J(GetBaseScreenHeight)(JNIEnv*env,jclass c){(void)env;(void)c;return G.base_h>0?G.base_h:800;}
JNIEXPORT void JNICALL J(ThreadCreate)(JNIEnv*env,jclass c,jint a,jint b){(void)env;(void)c;(void)a;(void)b;LOGI("ThreadCreate");}
JNIEXPORT void JNICALL J(TMGCmakeAlarmInfoData)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("AlarmInfo");}

/* ---- helpers de callback Java (para v0.53: carregar textura .png real) ---- */
__attribute__((unused))
static int call_load_texture(const char*path){
    JNIEnv*env=get_env(); if(!env||!G.natives||!G.mLoadTexture) return -1;
    jstring s=(*env)->NewStringUTF(env,path);
    jint h=(*env)->CallStaticIntMethod(env,G.natives,G.mLoadTexture,s);
    (*env)->DeleteLocalRef(env,s); return (int)h;
}
