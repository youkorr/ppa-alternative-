#include "lvgl.h"
#include "lv_draw_ortho.h"
#include "lv_draw_ortho_ppa.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static uint32_t ticks(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec*1000+t.tv_nsec/1000000;}
static void flush(lv_display_t*d,const lv_area_t*a,uint8_t*p){lv_display_flush_ready(d);}
int main(int argc,char**argv){
 if(argc<3)return 2;int scene=atoi(argv[2]);lv_init();lv_tick_set_cb(ticks);lv_draw_ortho_init();lv_draw_ortho_ppa_init();if(argc>3 && atoi(argv[3]))lv_draw_ortho_set_fast_paths(false);
 const int nw=720,nh=1280;uint32_t stride=lv_draw_buf_width_to_stride(nw,LV_COLOR_FORMAT_RGB565);size_t bytes=stride*nh;
 uint8_t *data=aligned_alloc(128,(bytes+127)/128*128);memset(data,0,bytes);
 lv_display_t*d=lv_display_create(nw,nh);lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers_with_stride(d,data,NULL,bytes,stride,LV_DISPLAY_RENDER_MODE_DIRECT);lv_display_set_flush_cb(d,flush);lv_display_set_rotation(d,LV_DISPLAY_ROTATION_90);lv_display_set_matrix_rotation(d,true);lv_display_set_software_object_transforms(d,true);
 lv_obj_t*s=lv_display_get_screen_active(d);lv_obj_set_style_bg_color(s,lv_color_hex(0x152540),0);lv_obj_set_style_bg_grad_color(s,lv_color_hex(0x506080),0);lv_obj_set_style_bg_grad_dir(s,LV_GRAD_DIR_VER,0);lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
 for(int i=0;i<(scene==2?0:48);i++){lv_obj_t*o=lv_obj_create(s);lv_obj_set_pos(o,12+(i%8)*158,10+(i/8)*115);lv_obj_set_size(o,146,105);lv_obj_set_style_radius(o,9,0);lv_obj_set_style_bg_color(o,lv_color_hex(0x203050+i*1593),0);lv_obj_set_style_border_width(o,2,0);lv_obj_set_style_shadow_width(o,6,0);lv_obj_t*l=lv_label_create(o);lv_label_set_text_fmt(l,"Card %d",i);lv_obj_set_pos(l,4,5);}
 if(scene==1){lv_obj_t*o=lv_obj_create(s);lv_obj_set_pos(o,50,40);lv_obj_set_size(o,1180,640);lv_obj_set_style_radius(o,12,0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);lv_obj_set_style_bg_color(o,lv_color_hex(0x456789),0);lv_obj_set_style_shadow_width(o,8,0);lv_obj_t*l=lv_label_create(o);lv_label_set_text(l,"A large overlay");lv_obj_set_pos(l,50,50);}

 if(scene==2){
  lv_obj_t*o=lv_obj_create(s);lv_obj_remove_style_all(o);lv_obj_set_pos(o,100,100);lv_obj_set_size(o,50,50);lv_obj_set_style_bg_color(o,lv_color_hex(0xf80000),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
  static uint16_t px[50*50];for(int i=0;i<50*50;i++)px[i]=0x07e0;
  static lv_image_dsc_t im;im=(lv_image_dsc_t){.header={.magic=LV_IMAGE_HEADER_MAGIC,.cf=LV_COLOR_FORMAT_RGB565,.w=50,.h=50,.stride=100},.data_size=sizeof(px),.data=(uint8_t*)px};
  lv_obj_t*i=lv_image_create(s);lv_image_set_src(i,&im);lv_obj_set_pos(i,100,100);lv_matrix_t m;lv_matrix_identity(&m);lv_matrix_translate(&m,100,0);lv_obj_set_transform(i,&m);
 }
 lv_refr_now(d);lv_draw_ortho_reset_stats();lv_obj_invalidate(s);lv_refr_now(d);const lv_draw_ortho_stats_t*z=lv_draw_ortho_get_stats();
 printf("scene=%d batches=%u tiles=%u unread=%u general=%u drawn=%llu turned=%llu waits=%u\n",scene,z->batches,z->tiles,z->tiles_unread,z->tasks_general,(unsigned long long)z->bytes_drawn,(unsigned long long)z->bytes_turned,z->waits);
 FILE*f=fopen(argv[1],"wb");fwrite(data,1,bytes,f);fclose(f);lv_display_delete(d);free(data);lv_deinit();return 0;}
