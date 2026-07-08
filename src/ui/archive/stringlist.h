/*
 * 简单的多个字符list
 * exampe : string_list *list = g_list.create();
 *          g_list.add(list, "123");
 *          g_list.add(list, "456");
 *          g_list.add(list, "789");       {123, 456, 789}
 *
 *          g_list.remove(list, 0);         { 456, 789}
 *
 *          const char *data= g_list.data(list, 0)  ->456
 *          g_list.destory(list)
 */
#ifndef STRINGLIST_H
#define STRINGLIST_H

#ifdef __cplusplus
extern "C" {
#endif
typedef  struct string_list  string_list;

typedef
struct stringlist{
    string_list * (*create)();
    void (*destroy)(string_list *list);
    void (*add)(string_list *list, const char *str);
    void (*remove)(string_list *list, int index) ;
    void (*modify)(string_list *list, int index, const char *new_str) ;
    void (*print)(string_list *list);
    const char * (*data)(string_list *list, int index);
    int (*count)(string_list *list);
    void (*clear)(string_list *list);

}stringlist;

extern stringlist g_list;
void init_stringlist();
#ifdef __cplusplus
}
#endif

#endif
