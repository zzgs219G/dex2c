# d2c 对比与构造函数闪退分析（汇报）

> 2026-10-14 · 以 Dex2C 为主体、汲取 d2c 优点前的调研汇报
> 对比对象：`Kirlif/d2c`（本地 `../d2c`）vs 本仓库（codehasan/Dex2C 上游 + 本 fork）
> v2 修订：补充了「过滤器守卫被注释」这条独立抽取路径（初版只覆盖了加载器路径）

---

## 0. 摘要

1. **d2c 与本项目同宗**：都源自 amimo/dcc（2019）。核心引擎 4 个文件
   （`basic_blocks.py` / `instruction.py` / `opcode_ins.py` / `writer.py`）**逐字节一致**，
   JNI 侧 7 个头/源文件也完全一致。**保护能力同源，汲取优点 = 改外围，风险小。**
2. **构造函数闪退有两条独立的抽取路径**（已发布 v1.0.0 同时中招）：
   - **路径 A（过滤器）**：上游把 `MethodFilter.should_compile` 里的 `<init>` 跳过守卫
     **注释掉了** → 默认 `.*` 过滤器把**所有 app 构造函数**抽成 native → 崩；
   - **路径 B（加载器）**：加载器模板把 `<init>` 声明为 native，C++ 桩用
     `CallVoidMethodA` 调构造函数（JNI 违规）→ manifest 无自定义 Application 的 app
     启动即崩。
3. **引擎层面根因**（两个项目共有）：转译 `invoke-direct {p0}, Lsuper;-><init>()V`
   时发出 `env->CallVoidMethodA(实例, <init>的methodID, ...)`——JNI 规范禁止；
   正确的 `CallNonvirtual*` 分支只挂在一条死代码（`invoke-super`，真实 dex 不存在该
   指令）上。**所以"抽取构造函数"在当前引擎下不可能安全**，守卫是唯一防线。
   d2c 默认守卫开着（另有 `--allow-init`，走的是同一条坏路径，同样不安全），
   所以 d2c 没有这个 bug。
4. 修复：恢复守卫（路径 A）+ 加载器构造函数还原为普通 smali 并删除坏桩（路径 B）。
   **不建议移植 d2c 的 `--allow-init`**，除非先修引擎。
5. 连带发现：`--custom-loader` 在本项目实际不可用（JNI_OnLoad 硬编码类名 +
   `exit(1)`；`init()` 的 JNI 符号名写死）。

---

## 1. 两项目差异全景（浓缩）

| 维度 | d2c (Kirlif) | 本项目 (Dex2C) |
|---|---|---|
| 最后上游提交 | 2025-01-06（停滞 ~21 个月） | 2026-03-12 + 本 fork 活跃 |
| Python 依赖 | 仅 `lxml` | 8 个包（含 `future` py2 残留） |
| vendored androguard | 274K（裁剪版） | 8.5M（完整版） |
| APKEditor | 1.4.1 | **1.4.7（更新）** |
| 加载器策略 | **app 自身 Application 即加载器**，不注入新类 | 总是注入 `DccApplication` 派生类 |
| 构造函数守卫 | 开（`--allow-init` 可关，但不安全，见 §2.4） | **上游注释掉了（本 bug 路径 A）** |
| 签名 | 按真实 minSdk 自适应（API≥24 自动禁 v1） | 静态区间硬编码 |
| APP_PLATFORM | 按 minSdk 自动调整 | 固定 android-21 |
| 16KB 页对齐 | ❌ 缺失（Android 15+ 新设备风险） | ✅ `-Wl,-z,max-page-size=16384` |
| OLLVM | ❌ | ✅ cfg 开关注入 `LOCAL_CPPFLAGS` |
| `--allow-global` 护栏 / `--disable-signing` | ❌ | ✅ |
| zipalign | 不依赖 | 依赖 PATH（保持，不移植 d2c 的省略） |
| DEX 输入 | ✅（输出 libs+dex 的 zip） | ❌ 仅 APK 流程 |
| 发布流水线 | ❌ | ✅ 打 tag → zip → Release（v1.0.0 已验证） |

**结论**：继续以 Dex2C 为主体（16KB 对齐、新 APKEditor、护栏、流水线都是实资产），
汲取 d2c 的加载器策略与自适应逻辑，不移植其 zipalign 省略、旧版 APKEditor，
**也不移植 `--allow-init`**（见 §2.4）。

---

## 2. 闪退根因分析

### 2.1 现象与触发条件

| 场景（默认 `.*` 过滤器） | 触发路径 | 结果 |
|---|---|---|
| 任何 app，构造函数被过滤器匹配 | **路径 A** | 构造函数被抽成 native，运行到 super 构造调用即崩 |
| manifest 无自定义 Application（小工具类 app 常见） | **路径 B**（叠加 A） | 框架实例化注入的加载器 → native `<init>` → 坏桩 → 启动即闪退 |
| app 有自定义 Application | 路径 B 不触发 | 仍受路径 A 影响 |

### 2.2 路径 A：过滤器守卫被注释（已发布 v1.0.0 的实际状态）

`git show v1.0.0:dcc.py`（`MethodFilter.should_compile`）：

```python
        # Skip static constructor
        if name == "<clinit>":
            return False

        # Skip public constructor
        # if name == "<init>":
            # return False          ← 上游把守卫注释掉了
```

d2c 对应位置守卫是**开着的**：

```python
        if name == "<clinit>":
            return False
        if name == "<init>" and not self.allow_init_methods:
            return False
```

### 2.3 路径 B：加载器模板 native 构造函数 + 违规 C++ 桩

**① 模板把构造函数"抽"成 native** — `loader/DccApplication.smali`：

```smali
.method public native constructor <init>()V
.end method
```

**② C++ 桩用违规 JNI 调构造函数** — `project/jni/nc/Dex2C.cpp`：

```cpp
Java_amimo_dcc_DccApplication__0003cinit_0003e__(JNIEnv *env, jobject thiz) {
    auto instance = (jobject) env->NewLocalRef(thiz);
    jclass application = env->FindClass("android/app/Application");
    jmethodID init = env->GetMethodID(application, "<init>", "()V");
    env->CallVoidMethodA(instance, init, {});   // ← 违规：构造函数不能走 Call*Method
}
```

d2c 的同一文件是普通 smali 构造函数（`invoke-direct` 调 super），无 C++ 桩 → 免疫。

### 2.4 引擎层根因（两项目共有，解释"为什么一抽就崩"）

`writer.py` 的调用发射逻辑：

```python
if invoke_type == 'super':      # CallNonvirtual —— 正确做法
    env->CallNonvirtual%sMethodA(...)
elif invoke_type == 'static':
    env->CallStatic%sMethodA(...)
else:                           # 'direct'/'virtual'/'interface'
    env->Call%sMethodA(...)    # ← invoke-direct 全部落到这里
```

真实 dex 里 super 构造调用 = `invoke-direct {p0}, Lsuper;-><init>()V` →
标记为 `'direct'` → 发射 `CallVoidMethodA(实例, <init>的methodID, args)` →
**JNI 违规，ART 未定义行为**。

唯一产生 `'super'` 的处理器是 `invokesuper()`（`opcode_ins.py:982`）——而 Dalvik
**根本没有 `invoke-super` 指令**，该分支是死代码。

**推论**：
1. 当前引擎下抽取构造函数**不可能安全**——守卫是唯一防线，上游注释守卫 =
   引入回归；
2. d2c 的 `--allow-init` 走的是同一条 `Call*MethodA` 坏路径，**同样不安全**，
   不要移植（要支持必须先改引擎：`invoke-direct` 且目标为 `<init>` 时发
   `CallNonvirtual*`，并处理未初始化 `this` 的语义——工程量另评估）。

### 2.5 连带 bug（同一片代码里挖出来的）

| # | 位置 | 问题 | 后果 |
|---|---|---|---|
| ② | `Dex2C.cpp` `JNI_OnLoad` | `FindClass("amimo/dcc/DccApplication")` 硬编码 + `exit(1)` | `--custom-loader` 一改名，so 加载即 `exit(1)` → **任何 app 闪退** |
| ③ | `Dex2C.cpp` 桩符号名 | `Java_amimo_dcc_DccApplication_init__` 等 JNI 符号写死默认类名 | 改名后 `init()` 解析不到 → `UnsatisfiedLinkError` |

即：**`--custom-loader` 在本项目实际不可用**（默认名能用，改名必炸）。
d2c 无此三处代码，天然免疫。

### 2.6 为什么 d2c 没有这一整类问题

d2c 的加载器策略在架构上绕开了 JNI 边界：
- App 有 Application → 在**它自己的 `<clinit>`** 里插 2 条普通指令
  （`const-string` + `System.loadLibrary`），**不注入任何新类**；
- App 无 Application → 注入模板，但构造函数是普通 smali；
- 没有 `init()` native 钩子、没有硬编码 `FindClass`、没有 native 构造函数。

加载器类零 native 成员 → 无 JNI 符号问题、无构造函数问题、壳特征也更少。

---

## 3. 修复方案

### 方案 A：最小修复（Phase 1，止崩溃）

1. **恢复 `should_compile` 的 `<init>` 守卫**（对齐 d2c 默认行为）；
2. `loader/DccApplication.smali`：构造函数改为普通 smali（照抄 d2c 写法）；
3. `Dex2C.cpp`：删除坏的 `Java_amimo_dcc_DccApplication__0003cinit_0003e__` 桩。

保留 `init()` native 钩子（found-branch 依赖它），**不动**连带 bug ②③。
风险最低，可发 v1.0.1。

### 方案 B：结构性修复（Phase 2，与"汲取 d2c 加载器策略"合并）

加载器零 native 成员 + 无硬编码类名，一次性根治 §2.5 全部连带 bug：

1. found-branch 改插 `loadLibrary` 两指令（不插 `init()` 调用）→ 删除 `init__` 桩；
2. loader 模板纯 smali 化；
3. `JNI_OnLoad` 删除硬编码 `FindClass` + `exit(1)`（保留
   `cache_well_known_classes` / `dynamic_register` 初始化）；
4. 收获 d2c 的隐蔽性：app 有 Application 时**不注入新类**，特征面归零。

前置条件已具备：Dex2C 的 found-branch 已有 clinit 插入与"寄存器 0→1 补齐"逻辑
（`dcc.py:1069-1073`），只需把插入内容从 `init()` 调用换成 `const-string`+`loadLibrary`。

**建议：A 先行止崩溃；B 与第 4 节优点 1 一起做。**

---

## 4. 汲取 d2c 优点清单（按收益排序）

| 优先级 | 优点 | 收益 | 改动面 |
|---|---|---|---|
| 1 | **加载器策略**（app 自身 Application 即加载器） | 防特征收益最大 + 根治 §2.5 全部 bug | dcc.py 注入逻辑 + loader 模板 + Dex2C.cpp（=方案 B） |
| 2 | minSdk 自适应签名（API≥24 禁 v1）+ APP_PLATFORM 自动调整 | 签名兼容性、平台适配 | dcc.py `sign()` + `Application.mk` 改写 |
| 3 | `--lib-name` CLI 选项 | 免手改 Android.mk | dcc.py 参数 + Android.mk 生成 |
| 4 | （可选）androguard 瘦身 + 依赖裁到 lxml-only | Termux/CI 依赖面 8→1 | 大，收益中等，放最后 |

**不移植**：`--allow-init`（§2.4，引擎不支持）、zipalign 省略（16KB 时代对齐
更重要）、APKEditor 1.4.1（本项目已是 1.4.7）。

---

## 5. 行动计划与验收

| Phase | 内容 | 验收 |
|---|---|---|
| 1 | 方案 A（止崩溃） | 无 Application 的测试 APK 加固后能启动（真机/模拟器）；smali/ndk-build 产物人工审查 | 
| 2 | 方案 B + 优点 1 | 有/无 Application 两类 app 加固后均正常启动；`--custom-loader` 改名不再闪退 |
| 3 | 优点 2、3 | 新参数冒烟 + 真机签名校验 |
| 4 | （可选）依赖瘦身 | CI 冒烟测试等价通过 |

**待验证项纪律**：启动类验证必须真机/模拟器，无设备时一律标"待验证"，不伪造结论。
CI 冒烟测试只能证明导入链/配置，证明不了运行期 JNI 行为。

---

## 6. 执行记录（2026-10-14）

**Phase 1 已实施：**
- `dcc.py`：恢复 `should_compile` 的 `<init>` 守卫（取消上游注释）
- `loader/DccApplication.smali`：native 构造函数 → 普通 smali 构造函数（`invoke-direct` super）
- `Dex2C.cpp`：删除坏的 `_0003cinit_0003e_` JNI 桩
- `.gitignore`：忽略本地 SDK 目录 `build-tools/`、`platform-tools/`

**验证情况：**
- ✅ `python3 -m py_compile dcc.py` 通过（本机 Termux）
- ✅ smali 模板与 d2c 已验证版本逐指令一致（仅 `.registers 1` vs `.locals 0` 表述差异，语义等价）
- ⚠️ **待验证**：真机加固 + 启动（需 NDK + 真机；本地 ndk-build 不可行——Termux 无 aarch64 编译该工程的条件）。ndk-build/启动验证由消费方流水线或真机完成
- 本机 `dcc.cfg` 的 `ndk_dir` 已指向 Termux NDK（`/data/.../android-ndk-r29`），属本地环境配置，未纳入提交
