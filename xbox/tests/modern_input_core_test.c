#include "../src/controls/input_core.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
int main(void) {
 NiConfig c,d; NiRawState r={0}; NiOutput o; float x,y; char label[64]; FILE *f;
 ni_config_defaults(&c); assert(ni_config_valid(&c)); r.focused=1;
 /* xinput_xbox.h ABI: BLACK=4, WHITE=5; legacy left shoulder is Black. */
 r.pad_buttons=1u<<9; ni_evaluate(&c,&r,NI_GAMEPLAY,.016f,&o); assert(o.analog[4]==255 && o.analog[5]==0); r.pad_buttons=0;
 r.keys[29]=1; ni_evaluate(&c,&r,NI_GAMEPLAY,0.016f,&o); assert(o.analog[0]==255);
 r.keys[29]=0; r.keys[26]=r.keys[7]=1; ni_evaluate(&c,&r,NI_GAMEPLAY,0.016f,&o);
 assert(o.lx>23000 && o.lx<23200 && o.ly==o.lx);
 r.keys[4]=1; ni_evaluate(&c,&r,NI_GAMEPLAY,0.016f,&o); assert(o.lx==0 && o.ly==32767);
 r.focused=0; ni_evaluate(&c,&r,NI_GAMEPLAY,0.016f,&o); assert(o.ly==0 && o.analog[0]==0);
 ni_deadzone(.1f,0,.2f,.95f,&x,&y); assert(x==0 && y==0);
 ni_deadzone(1,1,.2f,.95f,&x,&y); assert(fabsf(x-.7071067f)<.0001f);
 ni_deadzone(.575f,0,.2f,.95f,&x,&y); assert(fabsf(x-.5f)<.0001f);
 ni_deadzone(NAN,0,.2f,.95f,&x,&y); assert(x==0 && y==0);
 memset(&r,0,sizeof r); r.focused=1; r.axes[4]=.5f;
 ni_evaluate(&c,&r,NI_GAMEPLAY,.016f,&o); assert(o.analog[6]>100 && o.analog[6]<140);
 r.axes[3]=-.8f; c.pad_invert_y=1; ni_evaluate(&c,&r,NI_GAMEPLAY,.016f,&o); assert(o.ry<0);
 r.axes[0]=.6f; r.keys[4]=1; r.pad_activity[0]=1; r.keyboard_activity[0]=2;
 ni_evaluate(&c,&r,NI_GAMEPLAY,.016f,&o); assert(o.lx==-32767);
 r.pad_activity[0]=3; ni_evaluate(&c,&r,NI_GAMEPLAY,.016f,&o); assert(o.lx>0 && o.lx<32767);
 r.axes[4]=.01f; ni_evaluate(&c,&r,NI_GAMEPLAY,.016f,&o); assert(o.analog[6]==0);
 r.axes[4]=1; ni_evaluate(&c,&r,NI_GAMEPLAY,.016f,&o); assert(o.analog[6]==255);
 c.bindings[NI_MENU][NI_KEYBOARD][NI_A][0]=(NiBinding){NI_KEY,40,1};
 r.keys[29]=1; ni_evaluate(&c,&r,NI_MENU,.016f,&o); assert(o.analog[0]==0);
 r.keys[40]=1; ni_evaluate(&c,&r,NI_MENU,.016f,&o); assert(o.analog[0]==255);
 ni_binding_label((NiBinding){NI_PAD_BUTTON,0,1},NI_FAMILY_PLAYSTATION,label,sizeof label); assert(strcmp(label,"Cross")==0);
 ni_binding_label((NiBinding){NI_KEY,26,1},NI_FAMILY_KEYBOARD,label,sizeof label); assert(strcmp(label,"W")==0);
 ni_binding_label((NiBinding){NI_PAD_BUTTON,0,1},NI_FAMILY_NINTENDO,label,sizeof label); assert(strcmp(label,"B")==0);
 ni_binding_label((NiBinding){NI_PAD_BUTTON,15,1},NI_FAMILY_GENERIC,label,sizeof label); assert(strcmp(label,"Button 16")==0);
 assert(ni_binding_conflict(&c,NI_GAMEPLAY,NI_KEYBOARD,NI_B,0,(NiBinding){NI_KEY,29,1}));
 assert(ni_config_save(&c,"modern_input_test.ini")); assert(ni_config_load(&d,"modern_input_test.ini")); assert(memcmp(&c,&d,sizeof c)==0);
 f=fopen("modern_input_test.ini","w"); assert(f); fputs("version=999\n",f); fclose(f);
 assert(!ni_config_load(&d,"modern_input_test.ini")); assert(ni_config_valid(&d));
 f=fopen("modern_input_test.ini","r"); assert(f); assert(fgetc(f)=='v'); fclose(f);
 f=fopen("modern_input_test.ini","w"); assert(f); fputs("version=1\nleft_inner=nan\n",f); fclose(f);
 assert(!ni_config_load(&d,"modern_input_test.ini")); remove("modern_input_test.ini");
 ni_config_pc_defaults(&d); memset(&r,0,sizeof r); r.focused=1; r.mouse_dx=10; r.mouse_dy=5;
 r.mouse_buttons=1; r.keys[44]=1; r.keys[8]=1;
 ni_evaluate(&d,&r,NI_GAMEPLAY,.01f,&o);
 assert(o.analog[0]==255 && o.analog[1]==255 && o.analog[2]==255 && o.analog[4]==0);
 r.mouse_buttons=0; r.keys[44]=r.keys[8]=0;
 ni_evaluate(&d,&r,NI_GAMEPLAY,.01f,&o); x=o.camera_yaw; y=o.camera_pitch;
 ni_evaluate(&d,&r,NI_GAMEPLAY,.08f,&o); assert(o.camera_yaw==x && o.camera_pitch==y && x>0 && y<0);
 d.mouse_invert_x=d.mouse_invert_y=1; ni_evaluate(&d,&r,NI_GAMEPLAY,.01f,&o); assert(o.camera_yaw==-x && o.camera_pitch==-y);
 ni_evaluate(&d,&r,NI_MENU,.01f,&o); assert(o.camera_yaw==0 && o.camera_pitch==0);
 d.bindings[NI_GAMEPLAY][NI_KEYBOARD][NI_A][1]=(NiBinding){NI_MOUSE_WHEEL,0,1}; r.mouse_wheel=1;
 ni_evaluate(&d,&r,NI_GAMEPLAY,.01f,&o); assert(o.analog[0]==255);
 c.left_inner=NAN; assert(!ni_config_valid(&c)); assert(!ni_config_save(&c,"modern_input_test.ini"));
 puts("modern input core: passed"); return 0;
}
