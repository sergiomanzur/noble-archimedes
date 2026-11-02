#include "input_core.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
static float clamp(float f,float lo,float hi) { return !isfinite(f)?0:f<lo?lo:f>hi?hi:f; }
static void bind(NiConfig *c,int a,int device,int kind,int code,int sign,int slot) {
 int t; for(t=0;t<NI_CONTEXT_COUNT;t++) c->bindings[t][device][a][slot]=(NiBinding){kind,code,sign};
}
void ni_config_defaults(NiConfig *c) {
 static const int keys[NI_ACTION_COUNT]={82,81,80,79,40,42,225,224,29,27,6,25,8,20,30,32,26,22,4,7,12,14,13,15};
 static const int pads[16]={11,12,13,14,6,4,7,8,0,1,2,3,9,10,4,5};
 int a; memset(c,0,sizeof *c); c->version=NI_CONFIG_VERSION;
 c->left_inner=c->right_inner=.12f; c->left_outer=c->right_outer=1;
 c->trigger_threshold=.05f; c->mouse_sensitivity_x=c->mouse_sensitivity_y=.0025f;
 c->look_sensitivity_x=c->look_sensitivity_y=2;
 for(a=0;a<NI_ACTION_COUNT;a++) bind(c,a,NI_KEYBOARD,NI_KEY,keys[a],1,0);
 for(a=0;a<16;a++) bind(c,a,NI_GAMEPAD,a<14?NI_PAD_BUTTON:NI_PAD_AXIS,pads[a],1,0);
 for(a=NI_MOVE_UP;a<NI_ACTION_COUNT;a++) {
  int axis=a<NI_LOOK_UP?(a<NI_MOVE_LEFT?1:0):(a<NI_LOOK_LEFT?3:2);
  int sign=(a==NI_MOVE_UP||a==NI_MOVE_LEFT||a==NI_LOOK_UP||a==NI_LOOK_LEFT)?-1:1;
  bind(c,a,NI_GAMEPAD,NI_PAD_AXIS,axis,sign,0);
 }
 bind(c,NI_MOVE_UP,NI_KEYBOARD,NI_KEY,96,1,1); bind(c,NI_MOVE_DOWN,NI_KEYBOARD,NI_KEY,90,1,1);
 bind(c,NI_MOVE_LEFT,NI_KEYBOARD,NI_KEY,92,1,1); bind(c,NI_MOVE_RIGHT,NI_KEYBOARD,NI_KEY,94,1,1);
}
void ni_config_pc_defaults(NiConfig *c) {
 ni_config_defaults(c); c->mouse_camera=1;
 /* Keep the existing Z/X/C/V row while adding conventional PC alternatives.
  * Values map to original Xbox buttons; action names remain title-audited. */
 bind(c,NI_A,NI_KEYBOARD,NI_KEY,44,1,1);     /* Space */
 bind(c,NI_B,NI_KEYBOARD,NI_KEY,8,1,1);      /* E */
 bind(c,NI_X,NI_KEYBOARD,NI_MOUSE_BUTTON,1,1,1);
 bind(c,NI_Y,NI_KEYBOARD,NI_MOUSE_BUTTON,3,1,1);
 bind(c,NI_BLACK,NI_KEYBOARD,NI_KEY,9,1,0);  /* F: avoid E collision */
 bind(c,NI_START,NI_KEYBOARD,NI_KEY,41,1,1); /* Escape */
 bind(c,NI_BACK,NI_KEYBOARD,NI_KEY,16,1,1);  /* M */
}
static int binding_valid(NiBinding b,int device) {
 if(b.kind==NI_NONE) return b.code==0 && b.sign==0;
 if(b.sign!=1 && b.sign!=-1) return 0;
 switch(b.kind) {
 case NI_KEY:return device==NI_KEYBOARD && b.code>=0 && b.code<512;
 case NI_MOUSE_BUTTON:return device==NI_KEYBOARD && b.code>=1 && b.code<=32;
 case NI_MOUSE_WHEEL:return device==NI_KEYBOARD && b.code==0;
 case NI_PAD_BUTTON:return device==NI_GAMEPAD && b.code>=0 && b.code<32;
 case NI_PAD_AXIS:return device==NI_GAMEPAD && b.code>=0 && b.code<6;
 default:return 0;
 }
}
int ni_config_valid(const NiConfig *c) {
 int t,d,a,s; if(!c || c->version!=NI_CONFIG_VERSION) return 0;
 #define RANGE(v,l,h) if(!isfinite(c->v)||c->v<(l)||c->v>(h)) return 0
 RANGE(left_inner,0,.95f); RANGE(right_inner,0,.95f); RANGE(left_outer,.01f,1); RANGE(right_outer,.01f,1);
 if(c->left_inner>=c->left_outer||c->right_inner>=c->right_outer) return 0;
 RANGE(trigger_threshold,0,.99f); RANGE(mouse_sensitivity_x,0,10); RANGE(mouse_sensitivity_y,0,10);
 RANGE(look_sensitivity_x,0,100); RANGE(look_sensitivity_y,0,100);
 #undef RANGE
 if((unsigned)c->prompt_family>NI_FAMILY_GENERIC || (unsigned)c->mouse_camera>1 || (unsigned)c->mouse_invert_x>1 || (unsigned)c->mouse_invert_y>1 || (unsigned)c->pad_invert_x>1 || (unsigned)c->pad_invert_y>1) return 0;
 for(t=0;t<NI_CONTEXT_COUNT;t++) for(d=0;d<NI_DEVICE_COUNT;d++) for(a=0;a<NI_ACTION_COUNT;a++) for(s=0;s<2;s++) if(!binding_valid(c->bindings[t][d][a][s],d)) return 0;
 return 1;
}
void ni_deadzone(float x,float y,float inner,float outer,float *ox,float *oy) {
 float r,k; *ox=*oy=0; if(!isfinite(x)||!isfinite(y)||!isfinite(inner)||!isfinite(outer)||inner<0||outer<=inner) return;
 r=sqrtf(x*x+y*y); if(r<=inner || !isfinite(r)) return; k=clamp((r-inner)/(outer-inner),0,1)/r; *ox=x*k; *oy=y*k;
}
static float value(NiBinding b,const NiRawState *r) {
 switch(b.kind) {
 case NI_KEY:return r->keys[b.code]?1:0;
 case NI_MOUSE_BUTTON:return (r->mouse_buttons & (1u<<(b.code-1)))?1:0;
 case NI_MOUSE_WHEEL:return r->mouse_wheel*b.sign>0?1:0;
 case NI_PAD_BUTTON:return (r->pad_buttons&(1u<<b.code))?1:0;
 case NI_PAD_AXIS:return clamp(r->axes[b.code]*b.sign,0,1);
 default:return 0;
 }
}
void ni_evaluate(const NiConfig *c,const NiRawState *r,NiContext t,float seconds,NiOutput *o) {
 float v[2][NI_ACTION_COUNT]={{0}}, sticks[2][4]={{0}}, axis[4]; int d,a,s,k;
 static const uint16_t masks[8]={1,2,4,8,16,32,64,128};
 memset(o,0,sizeof *o); if(!r||!r->focused||!ni_config_valid(c)||(unsigned)t>=NI_CONTEXT_COUNT) return;
 for(d=0;d<2;d++) for(a=0;a<NI_ACTION_COUNT;a++) for(s=0;s<2;s++) {
  float f=value(c->bindings[t][d][a][s],r); if(a==NI_LEFT_TRIGGER||a==NI_RIGHT_TRIGGER) f=clamp((f-c->trigger_threshold)/(1-c->trigger_threshold),0,1);
  if(f>v[d][a]) v[d][a]=f;
 }
 for(a=0;a<16;a++) { float f=fmaxf(v[0][a],v[1][a]); if(a<8) { if(f>.5f)o->buttons|=masks[a]; } else { int index=a-8; o->analog[index]=(uint8_t)(f*255+.5f); } }
 for(d=0;d<2;d++) for(k=0;k<2;k++) {
  int base=k?NI_LOOK_UP:NI_MOVE_UP; float x=v[d][base+3]-v[d][base+2],y=v[d][base]-v[d][base+1];
  ni_deadzone(x,y,d?(k?c->right_inner:c->left_inner):0,d?(k?c->right_outer:c->left_outer):1,&sticks[d][k*2],&sticks[d][k*2+1]);
 }
 for(k=0;k<2;k++) {
  int pad=(sticks[1][2*k]!=0||sticks[1][2*k+1]!=0), keyboard=(sticks[0][2*k]!=0||sticks[0][2*k+1]!=0);
  d=pad && (!keyboard||r->pad_activity[k]>=r->keyboard_activity[k]);
  axis[k*2]=sticks[d][k*2]; axis[k*2+1]=sticks[d][k*2+1];
  if(k && d) { if(c->pad_invert_x)axis[2]=-axis[2]; if(c->pad_invert_y)axis[3]=-axis[3]; }
 }
 o->lx=(int16_t)(axis[0]*32767); o->ly=(int16_t)(axis[1]*32767); o->rx=(int16_t)(axis[2]*32767); o->ry=(int16_t)(axis[3]*32767);
 if(t==NI_GAMEPLAY) {
  seconds=clamp(seconds,0,.1f); o->camera_yaw=axis[2]*c->look_sensitivity_x*seconds; o->camera_pitch=axis[3]*c->look_sensitivity_y*seconds;
  if(c->mouse_camera) { o->camera_yaw+=clamp(r->mouse_dx,-10000,10000)*c->mouse_sensitivity_x*(c->mouse_invert_x?-1:1); o->camera_pitch-=clamp(r->mouse_dy,-10000,10000)*c->mouse_sensitivity_y*(c->mouse_invert_y?-1:1); }
 }
}
const char *ni_action_name(NiAction a) {
 static const char *names[]={"D-pad up","D-pad down","D-pad left","D-pad right","Start","Back","Left stick click","Right stick click","Guest A","Guest B","Guest X","Guest Y","Guest Black","Guest White","Left trigger","Right trigger","Move up","Move down","Move left","Move right","Look up","Look down","Look left","Look right"};
 return (unsigned)a<NI_ACTION_COUNT?names[a]:"Unknown action";
}
void ni_binding_label(NiBinding b,NiFamily f,char *l,size_t n) {
 static const char *xbox[]={"A","B","X","Y"},*ps[]={"Cross","Circle","Square","Triangle"},*nin[]={"B","A","Y","X"};
 if(!n)return; l[0]=0;
 if(b.kind==NI_NONE)snprintf(l,n,"Unbound");
 else if(b.kind==NI_KEY) { if(b.code>=4&&b.code<=29)snprintf(l,n,"%c",'A'+b.code-4); else if(b.code>=30&&b.code<=38)snprintf(l,n,"%d",b.code-29); else if(b.code==39)snprintf(l,n,"0"); else { const char *s=b.code==40?"Enter":b.code==41?"Escape":b.code==42?"Backspace":b.code==44?"Space":b.code==79?"Right":b.code==80?"Left":b.code==81?"Down":b.code==82?"Up":b.code==225?"Left Shift":b.code==224?"Left Ctrl":NULL; if(s)snprintf(l,n,"%s",s);else snprintf(l,n,"Scancode %d",b.code); } }
 else if(b.kind==NI_MOUSE_BUTTON)snprintf(l,n,"Mouse %d",b.code);
 else if(b.kind==NI_MOUSE_WHEEL)snprintf(l,n,"Wheel %s",b.sign>0?"up":"down");
 else if(b.kind==NI_PAD_BUTTON) { if(b.code<4 && b.code>=0 && f!=NI_FAMILY_GENERIC)snprintf(l,n,"%s",(f==NI_FAMILY_PLAYSTATION?ps:f==NI_FAMILY_NINTENDO?nin:xbox)[b.code]); else { static const char *names[]={"Back","Guide","Start","Left stick","Right stick","Left shoulder","Right shoulder","D-pad up","D-pad down","D-pad left","D-pad right"}; if(f!=NI_FAMILY_GENERIC&&b.code>=4&&b.code<=14)snprintf(l,n,"%s",names[b.code-4]);else snprintf(l,n,"Button %d",b.code+1); } }
 else if(b.kind==NI_PAD_AXIS)snprintf(l,n,"Axis %d %c",b.code+1,b.sign<0?'-':'+');
 else snprintf(l,n,"Unknown");
}
int ni_binding_conflict(const NiConfig *c,NiContext t,NiDevice d,NiAction a,int slot,NiBinding b) {
 int i,s; if(!c||(unsigned)t>=NI_CONTEXT_COUNT||(unsigned)d>=NI_DEVICE_COUNT||b.kind==NI_NONE)return 0;
 for(i=0;i<NI_ACTION_COUNT;i++)for(s=0;s<2;s++) { NiBinding other=c->bindings[t][d][i][s]; if((i!=(int)a||s!=slot)&&other.kind==b.kind&&other.code==b.code&&other.sign==b.sign)return 1; }return 0;
}
