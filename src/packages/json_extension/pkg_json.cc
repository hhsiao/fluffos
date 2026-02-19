#include "base/package_api.h"
#include "vm/internal/base/mapping.h"
#include "vm/internal/base/array.h"
#include "vm/internal/base/object.h"
#include "packages/core/file.h"
#include "yyjson.h"
#include <vector>
#include <unordered_set>
#include <algorithm>
#include <string>
#include <cstdio>
#include <cstring>
#include <climits>
// =========================================================================
// 編譯期最佳化宏
// =========================================================================
#if defined(__GNUC__) || defined(__clang__)
#define LIKELY(x)   __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define LIKELY(x)   (x)
#define UNLIKELY(x) (x)
#endif
// =========================================================================
// 配置常量
// =========================================================================
#define MAX_JSON_DEPTH 128
#define MAX_JSON_FILE_SIZE (256 * 1024 * 1024)
#define MAX_JSON_STRING_LENGTH (64 * 1024 * 1024)
#define MAX_JSON_ARRAY_SIZE 10000000
#define MAX_JSON_OBJECT_SIZE 5000000
#define CIRCULAR_CHECK_THRESHOLD 24  // 深度超過 24 啟用 Hash 加速
// =========================================================================
// RAII 輔助類
// =========================================================================
struct ScopedYYDoc {
yyjson_doc* doc;
explicit ScopedYYDoc(yyjson_doc* d) : doc(d) {}
~ScopedYYDoc() { if (doc) yyjson_doc_free(doc); }
};
struct ScopedYYMutDoc {
yyjson_mut_doc* doc;
explicit ScopedYYMutDoc(yyjson_mut_doc* d) : doc(d) {}
~ScopedYYMutDoc() { if (doc) yyjson_mut_doc_free(doc); }
};
// =========================================================================
// 快速整數轉字串
// =========================================================================
static inline int fast_i64toa(int64_t value, char* buffer) {
if (UNLIKELY(value == 0)) {
    buffer[0] = '0';
    buffer[1] = '\0';
    return 1;
}

char temp[24];
char* p = temp;
uint64_t uval = static_cast<uint64_t>(value);

if (value < 0) {
    uval = ~uval + 1;
}

while (uval > 0) {
    *p++ = static_cast<char>('0' + (uval % 10));
    uval /= 10;
}

int len = 0;
if (value < 0) buffer[len++] = '-';

while (p > temp) {
    buffer[len++] = *--p;
}
buffer[len] = '\0';

return len;
}
// =========================================================================
// 混合循環檢測器 v4.1
// =========================================================================
class CircularChecker {
private:
std::vector<void*> stack_;           // 儲存全量路徑，保證正確性
std::unordered_set<void*> deep_set_; // 深層加速索引
public:
CircularChecker() {
    stack_.reserve(64);  // 预分配，减少动态扩容
}
// v4.1 优化：传入 depth 避免浅层时检查空 Hash
inline bool contains(void* ptr, int depth) const {
    // 策略：優先檢查 Hash (O(1))，深層循環最危險需最快攔截
    // 最佳化：淺層時 deep_set_ 必然為空，直接跳過
    if (depth >= CIRCULAR_CHECK_THRESHOLD && !deep_set_.empty()) {
        if (deep_set_.find(ptr) != deep_set_.end()) {
            return true;
        }
    }
    // 兜底：線性掃描 Vector (O(N))
    // Vector 記憶體連續，CPU 預取器友好，無需拆分掃描
    for (auto it = stack_.rbegin(); it != stack_.rend(); ++it) {
        if (*it == ptr) return true;
    }
    return false;
}

inline void insert(void* ptr, int depth) {
    stack_.push_back(ptr);
    if (depth >= CIRCULAR_CHECK_THRESHOLD) {
        deep_set_.insert(ptr);
    }
}

inline void remove(void* ptr, int depth) {
    stack_.pop_back();
    if (depth >= CIRCULAR_CHECK_THRESHOLD) {
        deep_set_.erase(ptr);
    }
}
};
// =========================================================================
// Encoder: LPC -> JSON
// =========================================================================
static yyjson_mut_val* svalue_to_json_impl(yyjson_mut_doc* doc, svalue_t* sv,
                                         CircularChecker* checker, int depth) {
if (UNLIKELY(depth > MAX_JSON_DEPTH)) {
    return yyjson_mut_str(doc, "<error: max depth reached>");
}

switch (sv->type) {
    case T_NUMBER:
        return yyjson_mut_int(doc, sv->u.number);

    case T_REAL:
        return yyjson_mut_real(doc, sv->u.real);

    case T_STRING:
        return yyjson_mut_str(doc, sv->u.string);

    case T_ARRAY: {
        array_t* arr = sv->u.arr;
        int size = arr->size;

        if (UNLIKELY(size > MAX_JSON_ARRAY_SIZE)) {
            debug_message("json_encode: array size %d exceeds limit %d, encoding anyway\n",
                          size, MAX_JSON_ARRAY_SIZE);
        }
        // v4.1: 傳入 depth 參數
        if (UNLIKELY(checker->contains(arr, depth))) {
            return yyjson_mut_str(doc, "<circular_ref_array>");
        }
        checker->insert(arr, depth);

        yyjson_mut_val* json_arr = yyjson_mut_arr(doc);
        for (int i = 0; i < size; i++) {
            yyjson_mut_arr_append(json_arr,
                svalue_to_json_impl(doc, &arr->item[i], checker, depth + 1));
        }

        checker->remove(arr, depth);
        return json_arr;
    }

    case T_MAPPING: {
        mapping_t* map = sv->u.map;
        // v4.1: 傳入 depth 參數
        if (UNLIKELY(checker->contains(map, depth))) {
            return yyjson_mut_str(doc, "<circular_ref_mapping>");
        }
        checker->insert(map, depth);

        yyjson_mut_val* json_obj = yyjson_mut_obj(doc);
        char num_buf[32];
        int obj_count = 0;
        for (int i = 0; i <= map->table_size; i++) {
            for (mapping_node_t *elt = map->table[i]; elt; elt = elt->next) {
                if (UNLIKELY(++obj_count > MAX_JSON_OBJECT_SIZE)) {
                    debug_message("json_encode: object size exceeds limit %d, truncating\n",
                                  MAX_JSON_OBJECT_SIZE);
                    checker->remove(map, depth);
                    return json_obj;
                }

                svalue_t *key = &elt->values[0];
                svalue_t *val = &elt->values[1];

                if (key->type == T_STRING) {
                    yyjson_mut_obj_add(json_obj,
                        yyjson_mut_str(doc, key->u.string),
                        svalue_to_json_impl(doc, val, checker, depth + 1)
                    );
                } else if (key->type == T_NUMBER) {
                    int len = fast_i64toa(static_cast<int64_t>(key->u.number), num_buf);
                    yyjson_mut_obj_add(json_obj,
                        yyjson_mut_strn(doc, num_buf, static_cast<size_t>(len)),
                        svalue_to_json_impl(doc, val, checker, depth + 1)
                    );
                }
            }
        }

        checker->remove(map, depth);
        return json_obj;
    }

    case T_OBJECT:
        if (sv->u.ob && !(sv->u.ob->flags & O_DESTRUCTED)) {
            return yyjson_mut_str(doc, sv->u.ob->obname);
        }
        return yyjson_mut_null(doc);

    default:
        return yyjson_mut_null(doc);
}
}
// =========================================================================
// Decoder: JSON -> LPC
// =========================================================================
static void json_to_svalue(yyjson_val* val, svalue_t* out, int depth) {
out->type = T_NUMBER;
out->subtype = 0;
out->u.number = 0;

if (UNLIKELY(!val || depth > MAX_JSON_DEPTH)) return;

switch (yyjson_get_type(val)) {
    case YYJSON_TYPE_NULL:
        break;

    case YYJSON_TYPE_BOOL:
        out->u.number = yyjson_get_bool(val) ? 1 : 0;
        break;

    case YYJSON_TYPE_NUM:
        if (yyjson_is_real(val)) {
            out->type = T_REAL;
            out->u.real = yyjson_get_real(val);
        } else {
            out->u.number = static_cast<long>(yyjson_get_int(val));
        }
        break;

    case YYJSON_TYPE_STR: {
        size_t len = yyjson_get_len(val);

        if (UNLIKELY(len > MAX_JSON_STRING_LENGTH)) {
            debug_message("json_decode: string length %zu exceeds limit %d, truncating\n",
                          len, MAX_JSON_STRING_LENGTH);
            out->type = T_STRING;
            out->subtype = STRING_MALLOC;
            out->u.string = string_copy("", "json_decode_overflow");
            break;
        }

        out->type = T_STRING;
        out->subtype = STRING_MALLOC;
        out->u.string = string_copy(yyjson_get_str(val), "json_decode");
        break;
    }

    case YYJSON_TYPE_ARR: {
        size_t count = yyjson_arr_size(val);

        if (UNLIKELY(count > MAX_JSON_ARRAY_SIZE)) {
            debug_message("json_decode: array size %zu exceeds limit %d, truncating\n",
                          count, MAX_JSON_ARRAY_SIZE);
            count = MAX_JSON_ARRAY_SIZE;
        }

        array_t* lpc_arr = allocate_array(static_cast<int>(count));
        out->type = T_ARRAY;
        out->u.arr = lpc_arr;

        yyjson_val* item;
        size_t idx, max;
        yyjson_arr_foreach(val, idx, max, item) {
            if (idx >= count) break;
            json_to_svalue(item, &lpc_arr->item[idx], depth + 1);
        }
        break;
    }

    case YYJSON_TYPE_OBJ: {
        size_t count = yyjson_obj_size(val);

        if (UNLIKELY(count > MAX_JSON_OBJECT_SIZE)) {
            debug_message("json_decode: object size %zu exceeds limit %d, truncating\n",
                          count, MAX_JSON_OBJECT_SIZE);
            count = MAX_JSON_OBJECT_SIZE;
        }

        mapping_t* lpc_map = allocate_mapping(static_cast<int>(count));
        out->type = T_MAPPING;
        out->u.map = lpc_map;

        yyjson_val *key, *ele;
        size_t idx, max;
        int inserted = 0;
        yyjson_obj_foreach(val, idx, max, key, ele) {
            if (static_cast<size_t>(inserted) >= count) break;

            svalue_t key_sv;
            key_sv.type = T_STRING;
            key_sv.subtype = STRING_MALLOC;
            key_sv.u.string = string_copy(yyjson_get_str(key), "json_key");

            svalue_t* dest = find_for_insert(lpc_map, &key_sv, 1);
            if (dest) {
                json_to_svalue(ele, dest, depth + 1);
                inserted++;
            }
            free_string_svalue(&key_sv);
        }
        break;
    }
}
}
// =========================================================================
// EFUNS 實現
// =========================================================================
void f_json_encode(void) {
    svalue_t* arg = sp;

    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (UNLIKELY(!doc)) {
        pop_n_elems(1);
        push_number(0);
        return;
    }
    ScopedYYMutDoc doc_guard(doc);

    CircularChecker checker;
    yyjson_mut_val* root = svalue_to_json_impl(doc, arg, &checker, 0);
    yyjson_mut_doc_set_root(doc, root);

    size_t len;
    char* json_str = yyjson_mut_write(doc, 0, &len);

    if (UNLIKELY(!json_str)) {
        pop_n_elems(1);
        push_number(0);
        return;
    }

    pop_n_elems(1);
    copy_and_push_string(json_str);
    free(json_str);
}

void f_json_decode(void) {
    const char* str = sp->u.string;

    if (UNLIKELY(!str || !*str)) {
        pop_n_elems(1);
        push_number(0);
        return;
    }
    yyjson_read_err err;
    yyjson_doc* doc = yyjson_read_opts(const_cast<char*>(str), strlen(str), 0, NULL, &err);

    if (UNLIKELY(!doc)) {
        debug_message("json_decode failed: %s at pos %zu\n", err.msg, err.pos);
        pop_n_elems(1);
        push_number(0);
        return;
    }

    ScopedYYDoc doc_guard(doc);
    svalue_t result;
    json_to_svalue(yyjson_doc_get_root(doc), &result, 0);

    pop_n_elems(1);
    sp++;
    *sp = result;
}

void f_read_json(void) {
    const char* filename = sp->u.string;

    const char* real_path = check_valid_path(filename, current_object, "read_json", 0);
    if (UNLIKELY(!real_path)) {
        pop_n_elems(1);
        push_number(0);
        return;
    }
    FILE* fp = fopen(real_path, "rb");
    if (UNLIKELY(!fp)) {
        pop_n_elems(1);
        push_number(0);
        return;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (UNLIKELY(fsize <= 0 || fsize > MAX_JSON_FILE_SIZE)) {
        fclose(fp);
        pop_n_elems(1);
        push_number(0);
        return;
    }
    // In-Situ 解析最佳化：節省 50% 內存
    // 注意：必須新增 YYJSON_PADDING_SIZE 防止越界讀取
    size_t buf_size = static_cast<size_t>(fsize) + YYJSON_PADDING_SIZE;
    char* buffer = static_cast<char*>(malloc(buf_size));

    if (UNLIKELY(!buffer)) {
        fclose(fp);
        pop_n_elems(1);
        push_number(0);
        return;
    }
    size_t read_size = fread(buffer, 1, static_cast<size_t>(fsize), fp);
    fclose(fp);

    if (UNLIKELY(read_size != static_cast<size_t>(fsize))) {
        free(buffer);
        pop_n_elems(1);
        push_number(0);
        return;
    }
    yyjson_read_err err;
    // INSITU 模式：直接在 buffer 上解析，避免內部拷貝
    yyjson_doc* doc = yyjson_read_opts(buffer, static_cast<size_t>(fsize),
                                    YYJSON_READ_INSITU | YYJSON_READ_NOFLAG,
                                    NULL, &err);

    if (UNLIKELY(!doc)) {
        free(buffer);
        debug_message("read_json parse error: %s at pos %zu\n", err.msg, err.pos);
        pop_n_elems(1);
        push_number(0);
        return;
    }
    ScopedYYDoc doc_guard(doc);
    svalue_t result;
    // 關鍵：string_copy 會將資料拷貝到驅動託管記憶體
    // 絕對不可直接引用 buffer 中的字串，因為 buffer 即將釋放
    json_to_svalue(yyjson_doc_get_root(doc), &result, 0);
    // In-Situ 模式下，buffer 必須在 doc 銷毀前保持有效
    // 但由於 json_to_svalue 中的 string_copy 已完成拷貝，此處釋放安全
    free(buffer);
    pop_n_elems(1);
    sp++;
    *sp = result;
}

void f_write_json(void) {
    svalue_t* data = sp;
    const char* filename = (sp - 1)->u.string;

    const char* real_path = check_valid_path(filename, current_object, "write_json", 1);
    if (UNLIKELY(!real_path)) {
        pop_n_elems(2);
        push_number(0);
        return;
    }
    yyjson_mut_doc* doc = yyjson_mut_doc_new(NULL);
    if (UNLIKELY(!doc)) {
        pop_n_elems(2);
        push_number(0);
        return;
    }
    ScopedYYMutDoc doc_guard(doc);
    CircularChecker checker;
    yyjson_mut_val* root = svalue_to_json_impl(doc, data, &checker, 0);
    yyjson_mut_doc_set_root(doc, root);

    FILE* fp = fopen(real_path, "wb");
    if (UNLIKELY(!fp)) {
        pop_n_elems(2);
        push_number(0);
        return;
    }
    // 串流寫入：直接寫入文件，避免記憶體尖峰
    yyjson_write_err err;
    bool success = yyjson_mut_write_fp(fp, doc, 0, NULL, &err);
    fclose(fp);

    if (UNLIKELY(!success)) {
        debug_message("write_json failed: %s (code: %d)\n", err.msg, err.code);
    }
    pop_n_elems(2);
    push_number(success ? 1 : 0);
}