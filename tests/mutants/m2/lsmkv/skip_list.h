#pragma once

// ══════════════════════════════════════════════════════════
//  MUTANT M2 -- RandomLevel 不抛硬币，永远返回 1 层
//  自动生成，别改。只用于考验 tests/test_skip.cpp 抓不抓得住 bug。
// ══════════════════════════════════════════════════════════

#include <algorithm>  // std::max
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "lsmkv/record.h"

namespace lsmkv {

// ══════════════════════════════════════════════════════════════
//  跳表（SkipList）—— 2026-11-03
//
//  ── 【术语】────────────────────────────────────────────────
//  多层索引有序链表（multi-level indexed sorted linked list），
//  工程上叫 skiplist。LevelDB 的 MemTable、Redis 的 zset 都是它。
//
//  ── 【定义】────────────────────────────────────────────────
//  · 最底层（L0）是一条包含【全部】节点的、按 key 升序的普通单链表。
//  · 在 L0 之上叠若干层索引：第 k 层只保留约 1/2^(k-1) 的节点，
//    越往上越稀疏。每个索引节点多存一组 next 指针，直指更远的节点。
//  · 查找：从当前最高层出发，右边那个节点的 key 比目标小就跳过去，
//    比目标大就降一层继续。降到 L0 还没找到就是不存在。
//  · 期望查找/插入复杂度 O(log n)，靠的是「每层节点数减半」这一点。
//
//  ── 【层数怎么来】──────────────────────────────────────────
//  不是按 key 算出来的，是每个新节点【独立随机生成】的：
//  每往上升一层的概率固定为 1/2，所以层数服从几何分布，期望 2 层。
//  具体实现就是下面 RandomLevel() 里那 4 行 while 循环。
//
//  ── 【比喻】────────────────────────────────────────────────
//  像查纸质字典的两个动作：先靠书口上的章节标记大步跳到大概位置
//  （上层稀疏索引），再在那一叠里逐页翻（L0 顺序走）。
//  映射到代码：「大步跳」= p = p->next[上层的层号]；
//              「降一层」= --level，同一个 p，换更低的 next 下标。
//              「逐页翻」= p = p->next[0]。
//
//  为什么要它？因为 MemTable 需要一个「能按 key 有序遍历、插入够快」的结构。
//  红黑树也能做到，但红黑树插入要旋转，旋转要改一片指针（见 06-并发与树.html）。
//  跳表插入只改前驱的一个 next 指针，简单得多。
//
//  ★ 今天的任务只有三件（都在下面标了 TODO）
//
//    ① RandomLevel()   —— 随机生成新节点的层数（几何分布，每层 1/2）
//    ② 构造函数         —— 建一个头节点，层数记成 1
//    ③ 析构函数         —— 把所有节点 delete 掉，不能漏
//
//  Insert / Find / Delete 是周三周四的事，今天不做。
//
//  ★ 验收：跑 src/demo_skip.cpp
//      · Level() == 1、Size() == 0
//      · 10 万次 RandomLevel 的分布，前 5 层误差 < 3%
//      · 析构不崩、不漏
// ══════════════════════════════════════════════════════════════

// 最高层数。超过它的硬币结果直接截断。
//
// ★ 为什么是 12？
//
//   跳表的期望查找代价是 O(log₂n)。12 层大约能撑 2^12 = 4096 个节点
//   还保持「最上层几乎只有一个节点」的理想形态。
//
//   MemTable 到几 MB 就冻结落盘了，里面撑死几十万条 —— 12 层完全够。
//   LevelDB 用的也是 12（它的常量叫 kMaxHeight）。
//
//   设太大：每层都要维护，白费内存和指针。
//   设太小：上层用光了，退化到底层遍历，变慢。
inline constexpr int kMaxLevel = 12;

// ─────────────────────────────────────────────────────────────
//  节点：一个节点 = 一份数据 + 一把指针
//
//  ★ 最容易搞混的地方（已经画过图了，再强调一次）
//
//    一个节点只有【一份】key/value，但它有【多个】next 指针。
//    不是「每层一个节点」，是「一个节点在每层都有一条横线出去」。
//
//    key = "b", value = "2", 三层 →
//        next[0] → L0 的下一个节点
//        next[1] → L1 的下一个节点
//        next[2] → L2 的下一个节点
//
//    next.size() 就是这个节点的层数。每个节点可以不一样。
//    唯一的要求：next.size() >= 1（每个节点至少在 L0 上）。
// ─────────────────────────────────────────────────────────────
struct Node {
  std::string key;
  std::string value;
  RecordType type = RecordType::kPut;
  std::vector<Node*> next;  // 长度 = 这个节点的层数
};

class SkipList {
 public:
  // ───────────────────────────────────────────────────────────
  //  TODO 1（你写）：构造函数
  //
  //  要做三件事：
  //    · level_ = 1          （当前最高层，从 1 开始）
  //    · size_  = 0
  //    · head_  = NewNode("", "", RecordType::kPut, kMaxLevel)
  //
  //  ★ 为什么头节点要 kMaxLevel 层？
  //
  //    头节点是个「哨兵」：它的 key 是空的，不存真数据，只负责当起点。
  //    所有查找都从 head_ 的最高层开始往右走。
  //    所以它必须【永远有最高层数的指针】，否则将来升层时无处可挂。
  //
  //  ★ 为什么不在初始化列表里直接把 head_ 建好？
  //    NewNode 是成员函数，要用它得等对象构造到一半。
  //    写在函数体里最简单，别折腾。
  // ───────────────────────────────────────────────────────────
  SkipList() {
    level_ = 1;
    size_ = 0;
    head_ = NewNode("","",RecordType::kPut,kMaxLevel);
  }

  // ───────────────────────────────────────────────────────────
  //  TODO 2（你写）：析构函数
  //
  //  把从 head_ 开始的所有节点都 delete 掉。
  //
  //  ★ 关键：只沿 next[0] 走就够了，不用管别的层。
  //
  //    因为【L0 上有全部节点】—— 每个节点至少 1 层，所以每个节点都在 L0 上。
  //    上面几层的节点，在 L0 里必然也出现过，已经被访问过了。
  //    如果你顺着每一层都删一遍，会把同一个节点 delete 两次 → 程序崩溃。
  //
  //  ★ 注意写法：先存下一个再 delete 自己
  //
  //        Node* p = head_;
  //        while (p != nullptr) {
  //          Node* next = p->next[0];   // ① 先记住下一个
  //          delete p;                  // ② 再删自己
  //          p = next;                  // ③ 用记住的那个继续
  //        }
  //
  //    顺序反了（先 delete 再读 p->next）就是「访问已释放内存」—— use after free。
  //    这个错误很常见，而且小规模测试时经常不崩，等到线上才炸。
  // ───────────────────────────────────────────────────────────
  ~SkipList() {
      Node* p = head_;
      while(p != nullptr){
        Node* next = p -> next[0];
        delete p;
        p = next;
      }
  }

  // 跳表持有裸指针，绝不能拷贝 —— 和 AppendOnlyLog 是同一个理由。
  SkipList(const SkipList&) = delete;
  SkipList& operator=(const SkipList&) = delete;

  int Level() const { return level_; }
  size_t Size() const { return size_; }

  // ───────────────────────────────────────────────────────────
  //  TODO 3（你写）：随机生成节点层数 —— 今天最重要的逻辑
  //
  //  规格：
  //
  //    level = 1
  //    while (level < kMaxLevel && (NextRandom() & 1) == 0)  ++level
  //    return level
  //
  //    `(NextRandom() & 1) == 0` 就是上面【定义】里「每层 1/2 概率」的
  //    具体实现：取随机数的最低位，等于 0 就当这次「升」成功。
  //
  //  ★ 这个循环产生的是什么分布？
  //
  //    第 1 次就反面 → 停在 1 层，概率 1/2
  //    第 1 次正面、第 2 次反面 → 2 层，概率 1/4
  //    3 层 → 1/8，4 层 → 1/16 …
  //
  //    这叫【几何分布】。期望层数是 2 层。
  //    直接后果：一个节点出现在第 k 层的概率是 1/2^(k-1)，
  //    所以每往上一层，节点数量大致减半 —— 这就是索引稀疏的原因，
  //    也是 O(log n) 的来源。
  //
  //  ★ 为什么要 && level < kMaxLevel？
  //
  //    硬币理论上可以一直正面下去。不封顶的话，极端情况会分配一个
  //    巨大的 next 数组。加个上限兜底。
  //
  //  ★ 为什么放 public？
  //
  //    真实工程里它该是 private。这里放出来是为了让 demo_skip 能直接
  //    统计 10 万次的分布 —— 【为了可测试性放宽访问权限，是合理的妥协】。
  //    LevelDB 也有类似的「测试专用接口」。
  // ───────────────────────────────────────────────────────────
  int RandomLevel() {
    int level = 1;
    // MUTANT m2: 不抛硬币，永远 1 层
    return level;
  }

  // ───────────────────────────────────────────────────────────
  //  TODO 4（你写）：插入 —— 今天的主菜
  //
  //  【术语】有则更新、无则插入（insert-or-update），也叫 upsert。
  //
  //  【定义】给定 key，若已存在则就地改 value 与 type；
  //          若不存在则造一个新节点、按随机层数挂进各层链表。
  //          返回值 true 表示这次写入被接受。
  //
  //  ★ 先把语义定死（这一条比代码重要，定错了后面全歪）
  //
  //    key 已存在时必须【覆盖】，不许追加第二个同 key 节点。
  //
  //    理由：LSM 的语义是「同一个 key 后写的赢」。如果两个 "b" 同时躺在 L0，
  //    读的时候就要判断哪个更新 —— 要么给每条记录加序号，要么放弃
  //    「L0 严格升序」这条不变量。两个代价都比「就地改 value」大得多。
  //
  //    顺带一提：这就是为什么后面 MemTable 只存「每个 key 的最新值」，
  //    历史版本交给 WAL 和磁盘上的多份 SSTable 去维护。职责分开。
  //
  //  ★ 四步，顺序不能换
  //
  //    ① const int new_level = RandomLevel();
  //       决定这个节点连几根线。这是它唯一被调用的地方。
  //
  //    ② 从最高层往下走，把每一层的前驱记进 prevs[]
  //       走法 = Find 那一套（右边更小就进，否则降层）。
  //       只在 lv < new_level 时记录 —— 更高的层这个节点根本用不到。
  //       走到 L0 之后，如果 p->next[0] 的 key 正好等于 key → 覆盖，直接 return。
  //
  //    ③ if (new_level > level_)：多出来的层补 prevs[lv] = head_，再更新 level_。
  //       （为什么一定是 head_？因为那些层上现在一个节点都没有。见 08 文档第 02 节）
  //
  //    ④ 逐层挂线。两行，顺序不能反：
  //         n->next[lv]        = prevs[lv]->next[lv];
  //         prevs[lv]->next[lv] = n;
  //
  //  ★ 三个会写出 bug 的地方（08 文档第 05 节有对照表）
  //
  //    1. 两行赋值写反 → 新节点指向自己 → 自环 → 查询死循环、CPU 打满
  //    2. prevs 开成 level_ 大小 → new_level 更大时越界写堆（可能几十行后才崩）
  //    3. 升层时忘了补 head_ → prevs[lv] 是 nullptr → 解引用空指针
  //
  //  ★ 验收：.\build.ps1 demo_skip
  //    [4] 插 3 条：size_ / L0 顺序 / 三条不变量
  //    [5] 同 key 覆盖：节点数不变、值变成新的
  //    [6] 1 万条乱序插入：每 1000 条查一次不变量
  //    [7] 抽 1000 个 key 校验值、打印各层节点数
  // ───────────────────────────────────────────────────────────
  bool Insert(const std::string& key, const std::string& value,
              RecordType type = RecordType::kPut) {
    const int new_level = RandomLevel();
    std::vector<Node*> pre(static_cast<size_t>(std::max(level_,new_level)));
    Node* p = head_;
    for(int i = level_ - 1; i >= 0;--i){
      while(p -> next[i] != nullptr && p -> next[i] -> key < key) p = p->next[i];
      pre[i] = p;
    } 
    
    Node* change =  pre[0] -> next[0];
    if(change != nullptr && change -> key == key){
      change -> value  = value;
      change -> type = type;
      return true;
    }

    if(new_level > level_){
      for(int i = new_level - 1;i >= level_;--i){
        pre[i] = head_;
      }
      level_ = new_level;
    }

   
    Node* n = NewNode(key,value,type,new_level);

    for(int i = new_level - 1;i >= 0;--i){
      n -> next[i] = pre[i] -> next[i];
      pre[i] -> next[i] = n;
    }
    ++size_;
    return true;
  }

  // ───────────────────────────────────────────────────────────
  //  TODO 5（你写）：查找 —— 今天的主菜
  //
  //  【术语】点查询（point lookup）。相对的是范围查询（range scan），
  //          那个要等 Week 5 落盘时才做。
  //
  //  【定义】给定 key，存在 → 返回 true，并把 value / type 写到出参里；
  //          不存在 → 返回 false，出参一个字节都不动。
  //
  //  ★ 签名为什么长这样？两个都是必须做的设计决策，不是随手写的
  //
  //    ① 为什么用 bool 返回 + 出参，而不是直接 return 一个 std::string？
  //
  //       因为【空字符串是合法的 value】。拿 "" 表示「没找到」的话，
  //       一个真的存了空 value 的 key 就永远查不出来 —— 而且这种 bug
  //       上线前很难发现，因为绝大多数 key 的 value 都不是空的。
  //
  //       这就是 C++ 里「返回值管状态、出参管数据」这个套路的由来。
  //       （C++17 还有个 std::optional<std::string> 的写法更漂亮，
  //         但 LevelDB 的 DB::Get 用的就是 Status + 出参，你跟着它走。）
  //
  //    ② 为什么出参允许传 nullptr？
  //
  //       有时候只想判断「在不在」，不关心内容。允许 Get(key, nullptr, nullptr)
  //       就不用为了看一眼存在性而白拷一次字符串。
  //       内部必须写成 if (value != nullptr) *value = ...，不能直接解引用。
  //
  //  ★ 必须声明成 const 成员函数
  //
  //    查找不该改变任何东西。写上 const，编译器就帮你守住「别改成员」。
  //
  //    ★★ 但这里有一个 C++ 的经典陷阱，今天要顺便学掉：
  //
  //       const 成员函数里，head_ 的类型是 `Node* const` ——
  //       意思是【指针本身不能改】，【不是】「它指向的节点不能改」。
  //
  //       所以 `head_->value = "x";` 在 const 函数里照样编得过。
  //       要让编译器真的挡住，你得用 `const Node*` 去走链表：
  //           const Node* p = head_;
  //       这样任何 p->xxx = ... 都会变成编译错误。
  //
  //  ★ 三个步骤（其实就是 Insert 的前半段，一行都不用新写）
  //
  //    ① 从 level_-1 层开始往下：右边更小就进，否则降一层
  //    ② 降到底之后，候选是 p->next[0]（★ 可能为 nullptr，先判空）
  //    ③ 候选的 key == 目标 → 写出参、return true；否则 return false
  //
  //  ★ 验收：.\build.ps1 demo_skip
  //    [8] 边界：空表 / 最小 / 最大 / 中间不存在 / 空串 key / nullptr 出参
  //    [9] 与 std::map 对照：5000 次随机插入 + 查询，两个实现结果必须一致
  //    [10] 1 万次查询：跳表 vs 线性扫描，比耗时
  // ───────────────────────────────────────────────────────────
  bool Get(const std::string& key, std::string* value,
           RecordType* type) const {
    const Node* p = head_;
    for(int i = level_ - 1;i >= 0;--i){
      while(p->next[i] != nullptr && p->next[i]->key < key){
        p = p->next[i];
      }
    }
    // ★★★ 今天（11-06）加了 Delete 之后，下面这段逻辑【不够了】★★★
    //
    //   删除不是摘节点，是把节点改写成 type == kDelete 的墓碑。
    //   所以现在 Get("b") 会命中一条墓碑记录，上面这段会高兴地
    //   返回 true，把墓碑的空 value 交给调用方 ——
    //   用户看到的是「b 还在，值是空串」。
    //
    //   正确语义：墓碑 = 这个 key 不存在。
    //
    //   在哪补？一句话，但位置很关键：
    //     必须在写 *value 之前判断，否则出参已经被墓碑写脏了。
    //
    //   完整说明见下面 Delete 的 TODO 6，那里有一段专门讲这个。
    // ───────────────────────────────────────────────────────────
    if(p->next[0] != nullptr && p->next[0]->key == key && p->next[0]->type == RecordType::kPut){
      if(value != nullptr)
      *value = p->next[0]->value;
      if(type != nullptr)
      *type = p->next[0]->type;
      return true;
    }
    return false;
  }

  // ───────────────────────────────────────────────────────────
  //  TODO 6（你写）：删除 —— 代码量一行，但有两个坑在语义上
  //
  //  【术语】墓碑（tombstone）/ 逻辑删除（logical delete）
  //
  //  【定义】给 key 写一条 type == kDelete 的记录，节点不摘除；
  //          读的时候把这条记录当成「key 不存在」。
  //
  //  ── 坑 ① 不许先查再删 ────────────────────────────────────
  //
  //    std::map::erase 是真删，找不到就什么也不做。LSM 不能这么干，
  //    因为这个 key 的【老版本可能不在你手里】：
  //
  //      昨天  Put("b","2")  → MemTable 满了，落盘成 SSTable
  //      今天  Delete("b")   → MemTable 里根本没有 "b"
  //
  //    你要是「查一下，不在就跳过」，磁盘上那份 "b" 就永远活着。
  //    删了等于没删，还没有任何报错。
  //
  //    所以 Delete 必须【无条件】写墓碑。它不知道老数据在哪一层，
  //    只知道「从此刻起 b 是死的」。真正清理由 compaction 负责 ——
  //    它合并时看到墓碑，才会把更老的那份一起扔掉。
  //
  //    ★ 这是 LSM 和 B-Tree 最根本的区别之一：
  //      B-Tree 的删除是「就地消灭」，LSM 的删除是「追加一个噩耗」。
  //
  //  ── 【实现】就是一行 ─────────────────────────────────────
  //
  //        return Insert(key, "", RecordType::kDelete);
  //
  //    为什么能这么省？Insert 里该有的全都有了：查找前驱、同 key
  //    覆盖分支、随机层数、升层补 head_、++size_。
  //    删除只是「内容不同的一次写入」。
  //
  //    ★ 今天真正要带走的一句话：
  //      【删除是写入的一种】。LevelDB / RocksDB 的 Delete 走的就是
  //      Put 完全相同的代码路径，只是记录类型不同。
  //
  //  ── 坑 ② 返回值和 Size() ─────────────────────────────────
  //
  //    返回恒为 true。别写成「找不到就返回 false」—— 那等于把坑 ①
  //    写进了签名，调用方会以为删除失败了。
  //    true = 「这条删除已被接受」，不是「确实删掉了一条」。
  //
  //    Size() 会 +1，哪怕删的是从没存在过的 key —— 墓碑也是一条真实
  //    记录，也占内存。MemTable 按字节算大小，不按活着的 key 个数算。
  //
  //  ── ★ size_ 到底记的是什么（11-06 当晚补，差点把人带沟里）──
  //
  //        size_ = 物理记录数 = 活着的 key + 墓碑
  //
  //    不是「活着的 key 个数」。所以 Delete 一行都不用碰它：
  //      · 删不存在的 key → 建了新墓碑 → Insert 里 ++size_ 已经 +1  ✓
  //      · 删存在的 key   → 走覆盖分支 → 不 ++size_                ✓
  //
  //    你要是再补一个 size_--，这两种情况就全都算错了
  //    （第一种少算一条真实记录，第二种凭空减掉一条）。
  //
  //    为什么 MemTable 不按「活着的 key 数」记账？
  //    因为它真正关心的是【内存占用】—— 决定何时冻结落盘的是字节数，
  //    不是 key 的个数。墓碑也占内存，就该被算进去。
  //    （LevelDB 按字节估算，我们这里先用记录数凑合，Week 5 再换。）
  //
  //  ── 关于「bool 够不够」───────────────────────────────────
  //
  //    会想：Insert 只回一个 bool，我都不知道它是新建还是覆盖，
  //    没法维护 size_。对，但【你不该知道】：
  //      · LSM 的写入是幂等追加，新/旧对上层没有意义
  //      · 数据一旦分层落盘，这个区别彻底失效
  //    需要它 = 让调用方依赖一个迟早不成立的实现细节。
  //
  //    bool 真正缺的是【原因】，不是【新旧】。失败有多种原因、且
  //    调用方要分开处理时才需要 Status —— 内存结构做不到
  //    （只有 bad_alloc，抛异常），Week 5 落盘（磁盘满 / IO 错 /
  //    CRC 不对）才是 Status 的场子。见 include/lsmkv/status.h。
  //
  //  ── 顺带要改的另一处（在 Get 里）──────────────────────────
  //
  //    Get 现在会把墓碑当成「找到了」返回。加了 Delete 之后它必须
  //    把 type == kDelete 当成未命中，位置见 Get 函数体里的标注。
  //    ★ 先别急着改：跑一遍 demo，看 [13] 是怎么红的，红在哪一条，
  //      再动手。先看见症状，比先看答案有用。
  //
  //  ★ 验收：.\build.ps1 demo_skip
  //    [13] 删不存在的 key 也写墓碑 / Get 查不到 / 物理记录还在 /
  //         重复删幂等 / 删完再写能复活 / 2000 次随机删后不变量仍成立
  //    [14] 与 std::map 对照：随机 insert + delete，两边 Get 必须一致
  // ───────────────────────────────────────────────────────────
  bool Delete(const std::string& key) {
    bool ans = Insert(key,"",RecordType::kDelete);
    return ans;
  }

  // ═══════════════════════════════════════════════════════════
  //  以下是【测试脚手架】，我写的，你不用动。
  //
  //  为什么要有 CheckInvariants：
  //  Insert 写错的方式大多不崩——链子接错了，查询少几条数据，
  //  程序照样跑完、照样打印"成功"。只有把不变量显式断言出来，
  //  才能把「静默错」变成「立刻报错」。
  // ═══════════════════════════════════════════════════════════

  // 导出某一层的 key 序列（从 head_ 出发，沿这一层的 next 走）
  std::vector<std::string> LevelKeys(int lv) const {
    std::vector<std::string> out;
    if (head_ == nullptr || lv < 0 || lv >= kMaxLevel) return out;
    for (Node* p = head_->next[lv]; p != nullptr; p = p->next[lv]) {
      out.push_back(p->key);
      if (out.size() > 200000) break;  // 有环时别把测试挂死
    }
    return out;
  }

  // 沿 L0 线性找 key，返回它的 value（找不到返回空串）。仅用于测试。
  //
  // ★ 故意写成线性扫描而不是跳表查找：这样它和 Insert/Find 的实现
  //   完全独立，一个错了不会把另一个也带错。测试的参照物必须换个来源。
  std::string DebugGet(const std::string& key) const {
    if (head_ == nullptr) return std::string();
    for (Node* p = head_->next[0]; p != nullptr; p = p->next[0]) {
      if (p->key == key) return p->value;
    }
    return std::string();
  }

  // 沿 L0 线性找 key，返回整条记录（找不到返回 nullptr）。仅用于测试。
  //
  // ★ 为什么需要这个：加了 Delete 之后，Get 会把墓碑藏起来（返回 false），
  //   于是「墓碑到底写进去没有」就观测不到了 —— 而测试必须能看见物理真相。
  //   这个函数是【透过逻辑删除看物理记录】的唯一窗口。
  //
  //   Week 5 落盘时它就不再是测试专用了：compaction 要遍历 MemTable 里
  //   每一条物理记录（含墓碑）才能决定哪些老数据可以扔。
  const Node* DebugFind(const std::string& key) const {
    if (head_ == nullptr) return nullptr;
    for (Node* p = head_->next[0]; p != nullptr; p = p->next[0]) {
      if (p->key == key) return p;
    }
    return nullptr;
  }

  // 检查三条不变量，返回第一处违例的描述；返回空串 = 全对
  std::string CheckInvariants() const {
    if (head_ == nullptr) return "head_ is nullptr";
    if (head_->next.size() != static_cast<size_t>(kMaxLevel))
      return "head_ does not have kMaxLevel pointers";
    if (level_ < 1 || level_ > kMaxLevel) return "level_ out of range [1, kMaxLevel]";

    // ① L0 含全部节点、严格升序、无重复
    std::vector<Node*> l0;
    for (Node* p = head_->next[0]; p != nullptr; p = p->next[0]) {
      if (l0.size() > 200000) return "L0 walk exceeded 200000 nodes (cycle?)";
      l0.push_back(p);
    }
    if (l0.size() != size_) return "size_ does not match the node count on L0";
    for (size_t i = 0; i < l0.size(); ++i) {
      if (l0[i]->next.empty()) return "a node has zero levels (next must not be empty)";
      if (i > 0 && !(l0[i - 1]->key < l0[i]->key))
        return "L0 is not strictly increasing (or has duplicate key)";
    }

    // ② level_ 必须真实等于最高层
    int real_max = 1;
    for (size_t i = 0; i < l0.size(); ++i) {
      const int lv = static_cast<int>(l0[i]->next.size());
      if (lv > real_max) real_max = lv;
    }
    if (real_max != level_) return "level_ does not match the real highest level";

    // ③ 每一层：节点集合必须恰好等于「层数 > lv 的那些节点」，且顺序与 L0 一致
    for (int lv = 1; lv < level_; ++lv) {
      size_t need = 0;
      for (size_t i = 0; i < l0.size(); ++i)
        if (static_cast<int>(l0[i]->next.size()) > lv) ++need;

      size_t got = 0;
      size_t pos = 0;           // 在 L0 中的游标
      Node* prev = nullptr;
      for (Node* p = head_->next[lv]; p != nullptr; p = p->next[lv]) {
        if (++got > 200000) return "a level walk exceeded 200000 nodes (cycle?)";
        if (static_cast<int>(p->next.size()) <= lv)
          return "a node sits on a level above its own height (invariant 2)";
        if (prev != nullptr && !(prev->key < p->key))
          return "a level chain is not increasing";
        while (pos < l0.size() && l0[pos] != p) ++pos;
        if (pos >= l0.size()) return "a node on some level is missing from L0, or out of order";
        ++pos;
        prev = p;
      }
      if (got != need) return "a level chain has the wrong node count (missing or extra link)";
    }
    return "";
  }

 private:
  // 一个自己实现的伪随机数生成器（xorshift32）。
  //
  // ★ 为什么不用 rand()？
  //   1. rand() 是全局状态，被别人调用一次，你的序列就变了 —— 不可重现
  //   2. rand() 的实现各家不同，换个编译器结果就不一样 —— 没法写单测
  //   3. 这里自己维护 rng_state_，给同一个初值，序列永远一样
  //
  // 这个算法只有三行异或移位，质量够用（不是密码学级别）。
  uint32_t NextRandom() {
    uint32_t x = rng_state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state_ = x;
    return x;
  }

  // 造一个节点。已经给你写好了，直接调用。
  Node* NewNode(const std::string& key, const std::string& value,
                RecordType type, int level) {
    Node* n = new Node;
    n->key = key;
    n->value = value;
    n->type = type;
    n->next.assign(static_cast<size_t>(level), nullptr);
    return n;
  }

  Node* head_ = nullptr;   // 哨兵头节点，永远是 kMaxLevel 层
  int level_ = 1;          // 当前实际用到的最高层（从 1 开始）
  size_t size_ = 0;        // 节点个数（不含头节点）
  uint32_t rng_state_ = 0x2545F491u;  // 随机数状态
};

}  // namespace lsmkv
