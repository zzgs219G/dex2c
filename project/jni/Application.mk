# APP_STL 用 c++_static（不是 none）——
#   · 默认的哈希表实现不引用任何 STL，链接器不会从 libc++_static.a 拉东西进来，
#     体积与 APP_STL := none 完全一致（实测 4,352 字节 vs 4,352 字节）。
#   · 保留 c++_static 是为了让排障开关 D2C_USE_STL_MAP 能正常编译：
#     定义该宏会回到原版 std::map 实现，那时必须有 STL。
#
# 排障：真机出现闪退/缓存异常时，往下面 APP_CPPFLAGS 追加
#     -DD2C_USE_STL_MAP
# 即回到改动前的实现，无需改代码、无需回滚 dex2c.zip。
# 详见 nc/Dex2C.cpp 顶部「解析缓存的两套实现」注释。
APP_STL := c++_static

APP_CPPFLAGS += -fvisibility=hidden
APP_PLATFORM := android-21
# APP_PLATFORM := android-19
APP_ABI := armeabi-v7a arm64-v8a
# APP_ABI := armeabi-v7a arm64-v8a x86_64 x86
