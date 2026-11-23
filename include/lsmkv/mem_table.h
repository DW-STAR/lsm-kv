#pragma once

#include <cstddef>
#include <string>

#include "lsmkv/skip_list.h"
#include "lsmkv/status.h"

namespace lsmkv {

// ══════════════════════════════════════════════════════════════
//  一条记录的内存固定开销 —— 估算用的常数
//
//  ★ 为什么估 128 而不是 0？
//
//    key 和 value 是变长的，但一条记录还有【固定部分】：
//
//      std::string key        MSVC x64 上是 32 字节
//      std::string value      32
//      RecordType type        1（补齐到 8）
//      std::vector<Node*> next 24（三个指针：begin/end/cap）
//      ─────────────────────────────
//      合计                   约 96~128 字节（看对齐）
//
//    这还没算 next 数组在堆上的那几根指针（1~12 个 × 8 字节）。
//
//  ★ 所以这是一个【偏保守的估算】，不精确，也不需要精确。
//
//    存储引擎里所有「内存占用」都是估算 —— 因为精确统计要付出
//    额外的记账成本，而它只用来做一个决定：**要不要冻结落盘**。
//    为了这个决定，误差 20% 完全无所谓。
//    （LevelDB 用 arena 分配器，记的是「已分配的块数 × 块大小」，
//      同样是估算。工程上没有人去算精确值。）
//
//  ★ 你可以自己验一遍：demo_memtable 会打印 sizeof(lsmkv::Node)。
//    如果差得离谱，说明这个常数该调 —— 但别调成"精确值"，
//    它本来就不是干这个的。
// ══════════════════════════════════════════════════════════════
inline constexpr size_t kMemTableEntryOverhead = 128;

// 一张 MemTable 的默认容量上限：4 MB。
//
// 为什么是 4MB？没有理由。LevelDB 的 write_buffer_size 默认 4MB，
// 我们跟它对齐。这个数在 1MB~64MB 之间都属于合理区间，
// 调大 = 落盘更少但崩溃恢复更慢（要重放更多 WAL），调小反之。
inline constexpr size_t kDefaultMemTableBytes = 4u << 20;

// ══════════════════════════════════════════════════════════════
//  ★ 11-10 新增：查表的结果 —— 三态，不是两态
//
//    kNotFound  这张表里根本没有这个 key
//    kFound     有，而且是个值
//    kDeleted   有，但是一条墓碑
//
//  ★ 为什么"有墓碑"必须单独成一态，不能并进 kNotFound？
//
//    因为上层要做的事情相反：
//      kNotFound → 继续查更老的表
//      kDeleted  → 立刻停手，返回"不存在"
//
//    混在 kNotFound 里，删除过的 key 会被更老的表"复活" ——
//    这叫 resurrect（亡灵复生），是 LSM 里最经典的一类 bug：
//    它不崩、不报错、不丢数据，只是**删了又活过来**。
//
//  ★ 两态装不下三件事，这是命名问题不是技术问题：
//    你写 bool 的时候就已经决定了这个系统只能回答"有/没有"，
//    等发现需要"有什么"的时候，只能回来改签名。
// ══════════════════════════════════════════════════════════════
enum class LookupResult : unsigned char {
  kNotFound = 0,
  kFound = 1,
  kDeleted = 2,
};

// ══════════════════════════════════════════════════════════════
//  MemTable —— 2026-11-08
//
//  ── 【术语】memtable（内存表）。LSM 里"当前正在接收写入的那张内存表"。
//
//  ── 【定义】包着 SkipList 的一层薄壳，负责 SkipList 不该负责的事。
//
//  ── 【为什么不能直接用 SkipList】────────────────────────────
//
//    SkipList 只解决一个问题：**按 key 有序地存和查**。
//    它是纯粹的索引结构，不知道自己在什么系统里，也不该知道。
//
//    但 MemTable 在 LSM 里还要负责另外四件事：
//
//      ① 记账 —— 我现在占了多少内存？该不该冻结了？（今晚做）
//      ② 状态 —— 冻结之后只能读不能写（immutable）（11-09 做）
//      ③ 顺序 —— 每次写必须先落 WAL，再改内存（11-12 做）
//      ④ 所有权 —— 冻结后整张表要交给后台线程去落盘（11-11 做）
//
//    这四件事没有一件是"有序存查"的子问题。把它们塞进 SkipList，
//    SkipList 就再也不能被单独测试、单独替换了 —— 那才是真正的技术债。
//
//    ★ 一句话：**封装不是为了藏起来，是为了让两边都能只干一件事。**
//
//  ── 【比喻】────────────────────────────────────────────────
//    SkipList 是书架（按顺序摆好、能快速找到某本书）；
//    MemTable 是图书管理员，除了用书架，还得记着：
//    这架子还能放多少本、满了要不要叫人来清空、清空前要不要先登记。
//
//  ── 【和 SkipList 的分工边界】──────────────────────────────
//    凡是「键值有序存取」→ SkipList
//    凡是「资源、状态、生命周期」→ MemTable
//    这条边界以后每天都用得上。
// ══════════════════════════════════════════════════════════════
class MemTable {
 public:
  explicit MemTable(size_t max_bytes = kDefaultMemTableBytes)
      : max_bytes_(max_bytes) {}

  // 持有裸指针的 SkipList，同样不能拷贝。
  MemTable(const MemTable&) = delete;
  MemTable& operator=(const MemTable&) = delete;

  // ═══════════════════════════════════════════════════════════
  //  ★ 11-09 新增：冻结（freeze）
  //
  //  为什么需要它 —— 先算一笔账（demo_flush_cost 会把这三个数打出来）：
  //
  //    · 把 4MB 顺序写进文件并真正落盘（fsync）：几十毫秒量级
  //    · 如果「写满就直接刷盘」，这几十毫秒里【所有写请求都得等】
  //    · 10 万 QPS 下，几十毫秒 = 几千个请求排队，P99 直接爆
  //
  //  所以真实做法是【换表而不是刷表】：
  //
  //      写满了  →  把当前这张表标成「冻结」（只读）
  //                开一张新的空表，写请求立刻转向新表
  //                旧表交给后台线程慢慢刷，前端一秒都不用等
  //
  //  这一手有一个专门的名字：**immutable memtable**（不可变内存表）。
  //  LevelDB / RocksDB / TiKV 都是这个结构，一字不差。
  //
  //  ★ 为什么"冻结"必须是真的只读，而不只是"我们不写它"？
  //
  //    因为后台线程正在遍历这张表往文件里写。这时候谁来一条 Put，
  //    链表的指针就在遍历过程中被改了 —— 轻则漏数据，重则死循环。
  //    这不是"要不要检查"的风格问题，是**并发正确性的必要条件**。
  //
  //    （他 11-05 问过「我不会改链表，加 const 有什么意义」——
  //      这里是同一个问题的工业化版本：当有第二个线程的时候，
  //      "我不会写错"这条保证就失效了，必须由代码强制。）
  // ═══════════════════════════════════════════════════════════
  void Freeze() { frozen_ = true; }
  bool IsFrozen() const { return frozen_; }

  // 所有写路径的唯一关口。返回 OK 表示可以写。
  //
  // ★ 为什么收成一个函数，而不是在 Put / Delete 里各写一遍 if？
  //   因为以后还会有别的写入口（比如 11-12 的 WAL 回放）。
  //   三处各写一遍 if，将来改判定条件就会漏掉一处 —— 经典的"复制粘贴 bug"。
  //   一个关口，所有车都得从这儿过。
  Status CheckWritable() const {
    if (frozen_) {
      return Status::InvalidArgument(
          "memtable is frozen (immutable): write to the active table instead");
    }
    return Status::OK();
  }

  // ═══════════════════════════════════════════════════════════
  //  TODO 5（你写）：让 Put 和 Delete 先过 CheckWritable 这一关
  //
  //  在这两个函数的**最前面**各加两行（顺序不能变）：
  //
  //      const Status st = CheckWritable();
  //      if (!st.ok()) return st;
  //
  //  ★ 为什么必须是第一行，不能放在记账之后？
  //    冻结之后 bytes_ 一个字节都不能再涨 —— 它是"这张表的大小"，
  //    表都定格了，尺寸还在长就是自相矛盾。demo 的 [9] 会验这条。
  //
  //  ★ 为什么返回 InvalidArgument 而不是 IOError？
  //    磁盘坏了是**环境**问题，往冻结表里写是**调用方写错了**。
  //    两者要让上层能区分：前者该重试/降级，后者是代码 bug 该改代码。
  //    枚举值混用，上层就永远做不出正确的决策。
  // ═══════════════════════════════════════════════════════════

  // ═══════════════════════════════════════════════════════════
  //  TODO 1（你写）：Put —— 写一条记录，并记账
  //
  //  要做两件事，顺序不能换：
  //
  //    ①  tree_.Insert(key, value)          —— 真正存进去
  //    ②  bytes_ += 这条记录的估算字节数
  //
  //  估算公式：
  //
  //        kMemTableEntryOverhead + key.size() + value.size()
  //
  //  ★ 返回值为什么是 Status，不是 bool？
  //
  //    SkipList::Insert 返回 bool，这里要翻译成 Status::OK()。
  //    现在这个翻译看起来多此一举 —— 因为内存写入几乎不会失败。
  //    但 11-12 接上 WAL 之后，Put 的第一件事是「写日志」，
  //    而写日志【会】失败（磁盘满 / 只读 / 文件被删）。
  //    到那天，签名不用改，只需要在 ① 前面插一行 —— 这就是先定签名的作用。
  //
  //    （这也是上周你自己问过的那个「bool 够不够」的答案：
  //      在 SkipList 那一层 bool 够用，在 MemTable 这一层就不够了。
  //      不是 bool 变差了，是这层的失败原因变多了。）
  //
  //  ★★ 记账有一处反直觉的地方，想清楚再写：
  //
  //    同一个 key 写第二次，SkipList 是【覆盖】—— 节点数不变，
  //    但 bytes_ 应该「不变」还是「继续加」？
  //
  //    → 继续加。而且这不是偷懒，是 LevelDB 的真实做法。
  //
  //      因为 MemTable 的内存来自 arena 分配器，**写入只会往后要新内存，
  //      永远不回收**。旧那条记录占的字节确实还在（只是逻辑上被覆盖了），
  //      要等整张表落盘、整个 arena 一起释放，才真的还回去。
  //
  //      所以 bytes_ 的真实含义是：
  //
  //          bytes_ = 「这张表累计吃进去多少字节」
  //                  ≠ 「现在还活着的数据有多少字节」
  //
  //      这和上周 SkipList::size_ 是同一个坑的同一个解法：
  //      **问清楚一个计数器到底在数什么，比算对它更重要。**
  //
  //  ★ 验收：.\build.ps1 demo_memtable
  //    [1] 转发正确   [2] 记账随写入增长   [4] 覆盖写字节数继续涨
  // ═══════════════════════════════════════════════════════════
  Status Put(const std::string& key, const std::string& value) {
    const Status st = CheckWritable();
    if(!st.ok()){
      return st;
    }
    tree_.Insert(key,value);
    bytes_ += kMemTableEntryOverhead + key.size() + value.size();
    return Status::OK();
  }

  // ═══════════════════════════════════════════════════════════
  //  TODO 2（你写）：Delete —— 写墓碑，同样要记账
  //
  //  ①  tree_.Delete(key)
  //  ②  bytes_ += kMemTableEntryOverhead + key.size() + 0
  //
  //  ★ 注意 value 是空的 —— 但这一条记录【绝不可能只占 0 字节】。
  //
  //    一条墓碑照样是一个完整的 Node：一个 key 字符串、一个空 value
  //    字符串、一个 type、一个 next 数组。固定开销那一百多字节一分不少。
  // 
  //    如果这里只按 value.size() 记账，会得到什么？
  //      → 删 100 万个 key，bytes_ 几乎不涨
  //      → 表里躺着 100 万个墓碑，内存涨了几百 MB
  //      → ShouldFlush() 永远返回 false
  //      → 这张表永远不冻结，最后 OOM
  //
  //    这是 LSM 里一个真实的事故模式，叫「墓碑把内存撑爆」。
  //    demo 的 [3] 就是专门验它。
  // ═══════════════════════════════════════════════════════════
  Status Delete(const std::string& key) {
    const Status st = CheckWritable();
    if(!st.ok()){
      return st;
    }
    tree_.Delete(key);
    bytes_ += kMemTableEntryOverhead + key.size();
    return Status::OK();
  }

  // 读：直接转发。墓碑在 SkipList::Get 里已经被屏蔽了。
  //
  // ★ 11-10 起，这个函数只代表【用户视角】：有值 / 没值。
  //   引擎内部判断"要不要继续查老表"用的是下面的 Lookup。
  bool Get(const std::string& key, std::string* value) const {
    return tree_.Get(key, value, nullptr);
  }

  // ═══════════════════════════════════════════════════════════
  //  TODO 6（你写）：Lookup —— 三态查询，回答「命中的是什么」
  //
  //  地基是 SkipList::Find（物理查询，不屏蔽墓碑），把它翻成三态：
  //
  //      RecordType type;
  //      if (!tree_.Find(key, value, &type)) return LookupResult::kNotFound;
  //      if (type == RecordType::kDelete)    return LookupResult::kDeleted;
  //      return LookupResult::kFound;
  //
  //  ★ 就这四行，但它是今晚整条查找链的地基。
  //    上层拿到的不是"有没有值"，是"是什么东西" —— 只有后者
  //    才够决定"要不要继续往下查"。这就是 bool 不够用的地方。
  //
  //  ★ 为什么用 tree_.Find 而不是 tree_.Get？
  //    Get 会把墓碑报成 false，kDeleted 这一态就永远产生不出来，
  //    查找链也就永远学不会"该停手了"。
  //
  //  ★ 一个必须知道的副作用：kDeleted 时 value 已经被 Find 写成空串了。
  //    不影响正确性（调用方只在 kFound 才读它），但别在调用方假设
  //    "没值就是没动过"。
  //
  //  ★ 验收：.\build.ps1 demo_memset 的 [1] 和 [5]
  // ═══════════════════════════════════════════════════════════
  LookupResult Lookup(const std::string& key, std::string* value) const {
    RecordType type;
    if(!tree_.Find(key,value,&type)){
      return LookupResult::kNotFound;
    }
  
    if(type == RecordType::kDelete){
      return LookupResult::kDeleted;
    }
      return LookupResult::kFound;  // TODO 6
  }

  // ═══════════════════════════════════════════════════════════
  //  TODO 3（你写）：ApproximateMemoryUsage —— 报出估算占用
  //
  //  就是返回 bytes_。一行。
  //
  //  ★ 为什么名字里有 Approximate（近似）？
  //    因为它是估算，不是精确值。名字里带出来，是为了防止调用方
  //    拿它做「精确」的决策（比如「还剩多少字节可以写」）。
  //    **一个诚实的坏名字，胜过一个骗人的好名字。**
  // ═══════════════════════════════════════════════════════════
  size_t ApproximateMemoryUsage() const { return bytes_; }

  // ═══════════════════════════════════════════════════════════
  //  TODO 4（你写）：ShouldFlush —— 该冻结了吗
  //
  //  返回 ApproximateMemoryUsage() >= max_bytes_。
  //
  //  ★ 用 >= 而不是 >：刚好等于阈值时也该刷。
  //    理由是边界条件下两者差别无意义，但 >= 少一个可能漏判的分支。
  //
  //  ★ 注意这只是一个【判断】，不是动作。
  //    它不改任何状态 —— 真正的"冻结"是 11-09 的事。
  //    先能问出问题，再谈怎么回答。这个顺序别反过来。
  // ═══════════════════════════════════════════════════════════
  bool ShouldFlush() const { 
    return ApproximateMemoryUsage() >= max_bytes_;
  }  

  // ── 以下是测试和上层要用的只读信息，已经写好 ──────────────
  size_t EntryCount() const { return tree_.Size(); }  // 物理记录数（含墓碑）
  size_t max_bytes() const { return max_bytes_; }
  const SkipList& tree() const { return tree_; }  // 落盘时要遍历它

 private:
  SkipList tree_;     // 真正干"有序存查"的
  size_t bytes_ = 0;  // 累计吃进去的估算字节数（只加不减）
  size_t max_bytes_;  // 冻结阈值
  bool frozen_ = false;  // 冻结之后只读，写入口由 CheckWritable() 挡
};

}  // namespace lsmkv
