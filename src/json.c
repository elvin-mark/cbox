#include "json.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JSON_MAX_DEPTH 128

typedef struct {
    const char *p;
    int depth;
} json_parser_t;

static void skip_ws(json_parser_t *jp) {
    while (*jp->p && isspace((unsigned char)*jp->p)) {
        jp->p++;
    }
}

static json_value_t *parse_value(json_parser_t *jp);

static char *parse_raw_string(json_parser_t *jp) {
    if (*jp->p != '"') {
        return NULL;
    }
    jp->p++;

    size_t cap = 32;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        return NULL;
    }

    while (*jp->p && *jp->p != '"') {
        unsigned char c = (unsigned char)*jp->p++;
        if (c == '\\') {
            char esc = *jp->p++;
            if (!esc) {
                free(buf);
                return NULL;
            }
            switch (esc) {
                case '"':  c = '"'; break;
                case '\\': c = '\\'; break;
                case '/':  c = '/'; break;
                case 'b':  c = '\b'; break;
                case 'f':  c = '\f'; break;
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case 'u': {
                    unsigned int code = 0;
                    for (int i = 0; i < 4; i++) {
                        char h = jp->p[i];
                        if (!isxdigit((unsigned char)h)) {
                            free(buf);
                            return NULL;
                        }
                        code <<= 4;
                        if (h >= '0' && h <= '9') code |= (unsigned int)(h - '0');
                        else if (h >= 'a' && h <= 'f') code |= (unsigned int)(h - 'a' + 10);
                        else code |= (unsigned int)(h - 'A' + 10);
                    }
                    jp->p += 4;
                    c = (unsigned char)(code & 0xFF);
                    break;
                }
                default:
                    free(buf);
                    return NULL;
            }
        } else if (c < 0x20) {
            free(buf);
            return NULL;
        }

        if (len + 2 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
        buf[len++] = (char)c;
    }

    if (*jp->p != '"') {
        free(buf);
        return NULL;
    }
    jp->p++;
    buf[len] = '\0';
    return buf;
}

static json_value_t *alloc_val(json_type_t type) {
    json_value_t *v = calloc(1, sizeof(json_value_t));
    if (v) {
        v->type = type;
    }
    return v;
}

static json_value_t *parse_array(json_parser_t *jp) {
    jp->p++; /* skip '[' */
    json_value_t *arr = alloc_val(JSON_ARRAY);
    if (!arr) {
        return NULL;
    }

    skip_ws(jp);
    if (*jp->p == ']') {
        jp->p++;
        return arr;
    }

    while (1) {
        skip_ws(jp);
        json_value_t *item = parse_value(jp);
        if (!item) {
            json_free(arr);
            return NULL;
        }

        json_value_t **new_items =
            realloc(arr->u.array.items, (arr->u.array.count + 1) * sizeof(json_value_t *));
        if (!new_items) {
            json_free(item);
            json_free(arr);
            return NULL;
        }
        arr->u.array.items = new_items;
        arr->u.array.items[arr->u.array.count++] = item;

        skip_ws(jp);
        if (*jp->p == ',') {
            jp->p++;
        } else if (*jp->p == ']') {
            jp->p++;
            return arr;
        } else {
            json_free(arr);
            return NULL;
        }
    }
}

static json_value_t *parse_object(json_parser_t *jp) {
    jp->p++; /* skip '{' */
    json_value_t *obj = alloc_val(JSON_OBJECT);
    if (!obj) {
        return NULL;
    }

    skip_ws(jp);
    if (*jp->p == '}') {
        jp->p++;
        return obj;
    }

    while (1) {
        skip_ws(jp);
        char *key = parse_raw_string(jp);
        if (!key) {
            json_free(obj);
            return NULL;
        }

        skip_ws(jp);
        if (*jp->p != ':') {
            free(key);
            json_free(obj);
            return NULL;
        }
        jp->p++;

        skip_ws(jp);
        json_value_t *val = parse_value(jp);
        if (!val) {
            free(key);
            json_free(obj);
            return NULL;
        }

        json_pair_t *new_pairs =
            realloc(obj->u.object.pairs, (obj->u.object.count + 1) * sizeof(json_pair_t));
        if (!new_pairs) {
            free(key);
            json_free(val);
            json_free(obj);
            return NULL;
        }
        obj->u.object.pairs = new_pairs;
        obj->u.object.pairs[obj->u.object.count].key = key;
        obj->u.object.pairs[obj->u.object.count].value = val;
        obj->u.object.count++;

        skip_ws(jp);
        if (*jp->p == ',') {
            jp->p++;
        } else if (*jp->p == '}') {
            jp->p++;
            return obj;
        } else {
            json_free(obj);
            return NULL;
        }
    }
}

static json_value_t *parse_value(json_parser_t *jp) {
    if (jp->depth >= JSON_MAX_DEPTH) {
        return NULL;
    }
    skip_ws(jp);
    char c = *jp->p;
    if (c == '\0') {
        return NULL;
    }

    if (c == '"') {
        char *s = parse_raw_string(jp);
        if (!s) {
            return NULL;
        }
        json_value_t *v = alloc_val(JSON_STRING);
        if (!v) {
            free(s);
            return NULL;
        }
        v->u.string = s;
        return v;
    }
    if (c == '{') {
        jp->depth++;
        json_value_t *o = parse_object(jp);
        jp->depth--;
        return o;
    }
    if (c == '[') {
        jp->depth++;
        json_value_t *a = parse_array(jp);
        jp->depth--;
        return a;
    }
    if (strncmp(jp->p, "true", 4) == 0) {
        jp->p += 4;
        json_value_t *v = alloc_val(JSON_BOOL);
        if (v) v->u.boolean = true;
        return v;
    }
    if (strncmp(jp->p, "false", 5) == 0) {
        jp->p += 5;
        json_value_t *v = alloc_val(JSON_BOOL);
        if (v) v->u.boolean = false;
        return v;
    }
    if (strncmp(jp->p, "null", 4) == 0) {
        jp->p += 4;
        return alloc_val(JSON_NULL);
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *endptr = NULL;
        errno = 0;
        double num = strtod(jp->p, &endptr);
        if (errno != 0 || endptr == jp->p) {
            return NULL;
        }
        jp->p = endptr;
        json_value_t *v = alloc_val(JSON_NUMBER);
        if (v) v->u.number = num;
        return v;
    }

    return NULL;
}

json_value_t *json_parse(const char *input) {
    if (!input) {
        return NULL;
    }
    json_parser_t jp = {.p = input, .depth = 0};
    json_value_t *root = parse_value(&jp);
    if (!root) {
        return NULL;
    }
    skip_ws(&jp);
    if (*jp.p != '\0') {
        json_free(root);
        return NULL;
    }
    return root;
}

json_value_t *json_parse_file(const char *path) {
    if (!path) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 16 * 1024 * 1024) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t nread = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nread] = '\0';

    json_value_t *root = json_parse(buf);
    free(buf);
    return root;
}

void json_free(json_value_t *val) {
    if (!val) {
        return;
    }
    switch (val->type) {
        case JSON_STRING:
            free(val->u.string);
            break;
        case JSON_ARRAY:
            for (size_t i = 0; i < val->u.array.count; i++) {
                json_free(val->u.array.items[i]);
            }
            free(val->u.array.items);
            break;
        case JSON_OBJECT:
            for (size_t i = 0; i < val->u.object.count; i++) {
                free(val->u.object.pairs[i].key);
                json_free(val->u.object.pairs[i].value);
            }
            free(val->u.object.pairs);
            break;
        default:
            break;
    }
    free(val);
}

const json_value_t *json_obj_get(const json_value_t *obj, const char *key) {
    if (!obj || obj->type != JSON_OBJECT || !key) {
        return NULL;
    }
    for (size_t i = 0; i < obj->u.object.count; i++) {
        if (strcmp(obj->u.object.pairs[i].key, key) == 0) {
            return obj->u.object.pairs[i].value;
        }
    }
    return NULL;
}

const char *json_as_string(const json_value_t *val) {
    if (!val || val->type != JSON_STRING) {
        return NULL;
    }
    return val->u.string;
}

bool json_as_bool(const json_value_t *val, bool default_val) {
    if (!val || val->type != JSON_BOOL) {
        return default_val;
    }
    return val->u.boolean;
}

int64_t json_as_int64(const json_value_t *val, int64_t default_val) {
    if (!val || val->type != JSON_NUMBER) {
        return default_val;
    }
    return (int64_t)val->u.number;
}
