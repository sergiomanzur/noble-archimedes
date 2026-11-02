#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "input_core.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif
/* All fields are mandatory; a truncated config must never silently alter controls. */
typedef struct { const char *name; size_t offset; int real; } Field;
#define F(n) {#n,offsetof(NiConfig,n),1}
#define I(n) {#n,offsetof(NiConfig,n),0}
static const Field fields[]={F(left_inner),F(left_outer),F(right_inner),F(right_outer),F(trigger_threshold),F(mouse_sensitivity_x),F(mouse_sensitivity_y),F(look_sensitivity_x),F(look_sensitivity_y),I(mouse_invert_x),I(mouse_invert_y),I(pad_invert_x),I(pad_invert_y),I(mouse_camera),I(prompt_family)};
#define FIELD_COUNT (sizeof fields/sizeof fields[0])
static int write_config(FILE *f,const NiConfig *c) {
 size_t i;int t,d,a,s; if(fprintf(f,"# Noble Archimedes controls; SDL numeric scancodes/buttons/axes\nversion=%u\n",c->version)<0)return 0;
 for(i=0;i<FIELD_COUNT;i++) { const char *p=(const char*)c+fields[i].offset; if(fields[i].real)fprintf(f,"%s=%.9g\n",fields[i].name,(double)*(const float*)p);else if(i==FIELD_COUNT-1)fprintf(f,"%s=%d\n",fields[i].name,(int)c->prompt_family);else fprintf(f,"%s=%d\n",fields[i].name,*(const int*)p); }
 for(t=0;t<NI_CONTEXT_COUNT;t++)for(d=0;d<NI_DEVICE_COUNT;d++)for(a=0;a<NI_ACTION_COUNT;a++)for(s=0;s<2;s++) { NiBinding b=c->bindings[t][d][a][s]; fprintf(f,"binding.%d.%d.%d.%d=%d,%d,%d\n",t,d,a,s,b.kind,b.code,b.sign); }
 return !ferror(f);
}
int ni_config_save(const NiConfig *c,const char *path) {
 char tmp[4096];FILE *f;int ok=0;unsigned attempt;
 if(!path||!ni_config_valid(c)||strlen(path)>sizeof tmp-80)return 0;
 for(attempt=0;attempt<100;attempt++) {
#ifdef _WIN32
  snprintf(tmp,sizeof tmp,"%s.tmp.%lu.%u",path,(unsigned long)GetCurrentProcessId(),attempt);
#else
  snprintf(tmp,sizeof tmp,"%s.tmp.%lu.%u",path,(unsigned long)getpid(),attempt);
#endif
  f=fopen(tmp,"wx");if(f)break; if(errno!=EEXIST)return 0;
 }
 if(!f)return 0;
 ok=write_config(f,c)&&fflush(f)==0;
#ifdef _WIN32
 if(ok)ok=_commit(_fileno(f))==0;
#else
 if(ok)ok=fsync(fileno(f))==0;
#endif
 if(fclose(f)!=0)ok=0;
 if(ok) {
#ifdef _WIN32
  ok=MoveFileExA(tmp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
  ok=rename(tmp,path)==0;
#endif
 }
 if(!ok)remove(tmp);return ok;
}
int ni_config_load(NiConfig *out,const char *path) {
 NiConfig c;FILE *f;char line[256];unsigned char seen[NI_CONTEXT_COUNT][NI_DEVICE_COUNT][NI_ACTION_COUNT][2]={0},scalars[FIELD_COUNT]={0};int version=0,ok=1;size_t i;
 ni_config_defaults(out);memset(&c,0,sizeof c);if(!path||(f=fopen(path,"r"))==NULL)return 0;
 while(fgets(line,sizeof line,f)) {
  char *eq,*end,*key=line;size_t len=strlen(line);int t,d,a,s,n=0;long integer;float real;
  if(len==sizeof(line)-1 && line[len-1]!='\n'){ok=0;break;}
  while(*key==' '||*key=='\t')key++;if(*key=='#'||*key==';'||*key=='\r'||*key=='\n'||!*key)continue;
  eq=strchr(key,'=');if(!eq){ok=0;break;}*eq++=0;
  if(strcmp(key,"version")==0) { integer=strtol(eq,&end,10);if(version||integer!=NI_CONFIG_VERSION){ok=0;break;}c.version=(unsigned)integer;version=1; }
  else if(sscanf(key,"binding.%d.%d.%d.%d%n",&t,&d,&a,&s,&n)==4 && key[n]==0) {
   NiBinding b;int consumed=0;if((unsigned)t>=NI_CONTEXT_COUNT||(unsigned)d>=NI_DEVICE_COUNT||(unsigned)a>=NI_ACTION_COUNT||(unsigned)s>=2||seen[t][d][a][s]){ok=0;break;}
   if(sscanf(eq,"%d,%d,%d%n",&b.kind,&b.code,&b.sign,&consumed)!=3){ok=0;break;}end=eq+consumed;c.bindings[t][d][a][s]=b;seen[t][d][a][s]=1;
  } else {
   for(i=0;i<FIELD_COUNT;i++)if(strcmp(key,fields[i].name)==0)break;
   if(i==FIELD_COUNT||scalars[i]){ok=0;break;}scalars[i]=1;
   errno=0;if(fields[i].real){real=strtof(eq,&end);if(end==eq||errno){ok=0;break;}*(float*)((char*)&c+fields[i].offset)=real;}
   else {integer=strtol(eq,&end,10);if(end==eq||errno||integer<0||integer>NI_FAMILY_GENERIC){ok=0;break;}if(i==FIELD_COUNT-1)c.prompt_family=(NiFamily)integer;else *(int*)((char*)&c+fields[i].offset)=(int)integer;}
  }
  while(*end==' '||*end=='\t'||*end=='\r'||*end=='\n')end++;if(*end){ok=0;break;}
 }
 if(ferror(f))ok=0;fclose(f);if(!version)ok=0;
 for(i=0;i<FIELD_COUNT;i++)if(!scalars[i])ok=0;
 for(i=0;i<sizeof seen;i++)if(!((unsigned char*)seen)[i])ok=0;
 if(!ok||!ni_config_valid(&c))return 0;*out=c;return 1;
}
