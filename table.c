/* Simple robin hood hashing implementation
 * Author: Josh Tiras
 * Date: 2016-09-3
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "table.h"

#define TABLE_SIZE_DEFAULT 547
#define TABLE_MAX_LOAD_FACTOR 0.95

#define MAX(a,b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
       _a > _b ? _a : _b; })

#define MIN(a,b) \
    ({ __typeof__ (a) _a = (a); \
       __typeof__ (b) _b = (b); \
       _a < _b ? _a : _b; })

typedef int(*cmp_func)(void*, void*, size_t);
typedef unsigned long(*hash_func)(void*, size_t);

struct entry {
    unsigned long hash;
    void *key;
    void *data;
    size_t keylen;
    unsigned int probepos;
    unsigned int alive;
};

struct table {
    struct entry *table;
    size_t size;
    size_t step_prime;
    unsigned int totalweight;
    unsigned int maxprobe;
    unsigned int elements;
    hash_func hash;
    cmp_func cmp;
};

static int is_prime(size_t n);
static size_t next_prime_size(size_t cur_size, float scalar);
static size_t next_prime_step(size_t cur_size);
static unsigned long table_hash(void *k, size_t len);
static int table_cmp(void *k1, void *k2, size_t len);
static ssize_t internal_search(table_t t, void *key, size_t keylen);
static int grow_table(table_t t);

/* Wrapper to cast the keys for compare */
static int table_cmp(void *k1, void *k2, size_t len)
{
    return strncmp((const char *)k1, (const char *)k2, len);
}

/* Type checking needs to be done before
 * Simple djb2 hash 
 */
static unsigned long table_hash(void *k, size_t len)
{
    char *key = k;
    unsigned long hash = 5381;
    int c = 0;
    for(; c < len; c++)
        hash = ((hash << 5) + hash) + key[c];
    return hash;
}

/* Very simple primality test. 
 * We scale the size by some scalar and then
 * walk up until we find the next prime to use
 * as the table size 
 */
static int is_prime(size_t n) {
    int i = 5;
    if(n <= 1)
        return 0;
    else if(n <= 3)
        return 1;
    else if((n % 2) == 0 || (n % 3) == 0)
        return 0;
    while(i * i < n) {
        if ( (n % i) == 0 || (n % (i + 2)) == 0)
            return 0;
        i += 6;
    }
    return 1;
}

/* Walk to higher numbers to ensure the size is >= requested */
static size_t next_prime_size(size_t cur_size, float scalar)
{
    size_t new_size = cur_size * scalar;
    while(!is_prime(new_size))
        new_size++;

    return new_size;
}

/* Walk downwards to ensure the size of the step is
 * less than the table size
 */
static size_t next_prime_step(size_t cur_size)
{
    size_t new_step = cur_size - 1;
    while(!is_prime(new_step))
        new_step--;
    return new_step;
}

/* Grow the table by 2.5 times to the next closest
 * prime above cur_size * 2.5
 */
static int grow_table(table_t t)
{
    float scalar = 2.5;
    struct table *ta = t;
    struct entry *old_table = ta->table;
    size_t new_size = next_prime_size(ta->size, scalar), old_size = ta->size;
    int i = 0;

    ta->table = calloc(new_size, sizeof(*ta->table));
    if(!ta->table) {
        ta->table = old_table;
        return -1;
    }

    ta->size = new_size;
    ta->step_prime = next_prime_step(ta->size);
    // If I'm re-using insert I need to reset element count
    // and maxprobe/weight etc.
    ta->elements = 0;
    ta->maxprobe = 0;
    ta->totalweight = 0;

    for(; i < old_size; i++) {
        if(old_table[i].alive) {
            table_insert(t, old_table[i].key, old_table[i].keylen, old_table[i].data);
        }
    }

    free(old_table);
    return 0;
}

void table_print_stats(table_t t)
{
    struct table *ta = t;
    printf("Table Diagnostics\n");
    printf("-----------------\n");
    printf("Table Size: %lu, Elements %d, Load Factor: %f\n", ta->size, ta->elements, (float)ta->elements/(float)ta->size);
    printf("Table Weight: %d, Average Probe: %d, Max Probe: %d\n", ta->totalweight, ta->totalweight/ta->elements, ta->maxprobe);
}
/* table_new generates a new table at the default size
 * it takes optionally a hashing function and a compare
 * function. By default it uses string keys and double hashing
 */
table_t table_new(hash_func h, cmp_func c)
{
    struct table *t = malloc(sizeof(*t));
    if(!t) {
        return NULL;
    }

    t->table = calloc(TABLE_SIZE_DEFAULT, sizeof(*t->table));
    if(!t->table) {
        free(t);
        return NULL;
    }

    t->size = TABLE_SIZE_DEFAULT;
    t->totalweight = 0;
    t->elements = 0;
    t->maxprobe = 0;
    t->hash = h?h:table_hash;
    t->cmp = c?c:table_cmp;
    t->step_prime = next_prime_step(t->size);

    return t;
}

/* table_insert adds a new element to the table if it doesn't already exist.
 * returns 0 on success, non-zero error
 */
int table_insert(table_t t, void *key, size_t keylen, void *data)
{
    struct table *ta = t;
    struct entry *e = NULL, r;
    unsigned long step;

    r.hash = ta->hash(key, keylen);
    r.key = key;
    r.data = data;
    r.probepos = 0;
    r.alive = 1;
    r.keylen = keylen;

    step = ta->step_prime - (r.hash % ta->step_prime);

    if(ta->elements == ta->size)
        return -1;

    if((float)ta->elements/(float)ta->size > TABLE_MAX_LOAD_FACTOR)
        grow_table(t);

    for(;;) {
        r.probepos++;
        ta->totalweight++;

        e = &ta->table[(r.hash + r.probepos * step) % ta->size];
        if(!e->alive) {
            if(e->key) {
                // If we are using a recycled position to insert, we first 
                // need to check that this key isn't alive further down the
                // probe sequence (i.e. the recycled position opened up between
                // the first insert and this one for this key). If we find the key
                // we should clear it and decrement the table totalweight appropriately
                ssize_t pos = internal_search(t, r.key, r.keylen);
                if(pos != -1) {
                    memcpy(e, &r, sizeof(struct entry));
                    ta->table[pos].alive = 0;
                    ta->totalweight -= ta->table[pos].probepos;
                    // Exit without updating elements/maxprobe.
                    return 0;
                }
            }
            memcpy(e, &r, sizeof(struct entry));
            break;
        } else {
            if(e->probepos < r.probepos || (e->probepos == r.probepos && r.hash < e->hash)) {
                struct entry temp;
                ta->maxprobe = MAX(r.probepos, ta->maxprobe);

                memcpy(&temp, e, sizeof(struct entry));
                memcpy(e, &r, sizeof(struct entry));
                memcpy(&r, &temp, sizeof(struct entry));
                // Reset step for new record
                step = ta->step_prime - (r.hash % ta->step_prime);
            } else if(e->probepos == r.probepos && r.hash == e->hash && !strncmp(r.key, e->key, r.keylen)) {
                // The key already exists, simply update the value
                e->data = r.data;
                // Update the totalweight since we didn't actually add anything
                ta->totalweight -= r.probepos;
                // Exit eithout updating elements/maxprobe
                return 0;
            }
        }
    }

    ta->elements++;
    ta->maxprobe = MAX(r.probepos, ta->maxprobe);

    return 0;
}

static ssize_t internal_search(table_t t, void *key, size_t keylen)
{
    struct table *ta = t;
    struct entry *e = NULL;
    unsigned long hash = ta->hash(key, keylen);
    unsigned long step = ta->step_prime - (hash % ta->step_prime);
    int found = 0, walk = 0, start = 0, topdone = 0, botdone = 0;
    ssize_t pos = -1;

    if(ta->elements == 0)
        return -1;

    start = ta->totalweight/ta->elements;

    for(;;) {
        if(!topdone && (start + walk) <= ta->maxprobe) {
            pos = (hash + (start + walk) * step) % ta->size;
            e = &ta->table[pos];
            if(e->key == NULL) {
                // We can exit early on top key being NULL
                // in this case NOTHING has ever reached this
                // node, including the currently sought probe-sequence
                // meaning it either doesn't exist or lives below.
                topdone = 1;
            } else if(e->alive) {
                if(!ta->cmp(key, e->key, keylen)) {
                    found = 1;
                    break;
                }
            }
        } else {
            topdone = 1;
        }

        if(!botdone && (start - walk) >= 1) {
            pos = (hash + (start - walk) * step) % ta->size;
            e = &ta->table[pos];
            if(e->alive) {
                if(!ta->cmp(key, e->key, keylen)) {
                    found = 1;
                    break;
                }
            }
        } else {
            botdone = 1;
        }

        if(topdone && botdone)
            break;

        walk++;
    }

    if(found)
        return pos;
    else
        return -1;
}

/* Fetch a record walking outwards from average to cover probe 1 -> maxprobe */
int table_get(table_t t, void *key, size_t keylen, void **data_ptr)
{
    struct table *ta = t;
    ssize_t pos = internal_search(t, key, keylen);

    if(pos > 0) {
        *data_ptr = ta->table[pos].data;
        return 0;
    } else {
        *data_ptr = NULL;
        return -1;
    }
}

/* Remove an item. Simply set alive = 0 */
int table_remove(table_t t, void *key, size_t keylen)
{
    struct table *ta = t;
    ssize_t pos = internal_search(t, key, keylen);

    if(pos > 0) {
        ta->table[pos].alive = 0;
        ta->elements--;
        ta->totalweight -= ta->table[pos].probepos;
        return 0;
    } else {
        return -1;
    }
}

/* Fetch key pointer */
void *table_fetch_key(table_t t, void *key, size_t keylen)
{
    struct table *ta = t;
    ssize_t pos = internal_search(t, key, keylen);

    if(pos > 0) {
        return ta->table[pos].key;
    } else {
        return NULL;
    }
}

/* Fetch data pointer */
void *table_fetch_val(table_t t, void *key, size_t keylen)
{
    struct table *ta = t;
    ssize_t pos = internal_search(t, key, keylen);

    if(pos > 0) {
        return ta->table[pos].data;
    } else {
        return NULL;
    }
}

/* Iterate over the table, invoking provided function with
 * key, keylen, data, and arg
 */
int table_iter(table_t t, iter_func f, void *arg)
{
    struct table *ta = t;
    size_t pos = 0;
    struct entry *e = NULL;
    int ret = 0;

    for(; pos < ta->size; pos++) {
        e = &ta->table[pos];
        if(e->alive) {
            if((ret = f(arg, e->key, e->keylen, e->data) != 0))
                break;
        }
    }

    return ret;
}
