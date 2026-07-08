#ifndef SKETCHYDB_H
#define SKETCHYDB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SKDB_OK 0
#define SKDB_ERROR 1
#define SKDB_NOMEM 7
#define SKDB_MISUSE 21

typedef struct skdb skdb;

typedef int (*skdb_callback)(
    void* user_data,
    int column_count,
    char** column_values,
    char** column_names);

int skdb_open(const char* filename, skdb** out_db);
int skdb_close(skdb* db);
int skdb_exec(
    skdb* db,
    const char* sql,
    skdb_callback callback,
    void* user_data,
    char** error_message);

const char* skdb_errmsg(skdb* db);
uint64_t skdb_approx_memory_bytes(skdb* db);
void skdb_free(void* ptr);
const char* skdb_libversion(void);

#ifdef __cplusplus
}
#endif

#endif
