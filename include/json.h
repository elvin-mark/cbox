#ifndef TOY_JSON_H
#define TOY_JSON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    JSON_NULL = 0,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} json_type_t;

typedef struct json_value json_value_t;

typedef struct {
    char *key;
    json_value_t *value;
} json_pair_t;

struct json_value {
    json_type_t type;
    union {
        bool boolean;
        double number;
        char *string;
        struct {
            json_value_t **items;
            size_t count;
        } array;
        struct {
            json_pair_t *pairs;
            size_t count;
        } object;
    } u;
};

json_value_t *json_parse(const char *input);
json_value_t *json_parse_file(const char *path);
void json_free(json_value_t *val);

const json_value_t *json_obj_get(const json_value_t *obj, const char *key);
const char *json_as_string(const json_value_t *val);
bool json_as_bool(const json_value_t *val, bool default_val);
int64_t json_as_int64(const json_value_t *val, int64_t default_val);

#endif /* TOY_JSON_H */
