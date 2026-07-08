
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stringlist.h"

#define INIT_CAPACITY 1

stringlist g_list;

struct string_list{
    char **data;
    int size;
    int capacity;
} ;

static string_list * string_list_create() {

    string_list *list = (string_list*)malloc(sizeof(string_list));
    list->data = malloc(INIT_CAPACITY * sizeof(char*));
    list->size = 0;
    list->capacity = INIT_CAPACITY;

    return list;
}

static void string_list_destroy(string_list *list) {
    if(list == NULL)
        return;

    for (int i = 0; i < list->size; i++) {
        free(list->data[i]);
    }
    free(list->data);

    free(list);
}


static void string_list_add(string_list *list, const char *str)
{
    if(!list)
        return;

    if (list->size >= list->capacity) {
        list->capacity *= 2;
        list->data = realloc(list->data, list->capacity * sizeof(char*));
    }
    list->data[list->size] = malloc(strlen(str) + 1);
    strcpy(list->data[list->size], str);
    list->size++;
}

static void string_list_remove(string_list *list, int index)
{
    if (!list || index < 0 || index >= list->size) return;

    free(list->data[index]);
    for (int i = index; i < list->size - 1; i++) {
        list->data[i] = list->data[i + 1];
    }
    list->size--;
}

static void string_list_modify(string_list *list, int index, const char *new_str)
{
    if (!list ||index < 0 || index >= list->size)
        return;

    free(list->data[index]);
    list->data[index] = malloc(strlen(new_str) + 1);
    strcpy(list->data[index], new_str);
}

static const char * string_list_data(string_list *list, int index){
    if(!list)
        return "";

    if(index <list->size)
        return list->data[index];
    return "";
}

static int string_list_count(string_list *list){
    if(!list)
        return 0;
    return list->size;
}

static void string_list_print(string_list *list) {
    if(!list)
        return ;

    for (int i = 0; i < list->size; i++) {
        printf("%d: %s\n", i, list->data[i]);
    }
}

static void string_list_clear(string_list *list){
    if(!list)
        return ;

    for (int i = 0; i < list->size; i++) {
        free(list->data[i]);
    }

    list->size = 0;
    list->capacity = INIT_CAPACITY;
    list->data = realloc(list->data, list->capacity * sizeof(char*));

}

void init_stringlist() {
    stringlist *list = &g_list;
    list->create = string_list_create;
    list->destroy = string_list_destroy;
    list->add = string_list_add;
    list->remove = string_list_remove;
    list->modify = string_list_modify;
    list->print = string_list_print;
    list->data = string_list_data;
    list->count = string_list_count;
    list->clear = string_list_clear;
}
