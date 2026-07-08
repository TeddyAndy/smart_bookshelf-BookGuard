#include "window.h"

#include <lvgl/lvgl.h>
#include <lvgl/demos/scroll/lv_demo_scroll.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/select.h>
#ifndef LVGL_SIMULATION
#include <linux/input.h>
#include <libevdev/libevdev.h>
#endif
#include <unistd.h>
#include <stdbool.h>
#include <signal.h>
#include <libgen.h>
#include "mouse_cursor.h"

/* 预生成的中文位图字体 (替换 TTF FreeType 渲染) */
extern const lv_font_t font_songti_24;

window g_window;

static void window_message_box_show(lv_event_t *e){

    lv_obj_t *btn = lv_event_get_target(e);
    //lv_obj_t * label = lv_obj_get_child(btn, 0);
    lv_obj_t *msg  = lv_obj_get_user_data(btn);
    int32_t index = lv_obj_get_index(btn);
    switch (index) {
    case 0:
        printf("确定\n");
        break;
    case 1:
        printf("取消\n");
        break;
    default:
        break;
    }
    lv_msgbox_close(msg);
}

static lv_obj_t * window_create_msgbox(const char *title,
                                       const char *msg,
                                       lv_event_cb_t event_cb,
                                       void *user_data)
{
    lv_obj_t * mbox1 = lv_msgbox_create(NULL);
    lv_obj_set_size(mbox1, 300, 240);

    lv_obj_t *t =lv_msgbox_add_title(mbox1, title);
    lv_obj_set_style_text_color(t, lv_color_make(8,8,8), 0);
    lv_obj_set_style_text_font(t, g_window.font_songti, 0);
    lv_msgbox_add_close_button(mbox1);

    lv_obj_t *context_text = lv_msgbox_add_text(mbox1, msg);
    lv_obj_center(context_text);
    lv_obj_set_style_text_font(context_text, g_window.font_songti, 0);

    lv_obj_t * btn = lv_msgbox_add_footer_button(mbox1, "确定");
    lv_obj_set_style_text_font(btn, g_window.font_songti, 0);
    if(event_cb)
        lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, user_data);
    else
        lv_obj_add_event_cb(btn, window_message_box_show, LV_EVENT_CLICKED, NULL);

    lv_obj_set_user_data(btn, mbox1);

    btn = lv_msgbox_add_footer_button(mbox1, "取消");
    lv_obj_set_style_text_font(btn, g_window.font_songti, 0);
    if(event_cb)
        lv_obj_add_event_cb(btn, event_cb, LV_EVENT_CLICKED, user_data);
    else
        lv_obj_add_event_cb(btn, window_message_box_show, LV_EVENT_CLICKED, NULL);

    lv_obj_set_user_data(btn, mbox1);

    lv_obj_t * titlebar = lv_msgbox_get_header(mbox1);
    lv_obj_set_height(titlebar, 60);

    lv_obj_t * footer = lv_msgbox_get_footer(mbox1);
    lv_obj_set_height(footer, 60);

    return mbox1;
}

static lv_obj_t * window_create_checkbox(lv_obj_t *parent,
                                         const char *text,
                                         lv_event_cb_t event_cb,
                                         void *user_data)
{
    lv_obj_t *obj = lv_checkbox_create(parent);
    lv_checkbox_set_text(obj, text);
    lv_obj_set_style_text_font(obj, g_window.font_songti, 0);
    lv_obj_set_size(obj, 120, 40);
    if(event_cb)
        lv_obj_add_event_cb(obj, event_cb, LV_EVENT_CLICKED, user_data);
    return obj;
}

static lv_obj_t * window_create_slider(lv_obj_t *parent ,
                                       int min,
                                       int max,
                                       lv_event_cb_t event_cb,
                                       void *user_data)
{
    lv_obj_t *obj = lv_slider_create(parent);
    lv_slider_set_range(obj, min, max);
    lv_obj_add_event_cb(obj, event_cb, LV_EVENT_VALUE_CHANGED, user_data);
    return obj;
}

static lv_color_t darken(const lv_color_filter_dsc_t * dsc, lv_color_t color, lv_opa_t opa)
{
    LV_UNUSED(dsc);
    return lv_color_darken(color, opa);
}

static void window_create_button_style(void)
{
    /*Create a simple button style*/
    lv_style_t *style_btn = &g_window.style_btn;
    lv_style_init(style_btn);
    lv_style_set_radius(style_btn, 10);
    lv_style_set_bg_opa(style_btn, LV_OPA_COVER);
    lv_style_set_bg_color(style_btn, lv_palette_lighten(LV_PALETTE_GREY, 3));
    lv_style_set_bg_grad_color(style_btn, lv_palette_main(LV_PALETTE_GREY));
    lv_style_set_bg_grad_dir(style_btn, LV_GRAD_DIR_VER);

    lv_style_set_border_color(style_btn, lv_color_black());
    lv_style_set_border_opa(style_btn, LV_OPA_20);
    lv_style_set_border_width(style_btn, 2);

    lv_style_set_text_color(style_btn, lv_color_black());

    /*Create a style for the pressed state.
     *Use a color filter to simply modify all colors in this state*/
    static lv_color_filter_dsc_t color_filter ;
    lv_color_filter_dsc_init(&color_filter, darken);

    lv_style_t *style_btn_pressed =& g_window.style_btn_press;
    lv_style_init(style_btn_pressed);
    lv_style_set_color_filter_dsc(style_btn_pressed, &color_filter);
    lv_style_set_color_filter_opa(style_btn_pressed, LV_OPA_20);

    /*Create a red style. Change only some colors.*/
    lv_style_t  *style_btn_red  = &g_window.style_btn_red;
    lv_style_init(style_btn_red);
    lv_style_set_bg_color(style_btn_red, lv_palette_main(LV_PALETTE_RED));
    lv_style_set_bg_grad_color(style_btn_red, lv_palette_lighten(LV_PALETTE_RED, 3));


    /*Create a red style. Change only some colors.*/
    lv_style_t  *style_btn_brown  = &g_window.style_btn_brown;
    lv_style_init(style_btn_brown);
    lv_style_set_bg_color(style_btn_brown, lv_palette_main(LV_PALETTE_BROWN));
    lv_style_set_bg_grad_color(style_btn_brown, lv_palette_lighten(LV_PALETTE_BROWN, 3));

    lv_style_t *disable = &g_window.style_btn_disable;
    lv_style_init(disable);
    lv_style_set_bg_color(disable, lv_palette_main(LV_PALETTE_GREY));

}

static lv_obj_t *window_create_button(lv_obj_t *parent,
                                      const char *text,
                                      lv_event_cb_t event_cb,
                                      void *user_data)
{
    lv_obj_t *click_btn = lv_button_create(parent);
    lv_obj_remove_style_all(click_btn);
    lv_obj_t *label = lv_label_create(click_btn);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, g_window.font_songti, 0);
    lv_obj_remove_style_all(click_btn);
    lv_obj_add_style(click_btn, &g_window.style_btn, 0);
    lv_obj_add_style(click_btn, &g_window.style_btn_red, 0);
    lv_obj_add_style(click_btn, &g_window.style_btn_press, LV_STATE_PRESSED);

    lv_obj_add_style(click_btn, &g_window.style_btn_press, LV_STATE_DISABLED);
    lv_obj_add_style(click_btn, &g_window.style_btn_disable, LV_STATE_DISABLED);

    lv_obj_set_size(click_btn, 150, 60);
    lv_obj_center(label);

    if(event_cb)
        lv_obj_add_event_cb(click_btn, event_cb, LV_EVENT_CLICKED, user_data);

    return click_btn;
}

static void ta_event_cb_pinyin(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = (lv_obj_t *)lv_event_get_target(e);
    lv_obj_t * kb = (lv_obj_t *)lv_event_get_user_data(e);

    if(code == LV_EVENT_FOCUSED) {
        if(lv_indev_get_type(lv_indev_active()) != LV_INDEV_TYPE_KEYPAD) {
            lv_keyboard_set_textarea(kb, ta);
            lv_obj_remove_flag(kb, LV_OBJ_FLAG_HIDDEN);
        }
    }
    else if(code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_state(ta, LV_STATE_FOCUSED);
        lv_indev_reset(NULL, ta);   /*To forget the last clicked object to make it focusable again*/
    }
}

static lv_obj_t* window_create_keyboard(lv_obj_t *parent,
                                        bool bPinyIn)
{

    lv_obj_t * kb = lv_keyboard_create(parent);

    if(bPinyIn){
        lv_obj_t * pinyin_ime = lv_ime_pinyin_create(parent);
        if (!g_window.font_cjk_full) {
            g_window.font_cjk_full = g_window.create_font("/root/STSONG.TTF", 24,
                                                          LV_FREETYPE_FONT_STYLE_NORMAL);
            if (!g_window.font_cjk_full) {
                g_window.font_cjk_full = g_window.font_songti;
            }
        }
        lv_obj_set_style_text_font(pinyin_ime, g_window.font_cjk_full, 0);
        lv_ime_pinyin_set_keyboard(pinyin_ime, kb);
        lv_obj_set_style_text_font(kb, g_window.font_songti, 0);
        lv_obj_t *cand_panel = lv_obj_get_child(pinyin_ime, 0);
        if (cand_panel) {
            lv_obj_set_style_text_font(cand_panel, g_window.font_cjk_full, 0);
            for (uint32_t i = 0; i < lv_obj_get_child_count(cand_panel); ++i) {
                lv_obj_t *child = lv_obj_get_child(cand_panel, i);
                lv_obj_set_style_text_font(child, g_window.font_cjk_full, 0);
            }
        }
    }
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);

    g_window.kb = kb;

    return kb;
}

static lv_obj_t * window_create_line_edit(lv_obj_t *parent,
                                          const char * place_text)
{

    lv_obj_t * edit = lv_textarea_create(parent);
    lv_textarea_set_one_line(edit, true);
    lv_obj_set_style_text_font(edit, g_window.font_songti, 0);
    lv_obj_set_size(edit, 200, 60);
    if(place_text)
        lv_textarea_set_placeholder_text(edit, place_text);

    lv_obj_add_event_cb(edit, ta_event_cb_pinyin, LV_EVENT_ALL, g_window.kb);
    lv_obj_set_scrollbar_mode(edit, LV_SCROLLBAR_MODE_OFF);

    return edit;
}

static lv_obj_t *window_create_lable(lv_obj_t *parent,
                                     const char *text)
{
    lv_obj_t *lable = lv_label_create(parent);
    lv_label_set_text(lable, text);
    lv_obj_set_style_text_font(lable, g_window.font_songti, 0);
    lv_obj_center(lable);
    return lable;
}

static lv_obj_t * window_create_gif(lv_obj_t *obj,
                                    const char *filename)
{
    lv_obj_t *gif = lv_gif_create(obj);
    lv_gif_set_src(gif ,filename);

    return gif;
}

//creat create_ani
static lv_anim_t  window_create_anim(lv_obj_t *obj,
                                     lv_anim_exec_xcb_t exec_cb,
                                     int start_value,
                                     int end_value,
                                     int during,
                                     int start_delay,
                                     int repeate_count){
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, exec_cb);
    lv_anim_set_values(&a, start_value, end_value);
    lv_anim_set_duration(&a, during);
    lv_anim_set_repeat_delay(&a, start_delay);
    lv_anim_set_repeat_count(&a, repeate_count);    /*Just for the demo*/

    return a;
}

static lv_obj_t* window_create_arc(lv_obj_t *parent,
                                   const char *text,
                                   lv_event_cb_t event_cb,
                                   void *user_data)
{
    /*Create an Arc*/
    lv_obj_t * arc = lv_arc_create(parent);
    lv_arc_set_value(arc, 0);
    lv_arc_set_rotation(arc, 125);
    lv_arc_set_bg_angles(arc, 0, 360);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);   /*Be sure the knob is not displayed*/
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);  /*To not allow adjusting by click*/
    lv_obj_align(arc, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_size(arc, 100, 100);

    lv_obj_t *lable = lv_label_create(arc);
    lv_label_set_text(lable, "");
    lv_obj_align(lable, LV_ALIGN_TOP_MID, 0, 50);

    if(event_cb)
        lv_obj_add_event_cb(arc, event_cb, LV_EVENT_VALUE_CHANGED, user_data);

    lv_obj_set_user_data(arc, lable);
    lable = lv_label_create(arc);
    lv_label_set_text(lable, text);
    lv_obj_align(lable, LV_ALIGN_BOTTOM_MID, 0, -20);

    /*Manually update the label for the first time*/
    lv_obj_send_event(arc, LV_EVENT_VALUE_CHANGED, NULL);

    return arc;
}

static lv_obj_t *window_create_dropdown(lv_obj_t *parent ,
                                        string_list *items,
                                        lv_event_cb_t event_cb,
                                        void *user_data)
{
    lv_obj_t * obj = lv_dropdown_create(parent);
    if(event_cb)
        lv_obj_add_event_cb(obj, event_cb, LV_EVENT_VALUE_CHANGED, user_data);
    lv_dropdown_clear_options(obj);

    static lv_style_t style;
    lv_style_init(&style);
    lv_style_set_text_font(&style, g_window.font_songti);
    lv_obj_add_style(obj, &style, 0);


    int count = g_list.count(items);
    for(int i=0; i<count; i++){
        const char *item = g_list.data(items, i);
        lv_dropdown_add_option(obj, item, i);
    }

    lv_obj_t *list =lv_dropdown_get_list(obj);
    lv_obj_set_style_width(list, 0, LV_PART_SCROLLBAR);
    lv_obj_set_style_text_font(list, g_window.font_songti, 0);
    for(uint32_t i=0; i<lv_obj_get_child_count(list); i++){
        lv_obj_t *child = lv_obj_get_child(list, i);
        lv_obj_set_style_text_font(child, g_window.font_songti, 0);
    }

    return obj;
}

static lv_font_t *window_create_font(const char *fileName,
                                     uint32_t size,
                                     lv_freetype_font_style_t font_style)
{
    // lvgl 字体使用Freetype 加载字体文件, 在内存受限制设备类，可以使用lvgl 在线工具生成固定个数的字体文件 exampes: lv_font_songti_16.c
    /*  font_style
 *  LV_FREETYPE_FONT_STYLE_NORMAL, 正常
    LV_FREETYPE_FONT_STYLE_ITALIC, 粗体
    LV_FREETYPE_FONT_STYLE_BOLD  斜体
*/
    //创建字体
    lv_font_t * font = lv_freetype_font_create(fileName,
                                               LV_FREETYPE_FONT_RENDER_MODE_BITMAP,
                                               size,
                                               font_style);
    return font;
}

static lv_obj_t *window_create_image(lv_obj_t *parent,
                                     const void *filename){
    lv_obj_t *image = lv_image_create(parent);
    lv_image_set_src(image, filename);
    lv_obj_center(image);
    return image;
}

static const char  * window_app_path()
{
    if(g_window.path == NULL){
        return "";
    }

    return g_strings.data(g_window.path);
}

// 加载中文字体: 使用编译进二进制的 LVGL 位图字体，避免板端 FreeType 渲染卡顿。
static void window_create_chinese_font()
{
    g_window.font_songti = (lv_font_t *)&font_songti_24;
    printf("[Font] bitmap font loaded: font_songti_24\n");
}

static const char *window_app_path_a()
{
    if(g_window.apath ==NULL)
        return "";

    return g_strings.data(g_window.apath);
}

static lv_obj_t *window_create_obj(lv_obj_t *parent){
    return lv_obj_create(parent);
}

static lv_obj_t * window_create_radiobutton(lv_obj_t *parent ,
                                            const char *text,
                                            lv_event_cb_t event_cb,
                                            void *user_data)
{
    static lv_style_t style_radio;
    lv_style_init(&style_radio);
    lv_style_set_radius(&style_radio, LV_RADIUS_CIRCLE);

    static lv_style_t style_radio_chk;
    lv_style_init(&style_radio_chk);
    lv_style_set_bg_image_src(&style_radio_chk, NULL);


    lv_obj_t * obj = window_create_checkbox(parent, text, event_cb, user_data);
    lv_checkbox_set_text(obj, text);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_style(obj, &style_radio, LV_PART_INDICATOR);
    lv_obj_add_style(obj, &style_radio_chk, LV_PART_INDICATOR | LV_STATE_CHECKED);

    return obj;
}

static lv_obj_t *window_create_control(lv_obj_t *parent, control_type t,  void *text, lv_event_cb_t callback, void *userdata){
    lv_obj_t *obj = NULL;
    switch (t) {
    case control_button:
        obj = g_window.create_button(parent, text, callback, userdata);
        break;
    case control_edit:
        obj = g_window.create_line_edit(parent, text);
        break;
    case control_lable:
        obj = g_window.create_lable(parent, text);
        break;
    case control_dropdown:{
        string_list *list = text;
        obj = g_window.create_dropdown(parent, list, callback, userdata);
    }break;
    case control_checkbox:
        obj = g_window.create_checkbox(parent, text, callback, userdata);
        break;

    case control_radiobutton:
        obj = g_window.create_radiobutton(parent, text, callback, userdata);
        break;
    default:
        obj = g_window.create_obj(parent);
        break;
    }

    return obj;
}

static void window_set_margin_border(lv_obj_t *o, int margin,  int border){
    lv_obj_set_style_border_width(o, border, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, margin, 0);
}

static void window_create_data(int argc, char *argv[]){
    LV_UNUSED(argc);
     LV_UNUSED(argv);

    char path[256];
    memset(path, 0, sizeof (path));
    if (!getcwd(path, sizeof(path))) {
        snprintf(path, sizeof(path), ".");
    }
    g_window.path= g_strings.create(path);

    //图片等需要格式: A+绝对路径
    g_window.apath = g_strings.create("A");

    g_strings.add(g_window.apath, g_window.app_path());
}

static void  window_destory_data()
{
    if(g_window.path)
        g_strings.destroy(g_window.path);

    if(g_window.apath)
        g_strings.destroy(g_window.apath);
}

void window_init_window(int argc, char *argv[]){
    g_window.create_msgbox = window_create_msgbox;
    g_window.create_line_edit = window_create_line_edit;
    g_window.create_lable = window_create_lable;
    g_window.create_dropdown = window_create_dropdown;
    g_window.create_gif =window_create_gif;
    g_window.create_arc = window_create_arc;
    g_window.create_button = window_create_button;
    g_window.create_anim = window_create_anim;
    g_window.create_font = window_create_font;
    g_window.create_image =window_create_image;
    g_window.create_slider = window_create_slider;
    g_window.create_keyboard = window_create_keyboard;
    g_window.create_checkbox = window_create_checkbox;
    g_window.create_radiobutton = window_create_radiobutton;
    g_window.create_obj = window_create_obj;
    g_window.create_control = window_create_control;
    g_window.app_path = window_app_path;
    g_window.set_margin_border = window_set_margin_border;
    g_window.app_path_a = window_app_path_a;
    g_window.kb = NULL;
    g_window.font_cjk_full = NULL;
    g_window.path = NULL;
    g_window.apath = NULL;
    g_window.runing = true;


    //此处只用于保存应用程序的绝对路径，
    window_create_data(argc, argv);

    //此处简单的创建两个中文字体
    window_create_chinese_font();

    //此处简单的创建button 的样式，开发者可以自定义好自己的一套样式
    window_create_button_style();

}

#ifndef LVGL_SIMULATION
//查找触摸设备.
static void find_touch_device_path(char *buf)
{
    if(buf == NULL)
        return;

    const char *path="/dev/input";
    DIR *d = opendir(path);
    if(d == NULL){
        printf("not no path: %s", path);
        return;
    }

    struct dirent *dirent = NULL;
    while((dirent = readdir(d)) != NULL){
        if(strstr(dirent->d_name, "event") == NULL)
            continue;

       // printf("input=%s\n", dirent->d_name);

        char filename[100];
        strcpy(filename, path);
        strcat(filename, "/");
        strcat(filename, dirent->d_name);
        int fd = open(filename, O_RDONLY);
        if(fd <=0){
            perror("open failre\n");
            continue;
        }

        struct evdev_device *dev;
        if (libevdev_new_from_fd(fd, (struct libevdev **)&dev) < 0) {
            fprintf(stderr, "Failed to create evdev device from fd\n");
            close(fd);
            return ;
        }


        // 检查是否支持触摸事件
        bool is_touchscreen = libevdev_has_property((struct libevdev *)dev, INPUT_PROP_DIRECT);

        libevdev_free((struct libevdev *)dev);
        close(fd);

        if (!is_touchscreen) {
            continue;
        }

        strcpy(buf, filename);
    }

    closedir(d);
    //printf("touch_dev= %s\n", buf);
}
#endif /* LVGL_SIMULATION */

static void handle_signal(int sig);

void init_window_create(int argc, char *argv[]){
    //SDK初始化接口
    lv_init();

    //创建显示
#ifdef LVGL_SIMULATION
    int monitor_hor_res =1024;
    int monitor_ver_res =600;
    lv_display_t * disp = lv_x11_window_create("LVGL X11 Simulation", monitor_hor_res, monitor_ver_res);
    lv_x11_inputs_create(disp, &mouse_cursor);
#else
#ifdef LV_USE_LINUX_DRM
    lv_display_t * disp = lv_linux_drm_create();
    lv_linux_drm_set_file(disp, "/dev/dri/card0",-1);
#else
    lv_display_t * disp = lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(disp, "/dev/fb0");
#endif
    //创建触摸.
    char touch_dev[100];
    lv_memset(touch_dev, 0, sizeof (touch_dev));
    find_touch_device_path(touch_dev);

    //如果找不到鼠标，使用应用程序参数传递进来的触摸节点/dev/input/xxx
    if(0 != strncmp(touch_dev, "/dev/input", 10) && argc>1){
           memset(touch_dev, 0, sizeof (touch_dev));
           strcpy(touch_dev, argv[1]);
    }

    lv_evdev_create(LV_INDEV_TYPE_POINTER, touch_dev);
#endif

    //初始化串口接口
    window_init_window(argc, argv);

    //抓取Ctrl+C 和ctrl+Z, 使用应用退出时可以执行资源的释放处理
    signal(SIGINT, handle_signal);
    signal(SIGTSTP, handle_signal);
}


void exec_event(window_destory destory, void *user_data)
{
    //执行lvgl 事件和空闲时睡眠
    while(g_window.runing){
        int32_t mSec = lv_timer_handler();
        usleep(mSec*1000);
    }

    //如果存在销毁资源，则调用回调函数销毁。
    if(destory){
        destory(user_data);
    }

    //销毁接口创建时分配的内存.
    window_destory_data();

    //销毁lvgl SDK内部的资源.
    lv_deinit();
    printf("quit runnig\n");
}

void quit_event()
{
    g_window.runing = false;
}

static void handle_signal(int sig){
    switch (sig) {
    case SIGINT:
    case SIGTSTP:
        quit_event();
        break;
    default:
        break;
    }
}
