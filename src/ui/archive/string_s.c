#include "string_s.h"
#include <stdlib.h>
#include<string.h>
Strings g_strings;

#define MIN_CAPACITY 16
#define GROWTH_FACTOR 2
struct string_s{
    char *data;
    size_t size;
    size_t capacity;
};

static void ensure_capacity(string_s* str, size_t required_len) {
    if (str->capacity <= required_len) {
        size_t new_cap = str->capacity * GROWTH_FACTOR;
        while (new_cap <= required_len) {
            new_cap *= GROWTH_FACTOR;
        }
        str->data = realloc(str->data, new_cap);
        str->capacity = new_cap;
    }
}

static void reszie_capacity(string_s* str, size_t required_len) {
    if (!str )
        return ;

    if(str->size <required_len){
        str->data = realloc(str->data, required_len);
    }
}


static string_s * strings_create(const char *s){
    string_s *str = (string_s*)malloc(sizeof(string_s));
    if(!str)
        return NULL;

    int len = str ? strlen(s) : 0;
    str->capacity  = len > MIN_CAPACITY ? len +1 : MIN_CAPACITY;
    str->data = (char *)malloc(str->capacity);
    if(!str->data)
    {
        free(str);
        return NULL;
    }
    str->size  = len;
    if(s){
        strcpy(str->data, s);
    }else{
        str->data[0]='\0';
    }

    return str;
}

static void strings_destroy(string_s *str){

    if(str){
        free(str->data);
        free(str);
    }
}

static const char * strings_data(string_s *str)
{
    if(!str)
        return NULL;
    return str->data;
}

static int strings_count(string_s *str){
    if(!str)
        return 0;
    return str->size;
}


static void strings_add(string_s *str, const char *s)
{
    if (!str || !s) return;

    size_t str_len = strlen(s);
    if (str_len == 0) return;

    ensure_capacity(str, str->size + str_len);
    strcpy(str->data + str->size, s);
    str->size += str_len;
}

void strings_remove(string_s *str, const char *s){

    if (!str || !s || !str->data || *s == '\0')
        return;

    char * pos = str->data;
    int size = str->size;
    do{
        pos = strstr(pos, s);
        if(!pos)
            break;

        size_t str_len = strlen(s);
        size_t remaining_len = size - (pos - str->data + str_len);
        memmove(pos, pos + str_len, remaining_len + 1);
        str->size -=str_len;
    }while(pos);

    reszie_capacity(str, str->size+1);
}
char * strings_conains(string_s *str, const char *s){
    if (!str || !s || !str->data)
        return  NULL;

    return strstr(str->data, s);
}

void strings_clear(string_s *str){
    if(!str)
        return;

    void *tmp = realloc(str->data, MIN_CAPACITY);
    if (tmp) {
        str->data = tmp;
        str->size = 0;
        str->capacity = MIN_CAPACITY;
    }
}

void init_Strings()
{
    Strings  *s = &g_strings;
    s->create = strings_create;
    s->destroy = strings_destroy;
    s->add = strings_add;
    s->count = strings_count;
    s->data = strings_data;
    s->contains =strings_conains;
    s->remove = strings_remove;
    s->clear = strings_clear;
}
