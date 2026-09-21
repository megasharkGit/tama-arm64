/* ============================================================================
 *  Tamagotchi L.i.f.e. — Motor nativo ARM64 (Via 2)
 *  v0.60 — O BICHO VIVO NO LCD  🐣
 *  ----------------------------------------------------------------------------
 *  Conquistas anteriores: shell real desenhada, UV-crop do conteudo (v0.58) e
 *  aspect-fit na proporcao 595:732 (v0.59).
 *
 *  NOVO nesta versao (dados extraidos por engenharia inversa dos assets):
 *   - Tama2Movie_rect.bin DESCODIFICADO: big-endian, u32 count=246, depois 246
 *     registos de 32 bytes = 8 x int32: (srcX, srcY, w, h, pivotX, pivotY, 0, 0)
 *     -> 225 sprites validos no atlas Tama2Movie_texture.tgd (512x512).
 *     Bichos identificados: #87-90 Babytchi, #95-104 Marutchi, #105+ evolucoes.
 *   - Janela LCD da shell MEDIDA: x=144..446, y=245..527 em 595x732
 *     => u=[0.2420,0.7513]  v=[0.3347,0.7213]
 *
 *  Ordem de desenho (a shell tem a janela TRANSPARENTE, por isso vai por cima):
 *     1) limpa o ecra a preto  (corrige as faixas azuis das margens)
 *     2) fundo do LCD (verde classico) na janela
 *     3) o BICHO (sprite 16x16 do atlas), animado entre 2 frames ~1 Hz
 *     4) a shell por cima, com alpha-blending
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

/* ---------------- texturas ---------------- */
#define TEX_SHELL 0
#define TEX_ATLAS 1
typedef struct { const char*name; float u1, v1; } TexDef;
static const TexDef TEX_LIST[] = {
    { "body_00000.tgd",         0.5811f, 0.7148f },  /* shell: 595x732 de 1024   */
    { "Tama2Movie_texture.tgd", 0.9258f, 0.3711f },  /* atlas: 474x190 de 512    */
};
#define TEX_COUNT ((int)(sizeof(TEX_LIST)/sizeof(TEX_LIST[0])))

/* shell: dimensoes do conteudo e proporcao */
#define SHELL_W 595.0f
#define SHELL_H 732.0f
#define SHELL_ASPECT (SHELL_W/SHELL_H)     /* 0.8128 */

/* janela LCD, em fracao do conteudo da shell (medida) */
#define LCD_U0 0.2420f
#define LCD_U1 0.7513f
#define LCD_V0 0.3347f
#define LCD_V1 0.7213f

/* atlas */
#define ATLAS_PX 512.0f

/* sprites do bicho (Marutchi): 2 frames de idle, 16x16 no atlas */
typedef struct { int x,y,w,h; } Rect;
static const Rect CREATURE[2] = {
    {  16, 16, 16, 16 },   /* #96 Marutchi, frame A (verificado no rect.bin) */
    {  32, 16, 16, 16 },   /* #97 Marutchi, frame B                          */
};

typedef struct {
    JavaVM   *vm;
    jclass    natives;
    jmethodID mLoadTexture, mGetTextureID;

    int  tex_glid[TEX_COUNT];
    int  tex_loaded;
    long gl_epoch, tex_epoch;
    int  surf_w, surf_h;

    int  hunger, happy, sick, calling;
    long ticks;
    double t0;
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
        int glid=(idx>=0)?jcall_int_int(env,G.mGetTextureID,idx):-1;
        G.tex_glid[i]=glid;
        if(glid>=0) G.tex_loaded++;
        LOGI("tex[%d] '%s' idx=%d glId=%d", i, TEX_LIST[i].name, idx, glid);
    }
    G.tex_epoch=G.gl_epoch;
    LOGI("=== Texturas %d/%d | shell=%d atlas=%d ===",
         G.tex_loaded,TEX_COUNT,G.tex_glid[TEX_SHELL],G.tex_glid[TEX_ATLAS]);
}
static void on_context_created(void){
    G.gl_epoch++; G.tex_loaded=0;
    for(int i=0;i<TEX_COUNT;i++) G.tex_glid[i]=-1;
    int ew=0,eh=0,ok=egl_size(&ew,&eh); if(ok){G.surf_w=ew;G.surf_h=eh;}
    LOGI("Contexto GL epoca=%ld | EGL_surface=%dx%d(ok=%d)",G.gl_epoch,ew,eh,ok);
}

/* --------- helpers de desenho --------- */
static void quad_tex(float x0,float y0,float x1,float y1,
                     float u0,float v0,float u1,float v1){
    GLfloat vtx[]={ x0,y0,  x1,y0,  x0,y1,  x1,y1 };
    GLfloat uv []={ u0,v1,  u1,v1,  u0,v0,  u1,v0 };
    glEnableClientState(GL_VERTEX_ARRAY);
    glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2,GL_FLOAT,0,vtx);
    glTexCoordPointer(2,GL_FLOAT,0,uv);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);
}
static void quad_color(float x0,float y0,float x1,float y1,
                       float r,float g,float b,float a){
    glDisable(GL_TEXTURE_2D);
    glColor4f(r,g,b,a);
    GLfloat vtx[]={ x0,y0, x1,y0, x0,y1, x1,y1 };
    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(2,GL_FLOAT,0,vtx);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glDisableClientState(GL_VERTEX_ARRAY);
    glColor4f(1,1,1,1);
}

/* converte fracao UV da shell -> NDC, dado o rect da shell em NDC */
static float sx_of(float u,float sx){ return sx*(2.0f*u-1.0f); }
static float sy_of(float v,float sy){ return sy*(1.0f-2.0f*v); }

static void render(void){
    int w=0,h=0;
    if(egl_size(&w,&h)){ G.surf_w=w; G.surf_h=h; }
    else { w=G.surf_w; h=G.surf_h; }
    if(w<=0||h<=0){ w=480; h=800; }
    glViewport(0,0,w,h);

    /* 1) fundo preto (substitui as faixas azuis) */
    glClearColor(0.0f,0.0f,0.0f,1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    /* aspect-fit da shell */
    float scr=(float)w/(float)h, sx=1.0f, sy=1.0f;
    if(scr > SHELL_ASPECT){ sx = SHELL_ASPECT/scr; sy = 1.0f; }
    else                  { sx = 1.0f; sy = scr/SHELL_ASPECT; }

    /* janela LCD em NDC */
    float lx0=sx_of(LCD_U0,sx), lx1=sx_of(LCD_U1,sx);
    float ly0=sy_of(LCD_V0,sy), ly1=sy_of(LCD_V1,sy);  /* ly0=topo, ly1=fundo */

    /* 2) fundo do LCD (verde classico) */
    quad_color(lx0,ly1,lx1,ly0, 0.70f,0.78f,0.55f,1.0f);

    /* 3) o BICHO, centrado no LCD, quadrado em pixeis de ecra */
    if(G.tex_glid[TEX_ATLAS]>=0 && glIsTexture((GLuint)G.tex_glid[TEX_ATLAS])){
        int frame = (int)((now_ms()-G.t0)/700.0) & 1;      /* ~1.4 Hz */
        const Rect*R=&CREATURE[frame];

        /* lado do bicho = 42% da largura da janela, em pixeis da shell */
        float side_px = 0.50f*(LCD_U1-LCD_U0)*SHELL_W;     /* ~152 px */
        float du = side_px/SHELL_W;                        /* fracao horizontal */
        float dv = side_px/SHELL_H;                        /* mesma medida vertical */
        float ucx=(LCD_U0+LCD_U1)*0.5f, vcy=(LCD_V0+LCD_V1)*0.5f;

        float cx0=sx_of(ucx-du*0.5f,sx), cx1=sx_of(ucx+du*0.5f,sx);
        float cy0=sy_of(vcy-dv*0.5f,sy), cy1=sy_of(vcy+dv*0.5f,sy);

        float au0=R->x/ATLAS_PX,  au1=(R->x+R->w)/ATLAS_PX;
        float av0=R->y/ATLAS_PX,  av1=(R->y+R->h)/ATLAS_PX;

        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D,(GLuint)G.tex_glid[TEX_ATLAS]);
        /* NEAREST: pixel-art nitida, como o LCD original */
        glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glColor4f(1,1,1,1);
        quad_tex(cx0,cy1,cx1,cy0, au0,av0,au1,av1);
    }

    /* 4) a shell POR CIMA (janela transparente deixa ver o LCD) */
    if(G.tex_glid[TEX_SHELL]>=0 && glIsTexture((GLuint)G.tex_glid[TEX_SHELL])){
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D,(GLuint)G.tex_glid[TEX_SHELL]);
        glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glColor4f(1,1,1,1);
        quad_tex(-sx,-sy,sx,sy, 0.0f,0.0f,TEX_LIST[TEX_SHELL].u1,TEX_LIST[TEX_SHELL].v1);
    }
    glDisable(GL_TEXTURE_2D);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*vm,void*reserved){
    (void)reserved; memset(&G,0,sizeof(G));
    for(int i=0;i<TEX_COUNT;i++) G.tex_glid[i]=-1;
    G.gl_epoch=0; G.tex_epoch=-1; G.vm=vm; G.t0=now_ms();
    JNIEnv*env=NULL;
    if((*vm)->GetEnv(vm,(void**)&env,JNI_VERSION_1_6)!=JNI_OK||!env){LOGE("GetEnv falhou");return JNI_ERR;}
    jclass local=(*env)->FindClass(env,NATIVES_CLASS);
    if(local){
        G.natives=(jclass)(*env)->NewGlobalRef(env,local);
        G.mLoadTexture =(*env)->GetStaticMethodID(env,local,"OnLoadTexture","(Ljava/lang/String;)I");
        G.mGetTextureID=(*env)->GetStaticMethodID(env,local,"OnGetTextureID","(I)I");
        (*env)->ExceptionClear(env);
    } else {(*env)->ExceptionClear(env);LOGE("classe %s nao encontrada",NATIVES_CLASS);}
    LOGI("Motor ARM64 v0.60 carregado. Bicho no LCD.");
    return JNI_VERSION_1_6;
}

#define J(name) Java_com_namcobandaigames_tamagotchilife_SampleGameNatives_##name

JNIEXPORT void JNICALL J(init)(JNIEnv*env,jclass c,jint w,jint h){
    (void)c;
    G.hunger=80; G.happy=80; G.sick=0; recompute_calling(); G.t0=now_ms();
    on_context_created();
    load_all_textures(env);
    LOGI("init(%d,%d) tex=%d/%d",w,h,G.tex_loaded,TEX_COUNT);
}
JNIEXPORT void JNICALL J(main)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("main()");}
JNIEXPORT void JNICALL J(step)(JNIEnv*env,jclass c){
    (void)c;
    if(G.tex_epoch!=G.gl_epoch && G.mLoadTexture) load_all_textures(env);
    G.ticks++;
    if((G.ticks%1800)==0){ G.hunger=clampi(G.hunger-1,0,100); G.happy=clampi(G.happy-1,0,100); recompute_calling(); }
    render();
}
JNIEXPORT void JNICALL J(stop)(JNIEnv*env,jclass c){(void)env;(void)c;LOGI("stop()");}
JNIEXPORT void JNICALL J(GameTerm)(JNIEnv*env,jclass c){
    (void)c; if(G.natives&&env){(*env)->DeleteGlobalRef(env,G.natives);G.natives=NULL;} LOGI("GameTerm()");
}
JNIEXPORT jint JNICALL J(GameGetPart)(JNIEnv*env,jclass c){(void)env;(void)c;return 0;}
JNIEXPORT void JNICALL J(GameSetAppRequest)(JNIEnv*env,jclass c,jint req){
    switch(req){case 1:G.hunger=clampi(G.hunger+30,0,100);break;
    case 2:G.happy=clampi(G.happy+30,0,100);break;case 3:G.sick=0;break;default:break;}
    recompute_calling(); (void)env; (void)c;
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
