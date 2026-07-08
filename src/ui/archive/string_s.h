/*
 * 简单的字符串处理接口，提供一些字符串简单操作
 *    string_s *str =g_strings.create("123");
 *    g_strings.add(str, 456)   "123456"
 *    g_strings.remove(str, 45)   "1236"
 *    g_strings.destory(str)
 *
 */
#ifndef STRING_S_H
#define STRING_S_H

#ifdef __cplusplus
extern "C" {
#endif
typedef  struct string_s  string_s;

typedef
struct Strings{
    string_s * (*create)(const char *s);
    void (*destroy)(string_s *list);
    void (*add)(string_s *str, const char *s);
    void (*remove)(string_s *str, const char *s);
    const char * (*data)(string_s *str);
    int (*count)(string_s *str);
    char * (*contains)(string_s *str, const char *s);
    void (*clear)(string_s *str);

}Strings;

extern Strings g_strings;
void init_Strings();

#ifdef __cplusplus
}
#endif

#endif
