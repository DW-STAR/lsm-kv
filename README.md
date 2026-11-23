# lsm-kv

用 **C++17** 从零实现的 LSM-Tree 键值存储引擎。不依赖任何第三方库，不使用现成的存储组件。

目标是能白板画出架构、能回答每一个设计取舍，而不只是"跑通一个 demo"。

---

## 它现在能做什么

```cpp
lsmkv::DB* db = nullptr;
lsmkv::DB::Open("./mydb", &db);

db->Put("name", "liupei");
db->Delete("obsolete");

std::string v;
db->Get("name", &v);        // OK, v == "liupei"
db->Get("obsolete", &v);    // NotFound —— 删掉的不会自己爬回来

delete db;                  // 数据落盘
// 进程退出。下次 Open 同一个目录，数据还在。
```

已实现：

- **有序内存表** — 跳表，带 iterator 与 `Seek`（第一个 `key >= target`）
- **双层内存缓冲** — active / immutable，写满自动冻结换表，不阻塞写入
- **三态语义** — `kFound` / `kDeleted` / `kNotFound`，墓碑能终结查找
- **带索引的 SSTable** — 稀疏索引 + 二分定位，每 16 条记录一个锚点
- **文件格式自校验** — 记录级 CRC32，footer 带 magic 与版本号
- **崩溃恢复的组件** — WAL 追加日志（已独立验收，尚未接入 DB，见「路线」）

---

## 架构

```text
写入
  Put(k, v)
   └─ MemTableSet::Put
        ├─ active_->Put()             跳表插入，O(log n)
        └─ if ShouldFlush(): Rotate()
             ├─ active_ 冻结为 immutable_
             ├─ 换一张空 active_ 继续收写入   ← 写请求一秒都不等
             └─ FlushImmutable()
                  └─ 顺序遍历跳表 → SSTableBuilder → 000001.sst

读取
  Get(k)
   ├─ Lookup(active)        → kFound / kDeleted / kNotFound
   ├─ Lookup(immutable)     → 同上
   └─ for sst in 新 → 旧
        └─ FindRecord(k)    → 二分锚点 + 块内扫描
```

**整条链只有一条规则**：命中值或命中墓碑，立刻停手；只有 `kNotFound` 才有资格往下走一层。
任何一个环节把三态压成两态，"删掉的数据从更老的层复活"就会发生。

---

## 几个关键取舍

**为什么先定 `db.h` 再写实现**
接口是承诺，实现是私事。这个项目换过三版实现（`std::map` → 跳表 → 接 SSTable），
`include/lsmkv/db.h` 一个字节都没改过，所有测试与调用方一行未动。

**内存表为什么用跳表而不是红黑树**
跳表实现简单、无旋转、并发下更容易做无锁，且天然支持范围扫描。
代价是常数因子略大、内存占用略高 —— 在一个"写入远多于读取"的场景里这笔账是划算的。

**为什么需要 immutable 这张"多余"的表**
把 4MB 刷进磁盘要几十毫秒，这期间如果继续往同一张表里写，指针会在遍历过程中被改。
所以写满时不是"刷这张表"，而是"冻结它、换一张新的"：前台继续收写入，后台慢慢刷。
**这也解释了为什么这里必须是 `unique_ptr` 而不是 `shared_ptr`** —— 一张表在同一时刻只能有一个主人。

**为什么查询返回三态而不是 `bool`**
`bool` 装不下"墓碑"这件事。把墓碑报成"没有"，上层就会去查更老的表，那里的旧值立刻复活。
这个 bug 不崩、不报错、不丢数据，只是删了又活 —— 它在本项目里真实发生过一次。

**索引为什么是稀疏的**
每条记录都建索引，索引区会和数据一样大，失去意义。每 16 条记一个锚点，
查询变成 `log2(N/16)` 次二分 + 最多 16 条顺序扫描，仍然是 O(log N)，但索引只占 1/16。
（常数 16 是教学选择；真实引擎按字节切块，通常 4KB，对齐 I/O 页。）

---

## 目录

```
include/lsmkv/
  db.h               对外接口（三版实现共用）
  status.h           错误码与消息
  slice.h            零拷贝的只读视图
  record.h           记录格式 + CRC32 + Encode/Decode
  crc32.h            CRC32 查表实现
  append_only_log.h  WAL 追加日志 + 崩溃恢复
  skip_list.h        跳表（含 iterator / Seek）
  mem_table.h        单张内存表（记账、冻结、三态 Lookup）
  mem_table_set.h    active + immutable 与查找顺序
  sstable.h          SSTable v2：builder / reader / 稀疏索引
src/
  db_mem.cpp         第 2 版：纯内存
  db_sst.cpp         第 3 版：内存 + 磁盘
  demo_*.cpp         17 个可执行验收程序
tests/
  test_basic.cpp     接口级测试
  test_skip.cpp      跳表单测（含 6 个变异体检测）
  test_iter.cpp      iterator / Seek
docs/
  whats-next.html    路线图
```

---

## 构建

**Windows（MinGW g++）**

```powershell
.\build.ps1 list              # 看看有哪些验收程序
.\build.ps1 all               # 编译全部
.\build.ps1 demo_restart init # 写入数据后退出
.\build.ps1 demo_restart      # 新进程读回来
```

**Linux / macOS**

```bash
g++ -std=c++17 -Wall -Wextra -Iinclude src/demo_sstable.cpp -o demo_sstable && ./demo_sstable
```

---

## 验收

17 个可执行程序，每个都是"跑起来自己断言"，全部通过才退出 0。

| 程序 | 验证什么 |
|---|---|
| `demo_crash` | 进程被 kill 后，WAL 能恢复到最后一个完整记录 |
| `demo_corrupt` | 尾部截断 / 单 bit 翻转 / 长度字段损坏，四种结局各就各位 |
| `demo_memset` | 三态查找、冻结、背压、与 `std::map` 随机对拍 |
| `demo_sstable` | 索引区、footer、二分定位、乱序写入被拒 |
| `demo_restart` | 8000 条数据跨进程重启，6857 条存活 / 1143 条墓碑生效 / 0 条错 |

`demo_restart` 的实测输出：

```text
phase 1   wrote 8000 keys, deleted 1143 (every 7th), process exited
phase 2   6857 alive / 1143 deleted / 0 wrong
          every deleted key stayed deleted   PASS
```

---

## 路线

```text
01 地基     记录格式 / CRC / 跳表             ✅
02 内存     MemTable / 冻结 / 三态            ✅
03 磁盘     SSTable / 索引二分 / 接到 DB      ✅
04 崩溃安全 WAL 接入 DB + 重放 + 清日志       ⏳
05 空间并发 compaction / 后台线程 / bloom     ⬜
06 能讲     benchmark / 文档                  ⬜
```

---

## 已知局限

这一版有意留下四个洞，每一条都对应一个明确的设计问题：

1. **没有接入 WAL** —— 现在靠析构函数强行刷盘。进程被 `kill -9` 会丢掉未落盘的部分。
2. **没有 MANIFEST** —— 启动时从 `000001.sst` 逐个探测，中间缺一个文件，后面的全部读不到。
3. **没有 compaction** —— `.sst` 只增不减，墓碑占的空间永不回收，读放大随文件数线性增长。
4. **刷盘是同步的** —— `Put` 会等文件写完。真实引擎把这一步交给后台线程。

---

## 参考

- *The Log-Structured Merge-Tree* (O'Neil et al., 1996)
- LevelDB / RocksDB 的设计笔记
