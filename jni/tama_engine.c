/* ============================================================================
 *  Tamagotchi L.i.f.e. — Motor nativo ARM64 (Via 2)
 *  v0.53 — DESENHAR A TEXTURA REAL (shell do Tamagotchi)
 *  ----------------------------------------------------------------------------
 *  Factos confirmados por engenharia inversa do classes.dex (v0.52):
 *   - Os .tgd sao PNG normais RGBA8888 (ex.: body_00000 = 1024x1024).
 *   - OnLoadTexture(String) devolve um INDICE (nao o nome GL). O upload real e:
 *        BitmapFactory.decodeStream -> GLUtils.texImage2D  (RGBA8888, correto).
 *   - Para desenhar: resolver OnGetTextureID(indice) -> NOME GL, e so entao
 *        glBindTexture(GL_TEXTURE_2D, nome).
 *   - Ecra magenta na v0.52 = as 7 texturas carregaram (GameGetPart=1). Faltava
 *        apenas DESENHA-LAS. E o que esta versao faz.
 *
 *  O que a v0.53 mostra: a SHELL real do Tamagotchi (body_00000.tgd) desenhada
 *  em ecra inteiro, com a cor natural (sem modulacao). Prova o pipeline completo
 *  .tgd(PNG) -> Bitmap -> GL -> quad na tela, no motor ARM64.
 *
 *  Correcoes de robustez mantidas da v0.52:
 *   - Recarga de texturas a cada (re)criacao de contexto GL (perda ao adormecer).
 *   - Diagnostico via logcat (id, glIsTexture, glGetError, GL_VENDOR/RENDERER).
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

/* Texturas a carregar. Indice 0 = a shell que vamos desenhar em ecra inteiro. */
static const char *const TEX_LIST[] = {
    "body_00000.tgd",           /* 0: shell azul (1024x1024) -> desenhada       */
    "Tama2Movie_texture.tgd",   /* 1: atlas do bicho (512x512)                  */
    "Tama2Movie_texture_c.tgd", /* 2: atlas a cores                             */
    "seg_00000.tgd",            /* 3: segmentos LCD                             */
    "seg_icon_00000.tgd",       /* 4: icones de cuidado                         */
    "font.tgd",                 /* 5: fonte                                     */
    "number_texture.tgd",       /* 6: digitos                                   */
};
#define TEX_COUNT ((int)(sizeof(TEX_LIST)/sizeof(TEX_LIST[0])))
#define TEX_SHELL 0             /* indice da textura desenhada em ecra inteiro  */

typedef struct {
    JavaVM   *vm;
    jclass    natives;
    jmethodID mLoadTexture;     /* (Ljava/lang/String;)I -> indice   */
    jmethodID mGetTextureID;    /* (I)I -> nome GL                    */
    jmethodID mGetTextureW;     /* (I)I                              */
    jmethodID mGetTextureH;     /* (I)I                              */

    int base_w, base_h;
    int real_w, real_h;

    int  tex_index[TEX_COUNT];  /* indice devolvido por OnLoadTexture */
    int  tex_glid[TEX_COUNT];   /* nome GL resolvido por OnGetTextureID */
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
    LOGI("Contexto GL (re)criado epoca=%ld | %s | %s",
         G.gl_epoch, r?(const char*)r:"?", v?(const char*)v:"?");
}

/* Desenha uma textura (por nome GL) num quad de ecra inteiro. */
static void draw_fullscreen_tex(int glid){
    int W=G.base_w>0?G.base_w:480, H=G.base_h>0?G.base_h:800;

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glOrthof(0.0f,(GLfloat)W,(GLfloat)H,0.0f,-1.0f,1.0f);   /* origem no topo-esq */
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    glColor4f(1,1,1,1);                    /* cor natural, sem modulacao */
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D,(GLuint)glid);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);

    GLfloat v[]={ 0,0,  (GLfloat)W,0,  0,(GLfloat)H,  (GLfloat)W,(GLfloat)H };
    GLfloat t[]={ 0,0,  1,0,          0,1,           1,1 };
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
    LOGI("Motor ARM64 v0.53 carregado. OnLoadTexture=%s OnGetTextureID=%s.",
         G.mLoadTexture?"OK":"-", G.mGetTextureID?"OK":"-");
    return JNI_VERSION_1_6;
}

#define J(name) Java_com_namcobandaigames_tamagotchilife_SampleGameNatives_##name

JNIEXPORT void JNICALL J(init)(JNIEnv*env,jclass c,jint w,jint h){
    (void)c;
    G.base_w=(w>0)?w:480; G.base_h=(h>0)?h:800; G.real_w=G.base_w; G.real_h=G.base_h;
    G.hunger=80; G.happy=80; G.sick=0; recompute_calling();
    on_context_created();     /* contexto GL novo (onSurfaceCreated) */
    load_all_textures(env);
    LOGI("init(%d,%d) base=%dx%d tex=%d/%d",w,h,G.base_w,G.base_h,G.tex_loaded,TEX_COUNT);
}

JNIEXPORT void JNICALL J(main)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("main()");}

JNIEXPORT void JNICALL J(step)(JNIEnv*env,jclass c){
    (void)c;
    /* recarrega se o contexto foi recriado sem passar por init */
    if(G.tex_epoch!=G.gl_epoch && G.mLoadTexture) load_all_textures(env);

    G.ticks++;
    if((G.ticks%30)==0){ G.hunger=clampi(G.hunger-1,0,100); G.happy=clampi(G.happy-1,0,100); recompute_calling(); }

    /* >>> DESENHA A SHELL REAL, se ja foi carregada e resolvida <<< */
    if(G.tex_glid[TEX_SHELL] >= 0 && glIsTexture((GLuint)G.tex_glid[TEX_SHELL]))
        draw_fullscreen_tex(G.tex_glid[TEX_SHELL]);
}

JNIEXPORT void JNICALL J(stop)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("stop()");}
JNIEXPORT void JNICALL J(GameTerm)(JNIEnv*env,jclass c){
    (void)c; if(G.natives&&env){(*env)->DeleteGlobalRef(env,G.natives);G.natives=NULL;} LOGI("GameTerm()");
}

/* Fundo: 0 = preto/vermelho estavel; a shell tapa o ecra por cima. */
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
