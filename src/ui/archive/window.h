/*提供一些二次封装通用创建控件接口.用于lvgl 和lvgl 初始化的简单接口
 */

#ifndef WINDOW_H
#define WINDOW_H
#ifdef __cplusplus
extern "C" {
#endif
#include <lvgl/lvgl.h>
#include "stringlist.h"
#include "string_s.h"

typedef enum{
    control_button,
    control_edit,
    control_lable,
    control_dropdown,
    control_checkbox,
    control_radiobutton,
    control_obj
}control_type;
typedef  struct window{

    const char *(*app_path)();
    const char *(*app_path_a)();

    lv_obj_t * (*create_msgbox)(const char *title,
                                const char *msg,
                                lv_event_cb_t event_cb,
                                void *user_data);

    lv_obj_t *(*create_line_edit)(lv_obj_t *parent,
                                  const char * place_text);

    lv_obj_t *(*create_lable)(lv_obj_t *parent,
                              const char *text);

    lv_obj_t *(*create_dropdown)(lv_obj_t *parent , string_list *items,
                                 lv_event_cb_t event_cb,
                                 void *user_data);

    lv_obj_t * (*create_gif)(lv_obj_t * parent,
                             const char *filename);

    lv_obj_t *(*create_arc)(lv_obj_t *parent,
                            const char *text,
                            lv_event_cb_t event_cb,
                            void *user_data);

    lv_obj_t *(*create_button)(lv_obj_t *parent,
                               const char *text,
                               lv_event_cb_t event_cb,
                               void *user_data);

    lv_anim_t (*create_anim)(lv_obj_t *obj,
                             lv_anim_exec_xcb_t exec_cb,
                             int start_value,
                             int end_value,
                             int during,
                             int start_delay,
                             int repeate_count);

    lv_font_t *(*create_font)(const char *fileName,
                              uint32_t size,
                              lv_freetype_font_style_t font_style);

    lv_obj_t *(*create_image)(lv_obj_t *parent,
                              const void *filename);

    lv_obj_t * (* create_slider)(lv_obj_t *parent ,
                                 int min,
                                 int max,
                                 lv_event_cb_t event_cb,
                                 void *user_data);

    lv_obj_t *(*create_keyboard)(lv_obj_t *parnet,
                                 bool bPinyin);

    lv_obj_t * (* create_checkbox)(lv_obj_t *parent ,
                                   const char *text,
                                   lv_event_cb_t event_cb,
                                   void *user_data);

    lv_obj_t * (*create_radiobutton)(lv_obj_t *parent ,
                                     const char *text,
                                     lv_event_cb_t event_cb,
                                     void *user_data);

    lv_obj_t *(*create_obj)(lv_obj_t *parent);

    lv_obj_t *(*create_control)(lv_obj_t *parent, control_type t,  void *text, lv_event_cb_t callback, void *userdata);

    void (*set_margin_border)(lv_obj_t *o, int margin,  int border);

    lv_obj_t *kb;

    //创建的button 样式
    lv_style_t style_btn;
    lv_style_t style_btn_press;
    lv_style_t style_btn_red;
    lv_style_t style_btn_brown;
    lv_style_t style_btn_disable;

    //初始化时创建两种字体
    lv_font_t *font_songti;
    lv_font_t *font_cjk_full;

    //private.
    string_s *path;
    string_s *apath;
    bool runing;
}window;

extern window g_window;

extern void init_window_create(int argc, char *argv[]);

typedef  void (*window_destory)(void *user_data);
extern void exec_event(window_destory destory, void *user_data);

extern void quit_event();

#ifdef __cplusplus
}
#endif

#endif // WINDOW_H
