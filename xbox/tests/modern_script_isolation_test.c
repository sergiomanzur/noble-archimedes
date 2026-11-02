/* Compile this one translation unit: it includes the real USB device, replacing
 * only the host input source and clock so reports are deterministic. */
#define _CRT_SECURE_NO_WARNINGS
#include "input/xinput_xbox.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x);exit(1);} } while(0)
static DWORD fake_clock=1000;
static int physical_reads;
static DWORD fake_tick(void){return fake_clock;}
DWORD xbox_InputGetState(DWORD port,XBOX_INPUT_STATE *state){
 (void)port;physical_reads++;memset(state,0,sizeof *state);
 state->Gamepad.wButtons=XBOX_GAMEPAD_DPAD_LEFT;state->Gamepad.bAnalogButtons[XBOX_BUTTON_A]=255;
 state->Gamepad.sThumbLX=12345;state->Gamepad.sThumbRY=-23456;return ERROR_SUCCESS;
}
int xbox_FramebufferKeyDown(int vk){(void)vk;return 0;}
#define GetTickCount fake_tick
#include "../../tools/xboxrecomp/src/usb/usb_gamepad.c"
#undef GetTickCount
static void check_neutral_payload(const uint8_t *report,int digital,int analog_index){int i;
 CHECK(report[0]==0&&report[1]==20&&report[2]==digital&&report[3]==0);
 for(i=4;i<20;i++)CHECK(report[i]==(i==analog_index?255:0));
}
int main(int argc,char **argv){uint8_t report[20];int scripted,live;char live_path[MAX_PATH]={0};
 CHECK(argc==2);scripted=!strcmp(argv[1],"script");live=!strcmp(argv[1],"live");
 CHECK(scripted||live||!strcmp(argv[1],"physical"));
 _putenv_s("RECOMP_PAD_SCRIPT",scripted?"0:start,200:p2-b:100":"");
 _putenv_s("RECOMP_PAD_PRESS","");_putenv_s("RECOMP_PAD_FRAMES","");_putenv_s("RECOMP_INPUT_DIAG",scripted?"1":"");
 _putenv_s("RECOMP_PAD_LIVE","");
 if(live){char temp[MAX_PATH];FILE *file;CHECK(GetTempPathA(sizeof temp,temp));CHECK(GetTempFileNameA(temp,"nsi",0,live_path));file=fopen(live_path,"wb");CHECK(file);CHECK(fputs("b\n",file)>=0);CHECK(!fclose(file));_putenv_s("RECOMP_PAD_LIVE",live_path);}
 CHECK(usb_gamepad_report(0,report,sizeof report)==20);
 if(scripted){check_neutral_payload(report,XBOX_GAMEPAD_START,-1);CHECK(physical_reads==0);
  CHECK(usb_gamepad_report(1,report,sizeof report)==20);check_neutral_payload(report,0,-1);CHECK(physical_reads==0);
  fake_clock+=200;CHECK(usb_gamepad_report(1,report,sizeof report)==20);check_neutral_payload(report,0,5);CHECK(physical_reads==0);
  CHECK(usb_gamepad_report(0,report,sizeof report)==20);check_neutral_payload(report,0,-1);CHECK(physical_reads==0);
 }else{CHECK(physical_reads==1);CHECK(report[2]==XBOX_GAMEPAD_DPAD_LEFT&&report[4]==255);CHECK(report[12]==0x39&&report[13]==0x30);CHECK(report[18]==0x60&&report[19]==0xa4);if(live)CHECK(report[5]==255);}
 if(live){_putenv_s("RECOMP_PAD_LIVE","");CHECK(!remove(live_path));}
 printf("modern_script_isolation_test (%s): PASS\n",argv[1]);return 0;
}
