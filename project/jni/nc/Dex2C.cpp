#include "Dex2C.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "ScopedLocalRef.h"
#include "ScopedPthreadMutexLock.h"
#include "well_known_classes.h"
#include "DynamicRegister.h"

/* ── 解析缓存的两套实现（排障开关 D2C_USE_STL_MAP）────────────────────
 *
 * 默认走哈希表（体积小）。定义 D2C_USE_STL_MAP 则回到原版 std::map。
 *
 * 为什么要留这个开关：std::map 会把 libc++ 整条链（红黑树 → 异常运行时
 * → __cxa_demangle → libunwind）拖进 so，实测 +250KB。哈希表去掉了这条
 * 链，但"去掉"本身也可能引入新问题（缓存容量、并发、键比较语义）。
 *
 * 真机一旦出问题，只要在 Application.mk 的 APP_CPPFLAGS 里加
 *   -DD2C_USE_STL_MAP
 * 就回到原实现，用不着改代码、也用不着回滚整个 dex2c.zip。
 * 两套实现的对外接口完全一致（put/find/size），切换不改任何调用点。
 *
 * ⚠️ 用 std::map 时必须 APP_STL := c++_static（不能用 none）。
 *    默认的 Application.mk 已是 c++_static，两种实现都能编。
 *    ——不用 STL 时代码不引用 libc++_static.a，体积不受影响（实测 4KB 级
 *      产物在 c++_static 与 none 下同为 4,352 字节）。
 */
#ifdef D2C_USE_STL_MAP
#include <map>
#endif

/* 解析缓存的键：(类名, 成员名, 签名)。类缓存时后两者为 NULL。
 * 刻意保持为 POD(无用户构造函数)：下方 CacheTable 用 calloc 分配槽位数组，
 * 全零字节即"三个空指针 + used=false"，对 POD 是合法初始化。 */
struct MemberTriple {
    const char *class_name_;
    const char *member_name_;
    const char *signautre_;
};

/* 按【内容】比较。原实现是 std::map + 指针比较(class_name_ < other.class_name_)，
 * 同一类名在不同翻译单元取到不同字面量地址时会被当成两个键 → 缓存静默失效、
 * 反复 FindClass。这里改成 strcmp，顺带修掉那个隐患。 */
static inline bool triple_equal(const MemberTriple &a, const MemberTriple &b) {
    if (a.member_name_ == NULL || b.member_name_ == NULL) {
        /* 类缓存只比类名，且要求两者同为类缓存，避免与成员缓存串键 */
        if (a.member_name_ != b.member_name_) return false;
        return strcmp(a.class_name_, b.class_name_) == 0;
    }
    return strcmp(a.class_name_, b.class_name_) == 0 &&
           strcmp(a.member_name_, b.member_name_) == 0 &&
           strcmp(a.signautre_, b.signautre_) == 0;
}

static inline unsigned triple_hash(const MemberTriple &t) {
    unsigned long h = 5381;  /* djb2 */
    for (const char *p = t.class_name_; *p; ++p) h = h * 33 + (unsigned char) *p;
    h = h * 33 + (t.member_name_ ? 2 : 1);  /* 区分类缓存/成员缓存 */
    if (t.member_name_) {
        for (const char *p = t.member_name_; *p; ++p) h = h * 33 + (unsigned char) *p;
        for (const char *p = t.signautre_; *p; ++p) h = h * 33 + (unsigned char) *p;
    }
    return (unsigned) h;
}

/* 开放寻址哈希表，替代 std::map。
 *
 * 为什么不用 std::map：它会从 libc++_static.a 拖入整棵红黑树，红黑树抛
 * std::bad_alloc 又要异常运行时，异常运行时打印类型名又要 __cxa_demangle，
 * demangle 再带 libunwind —— 实测单个 so 从 4KB 涨到 264KB，其中约 105KB
 * 纯粹是 C++ 名字反混淆器。抽一个类 3 个方法的产物因此有 274KB，业务代码
 * 只占 3KB。换成这张表后同一产物 18.9KB。
 *
 * 懒分配：首次 put 才 calloc，容量不足翻倍。原来 .bss 里 2048 槽是 246KB
 * 常驻内存，现在按实际条目数增长，空表零占用。
 *
 * 容量上限：扩容失败(内存不足)时放弃写入 —— 只丢一条缓存，下次重新
 * GetMethodID，功能不受影响。这是与原 std::map(可无限增长)唯一的语义差异。
 */
#ifndef D2C_USE_STL_MAP
template <typename V>
class CacheTable {
public:
    CacheTable() : slots_(NULL), capacity_(0), count_(0) {}
    ~CacheTable() { free(slots_); }

    /* 命中返回 true 并写 *out */
    bool find(const MemberTriple &key, V *out) const {
        if (count_ == 0) return false;
        unsigned mask = capacity_ - 1;
        unsigned i = triple_hash(key) & mask;
        for (unsigned n = 0; n < capacity_; ++n, i = (i + 1) & mask) {
            if (!slots_[i].used) return false;  /* 开放寻址：空槽即不存在 */
            if (triple_equal(slots_[i].key, key)) {
                *out = slots_[i].value;
                return true;
            }
        }
        return false;
    }

    void put(const MemberTriple &key, V value) {
        if (capacity_ == 0 || count_ * 4 >= capacity_ * 3) {
            if (!grow()) return;  /* 扩容失败：静默放弃缓存，功能不变 */
        }
        insert_no_grow(key, value);
    }

    size_t size() const { return count_; }

private:
    struct Slot {
        MemberTriple key;
        V value;
        bool used;
    };

    bool grow() {
        unsigned new_cap = (capacity_ == 0) ? 256u : capacity_ * 2;
        Slot *fresh = (Slot *) calloc(new_cap, sizeof(Slot));
        if (fresh == NULL) return false;
        Slot *old = slots_;
        unsigned old_cap = capacity_;
        slots_ = fresh;
        capacity_ = new_cap;
        count_ = 0;
        for (unsigned i = 0; i < old_cap; ++i) {
            if (old[i].used) insert_no_grow(old[i].key, old[i].value);
        }
        free(old);
        return true;
    }

    /* 前提：capacity_ > 0 且负载未超阈值 */
    void insert_no_grow(const MemberTriple &key, V value) {
        unsigned mask = capacity_ - 1;
        unsigned i = triple_hash(key) & mask;
        for (unsigned n = 0; n < capacity_; ++n, i = (i + 1) & mask) {
            if (!slots_[i].used) {
                slots_[i].key = key;
                slots_[i].value = value;
                slots_[i].used = true;
                ++count_;
                return;
            }
            if (triple_equal(slots_[i].key, key)) {
                slots_[i].value = value;  /* 已存在则覆盖 */
                return;
            }
        }
    }

    Slot *slots_;
    unsigned capacity_;
    size_t count_;

    CacheTable(const CacheTable &);
    void operator=(const CacheTable &);
};

static CacheTable<jfieldID> resvoled_fields;
static CacheTable<jmethodID> resvoled_methods;
static CacheTable<jclass> resvoled_classes;

#else  /* D2C_USE_STL_MAP —— 原版实现，仅用于排障对比 */

struct MemberTripleLess {
    bool operator()(const MemberTriple &a, const MemberTriple &b) const {
        if (a.class_name_ != b.class_name_) return a.class_name_ < b.class_name_;
        if (a.member_name_ != b.member_name_) return a.member_name_ < b.member_name_;
        if (a.signautre_ != b.signautre_) return a.signautre_ < b.signautre_;
        return false;
    }
};

/* 原版语义的薄包装：对外接口与 CacheTable 一致，好让调用点零改动。
 * 注意：原版按【指针】比较类名（上面的 < 比的是 const char*），同一类名
 * 在不同翻译单元取到不同字面量地址时会被当成两个键 —— 缓存会多存几份，
 * 但 GetMethodID 每条都会成功，功能正确。这是历史行为，对比排障时保留原样。 */
template <typename V>
class MapCache {
public:
    bool find(const MemberTriple &key, V *out) const {
        typename std::map<MemberTriple, V, MemberTripleLess>::const_iterator it = map_.find(key);
        if (it == map_.end()) return false;
        *out = it->second;
        return true;
    }
    void put(const MemberTriple &key, V value) { map_[key] = value; }
    size_t size() const { return map_.size(); }

private:
    std::map<MemberTriple, V, MemberTripleLess> map_;
};

static MapCache<jfieldID> resvoled_fields;
static MapCache<jmethodID> resvoled_methods;
static MapCache<jclass> resvoled_classes;

#endif  /* D2C_USE_STL_MAP */
static pthread_mutex_t resovle_method_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t resovle_field_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t resovle_class_mutex = PTHREAD_MUTEX_INITIALIZER;

static const int max_global_reference = 1500;

static void cache_well_known_classes(JNIEnv *env) {
    d2c::WellKnownClasses::Init(env);

    resvoled_classes.put(MemberTriple{"Int", NULL, NULL}, d2c::WellKnownClasses::primitive_int);
    resvoled_classes.put(MemberTriple{"Long", NULL, NULL}, d2c::WellKnownClasses::primitive_long);
    resvoled_classes.put(MemberTriple{"Short", NULL, NULL}, d2c::WellKnownClasses::primitive_short);
    resvoled_classes.put(MemberTriple{"Char", NULL, NULL}, d2c::WellKnownClasses::primitive_char);
    resvoled_classes.put(MemberTriple{"Byte", NULL, NULL}, d2c::WellKnownClasses::primitive_byte);
    resvoled_classes.put(MemberTriple{"Boolean", NULL, NULL}, d2c::WellKnownClasses::primitive_boolean);
    resvoled_classes.put(MemberTriple{"Float", NULL, NULL}, d2c::WellKnownClasses::primitive_float);
    resvoled_classes.put(MemberTriple{"Double", NULL, NULL}, d2c::WellKnownClasses::primitive_double);
}

void d2c_throw_exception(JNIEnv *env, const char *class_name, const char *message) {
    LOGD("d2c_throw_exception %s %s", class_name, message);
    ScopedLocalRef<jclass> c(env, env->FindClass(class_name));
    if (c.get()) {
        env->ThrowNew(c.get(), message);
    }
}

void d2c_filled_new_array(JNIEnv *env, jarray array, const char *type, jint count, ...) {
    va_list args;
    va_start(args, count);
    char ty = type[0];
    bool ref = ty == '[' || ty == 'L';
    for (int i = 0; i < count; i++) {
        if (ref) {
            env->SetObjectArrayElement((jobjectArray) array, i, (jobject) va_arg(args, long));
        } else {
            int val = va_arg(args, jint);
            LOGD("idx = %d, val = %d", i, val);
            env->SetIntArrayRegion((jintArray) array, i, 1, &val);
        }
    }
    va_end(args);
}

int64_t d2c_double_to_long(double val) {
    int64_t result;
    if (val != val) {  // NaN
        result = 0;
    } else if (val > static_cast<double>(INT64_MAX)) {
        result = INT64_MAX;
    } else if (val < static_cast<double>(INT64_MIN)) {
        result = INT64_MIN;
    } else {
        result = static_cast<int64_t>(val);
    }
    return result;
}

int64_t d2c_float_to_long(float val) {
    int64_t result;
    if (val != val) {  // NaN
        result = 0;
    } else if (val > static_cast<float>(INT64_MAX)) {
        result = INT64_MAX;
    } else if (val < static_cast<float>(INT64_MIN)) {
        result = INT64_MIN;
    } else {
        result = static_cast<int64_t>(val);
    }
    return result;
}

int32_t d2c_double_to_int(double val) {
    int32_t result;
    if (val != val) {
        result = 0;
    } else if (val > static_cast<float>(INT32_MAX)) {
        result = INT32_MAX;
    } else if (val < static_cast<float>(INT32_MIN)) {
        result = INT32_MIN;
    } else {
        result = static_cast<int32_t>(val);
    }
    return result;
}

int32_t d2c_float_to_int(float val) {
    int32_t result;
    if (val != val) {
        result = 0;
    } else if (val > static_cast<float>(INT32_MAX)) {
        result = INT32_MAX;
    } else if (val < static_cast<float>(INT32_MIN)) {
        result = INT32_MIN;
    } else {
        result = static_cast<int32_t>(val);
    }
    return result;
}

bool d2c_is_instance_of(JNIEnv *env, jobject instance, const char *class_name) {
    if (instance == NULL) {
        return false;
    }

    ScopedLocalRef<jclass> c(env, env->FindClass(class_name));
    if (c.get()) {
        return env->IsInstanceOf(instance, c.get());
    } else {
        return false;
    }
}

bool d2c_check_cast(JNIEnv *env, jobject instance, jclass clz, const char *class_name) {
    if (env->IsInstanceOf(instance, clz)) {
        return false;
    } else {
        d2c_throw_exception(env, "java/lang/ClassCastException", class_name);
        return true;
    }
}

bool d2c_resolve_class(JNIEnv *env, jclass *cached_class, const char *class_name) {
    if (*cached_class) {
        return false;
    }

    MemberTriple triple;
    triple.class_name_ = class_name;
    triple.member_name_ = NULL;
    triple.signautre_ = NULL;

    if (max_global_reference > 0) {
        ScopedPthreadMutexLock lock(&resovle_class_mutex);

        jclass cached;
        if (resvoled_classes.find(triple, &cached)) {
            *cached_class = cached;
            return false;
        }
    }

    jclass clz = env->FindClass(class_name);
    if (clz) {
        LOGD("resvoled class %s %zu", class_name, resvoled_classes.size());
        if (max_global_reference > 0 &&
            resvoled_classes.size() < (size_t) max_global_reference) {
            ScopedPthreadMutexLock lock(&resovle_class_mutex);
            *cached_class = (jclass) env->NewGlobalRef(clz);
            resvoled_classes.put(triple, *cached_class);
            env->DeleteLocalRef(clz);
        } else {
            *cached_class = clz;
        }
        return false;
    } else {
        return true;
    }
}

bool d2c_resolve_method(JNIEnv *env, jclass *cached_class, jmethodID *cached_method, bool is_static,
                        const char *class_name, const char *method_name, const char *signature) {
    if (*cached_method) {
        return false;
    }

    if (d2c_resolve_class(env, cached_class, class_name)) {
        return true;
    }

    MemberTriple triple;
    triple.class_name_ = class_name;
    triple.member_name_ = method_name;
    triple.signautre_ = signature;
    {
        ScopedPthreadMutexLock lock(&resovle_method_mutex);

        jmethodID cached;
        if (resvoled_methods.find(triple, &cached)) {
            *cached_method = cached;
            return false;
        }
    }

    if (is_static) {
        *cached_method = env->GetStaticMethodID(*cached_class, method_name, signature);
    } else {
        *cached_method = env->GetMethodID(*cached_class, method_name, signature);
    }

    if (*cached_method) {
        ScopedPthreadMutexLock lock(&resovle_method_mutex);
        resvoled_methods.put(triple, *cached_method);
    }

    return *cached_method == NULL;
}

bool d2c_resolve_field(JNIEnv *env, jclass *cached_class, jfieldID *cached_field, bool is_static,
                       const char *class_name, const char *field_name, const char *signature) {
    if (*cached_field) {
        return false;
    }

    if (d2c_resolve_class(env, cached_class, class_name)) {
        return true;
    }

    MemberTriple triple;
    triple.class_name_ = class_name;
    triple.member_name_ = field_name;
    triple.signautre_ = signature;
    {
        ScopedPthreadMutexLock lock(&resovle_field_mutex);

        jfieldID cached;
        if (resvoled_fields.find(triple, &cached)) {
            *cached_field = cached;
            return false;
        }
    }

    if (is_static) {
        *cached_field = env->GetStaticFieldID(*cached_class, field_name, signature);
    } else {
        *cached_field = env->GetFieldID(*cached_class, field_name, signature);
    }

    if (*cached_field) {
        ScopedPthreadMutexLock lock(&resovle_field_mutex);
        resvoled_fields.put(triple, *cached_field);
    }

    return *cached_field == NULL;
}

extern "C" {

JNIEXPORT void JNICALL
Java_amimo_dcc_DccApplication_init__(JNIEnv *env, jobject thiz) {

}
}

JNIEXPORT jint JNI_OnLoad(JavaVM *vm, void *reserved) {
    JNIEnv *env;

    if (vm->GetEnv((void **) &env, JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }

    // Note: deliberately NO "is the loader class present" check here. The
    // so may be loaded by ANY class — pipeline consumers insert
    // System.loadLibrary into the classes that own the extracted methods,
    // and those apps never contain amimo.dcc.DccApplication. A FindClass +
    // exit(1) here would kill every such app at startup.
    cache_well_known_classes(env);
    const char *result = dynamic_register_compile_methods(env);
    if (result != nullptr)
    {
        LOGD("d2c_throw_exception %s", result);
        return JNI_ERR;
    }
    return JNI_VERSION_1_6;
}
