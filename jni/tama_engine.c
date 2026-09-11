/* ============================================================================
 *  Tamagotchi L.i.f.e. — Motor nativo ARM64 (Via 2)
 *  v0.54 — CORRIGIR ENQUADRAMENTO: quad de ecra inteiro em NDC
 *  ----------------------------------------------------------------------------
 *  Sintoma da v0.53: a shell real DESENHA, mas ocupa so ~metade x ~2/3 do ecra,
 *  encostada ao canto superior esquerdo.
 *  Causa: a projecao ortografica usava base_w/base_h (480x800), que NAO coincide
 *  com o glViewport real (definido pela app para a resolucao do dispositivo).
 *  Correcao: desenhar o quad em COORDENADAS NORMALIZADAS (NDC), com PROJECTION e
 *  MODELVIEW em identidade. Um quad de (-1,-1) a (1,1) preenche SEMPRE o viewport
 *  completo, independentemente do seu tamanho. Fim do desalinhamento.
 *
 *  Confirmado antes: os .tgd sao PNG RGBA8888; OnLoadTexture devolve INDICE e
 *  OnGetTextureID devolve o nome GL; a shell e body_00000.tgd.
 *
 *  Mantido da v0.52/0.53: recarga por epoca de contexto (perda ao adormecer) e
 *  diagnostico no logcat.
 * ==========================================================================*/
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <android/log.h>
#include <GLES/gl.h>

#define LOG_TAG "TamaEngineARM64"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define NATIVES_CLASS "com/namcobandaigames/tamagotchilife/SampleGameNatives"

static const char *const TEX_LIST[] = {
    "body_00000.tgd",           /* 0: shell azul -> desenhada em ecra inteiro */
    "Tama2Movie_texture.tgd",
    "Tama2Movie_texture_c.tgd",
    "seg_00000.tgd",
    "seg_icon_00000.tgd",
    "font.tgd",
    "number_texture.tgd",
};
#define TEX_COUNT ((int)(sizeof(TEX_LIST)/sizeof(TEX_LIST[0])))
#define TEX_SHELL 0

typedef struct {
    JavaVM   *vm;
    jclass    natives;
    jmethodID mLoadTexture, mGetTextureID, mGetTextureW, mGetTextureH;

    int base_w, base_h, real_w, real_h;

    int  tex_index[TEX_COUNT];
    int  tex_glid[TEX_COUNT];
    int  tex_loaded;
    long gl_epoch, tex_epoch;

    int  hunger, happy, sick, calling;
    long ticks;
    int   touch_active; float touch_x, touch_y;
} TamaState;

static TamaState G;

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (double)t.tv_sec*1000.0 + (double)t.tv_nsec/1.0e6; }
static int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
static void recompute_calling(void){ G.calling=(G.hunger<=25||G.happy<=25||G.sick)?1:0; }

static const char *gl_err(GLenum e){
    switch(e){case GL_NO_ERROR:return"NO_ERROR";case GL_INVALID_ENUM:return"INVALID_ENUM";
    case GL_INVALID_VALUE:return"INVALID_VALUE";case GL_INVALID_OPERATION:return"INVALID_OPERATION";
    case GL_OUT_OF_MEMORY:return"OUT_OF_MEMORY";default:return"?";}
}

static int jcall_int_str(JNIEnv*env,jmethodID m,const char*s){
    if(!env||!G.natives||!m) return -1;
    jstring js=(*env)->NewStringUTF(env,s);
    jint r=(*env)->CallStaticIntMethod(env,G.natives,m,js);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);(*env)->DeleteLocalRef(env,js);return -1;}
    (*env)->DeleteLocalRef(env,js);
    return (int)r;
}
static int jcall_int_int(JNIEnv*env,jmethodID m,int a){
    if(!env||!G.natives||!m) return -1;
    jint r=(*env)->CallStaticIntMethod(env,G.natives,m,(jint)a);
    if((*env)->ExceptionCheck(env)){(*env)->ExceptionClear(env);return -1;}
    return (int)r;
}

static void load_all_textures(JNIEnv*env){
    if(G.tex_epoch==G.gl_epoch && G.tex_loaded>0) return;
    G.tex_loaded=0;
    for(int i=0;i<TEX_COUNT;i++){
        int idx=jcall_int_str(env,G.mLoadTexture,TEX_LIST[i]);
        G.tex_index[i]=idx;
        int glid=(idx>=0)?jcall_int_int(env,G.mGetTextureID,idx):-1;
        G.tex_glid[i]=glid;
        GLboolean istex=(glid>=0)?glIsTexture((GLuint)glid):GL_FALSE;
        if(idx>=0) G.tex_loaded++;
        LOGI("tex[%d] '%s' -> indice=%d glId=%d glIsTexture=%d err=%s",
             i,TEX_LIST[i],idx,glid,(int)istex,gl_err(glGetError()));
    }
    G.tex_epoch=G.gl_epoch;
    LOGI("=== Texturas (epoca %ld): %d/%d ; shell glId=%d ===",
         G.gl_epoch,G.tex_loaded,TEX_COUNT,G.tex_glid[TEX_SHELL]);
}

static void on_context_created(void){
    G.gl_epoch++; G.tex_loaded=0;
    for(int i=0;i<TEX_COUNT;i++){ G.tex_index[i]=-1; G.tex_glid[i]=-1; }
    const GLubyte*r=glGetString(GL_RENDERER);
    const GLubyte*v=glGetString(GL_VERSION);
    /* regista tambem o viewport atual, so para diagnostico */
    GLint vp[4]={0,0,0,0}; glGetIntegerv(GL_VIEWPORT,vp);
    LOGI("Contexto GL (re)criado epoca=%ld | %s | %s | viewport=%d,%d,%d,%d",
         G.gl_epoch, r?(const char*)r:"?", v?(const char*)v:"?", vp[0],vp[1],vp[2],vp[3]);
}

/* Desenha uma textura por nome GL, em ECRA INTEIRO, usando NDC.
 * Independente de base_w/base_h e do tamanho do viewport. */
static void draw_fullscreen_tex(int glid){
    glMatrixMode(GL_PROJECTION); glLoadIdentity();   /* identidade => clip = NDC */
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    glColor4f(1,1,1,1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D,(GLuint)glid);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);

    /* quad em NDC: (-1,-1)..(1,1). V invertido para a imagem nao ficar de pernas p'o ar.
     * (topo do ecra = y=+1 ; topo da textura = v=0) */
    GLfloat v[] = { -1.0f,-1.0f,   1.0f,-1.0f,   -1.0f, 1.0f,   1.0f, 1.0f };
    GLfloat t[] = {  0.0f, 1.0f,   1.0f, 1.0f,    0.0f, 0.0f,   1.0f, 0.0f };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2,GL_FLOAT,0,v);
    glTexCoordPointer(2,GL_FLOAT,0,t);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

/* ================================ JNI_OnLoad =============================== */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*vm,void*reserved){
    (void)reserved; memset(&G,0,sizeof(G));
    for(int i=0;i<TEX_COUNT;i++){G.tex_index[i]=-1;G.tex_glid[i]=-1;}
    G.gl_epoch=0; G.tex_epoch=-1; G.vm=vm;
    JNIEnv*env=NULL;
    if((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK||!env){LOGE("GetEnv falhou");return JNI_ERR;}
    jclass local=(*env)->FindClass(env,NATIVES_CLASS);
    if(local){
        G.natives=(jclass)(*env)->NewGlobalRef(env,local);
        G.mLoadTexture =(*env)->GetStaticMethodID(env,local,"OnLoadTexture","(Ljava/lang/String;)I");
        G.mGetTextureID=(*env)->GetStaticMethodID(env,local,"OnGetTextureID","(I)I");
        G.mGetTextureW =(*env)->GetStaticMethodID(env,local,"OnGetTextureWidth","(I)I");
        G.mGetTextureH =(*env)->GetStaticMethodID(env,local,"OnGetTextureHeight","(I)I");
        (*env)->ExceptionClear(env);
    } else {(*env)->ExceptionClear(env);LOGE("classe %s nao encontrada",NATIVES_CLASS);}
    LOGI("Motor ARM64 v0.54 carregado. NDC fullscreen quad.");
    return JNI_VERSION_1_6;
}

#define J(name) Java_com_namcobandaigames_tamagotchilife_SampleGameNatives_##name

JNIEXPORT void JNICALL J(init)(JNIEnv*env,jclass c,jint w,jint h){
    (void)c;
    G.base_w=(w>0)?w:480; G.base_h=(h>0)?h:800; G.real_w=G.base_w; G.real_h=G.base_h;
    G.hunger=80; G.happy=80; G.sick=0; recompute_calling();
    on_context_created();
    load_all_textures(env);
    LOGI("init(%d,%d) tex=%d/%d",w,h,G.tex_loaded,TEX_COUNT);
}

JNIEXPORT void JNICALL J(main)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("main()");}

JNIEXPORT void JNICALL J(step)(JNIEnv*env,jclass c){
    (void)c;
    if(G.tex_epoch!=G.gl_epoch && G.mLoadTexture) load_all_textures(env);
    G.ticks++;
    if((G.ticks%30)==0){ G.hunger=clampi(G.hunger-1,0,100); G.happy=clampi(G.happy-1,0,100); recompute_calling(); }
    if(G.tex_glid[TEX_SHELL] >= 0 && glIsTexture((GLuint)G.tex_glid[TEX_SHELL]))
        draw_fullscreen_tex(G.tex_glid[TEX_SHELL]);
}

JNIEXPORT void JNICALL J(stop)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("stop()");}
JNIEXPORT void JNICALL J(GameTerm)(JNIEnv*env,jclass c){
    (void)c; if(G.natives&&env){(*env)->DeleteGlobalRef(env,G.natives);G.natives=NULL;} LOGI("GameTerm()");
}
JNIEXPORT jint JNICALL J(GameGetPart)(JNIEnv*env,jclass c){(void)env;(void)c;return 0;}
JNIEXPORT void JNICALL J(GameSetAppRequest)(JNIEnv*env,jclass c,jint req){
    switch(req){case 1:G.hunger=clampi(G.hunger+30,0,100);break;
    case 2:G.happy=clampi(G.happy+30,0,100);break;case 3:G.sick=0;break;
    case 0:J(init)(env,c,G.base_w,G.base_h);break;default:break;}
    recompute_calling();
}
JNIEXPORT void JNICALL J(GameInputSetTouches)(JNIEnv*env,jclass c,jint id,jfloat x,jfloat y,jfloat a,jfloat b){
    (void)env;(void)c;(void)id;(void)a;(void)b; G.touch_active=1;G.touch_x=x;G.touch_y=y;
    if(G.hunger<=G.happy)G.hunger=clampi(G.hunger+20,0,100);else G.happy=clampi(G.happy+20,0,100);
    recompute_calling();
}
JNIEXPORT void JNICALL J(GameInputReleaseTouches)(JNIEnv*env,jclass c,jint id,jfloat x,jfloat y){
    (void)env;(void)c;(void)id;(void)x;(void)y; G.touch_active=0;
}
JNIEXPORT jint JNICALL J(GetBaseScreenWidth)(JNIEnv*env,jclass c){(void)env;(void)c;return G.base_w>0?G.base_w:480;}
JNIEXPORT jint JNICALL J(GetBaseScreenHeight)(JNIEnv*env,jclass c){(void)env;(void)c;return G.base_h>0?G.base_h:800;}
JNIEXPORT void JNICALL J(ThreadCreate)(JNIEnv*env,jclass c,jint a,jint b){(void)env;(void)c;(void)a;(void)b;LOGI("ThreadCreate");}
JNIEXPORT void JNICALL J(TMGCmakeAlarmInfoData)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("AlarmInfo");}
