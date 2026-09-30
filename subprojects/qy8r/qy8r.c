#define EGL_EGLEXT_PROTOTYPES 1
#include "qy8r.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { GLuint id; } qy8r_program;
typedef struct { GLuint fbo, tex; int w, h, is_float; } qy8r_target;
typedef struct {
    EGLDisplay dpy; EGLContext ctx; EGLSurface surface; GLuint vao;
    GLuint texture; GLuint buffers[16]; int n_buffers;
    char error[512];
} qy8r_context;
static void seterr(qy8r_context *c,const char *s){snprintf(c->error,sizeof c->error,"%s (egl=0x%x gl=0x%x)",s,eglGetError(),glGetError());}
static int glok(qy8r_context *c,const char *where){GLenum e=glGetError();if(e==GL_NO_ERROR)return 1;snprintf(c->error,sizeof c->error,"%s: GL error 0x%x",where,e);return 0;}
static void copyerr(char *dst,size_t n,const char *s){if(dst&&n)snprintf(dst,n,"%s",s?s:"unknown error");}
void *qy8r_open(char *error,size_t error_size){
    qy8r_context *c=calloc(1,sizeof *c);if(!c){copyerr(error,error_size,"out of memory");return NULL;}
    PFNEGLGETPLATFORMDISPLAYEXTPROC getp=(PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if(getp)c->dpy=getp(EGL_PLATFORM_SURFACELESS_MESA,EGL_DEFAULT_DISPLAY,NULL);else c->dpy=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint major=0,minor=0;
    if(c->dpy==EGL_NO_DISPLAY||!eglInitialize(c->dpy,&major,&minor)||major<1||(major==1&&minor<4)){seterr(c,"EGL 1.4+ initialization failed");goto bad;}
    if(!eglBindAPI(EGL_OPENGL_ES_API)){seterr(c,"eglBindAPI GLES failed");goto bad;}
    const EGLint ca[]={EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig cfg;EGLint n=0;if(!eglChooseConfig(c->dpy,ca,&cfg,1,&n)||n!=1){seterr(c,"no ES3 RGBA8 pbuffer config");goto bad;}
    const EGLint pa[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};c->surface=eglCreatePbufferSurface(c->dpy,cfg,pa);
    const EGLint xa[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};c->ctx=eglCreateContext(c->dpy,cfg,EGL_NO_CONTEXT,xa);
    if(c->surface==EGL_NO_SURFACE||c->ctx==EGL_NO_CONTEXT||!eglMakeCurrent(c->dpy,c->surface,c->surface,c->ctx)){seterr(c,"ES3 context creation/current failed");goto bad;}
    GLint maj=0,min=0;glGetIntegerv(GL_MAJOR_VERSION,&maj);glGetIntegerv(GL_MINOR_VERSION,&min);
    if(maj<3){seterr(c,"backend did not provide GLES 3.0+");goto bad;}
    glGenVertexArrays(1,&c->vao);glBindVertexArray(c->vao);
    if(!glok(c,"initialize VAO"))goto bad;
    return c;
bad: copyerr(error,error_size,c->error);if(c->ctx!=EGL_NO_CONTEXT)eglDestroyContext(c->dpy,c->ctx);if(c->surface!=EGL_NO_SURFACE)eglDestroySurface(c->dpy,c->surface);if(c->dpy!=EGL_NO_DISPLAY)eglTerminate(c->dpy);free(c);return NULL;
}
void qy8r_close(void *p){qy8r_context*c=p;if(!c)return;eglMakeCurrent(c->dpy,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);if(c->texture)glDeleteTextures(1,&c->texture);for(int i=0;i<c->n_buffers;i++)glDeleteBuffers(1,&c->buffers[i]);if(c->vao)glDeleteVertexArrays(1,&c->vao);eglDestroyContext(c->dpy,c->ctx);eglDestroySurface(c->dpy,c->surface);eglTerminate(c->dpy);free(c);}
int qy8r_get_info(void*p,char*b,size_t n){qy8r_context*c=p;if(!c||!b||!n)return 0;int used=snprintf(b,n,"EGL_VERSION=%s\nGL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\nGLSL=%s\nGL_EXTENSIONS=",eglQueryString(c->dpy,EGL_VERSION),glGetString(GL_VENDOR),glGetString(GL_RENDERER),glGetString(GL_VERSION),glGetString(GL_SHADING_LANGUAGE_VERSION));GLint ext=0;glGetIntegerv(GL_NUM_EXTENSIONS,&ext);for(GLint i=0;i<ext&&used>0&&(size_t)used<n;i++){const char*x=(const char*)glGetStringi(GL_EXTENSIONS,(GLuint)i);used+=snprintf(b+used,n-(size_t)used,"%s%s",i?" ":"",x?x:"");}return glok(c,"query info");}
static int has_ext(const char *name){GLint n=0;glGetIntegerv(GL_NUM_EXTENSIONS,&n);for(GLint i=0;i<n;i++){const char*x=(const char*)glGetStringi(GL_EXTENSIONS,(GLuint)i);if(x&&!strcmp(x,name))return 1;}return 0;}
static int tfprobe(void){
    const char *vs="#version 100\nprecision highp float; attribute highp float a; varying highp float v; void main(){v=a;gl_Position=vec4(0.0,0.0,0.0,1.0);}";
    const char *fs="#version 100\nprecision highp float; varying highp float v; void main(){gl_FragColor=vec4(v);}";
    GLuint v=glCreateShader(GL_VERTEX_SHADER),f=glCreateShader(GL_FRAGMENT_SHADER),p=glCreateProgram();glShaderSource(v,1,&vs,NULL);glCompileShader(v);glShaderSource(f,1,&fs,NULL);glCompileShader(f);GLint ok=0;glGetShaderiv(v,GL_COMPILE_STATUS,&ok);if(!ok)goto no;glGetShaderiv(f,GL_COMPILE_STATUS,&ok);if(!ok)goto no;glAttachShader(p,v);glAttachShader(p,f);const GLchar *varyings[]={"gl_Position","v"};glTransformFeedbackVaryings(p,2,varyings,GL_INTERLEAVED_ATTRIBS);glLinkProgram(p);glGetProgramiv(p,GL_LINK_STATUS,&ok);glDeleteProgram(p);glDeleteShader(v);glDeleteShader(f);return ok?1:0;
no:glDeleteProgram(p);glDeleteShader(v);glDeleteShader(f);return 0;
}
int qy8r_get_caps(void*p,int*cbf,int*tf){qy8r_context*c=p;if(!c)return 0;if(cbf)*cbf=has_ext("GL_EXT_color_buffer_float");if(tf)*tf=tfprobe();return 1;}
static GLuint compile(GLenum type,const char*src,char*log,size_t n){GLuint s=glCreateShader(type);glShaderSource(s,1,&src,NULL);glCompileShader(s);GLint ok=0;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok&&log&&n)glGetShaderInfoLog(s,(GLsizei)n,NULL,log);if(!ok){glDeleteShader(s);return 0;}return s;}
static void *program_create(void*p,const char*vs,const char*fs,const char*const*tfv,int tfn,char*log,size_t n){qy8r_context*c=p;if(!c||!vs||!fs)return NULL;if(log&&n)log[0]=0;GLuint v=compile(GL_VERTEX_SHADER,vs,log,n);if(!v)return NULL;GLuint f=compile(GL_FRAGMENT_SHADER,fs,log,n);if(!f){glDeleteShader(v);return NULL;}qy8r_program*q=calloc(1,sizeof*q);if(!q){glDeleteShader(v);glDeleteShader(f);return NULL;}q->id=glCreateProgram();glAttachShader(q->id,v);glAttachShader(q->id,f);if(tfn>0)glTransformFeedbackVaryings(q->id,tfn,(const GLchar *const*)tfv,GL_INTERLEAVED_ATTRIBS);glLinkProgram(q->id);glDeleteShader(v);glDeleteShader(f);GLint ok=0;glGetProgramiv(q->id,GL_LINK_STATUS,&ok);if(!ok){if(log&&n)glGetProgramInfoLog(q->id,(GLsizei)n,NULL,log);glDeleteProgram(q->id);free(q);return NULL;}if(!glok(c,"link program")){glDeleteProgram(q->id);free(q);return NULL;}return q;}
void *qy8r_program_create(void*p,const char*vs,const char*fs,char*log,size_t n){return program_create(p,vs,fs,NULL,0,log,n);}
void *qy8r_program_create_tf(void*p,const char*vs,const char*fs,const char*const*varyings,int count,char*log,size_t n){if(!varyings||count<1)return NULL;return program_create(p,vs,fs,varyings,count,log,n);}
void qy8r_program_destroy(void*p,void*q){(void)p;qy8r_program*x=q;if(x){glDeleteProgram(x->id);free(x);}}
void *qy8r_target_create(void*p,int w,int h,int fl){qy8r_context*c=p;if(!c||w<1||h<1)return NULL;qy8r_target*t=calloc(1,sizeof*t);if(!t)return NULL;t->w=w;t->h=h;t->is_float=!!fl;glGenTextures(1,&t->tex);glBindTexture(GL_TEXTURE_2D,t->tex);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);GLint internal=fl?GL_RGBA32F:GL_RGBA8;glTexImage2D(GL_TEXTURE_2D,0,internal,w,h,0,GL_RGBA,fl?GL_FLOAT:GL_UNSIGNED_BYTE,NULL);glGenFramebuffers(1,&t->fbo);glBindFramebuffer(GL_FRAMEBUFFER,t->fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,t->tex,0);if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE||!glok(c,"create target")){qy8r_target_destroy(c,t);return NULL;}return t;}
void qy8r_target_destroy(void*p,void*q){(void)p;qy8r_target*t=q;if(!t)return;if(t->fbo)glDeleteFramebuffers(1,&t->fbo);if(t->tex)glDeleteTextures(1,&t->tex);free(t);}
int qy8r_target_bind(void*p,void*q){qy8r_context*c=p;qy8r_target*t=q;if(!c||!t)return 0;glBindFramebuffer(GL_FRAMEBUFFER,t->fbo);glViewport(0,0,t->w,t->h);return glok(c,"bind target");}
int qy8r_clear(void*p,float r,float g,float b,float a){qy8r_context*c=p;if(!c)return 0;glClearColor(r,g,b,a);glClear(GL_COLOR_BUFFER_BIT);return glok(c,"clear");}
int qy8r_use_program(void*p,void*q){qy8r_context*c=p;qy8r_program*x=q;if(!c||!x)return 0;glUseProgram(x->id);return glok(c,"use program");}
int qy8r_uniform_f32(void*p,void*q,const char*name,const float*v,int count){qy8r_context*c=p;qy8r_program*x=q;if(!c||!x||!name||!v)return 0;GLint l=glGetUniformLocation(x->id,name);if(l<0){snprintf(c->error,sizeof c->error,"required uniform not active: %s",name);return 0;}if(count==1)glUniform1fv(l,1,v);else if(count==2)glUniform2fv(l,1,v);else if(count==3)glUniform3fv(l,1,v);else if(count==4)glUniform4fv(l,1,v);else if(count==16)glUniformMatrix4fv(l,1,GL_FALSE,v);else return 0;return glok(c,"set float uniform");}
int qy8r_uniform_i32(void*p,void*q,const char*name,const int*v,int count){qy8r_context*c=p;qy8r_program*x=q;if(!c||!x||!name||!v)return 0;GLint l=glGetUniformLocation(x->id,name);if(l<0){snprintf(c->error,sizeof c->error,"required uniform not active: %s",name);return 0;}if(count==1)glUniform1iv(l,1,v);else if(count==2)glUniform2iv(l,1,v);else if(count==3)glUniform3iv(l,1,v);else if(count==4)glUniform4iv(l,1,v);else return 0;return glok(c,"set integer uniform");}
int qy8r_texture_rgba32f(void*p,void*q,const char*sampler,const float*rgba,int unit){qy8r_context*c=p;qy8r_program*x=q;if(!c||!x||!sampler||!rgba||unit<0)return 0;GLint l=glGetUniformLocation(x->id,sampler);if(l<0){snprintf(c->error,sizeof c->error,"required sampler not active: %s",sampler);return 0;}if(!c->texture)glGenTextures(1,&c->texture);glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,c->texture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA32F,1,1,0,GL_RGBA,GL_FLOAT,rgba);glUniform1i(l,unit);return glok(c,"upload float texture");}
int qy8r_begin_draw(void*p,void*q){qy8r_context*c=p;qy8r_program*x=q;if(!c||!x)return 0;for(int i=0;i<c->n_buffers;i++)glDeleteBuffers(1,&c->buffers[i]);c->n_buffers=0;glBindVertexArray(c->vao);glUseProgram(x->id);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_DITHER);return glok(c,"begin draw");}
int qy8r_attribute_f32(void*p,void*q,const char*name,const float*v,int comps,int count){qy8r_context*c=p;qy8r_program*x=q;if(!c||!x||!name||!v||comps<1||comps>4||count<1||c->n_buffers>=16)return 0;GLint loc=glGetAttribLocation(x->id,name);if(loc<0){snprintf(c->error,sizeof c->error,"required attribute not active: %s",name);return 0;}GLuint b;glGenBuffers(1,&b);c->buffers[c->n_buffers++]=b;glBindBuffer(GL_ARRAY_BUFFER,b);glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(sizeof(float)*comps*count),v,GL_STREAM_DRAW);glEnableVertexAttribArray((GLuint)loc);glVertexAttribPointer((GLuint)loc,comps,GL_FLOAT,GL_FALSE,0,0);return glok(c,"set attribute");}
int qy8r_draw_points(void*p,int count){qy8r_context*c=p;if(!c||count<1)return 0;glDrawArrays(GL_POINTS,0,count);glFinish();return glok(c,"draw points");}
int qy8r_draw_transform_feedback(void*p,int count,float*out,size_t nfloat){qy8r_context*c=p;if(!c||count<1||!out)return 0;size_t bytes=nfloat*sizeof(float);GLuint b;glGenBuffers(1,&b);glBindBuffer(GL_TRANSFORM_FEEDBACK_BUFFER,b);glBufferData(GL_TRANSFORM_FEEDBACK_BUFFER,(GLsizeiptr)bytes,NULL,GL_STREAM_READ);glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,b);glEnable(GL_RASTERIZER_DISCARD);glBeginTransformFeedback(GL_POINTS);glDrawArrays(GL_POINTS,0,count);glEndTransformFeedback();glDisable(GL_RASTERIZER_DISCARD);glFinish();void*m=glMapBufferRange(GL_TRANSFORM_FEEDBACK_BUFFER,0,(GLsizeiptr)bytes,GL_MAP_READ_BIT);if(!m){glDeleteBuffers(1,&b);return glok(c,"map transform feedback");}memcpy(out,m,bytes);GLboolean ok=glUnmapBuffer(GL_TRANSFORM_FEEDBACK_BUFFER);glBindBufferBase(GL_TRANSFORM_FEEDBACK_BUFFER,0,0);glDeleteBuffers(1,&b);return ok&&glok(c,"transform feedback readback");}
int qy8r_read_rgba8(void*p,void*q,unsigned char out[4]){qy8r_context*c=p;qy8r_target*t=q;if(!c||!t||!out||t->is_float)return 0;qy8r_target_bind(c,t);glReadPixels(0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,out);return glok(c,"read RGBA8");}
int qy8r_read_rgba32f(void*p,void*q,float out[4]){qy8r_context*c=p;qy8r_target*t=q;if(!c||!t||!out||!t->is_float)return 0;qy8r_target_bind(c,t);glReadPixels(0,0,1,1,GL_RGBA,GL_FLOAT,out);return glok(c,"read RGBA32F");}
const char *qy8r_last_error(void*p){qy8r_context*c=p;return c?c->error:"invalid context";}
