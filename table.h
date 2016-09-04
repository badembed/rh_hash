#ifndef _TABLE_H
#define _TABLE_H

#include <stddef.h>

typedef void* table_t;
typedef int(*cmp_func)(void*, void*, size_t);
typedef unsigned long(*hash_func)(void*, size_t);

table_t table_new(hash_func h, cmp_func c);

int table_insert(table_t, void *key, size_t keylen, void *data);

int table_get(table_t, void *key, size_t keylen, void **dataptr);

void *table_remove(table_t, void *key, size_t keylen);

#endif