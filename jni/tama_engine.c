/* ============================================================================
 *  Tamagotchi L.i.f.e. — Motor nativo ARM64 (Via 2)
 *  v0.61 — LOGICA DE JOGO COMPLETA (regras extraidas do proprio APK)
 *  ----------------------------------------------------------------------------
 *  FONTES REAIS usadas (engenharia inversa dos assets do APK):
 *   - assets/index.html  -> as REGRAS do jogo, documentadas pela Bandai:
 *       * Feed: "Meal" ou "Snack"; snacks a mais => adoece mais facilmente
 *       * Light: desligar quando tem sono; liga sozinha de manha
 *       * Play: minijogo; ganhar melhora o humor
 *       * Medicine: cura doenca. "So podem ser curados DUAS vezes! A terceira
 *                   vez, morrem."
 *       * Potty Duck: faz coco de poucas em poucas horas; se nao limpares,
 *                   pode adoecer
 *       * Health Meter: idade, peso, disciplina, fome e felicidade
 *       * Discipline: ralhar quando esta mimado
 *       * Call Signal: aparece quando chama (4 a 9 vezes por dia)
 *       * Sleep: dorme a noite; hora depende da maturidade e da disciplina
 *       * Doenca por: causas naturais / snacks a mais / nao limpar o coco
 *       * Morte por: fome prolongada / doente demasiado tempo /
 *                   adoecer vezes demais / fim da vida natural
 *   - assets/Tama2Movie_rect.bin -> 246 registos big-endian de 32 bytes
 *       (srcX,srcY,w,h,pivotX,pivotY) = 225 sprites no atlas 512x512
 *   - assets/tc_game_retro_btn.csv -> botoes A/B/C: (155,622) (268,639)
 *       (380,622), 64x64, em coordenadas da shell 595x732 (alinhamento
 *       verificado sobre a textura da shell)
 *
 *  CONTROLOS classicos (3 botoes da shell):
 *     A (esquerdo)  = seleciona o icone seguinte
 *     B (central)   = confirma / executa o icone selecionado
 *     C (direito)   = cancela / fecha
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
static const char *const TEX_NAME[] = { "body_00000.tgd", "Tama2Movie_texture.tgd" };
#define TEX_COUNT 2
#define SHELL_U1 0.5811f      /* 595/1024 */
#define SHELL_V1 0.7148f      /* 732/1024 */
#define SHELL_W  595.0f
#define SHELL_H  732.0f
#define SHELL_ASPECT (SHELL_W/SHELL_H)
#define ATLAS_PX 512.0f

/* janela LCD medida na shell (fracao do conteudo) */
#define LCD_U0 0.2420f
#define LCD_U1 0.7513f
#define LCD_V0 0.3347f
#define LCD_V1 0.7213f

/* botoes A/B/C do CSV, em coords da shell 595x732 */
typedef struct { float x,y,w,h; } RectF;
static const RectF BTN[3] = {
    { 155.f, 622.f, 64.f, 64.f },   /* A - seleciona   */
    { 268.f, 639.f, 64.f, 64.f },   /* B - confirma    */
    { 380.f, 622.f, 64.f, 64.f },   /* C - cancela     */
};

/* sprites do atlas (verificados no rect.bin) */
typedef struct { int x,y,w,h; } Spr;
static const Spr S_CREAT[2]  = { {16,16,16,16}, {32,16,16,16} };  /* #96 #97 */
static const Spr S_POOP      = {368,16,16,16};   /* #20 */
static const Spr S_SKULL     = {304, 8, 8, 8};   /* #52 */
static const Spr S_ZZZ       = {344, 0, 8, 8};   /* #15 */
static const Spr S_HEART_E   = {400, 8, 8, 8};   /* #70 */
static const Spr S_HEART_F   = {409, 8, 8, 8};   /* #71 */
static const Spr S_ATT       = {320, 0, 8, 8};   /* #3  chamada */
static const Spr S_DIGIT[10] = {
    {304,144,8,8},{320,144,8,8},{336,144,8,8},{352,144,8,8},{368,144,8,8},
    {384,144,8,8},{400,144,8,8},{416,144,8,8},{432,144,8,8},{448,144,8,8} };

/* 8 icones de cuidado (atlas) */
enum { IC_MEAL=0, IC_LIGHT, IC_PLAY, IC_MEDICINE, IC_DUCK, IC_HEALTH, IC_DISCIP, IC_ATT, IC_N };
static const Spr S_ICON[IC_N] = {
    {304,16,16,16},  /* #21 talheres  - comida     */
    {320,16,16,16},  /* #24 lampada   - luz        */
    {376, 0, 8, 8},  /* #63 bola      - brincar    */
    {352,16,16,16},  /* #19 seringa   - medicina   */
    {400,16,16,16},  /* #27 pato      - limpar     */
    {352,56,32, 8},  /* #64 medidor   - saude      */
    {336,16,16,16},  /* #26 treino    - disciplina */
    {320, 0, 8, 8},  /* #3  brilho    - atencao    */
};

/* ---------------- tempo de jogo ----------------
 * 1 segundo real = 1 minuto de jogo  => 1 dia = 24 min reais */
#define TICKS_PER_SEC   60
#define MIN_PER_DAY     1440

/* ---------------- estado do bicho ---------------- */
typedef struct {
    int hunger;        /* 0..4 coracoes */
    int happy;         /* 0..4 coracoes */
    int discipline;    /* 0..100 %      */
    int weight;        /* oz            */
    int age_days;
    int stage;         /* 0 bebe,1 crianca,2 adolescente,3 adulto */
    int sick;          /* 0/1           */
    int cures;         /* curas usadas; a 3a doenca mata  */
    int poop;          /* 0..4          */
    int sleeping;
    int light_on;
    int call;          /* sinal de chamada */
    int dead;
    int snacks;        /* snacks consumidos (risco de doenca) */
    long gmin;         /* minutos de jogo decorridos */
    long hungry_since; /* gmin em que a fome chegou a 0 (-1 = nao) */
    long sick_since;   /* gmin em que adoeceu (-1 = nao)           */
} Tama;

typedef struct {
    JavaVM *vm; jclass natives;
    jmethodID mLoadTexture, mGetTextureID;
    int tex[TEX_COUNT];
    int tex_loaded; long gl_epoch, tex_epoch;
    int surf_w, surf_h;
    long ticks; unsigned rngv;

    Tama t;
    int  sel;          /* icone selecionado 0..7, -1 = nenhum */
    int  menu_open;    /* painel de saude aberto */
    int  msg_timer;    /* frames a mostrar feedback */
    float cx, cy;      /* posicao do bicho no LCD (fracao 0..1) */
    float vx, vy;
    int  btn_down[3];
} Eng;

static Eng G;

/* ---------------- util ---------------- */
static int clampi(int v,int lo,int hi){ return v<lo?lo:(v>hi?hi:v); }
static unsigned rnd(void){ G.rngv = G.rngv*1103515245u + 12345u; return (G.rngv>>16)&0x7fff; }
static int chance(int pct){ return (int)(rnd()%100) < pct; }

static int egl_size(int*w,int*h){
    EGLDisplay d=eglGetCurrentDisplay(); EGLSurface s=eglGetCurrentSurface(EGL_DRAW);
    if(d==EGL_NO_DISPLAY||s==EGL_NO_SURFACE) return 0;
    EGLint a=0,b=0;
    if(!eglQuerySurface(d,s,EGL_WIDTH,&a)) return 0;
    if(!eglQuerySurface(d,s,EGL_HEIGHT,&b)) return 0;
    if(a<=0||b<=0) return 0; *w=a; *h=b; return 1;
}
static int jstr_int(JNIEnv*e,jmethodID m,const char*s){
    if(!e||!G.natives||!m) return -1;
    jstring js=(*e)->NewStringUTF(e,s);
    jint r=(*e)->CallStaticIntMethod(e,G.natives,m,js);
    if((*e)->ExceptionCheck(e)){(*e)->ExceptionClear(e);(*e)->DeleteLocalRef(e,js);return -1;}
    (*e)->DeleteLocalRef(e,js); return (int)r;
}
static int jint_int(JNIEnv*e,jmethodID m,int a){
    if(!e||!G.natives||!m) return -1;
    jint r=(*e)->CallStaticIntMethod(e,G.natives,m,(jint)a);
    if((*e)->ExceptionCheck(e)){(*e)->ExceptionClear(e);return -1;}
    return (int)r;
}
static void load_tex(JNIEnv*e){
    if(G.tex_epoch==G.gl_epoch && G.tex_loaded>0) return;
    G.tex_loaded=0;
    for(int i=0;i<TEX_COUNT;i++){
        int idx=jstr_int(e,G.mLoadTexture,TEX_NAME[i]);
        int id=(idx>=0)?jint_int(e,G.mGetTextureID,idx):-1;
        G.tex[i]=id; if(id>=0) G.tex_loaded++;
        LOGI("tex[%d] '%s' idx=%d glId=%d",i,TEX_NAME[i],idx,id);
    }
    G.tex_epoch=G.gl_epoch;
}

/* ============================ LOGICA DE JOGO ============================ */
static void tama_new(Tama*t){
    memset(t,0,sizeof(*t));
    t->hunger=4; t->happy=4; t->discipline=0; t->weight=5;
    t->age_days=0; t->stage=0; t->light_on=1;
    t->hungry_since=-1; t->sick_since=-1;
}

/* hora do dia 0..1439 */
static int hour_of_day(const Tama*t){ return (int)((t->gmin % MIN_PER_DAY)/60); }

/* hora de dormir/acordar conforme maturidade e disciplina (regra do FAQ:
 * jovens dormem cedo; mal-disciplinados ficam acordados ate mais tarde) */
static void sleep_window(const Tama*t,int*bed,int*wake){
    int b = 19 + t->stage;                 /* bebe 19h ... adulto 22h */
    if(t->discipline < 40) b += 1;         /* mimado deita-se mais tarde */
    *bed  = clampi(b,19,23);
    *wake = 8 - (t->stage>2?0:1);          /* 7h jovem, 8h adulto */
    if(*wake<6) *wake=6;
}

static void recompute_call(Tama*t){
    t->call = (!t->sleeping) && (t->hunger==0 || t->happy==0 || t->sick ||
                                 t->poop>0   || (t->discipline<50 && (t->hunger==0||t->happy==0)));
}

static void tama_get_sick(Tama*t,const char*why){
    if(t->sick||t->dead) return;
    t->sick=1; t->sick_since=t->gmin;
    LOGI("[JOGO] adoeceu (%s). curas usadas=%d",why,t->cures);
}

static void tama_die(Tama*t,const char*why){
    if(t->dead) return;
    t->dead=1; t->call=0;
    LOGI("[JOGO] *** morreu: %s (idade %d dias) ***",why,t->age_days);
}

/* um minuto de jogo */
static void tama_minute(Tama*t){
    if(t->dead) return;
    t->gmin++;
    if(t->gmin % MIN_PER_DAY == 0){
        t->age_days++;
        /* evolucao por idade (bebe->crianca->adolescente->adulto) */
        int ns = t->age_days>=9?3 : t->age_days>=4?2 : t->age_days>=1?1 : 0;
        if(ns!=t->stage){ t->stage=ns; LOGI("[JOGO] evoluiu para fase %d",ns); }
        /* fim de vida natural */
        if(t->age_days>=28 && chance(20)) tama_die(t,"fim de vida natural");
    }

    /* ciclo de sono */
    int bed,wake,h=hour_of_day(t);
    sleep_window(t,&bed,&wake);
    int night = (h>=bed || h<wake);
    if(night && !t->light_on) t->sleeping=1;
    if(!night){ t->sleeping=0; t->light_on=1; }   /* liga sozinha de manha */

    if(t->sleeping){ recompute_call(t); return; } /* nao ha cuidados a dormir */

    /* fome e humor */
    if(t->gmin%30==0 && t->hunger>0) t->hunger--;
    if(t->gmin%40==0 && t->happy>0)  t->happy--;

    /* fome prolongada mata */
    if(t->hunger==0){ if(t->hungry_since<0) t->hungry_since=t->gmin; }
    else t->hungry_since=-1;
    if(t->hungry_since>=0 && t->gmin-t->hungry_since > 6*60)
        tama_die(t,"fome prolongada");

    /* coco de poucas em poucas horas */
    if(t->gmin%180==0 && t->poop<4){ t->poop++; LOGI("[JOGO] fez coco (%d)",t->poop); }

    /* doenca: coco por limpar / snacks a mais / causas naturais
     * (o FAQ diz: adoecem mais quando sao jovens ou mal-disciplinados) */
    if(t->gmin%60==0 && !t->sick){
        int risk = 2;
        risk += t->poop*4;
        risk += (t->snacks>6)? (t->snacks-6)*3 : 0;
        if(t->stage<2)        risk += 3;
        if(t->discipline<40)  risk += 3;
        if(chance(risk)) tama_get_sick(t,"risco acumulado");
    }
    /* doente demasiado tempo mata */
    if(t->sick && t->gmin-t->sick_since > 12*60)
        tama_die(t,"doente demasiado tempo");

    recompute_call(t);
}

/* ---- acoes dos icones ---- */
static void act_meal(Tama*t){
    if(t->sleeping||t->dead) return;
    if(t->hunger>=4){ LOGI("[JOGO] ja esta cheio"); return; }
    t->hunger=clampi(t->hunger+1,0,4); t->weight+=1;
    LOGI("[JOGO] refeicao -> fome=%d peso=%d",t->hunger,t->weight);
}
static void act_snack(Tama*t){
    if(t->sleeping||t->dead) return;
    t->happy=clampi(t->happy+1,0,4); t->weight+=2; t->snacks++;
    LOGI("[JOGO] snack -> humor=%d peso=%d snacks=%d",t->happy,t->weight,t->snacks);
}
static void act_light(Tama*t){
    if(t->dead) return;
    t->light_on=!t->light_on;
    LOGI("[JOGO] luz %s",t->light_on?"ligada":"desligada");
}
static void act_play(Tama*t){
    if(t->sleeping||t->dead) return;
    /* minijogo resolvido: ganha em ~60% dos casos (melhor-de-cinco abstraido) */
    if(chance(60)){ t->happy=clampi(t->happy+1,0,4); LOGI("[JOGO] ganhou o jogo -> humor=%d",t->happy); }
    else LOGI("[JOGO] perdeu o jogo");
    if(t->weight>5) t->weight--;
}
static void act_medicine(Tama*t){
    if(t->dead) return;
    if(!t->sick){ LOGI("[JOGO] nao esta doente"); return; }
    t->cures++;
    if(t->cures>=3){ tama_die(t,"adoeceu vezes demais"); return; }  /* regra do FAQ */
    t->sick=0; t->sick_since=-1;
    LOGI("[JOGO] curado (cura %d de 2)",t->cures);
}
static void act_duck(Tama*t){
    if(t->dead) return;
    if(t->poop>0){ t->poop=0; LOGI("[JOGO] limpou o coco"); }
}
static void act_discipline(Tama*t){
    if(t->sleeping||t->dead) return;
    if(t->call && (t->hunger>0 && t->happy>0)){
        t->discipline=clampi(t->discipline+20,0,100);
        t->call=0;
        LOGI("[JOGO] ralhou -> disciplina=%d",t->discipline);
    } else {
        t->discipline=clampi(t->discipline-5,0,100);
        LOGI("[JOGO] ralhou sem razao -> disciplina=%d",t->discipline);
    }
}
static void do_action(int icon){
    Tama*t=&G.t;
    switch(icon){
        case IC_MEAL:      act_meal(t);       break;
        case IC_LIGHT:     act_light(t);      break;
        case IC_PLAY:      act_play(t);       break;
        case IC_MEDICINE:  act_medicine(t);   break;
        case IC_DUCK:      act_duck(t);       break;
        case IC_HEALTH:    G.menu_open=!G.menu_open; break;
        case IC_DISCIP:    act_discipline(t); break;
        case IC_ATT:       act_snack(t);      break;  /* snack */
        default: break;
    }
    recompute_call(t);
    G.msg_timer=40;
}

/* ---- botoes ---- */
static void press_A(void){ G.sel=(G.sel+1)%IC_N; G.menu_open=0; }
static void press_B(void){ if(G.sel>=0) do_action(G.sel); }
static void press_C(void){ G.sel=-1; G.menu_open=0; }

/* ============================ RENDER ============================ */
static float sx_g, sy_g;         /* meia-largura/altura da shell em NDC */
static float u2x(float u){ return sx_g*(2.0f*u-1.0f); }
static float v2y(float v){ return sy_g*(1.0f-2.0f*v); }

static void quad(float x0,float y0,float x1,float y1,float u0,float v0,float u1,float v1){
    GLfloat vt[]={x0,y0, x1,y0, x0,y1, x1,y1};
    GLfloat uv[]={u0,v1, u1,v1, u0,v0, u1,v0};
    glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    glVertexPointer(2,GL_FLOAT,0,vt); glTexCoordPointer(2,GL_FLOAT,0,uv);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
    glDisableClientState(GL_TEXTURE_COORD_ARRAY); glDisableClientState(GL_VERTEX_ARRAY);
}
static void fill(float x0,float y0,float x1,float y1,float r,float g,float b,float a){
    glDisable(GL_TEXTURE_2D); glColor4f(r,g,b,a);
    GLfloat vt[]={x0,y0, x1,y0, x0,y1, x1,y1};
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(2,GL_FLOAT,0,vt);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4); glDisableClientState(GL_VERTEX_ARRAY);
    glColor4f(1,1,1,1);
}
/* desenha sprite do atlas num rect NDC */
static void spr(const Spr*s,float x0,float y0,float x1,float y1){
    if(G.tex[TEX_ATLAS]<0) return;
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D,(GLuint)G.tex[TEX_ATLAS]);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    quad(x0,y0,x1,y1, s->x/ATLAS_PX, s->y/ATLAS_PX,
                      (s->x+s->w)/ATLAS_PX, (s->y+s->h)/ATLAS_PX);
}
/* sprite centrado em (u,v) do espaco da shell, com largura em fracao de shell */
static void spr_at(const Spr*s,float u,float v,float wfrac){
    float hfrac = wfrac*((float)s->h/(float)s->w)*(SHELL_W/SHELL_H);
    spr(s, u2x(u-wfrac*0.5f), v2y(v+hfrac*0.5f),
           u2x(u+wfrac*0.5f), v2y(v-hfrac*0.5f));
}
static void draw_number(int val,float u,float v,float dw){
    if(val<0) val=0;
    int d[4],n=0; if(val==0){d[n++]=0;} while(val>0&&n<4){d[n++]=val%10;val/=10;}
    float total=n*dw*1.1f, x=u-total*0.5f+dw*0.55f;
    for(int i=n-1;i>=0;i--){ spr_at(&S_DIGIT[d[i]],x,v,dw); x+=dw*1.1f; }
}

static void render(void){
    int w=0,h=0;
    if(egl_size(&w,&h)){ G.surf_w=w; G.surf_h=h; } else { w=G.surf_w; h=G.surf_h; }
    if(w<=0||h<=0){ w=480; h=800; }
    glViewport(0,0,w,h);
    glClearColor(0,0,0,1); glClear(GL_COLOR_BUFFER_BIT);
    glMatrixMode(GL_PROJECTION); glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);  glLoadIdentity();
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA);

    /* aspect-fit da shell, reduzido para deixar margem aos icones */
    float scr=(float)w/(float)h, sx,sy;
    if(scr>SHELL_ASPECT){ sx=SHELL_ASPECT/scr; sy=1.0f; }
    else                { sx=1.0f; sy=scr/SHELL_ASPECT; }
    sx*=0.90f; sy*=0.90f;
    sx_g=sx; sy_g=sy;

    /* --- LCD --- */
    float lx0=u2x(LCD_U0), lx1=u2x(LCD_U1);
    float ly0=v2y(LCD_V0), ly1=v2y(LCD_V1);
    int lit = G.t.light_on || !G.t.sleeping;
    if(lit) fill(lx0,ly1,lx1,ly0, 0.70f,0.78f,0.55f,1.0f);
    else    fill(lx0,ly1,lx1,ly0, 0.16f,0.20f,0.14f,1.0f);

    float lu0=LCD_U0,lu1=LCD_U1,lv0=LCD_V0,lv1=LCD_V1;
    float lw=lu1-lu0, lh=lv1-lv0;

    if(G.t.dead){
        spr_at(&S_SKULL, lu0+lw*0.5f, lv0+lh*0.5f, lw*0.35f);
    } else if(G.menu_open){
        /* painel de saude: idade, peso, fome, felicidade, disciplina */
        float x=lu0+lw*0.18f, y=lv0+lh*0.18f, dy=lh*0.20f, dw=lw*0.10f;
        draw_number(G.t.age_days, x, y, dw);
        draw_number(G.t.weight,   x, y+dy, dw);
        for(int i=0;i<4;i++)
            spr_at(i<G.t.hunger?&S_HEART_F:&S_HEART_E, lu0+lw*(0.45f+i*0.13f), y, dw);
        for(int i=0;i<4;i++)
            spr_at(i<G.t.happy ?&S_HEART_F:&S_HEART_E, lu0+lw*(0.45f+i*0.13f), y+dy, dw);
        draw_number(G.t.discipline, lu0+lw*0.5f, y+2*dy, dw);
    } else if(lit){
        /* o bicho */
        const Spr*cs=&S_CREAT[(G.ticks/42)&1];
        spr_at(cs, lu0+lw*G.cx, lv0+lh*G.cy, lw*0.34f);
        if(G.t.sleeping) spr_at(&S_ZZZ, lu0+lw*(G.cx+0.20f), lv0+lh*(G.cy-0.18f), lw*0.10f);
        if(G.t.sick)     spr_at(&S_SKULL, lu0+lw*(G.cx+0.22f), lv0+lh*(G.cy-0.20f), lw*0.11f);
        for(int i=0;i<G.t.poop;i++)
            spr_at(&S_POOP, lu0+lw*(0.14f+i*0.16f), lv0+lh*0.86f, lw*0.13f);
        if(G.t.call && ((G.ticks/20)&1))
            spr_at(&S_ATT, lu0+lw*0.5f, lv0+lh*0.10f, lw*0.10f);
    }

    /* --- shell por cima (janela transparente) --- */
    if(G.tex[TEX_SHELL]>=0){
        glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D,(GLuint)G.tex[TEX_SHELL]);
        glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameterx(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glColor4f(1,1,1,1);
        quad(-sx,-sy,sx,sy, 0,0, SHELL_U1, SHELL_V1);
    }

    /* --- barra de icones (4 em cima, 4 em baixo, fora da shell) --- */
    float iw=0.13f;                       /* largura em NDC */
    float ytop = sy + (1.0f-sy)*0.50f;
    float ybot = -ytop;
    for(int i=0;i<IC_N;i++){
        int top = i<4;
        float cxn = (-0.72f + (i%4)*0.48f);
        float cyn = top? ytop : ybot;
        float hh  = iw*((float)S_ICON[i].h/(float)S_ICON[i].w)*((float)w/(float)h);
        if(G.sel==i) fill(cxn-iw*0.62f,cyn-hh*0.78f,cxn+iw*0.62f,cyn+hh*0.78f,
                          1.0f,0.85f,0.20f,0.95f);
        spr(&S_ICON[i], cxn-iw*0.5f, cyn-hh*0.5f, cxn+iw*0.5f, cyn+hh*0.5f);
    }
    glDisable(GL_TEXTURE_2D);
}

/* ============================ JNI ============================ */
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM*vm,void*r){
    (void)r; memset(&G,0,sizeof(G));
    for(int i=0;i<TEX_COUNT;i++) G.tex[i]=-1;
    G.gl_epoch=0; G.tex_epoch=-1; G.vm=vm; G.rngv=12345u;
    G.sel=-1; G.cx=0.5f; G.cy=0.5f; G.vx=0.0035f; G.vy=0.0021f;
    tama_new(&G.t);
    JNIEnv*e=NULL;
    if((*vm)->GetEnv(vm,(void**)&e,JNI_VERSION_1_6)!=JNI_OK||!e){LOGE("GetEnv falhou");return JNI_ERR;}
    jclass c=(*e)->FindClass(e,NATIVES_CLASS);
    if(c){
        G.natives=(jclass)(*e)->NewGlobalRef(e,c);
        G.mLoadTexture =(*e)->GetStaticMethodID(e,c,"OnLoadTexture","(Ljava/lang/String;)I");
        G.mGetTextureID=(*e)->GetStaticMethodID(e,c,"OnGetTextureID","(I)I");
        (*e)->ExceptionClear(e);
    } else { (*e)->ExceptionClear(e); LOGE("classe nao encontrada"); }
    LOGI("Motor ARM64 v0.61 carregado. Logica de jogo completa.");
    return JNI_VERSION_1_6;
}

#define J(n) Java_com_namcobandaigames_tamagotchilife_SampleGameNatives_##n

JNIEXPORT void JNICALL J(init)(JNIEnv*e,jclass c,jint w,jint h){
    (void)c;
    G.gl_epoch++; G.tex_loaded=0;
    for(int i=0;i<TEX_COUNT;i++) G.tex[i]=-1;
    int a=0,b=0; if(egl_size(&a,&b)){G.surf_w=a;G.surf_h=b;}
    load_tex(e);
    LOGI("init(%d,%d) tex=%d/%d surf=%dx%d",w,h,G.tex_loaded,TEX_COUNT,G.surf_w,G.surf_h);
}
JNIEXPORT void JNICALL J(main)(JNIEnv*e,jclass c){(void)e;(void)c;LOGI("main()");}

JNIEXPORT void JNICALL J(step)(JNIEnv*e,jclass c){
    (void)c;
    if(G.tex_epoch!=G.gl_epoch && G.mLoadTexture) load_tex(e);
    G.ticks++;
    if(G.msg_timer>0) G.msg_timer--;

    /* 1 minuto de jogo por segundo real */
    if(G.ticks % TICKS_PER_SEC == 0) tama_minute(&G.t);

    /* o bicho passeia dentro do LCD */
    if(!G.t.sleeping && !G.t.dead && !G.menu_open){
        G.cx+=G.vx; G.cy+=G.vy;
        if(G.cx<0.22f||G.cx>0.78f){ G.vx=-G.vx; G.cx=clampi((int)(G.cx*1000),220,780)/1000.0f; }
        if(G.cy<0.26f||G.cy>0.74f){ G.vy=-G.vy; G.cy=clampi((int)(G.cy*1000),260,740)/1000.0f; }
    }
    render();
}

JNIEXPORT void JNICALL J(stop)(JNIEnv*e,jclass c){(void)e;(void)c;LOGI("stop()");}
JNIEXPORT void JNICALL J(GameTerm)(JNIEnv*e,jclass c){
    (void)c; if(G.natives&&e){(*e)->DeleteGlobalRef(e,G.natives);G.natives=NULL;} LOGI("GameTerm()");
}
JNIEXPORT jint JNICALL J(GameGetPart)(JNIEnv*e,jclass c){(void)e;(void)c;return 0;}

JNIEXPORT void JNICALL J(GameSetAppRequest)(JNIEnv*e,jclass c,jint req){
    (void)e;(void)c;
    if(req==0){ tama_new(&G.t); G.sel=-1; G.menu_open=0; LOGI("[JOGO] reset"); }
    else if(req>=1&&req<=8) do_action(req-1);
}

/* toque: converter para NDC e testar os 3 botoes da shell */
JNIEXPORT void JNICALL J(GameInputSetTouches)(JNIEnv*e,jclass c,jint id,jfloat x,jfloat y,jfloat a,jfloat b){
    (void)e;(void)c;(void)id;(void)a;(void)b;
    int w=G.surf_w>0?G.surf_w:480, h=G.surf_h>0?G.surf_h:800;
    float nx=2.0f*(float)x/(float)w-1.0f;
    float ny=1.0f-2.0f*(float)y/(float)h;
    for(int i=0;i<3;i++){
        float u0=BTN[i].x/SHELL_W, u1=(BTN[i].x+BTN[i].w)/SHELL_W;
        float v0=BTN[i].y/SHELL_H, v1=(BTN[i].y+BTN[i].h)/SHELL_H;
        float X0=u2x(u0),X1=u2x(u1),Y1=v2y(v0),Y0=v2y(v1);
        if(nx>=X0&&nx<=X1&&ny>=Y0&&ny<=Y1){
            if(!G.btn_down[i]){
                G.btn_down[i]=1;
                if(i==0) press_A(); else if(i==1) press_B(); else press_C();
                LOGI("[BTN] %c  sel=%d",(char)('A'+i),G.sel);
            }
            return;
        }
    }
    LOGI("[TOQUE] x=%.0f y=%.0f ndc=(%.2f,%.2f) surf=%dx%d",(double)x,(double)y,(double)nx,(double)ny,w,h);
}
JNIEXPORT void JNICALL J(GameInputReleaseTouches)(JNIEnv*e,jclass c,jint id,jfloat x,jfloat y){
    (void)e;(void)c;(void)id;(void)x;(void)y;
    G.btn_down[0]=G.btn_down[1]=G.btn_down[2]=0;
}
JNIEXPORT jint JNICALL J(GetBaseScreenWidth)(JNIEnv*e,jclass c){(void)e;(void)c;return 480;}
JNIEXPORT jint JNICALL J(GetBaseScreenHeight)(JNIEnv*e,jclass c){(void)e;(void)c;return 800;}
JNIEXPORT void JNICALL J(ThreadCreate)(JNIEnv*e,jclass c,jint a,jint b){(void)e;(void)c;(void)a;(void)b;LOGI("ThreadCreate");}
JNIEXPORT void JNICALL J(TMGCmakeAlarmInfoData)(JNIEnv*e,jclass c){(void)e;(void)c;LOGI("AlarmInfo");}
