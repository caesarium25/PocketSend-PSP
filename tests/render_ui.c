/* Native renderer preview: same pixel renderer and embedded assets as PSP. */
#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void save(const char *name){
 FILE *f=fopen(name,"wb");if(!f)exit(1);
 fprintf(f,"P6\n480 272\n255\n");
 for(int i=0;i<480*272;i++){unsigned char p[3]={gfx_pixels[i]&255,(gfx_pixels[i]>>8)&255,(gfx_pixels[i]>>16)&255};fwrite(p,1,3,f);}
 fclose(f);
}
int main(int argc,char **argv){
 if(argc!=2)return 1;
 char p[1024];
 gfx_home("192.168.43.123","ms0:/DOWNLOADS",2,0,0);
 snprintf(p,sizeof p,"%s/home.ppm",argv[1]);save(p);
 gfx_screen("Choose destination");
 gfx_text(20,54,"ms0:/",0,MUTED,335);
 gfx_text(370,54,"2 / 8",0,MUTED,90);
 const char *names[]={"DOWNLOADS","MUSIC","PICTURE","PSP","VIDEO"};
 for(int i=0;i<5;i++)gfx_item(i,names[i],"",i==1,1,0);
 gfx_hint("X  Open   Start  Use folder   O  Back","Triangle  Favorites     Square  More controls");
 snprintf(p,sizeof p,"%s/folders.ppm",argv[1]);save(p);
 gfx_progress("Summer playlist.mp3",64,6.4,10,240,15,2,3,14.4,27);
 snprintf(p,sizeof p,"%s/transfer.ppm",argv[1]);save(p);
 gfx_screen("Incoming files");
 gfx_text(20,58,"From: Aji's phone",0,INK,440);
 gfx_text(20,84,"3 files / 27.00 MiB",0,MUTED,440);
 gfx_round(18,111,444,76,8,PANEL);
 gfx_text(30,116,"SAVE TO",0,MUTED,400);
 gfx_marquee(30,140,"ms0:/MUSIC",1,INK,416,0);
 gfx_round(18,190,444,29,6,PANEL);
 gfx_text(20,194,"Summer playlist.mp3",0,INK,440);
 gfx_hint("X  Accept   O  Reject   Triangle  Folder","Square  Suggest folder    R  Review files");
 snprintf(p,sizeof p,"%s/incoming.ppm",argv[1]);save(p);
 gfx_screen("Device IPv4 address");
 gfx_round(18,55,444,30,6,PANEL);
 gfx_text(28,58,"192.168.43.1",1,MINT,404);
 const char *chars="abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._-:";
 for(int i=0;chars[i];i++){int x=18+(i%14)*32,y=92+(i/14)*25;
  gfx_round(x,y,28,22,4,i==58?SELECTED:PANEL);char c[2]={chars[i],0};
  gfx_text(x+7,y,c,1,i==58?MINT:INK,24);
  if(chars[i]==' ')gfx_rect(x+8,y+17,12,2,MUTED);
 }
 gfx_hint("X  Type     Square  Delete     Start  Done","D-pad  Move                     O  Cancel");
 snprintf(p,sizeof p,"%s/keyboard.ppm",argv[1]);save(p);
 gfx_screen("Browser controls");gfx_round(18,67,444,142,10,PANEL);
 gfx_wrap(32,82,"L: New folder. R: Add favorite. Select: Cancel. Up/Down: Scroll. Maximum 512 entries per folder.",1,INK,416,4);
 gfx_hint("X / O  Back","");
 snprintf(p,sizeof p,"%s/help.ppm",argv[1]);save(p);
 puts("Rendered 6 native-resolution UI previews using production renderer.");
 return 0;
}
