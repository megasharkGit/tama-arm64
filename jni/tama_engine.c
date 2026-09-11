/* ============================================================================
 *  Tamagotchi L.i.f.e. — Motor nativo ARM64 (Via 2)
 *  v0.58 — CAUSA RAIZ RESOLVIDA: amostrar so o conteudo da textura (UV crop)
 *  ----------------------------------------------------------------------------
 *  DESCOBERTA (analise pixel-a-pixel dos .tgd):
 *   - Os .tgd sao PNG power-of-two, mas o desenho util esta no CANTO SUPERIOR
 *     ESQUERDO, com o resto TRANSPARENTE (padding). Ex.: body_00000.tgd e
 *     1024x1024 mas a shell so ocupa 595x732 => 0.581 x 0.715.
 *   - Por isso, ao mapear a textura inteira (UV 0..1) para o ecra, a shell
 *     aparecia sempre num "quadrante" (canto sup-esq), com transparencia a
 *     volta. NAO era o viewport — era o UV. Isto explica TODOS os sintomas
 *     (v0.53: "1/2 largura x 2/3 altura" == 595/1024 x 732/1024).
 *
 *  CORRECAO: desenhar so o sub-retangulo de conteudo (UV 0..u1, 0..v1) esticado
 *  para um quad de ecra inteiro (NDC). Tabela de UV medida por textura; para a
 *  shell u1=595/1024, v1=732/1024. Tambem tentamos OnGetTextureWidth/Height e
 *  logamos, para automatizar nas proximas versoes.
 *
 *  Viewport: forcado para o tamanho REAL da superficie (EGL), que enche o ecra.
 *  Android.mk: LOCAL_LDLIBS := -llog -lGLESv1_CM -lEGL -lm
 * ==========================================================================*/
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <android/log.h>
#include <GLES/gl.h>
#include <EGL/egl.h>

#define LOG_TAG "TamaEngineARM64"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define NATIVES_CLASS "com/namcobandaigames/tamagotchilife/SampleGameNatives"

/* Tabela: nome + UV de conteudo (medido: contentW/PNGw, contentH/PNGh) */
typedef struct { const char*name; float u1, v1; } TexDef;
static const TexDef TEX_LIST[] = {
    { "body_00000.tgd",           0.5811f, 0.7148f },  /* 0: shell -> desenhada  */
    { "Tama2Movie_texture.tgd",   0.9258f, 0.3711f },
    { "Tama2Movie_texture_c.tgd", 1.0000f, 0.9834f },
    { "seg_00000.tgd",            0.6445f, 0.6094f },
    { "seg_icon_00000.tgd",       0.9629f, 0.9297f },
    { "font.tgd",                 0.9883f, 0.3750f },
    { "number_texture.tgd",       1.0000f, 1.0000f },
};
#define TEX_COUNT ((int)(sizeof(TEX_LIST)/sizeof(TEX_LIST[0])))
#define TEX_SHELL 0

typedef struct {
    JavaVM   *vm;
    jclass    natives;
    jmethodID mLoadTexture, mGetTextureID, mGetTextureW, mGetTextureH;

    int  init_w, init_h;

    int  tex_index[TEX_COUNT];
    int  tex_glid[TEX_COUNT];
    int  tex_loaded;
    long gl_epoch, tex_epoch;

    int  surf_w, surf_h;

    int  hunger, happy, sick, calling;
    long ticks;
    int   touch_active; float touch_x, touch_y;
} TamaState;

static TamaState G;

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (double)t.tv_sec*1000.0 + (double)t.tv_nsec/1.0e6; }
static int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
static void recompute_calling(void){ G.calling=(G.hunger<=25||G.happy<=25||G.sick)?1:0; }

static int egl_size(int*w,int*h){
    EGLDisplay dpy=eglGetCurrentDisplay(); EGLSurface s=eglGetCurrentSurface(EGL_DRAW);
    if(dpy==EGL_NO_DISPLAY||s==EGL_NO_SURFACE) return 0;
    EGLint ew=0,eh=0;
    if(!eglQuerySurface(dpy,s,EGL_WIDTH,&ew)) return 0;
    if(!eglQuerySurface(dpy,s,EGL_HEIGHT,&eh)) return 0;
    if(ew<=0||eh<=0) return 0; *w=ew;*h=eh; return 1;
}

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
    (*env)->DeleteLocalRef(env,js); return (int)r;
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
        int idx=jcall_int_str(env,G.mLoadTexture,TEX_LIST[i].name);
        G.tex_index[i]=idx;
        int glid=(idx>=0)?jcall_int_int(env,G.mGetTextureID,idx):-1;
        int rw =(idx>=0)?jcall_int_int(env,G.mGetTextureW,idx):-1;
        int rh =(idx>=0)?jcall_int_int(env,G.mGetTextureH,idx):-1;
        G.tex_glid[i]=glid;
        GLboolean istex=(glid>=0)?glIsTexture((GLuint)glid):GL_FALSE;
        if(idx>=0) G.tex_loaded++;
        LOGI("tex[%d] '%s' idx=%d glId=%d isTex=%d OnGetW/H=%dx%d UVtab=%.3f,%.3f err=%s",
             i,TEX_LIST[i].name,idx,glid,(int)istex,rw,rh,TEX_LIST[i].u1,TEX_LIST[i].v1,gl_err(glGetError()));
    }
    G.tex_epoch=G.gl_epoch;
    LOGI("=== Texturas (epoca %ld): %d/%d ; shell glId=%d ===",
         G.gl_epoch,G.tex_loaded,TEX_COUNT,G.tex_glid[TEX_SHELL]);
}
static void on_context_created(void){
    G.gl_epoch++; G.tex_loaded=0;
    for(int i=0;i<TEX_COUNT;i++){ G.tex_index[i]=-1; G.tex_glid[i]=-1; }
    const GLubyte*r=glGetString(GL_RENDERER);
    int ew=0,eh=0,ok=egl_size(&ew,&eh); if(ok){G.surf_w=ew;G.surf_h=eh;}
    LOGI("Contexto GL epoca=%ld | %s | EGL_surface=%dx%d(ok=%d)",
         G.gl_epoch, r?(const char*)r:"?", ew,eh,ok);
}

/* Desenha o sub-retangulo (0..u1,0..v1) da textura esticado a ECRA INTEIRO. */
static void draw_tex_content(int glid, float u1, float v1){
    int w=0,h=0;
    if(egl_size(&w,&h)){ G.surf_w=w; G.surf_h=h; glViewport(0,0,w,h); }
    else if(G.surf_w>0){ glViewport(0,0,G.surf_w,G.surf_h); }

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    glColor4f(1,1,1,1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D,(GLuint)glid);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);

    /* quad NDC ecra inteiro; UV so o conteudo, V invertido (topo textura = v=0) */
    GLfloat vtx[] = { -1.0f,-1.0f,   1.0f,-1.0f,   -1.0f, 1.0f,   1.0f, 1.0f };
    GLfloat uv[]  = {  0.0f, v1,     u1,  v1,       0.0f, 0.0f,   u1,  0.0f };

    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2,GL_FLOAT,0,vtx);
    glTexCoordPointer(2,GL_FLOAT,0,uv);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
    glDisable(GL_TEXTURE_2D);
}

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
    LOGI("Motor ARM64 v0.58 carregado. UV-crop do conteudo da textura.");
    return JNI_VERSION_1_6;
}

#define J(name) Java_com_namcobandaigames_tamagotchilife_SampleGameNatives_##name

JNIEXPORT void JNICALL J(init)(JNIEnv*env,jclass c,jint w,jint h){
    (void)c;
    G.init_w=(int)w; G.init_h=(int)h;
    G.hunger=80; G.happy=80; G.sick=0; recompute_calling();
    on_context_created();
    load_all_textures(env);
    LOGI("init(%d,%d) tex=%d/%d surf=%dx%d",w,h,G.tex_loaded,TEX_COUNT,G.surf_w,G.surf_h);
}

JNIEXPORT void JNICALL J(main)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("main()");}

JNIEXPORT void JNICALL J(step)(JNIEnv*env,jclass c){
    (void)c;
    if(G.tex_epoch!=G.gl_epoch && G.mLoadTexture) load_all_textures(env);
    G.ticks++;
    if((G.ticks%30)==0){ G.hunger=clampi(G.hunger-1,0,100); G.happy=clampi(G.happy-1,0,100); recompute_calling(); }
    if(G.tex_glid[TEX_SHELL] >= 0 && glIsTexture((GLuint)G.tex_glid[TEX_SHELL]))
        draw_tex_content(G.tex_glid[TEX_SHELL], TEX_LIST[TEX_SHELL].u1, TEX_LIST[TEX_SHELL].v1);
}

JNIEXPORT void JNICALL J(stop)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("stop()");}
JNIEXPORT void JNICALL J(GameTerm)(JNIEnv*env,jclass c){
    (void)c; if(G.natives&&env){(*env)->DeleteGlobalRef(env,G.natives);G.natives=NULL;} LOGI("GameTerm()");
}
JNIEXPORT jint JNICALL J(GameGetPart)(JNIEnv*env,jclass c){(void)env;(void)c;return 0;}
JNIEXPORT void JNICALL J(GameSetAppRequest)(JNIEnv*env,jclass c,jint req){
    switch(req){case 1:G.hunger=clampi(G.hunger+30,0,100);break;
    case 2:G.happy=clampi(G.happy+30,0,100);break;case 3:G.sick=0;break;
    case 0:J(init)(env,c,G.init_w,G.init_h);break;default:break;}
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
JNIEXPORT jint JNICALL J(GetBaseScreenWidth)(JNIEnv*env,jclass c){(void)env;(void)c;return 480;}
JNIEXPORT jint JNICALL J(GetBaseScreenHeight)(JNIEnv*env,jclass c){(void)env;(void)c;return 800;}
JNIEXPORT void JNICALL J(ThreadCreate)(JNIEnv*env,jclass c,jint a,jint b){(void)env;(void)c;(void)a;(void)b;LOGI("ThreadCreate");}
JNIEXPORT void JNICALL J(TMGCmakeAlarmInfoData)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("AlarmInfo");}
/* ============================================================================
 *  Tamagotchi L.i.f.e. — Motor nativo ARM64 (Via 2)
 *  v0.57 — VIEWPORT ROBUSTO: preencher o ecra inteiro sem adivinhar
 *  ----------------------------------------------------------------------------
 *  Factos confirmados por engenharia inversa do renderer (classe ao):
 *   - O Java NUNCA chama glViewport. O viewport fica no valor por omissao = o
 *     tamanho real do framebuffer (o que queremos).
 *   - init(mJ,mH) recebe o tamanho da superficie; GetBaseScreenWidth/Height=480/800.
 *
 *  Historico de sintomas:
 *   - v0.53 (glOrtho 480x800):      shell ~1/2 x ~2/3, canto sup-esq.
 *   - v0.54 (NDC, sem forcar vp):   shell ~1/2 x ~2/3.
 *   - v0.55/56 (forcar vp=EGL):     shell exatamente 1/4 (sup-esq) -> EGL devolveu
 *                                   ~metade da resolucao real.
 *
 *  Estrategia v0.57: medir QUATRO candidatos de tamanho e forcar glViewport para
 *  o de MAIOR area (o framebuffer real), sem depender de nenhuma fonte isolada:
 *     (1) viewport por omissao lido em on_context_created (glGetIntegerv)
 *     (2) EGL surface (eglQuerySurface)
 *     (3) init(w,h)
 *     (4) 480x800 (base, ultimo recurso)
 *  Loga os quatro para diagnostico. Um quad NDC preenche esse viewport.
 *
 *  Android.mk: LOCAL_LDLIBS := -llog -lGLESv1_CM -lEGL -lm
 * ==========================================================================*/
#include <jni.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <android/log.h>
#include <GLES/gl.h>
#include <EGL/egl.h>

#define LOG_TAG "TamaEngineARM64"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define NATIVES_CLASS "com/namcobandaigames/tamagotchilife/SampleGameNatives"

static const char *const TEX_LIST[] = {
    "body_00000.tgd",
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

    int init_w, init_h;      /* tamanho passado ao init() pelo Java */

    int  tex_index[TEX_COUNT];
    int  tex_glid[TEX_COUNT];
    int  tex_loaded;
    long gl_epoch, tex_epoch;

    int  fb_w, fb_h;         /* framebuffer real escolhido (maior area) */

    int  hunger, happy, sick, calling;
    long ticks;
    int   touch_active; float touch_x, touch_y;
} TamaState;

static TamaState G;

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (double)t.tv_sec*1000.0 + (double)t.tv_nsec/1.0e6; }
static int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
static void recompute_calling(void){ G.calling=(G.hunger<=25||G.happy<=25||G.sick)?1:0; }

static int egl_size(int*w,int*h){
    EGLDisplay dpy=eglGetCurrentDisplay(); EGLSurface s=eglGetCurrentSurface(EGL_DRAW);
    if(dpy==EGL_NO_DISPLAY||s==EGL_NO_SURFACE) return 0;
    EGLint ew=0,eh=0;
    if(!eglQuerySurface(dpy,s,EGL_WIDTH,&ew)) return 0;
    if(!eglQuerySurface(dpy,s,EGL_HEIGHT,&eh)) return 0;
    if(ew<=0||eh<=0) return 0; *w=ew;*h=eh; return 1;
}

/* Escolhe o framebuffer real = candidato de MAIOR area. Loga todos. */
static void choose_framebuffer(const char*ctx){
    GLint vp[4]={0,0,0,0}; glGetIntegerv(GL_VIEWPORT,vp);
    int ew=0,eh=0; int okegl=egl_size(&ew,&eh);
    int cand_w[4]={vp[2], okegl?ew:0, G.init_w, 480};
    int cand_h[4]={vp[3], okegl?eh:0, G.init_h, 800};
    const char*names[4]={"gl_default_vp","egl_surface","init","base480"};
    long best=0; int bw=480,bh=800;
    for(int i=0;i<4;i++){
        long area=(long)cand_w[i]*(long)cand_h[i];
        if(cand_w[i]>0 && cand_h[i]>0 && area>best){ best=area; bw=cand_w[i]; bh=cand_h[i]; }
    }
    G.fb_w=bw; G.fb_h=bh;
    LOGI("[%s] candidatos: gl_vp=%dx%d egl=%dx%d(ok=%d) init=%dx%d base=480x800 -> ESCOLHIDO %dx%d",
         ctx, vp[2],vp[3], ew,eh,okegl, G.init_w,G.init_h, bw,bh);
    for(int i=0;i<4;i++) LOGI("   cand[%s]=%dx%d",names[i],cand_w[i],cand_h[i]);
}

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
    (*env)->DeleteLocalRef(env,js); return (int)r;
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
    LOGI("Contexto GL epoca=%ld | %s", G.gl_epoch, r?(const char*)r:"?");
    choose_framebuffer("on_context_created");
}

static void draw_fullscreen_tex(int glid){
    /* re-mede a cada frame (o framebuffer pode ter mudado apos o 1o frame) */
    choose_framebuffer("draw");
    glViewport(0,0,G.fb_w,G.fb_h);

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();

    glColor4f(1,1,1,1);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D,(GLuint)glid);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);

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

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*vm,void*reserved){
    (void)reserved; memset(&G,0,sizeof(G));
    for(int i=0;i<TEX_COUNT;i++){G.tex_index[i]=-1;G.tex_glid[i]=-1;}
    G.gl_epoch=0; G.tex_epoch=-1; G.init_w=0; G.init_h=0; G.vm=vm;
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
    LOGI("Motor ARM64 v0.57 carregado. Viewport robusto (maior candidato).");
    return JNI_VERSION_1_6;
}

#define J(name) Java_com_namcobandaigames_tamagotchilife_SampleGameNatives_##name

JNIEXPORT void JNICALL J(init)(JNIEnv*env,jclass c,jint w,jint h){
    (void)c;
    G.init_w=(int)w; G.init_h=(int)h;
    G.hunger=80; G.happy=80; G.sick=0; recompute_calling();
    on_context_created();
    load_all_textures(env);
    LOGI("init(%d,%d) tex=%d/%d fb=%dx%d",w,h,G.tex_loaded,TEX_COUNT,G.fb_w,G.fb_h);
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
    case 0:J(init)(env,c,G.init_w,G.init_h);break;default:break;}
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
JNIEXPORT jint JNICALL J(GetBaseScreenWidth)(JNIEnv*env,jclass c){(void)env;(void)c;return 480;}
JNIEXPORT jint JNICALL J(GetBaseScreenHeight)(JNIEnv*env,jclass c){(void)env;(void)c;return 800;}
JNIEXPORT void JNICALL J(ThreadCreate)(JNIEnv*env,jclass c,jint a,jint b){(void)env;(void)c;(void)a;(void)b;LOGI("ThreadCreate");}
JNIEXPORT void JNICALL J(TMGCmakeAlarmInfoData)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("AlarmInfo");}
