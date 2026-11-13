# dex2c 分发包（dex2c.zip）

本仓库是 `artifact-pipeline` 唯一转译引擎的源码。`protect.sh` 调
`ensure_dex2c` 从 `tools/dex2c.zip` 解压后使用。

## 打包

### 正式发布：走 CI（唯一正确路径）

```bash
git commit -m "..."
git tag v1.0.3
git push origin main --tags     # ← 触发 .github/workflows/release.yml
```

CI 自动完成：冒烟测试（`compileall` + `dcc.py --help`）→ rsync 打包 →
**逐项校验 zip 内容**（`dcc.py`/`apkeditor.jar`/`project/jni/Android.mk` 等）
→ 创建 GitHub Release，产出 `Dex2C-v1.0.3.zip`。

然后把该 Release 资产改名成 `artifact-pipeline/tools/dex2c.zip`。

**为什么必须走 CI**：手工产物没有 tag、没有 Release 记录、没有冒烟测试，
出了问题无法追溯是哪个 commit。仓库的 tag 历史（`v1.0.0`/`v1.0.1`/`v1.0.2`）
就是靠这条链路维护的。

### 本地快速测试：`make-zip.sh`（**不是发布工具**）

改引擎后想立刻在本地验证产物行为/体积、不等 CI 时用：

```bash
./make-zip.sh v1.0.3-dev       # 建议加 -dev 后缀，一眼看出是本地包
```

它**没有** CI 的冒烟测试与 zip 校验，产出的包不能当发布物。
用白名单（不是排除法）收集文件——历史教训：手工 `zip -r` 曾把
`proj/`、`__pycache__/`、`build-tools/`、`platform-tools/` 一起塞进去，
包从 9.1MB 涨到 28MB。

### 部署到消费方

```bash
cp <zip> ../artifact-pipeline/tools/dex2c.zip
rm -rf ../artifact-pipeline/tools/Dex2C    # 必须删！ensure_dex2c 幂等靠 dcc.py 存在
```

⚠️ **`ensure_dex2c` 是幂等的**：只要 `tools/Dex2C/dcc.py` 还在，它就直接
复用旧解压产物、不再解压。换了 zip 但没删 `tools/Dex2C`，流水线会静默
继续用旧引擎。

## 产物体积：改引擎时最该盯的两件事

### 1. `.so` —— 别让 C++ 标准库被拖进来

`project/jni/nc/Dex2C.cpp` 的 JNI 解析缓存**不要用 `std::map`/`std::vector`**
等 STL 容器。链接器会从 `libc++_static.a` 拉入整棵红黑树 → 红黑树抛
`std::bad_alloc` 需要异常运行时 → 异常运行时打印类型名需要
`__cxa_demangle` → demangler 再带 libunwind。

实测因果（抽 1 个类的 4 个方法）：

| 配置 | arm64 .so |
|---|---|
| 空 so（仅 `JNI_OnLoad`） | 4,040 B |
| + 一个 `std::map` 成员和一次 insert | **263,712 B** |
| `std::map` → C 哈希表 | 51,240 B |
| 同上，但流水线另加 nox 锁箱 + VMP | 463,728 B |

当前实现是开放寻址哈希表（`CacheTable`），懒分配，首次 `put` 才 `calloc`。

### 2. `dex` —— 必须剥离 `debug_info`

`dcc.py` 的 `ApkEditor.decompile` 带 `-no-dex-debug`（对应 apktool 的
`--no-debug-info`）。

**不加的后果**（实测）：原包经 R8 处理后 `debug_info` 只有 1,037 条 /
65KB，但 dex2c 回编时把 smali 里的调试指令完整写回，暴涨到 15,821 条 /
**1,047,284 B**，未压缩 +914KB、压缩进 APK +313KB。

| | debug_info | dex 大小 | APK |
|---|---|---|---|
| 原始样本 | 1,037 条 | 7,538,948 | 4,880,835 |
| 无 `-no-dex-debug` | 15,821 条 | 8,452,924 | 5,229,143 |
| 有 `-no-dex-debug` | **0 条** | 7,403,416 | **4,938,327** |

这份信息只在崩溃时用来还原源码行号，对运行时零作用，Release 包不需要。

## 排障开关：`D2C_USE_STL_MAP`

哈希表替代了 `std::map`，去掉了 libc++ 整条链。但"去掉"本身也可能引入
新问题（缓存容量、并发、键比较语义）。真机出问题时：

```makefile
# project/jni/Application.mk
APP_CPPFLAGS += -fvisibility=hidden -DD2C_USE_STL_MAP
```

即回到改动前的 `std::map` 实现，**不改代码、不用回滚 zip**。两套实现对外
接口完全一致（`put`/`find`/`size`），切换不动任何调用点。

实测两版体积：`9,560 B`（哈希表）vs `268,192 B`（`std::map`）。

⚠️ 用 `std::map` 时必须有 STL。`APP_STL` 默认保持 `c++_static`（不是
`none`），就是为了让两条路都能编——不用 STL 时代码不引用
`libc++_static.a`，体积与 `none` 相同（实测 4,352 B = 4,352 B）。

## 已知的语义差异

| | `std::map`（原版） | 哈希表（当前） |
|---|---|---|
| 容量 | 无限增长 | 扩容失败时**静默丢弃**新缓存项 |
| 键比较 | 指针比较（同一类名不同字面量地址 → 存多份） | `strcmp` 内容比较（合并为一份） |

丢弃缓存项只意味着下次重新 `GetMethodID`，结果仍正确，只是略慢。
键比较改成 `strcmp` 是顺带修掉了原版的缓存失效隐患。
