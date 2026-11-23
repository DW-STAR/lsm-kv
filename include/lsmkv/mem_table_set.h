#pragma once

#include <memory>
#include <string>
#include <utility>

#include "lsmkv/mem_table.h"
#include "lsmkv/status.h"

namespace lsmkv {

// ══════════════════════════════════════════════════════════════
//  MemTableSet —— 2026-11-10
//
//  ── 【术语】memtable set（内存表组）。LSM 里"当前内存中所有还能被
//     读到的内存表"，由「一张可写的 + 至多一张冻结的」组成。
//     LevelDB 里没有这个类（它直接在 DBImpl 里放 mem_ / imm_ 两个成员），
//     我们单独提出来，因为它有一个可以被单独测清楚的语义：**查找顺序**。
//
//  ── 【定义】管两张表：
//
//        active_      能写。所有 Put / Delete 都进这张
//        immutable_   只读。active_ 写满之后被冻结成它
//
//  ── 【为什么必须两张，不能一张】──────────────────────────────
//
//    因为"刷盘"和"继续写"必须能同时进行：
//
//        active_ 写满 ──冻结──▶ immutable_ ──后台线程──▶ SSTable
//             │                      │
//             └── 立刻开一张新的空表，写请求一秒都不等
//
//    一张表做不到这件事：你不可能一边把它往文件里倒、一边往里写新数据。
//
//  ── 【比喻】────────────────────────────────────────────────
//    像收银台换钱箱。箱子满了就整个抽走交给后台的人去清点，
//    顺手推一个空箱子过来 —— 排队的人根本不知道刚才换过箱子。
//
//  ── 【今天唯一要记住的一句话】──────────────────────────────
//
//        读的时候，**从新到旧**查，命中就停 ——
//        不管命中的是值，还是一条墓碑。
//
//    "命中墓碑也停"这一条，是整个 LSM 里最容易写错、错了又最不显眼
//    的地方。它错了不会崩，只会让删掉的数据自己爬回来。
// ══════════════════════════════════════════════════════════════
class MemTableSet {
 public:
  explicit MemTableSet(size_t max_bytes_per_table = kDefaultMemTableBytes)
      : active_(std::make_unique<MemTable>(max_bytes_per_table)),
        max_bytes_per_table_(max_bytes_per_table) {}

  MemTableSet(const MemTableSet&) = delete;
  MemTableSet& operator=(const MemTableSet&) = delete;

  // ── 写：进 active_，然后看看该不该换表 ────────────────────
  //
  // 注意顺序：**先写成功，再判断换表**。
  // 反过来（先判后写）会让"刚好写满的那一条"落在新表里，
  // 旧表永远差一条不满 —— 不致命，但会让每张表的容量都对不上账。
  Status Put(const std::string& key, const std::string& value) {
    const Status st = active_->Put(key, value);
    if (!st.ok()) return st;
    MaybeRotate();
    return Status::OK();
  }

  Status Delete(const std::string& key) {
    const Status st = active_->Delete(key);
    if (!st.ok()) return st;
    MaybeRotate();  // 墓碑也占内存，也会把表写满
    return Status::OK();
  }

  // ═══════════════════════════════════════════════════════════
  //  仅供 WAL 重放使用：直接写进 active，**不触发换表**
  //
  //  ★ 为什么要单独开两个口，而不是重放时直接调 Put？
  //
  //    因为重放阶段一旦发生 Rotate，"这份日志属于哪张表"就说不清了：
  //    重放出来的记录会同时落进 immutable 和新 active，
  //    但它们在磁盘上还是同一个日志文件。之后 immutable 落盘、
  //    这个日志被删掉 —— 新 active 里的数据就跟着一起没了。
  //
  //    重放是一次"重演历史"，不该在这个阶段改变系统的结构。
  //    等重放结束，让下一次真实的 Put 去触发换表，那时候日志归属是清楚的。
  //
  //  ★ 为什么不是 private + friend？
  //    因为调用方是 DB::Open，它和这里不同层。加 friend 只是把
  //    "这个接口不该被乱用"这件事藏起来，而不是说清楚。
  //    名字里带 Direct、注释里写明"仅供重放"，比访问控制更有用 ——
  //    **一个会被误用的接口，改名字比改可见性管用。**
  // ═══════════════════════════════════════════════════════════
  Status PutDirect(const std::string& key, const std::string& value) {
    return active_->Put(key, value);
  }

  Status DeleteDirect(const std::string& key) {
    return active_->Delete(key);
  }

  // ═══════════════════════════════════════════════════════════
  //  TODO 7（你写）：Lookup —— 两级查找，从新到旧，命中即终结
  //
  //  顺序：active_（最新） → immutable_（次新） → 都没有 = kNotFound
  //
  //  对每张表拿三态，然后：
  //
  //      kFound     → 把 value 交出去，停
  //      kDeleted   → 停！不许再往下查（墓碑终结）
  //      kNotFound  → 换下一张表，继续
  //
  //  ★ 唯一会写错的地方，而且错了不崩、不报错、不丢数据：
  //
  //        if (r == LookupResult::kFound) return r;
  //        continue;              ← 把 kDeleted 也当成"继续"
  //
  //    后果：昨天删掉的 key，只要老表里还留着旧值，今天又活了。
  //    这叫 resurrect（亡灵复生）。demo 的 [5] 就是专门整这个的。
  //
  //  ★ 为什么 active_ 一定排第一？
  //    只有它能接收新写，所以它的记录一定比任何老表都新。
  //    反过来（老表优先）会让所有新写全部失效 —— 那不是"优化"，
  //    那是把数据库变成一张只读的旧快照。
  //
  //  ★ 注意"新赢"是靠【顺序】实现的，不是靠比较时间戳。
  //    这也是下面 Rotate 必须保证"冻结和换表不可分割"的原因：
  //    中间那一瞬间如果两条路径都能写，顺序就不再等于新旧。
  //
  //  ★ 为什么这个类没有返回 Status 的 Get（2026-11-22 定稿）──────
  //
  //    Status 只能装"成/败"，装不下"命中值 / 命中墓碑 / 压根没有"。
  //    上一版我在这里留过一个 Get，实现成 Lookup 的 switch 翻译。
  //    问题不是它写错了，是**它存在**：
  //
  //        留着它，就一定会有人用它；
  //        用了它，"墓碑"和"没有"就合成了同一个 NotFound。
  //
  //    **一个会把信息用丢的接口，不该存在。**
  //
  //    真正需要两态的地方只有一处：DB::Get 把结果交给用户。
  //    那个 switch 写在 DB 层，因为只有 DB 层同时知道
  //    "内存查完了、磁盘也查完了、走到这里是真终局"。
  // ═══════════════════════════════════════════════════════════
  LookupResult Lookup(const std::string& key, std::string* value) const {
    if (active_ != nullptr) {
      const LookupResult r = active_->Lookup(key, value);
      if (r != LookupResult::kNotFound) return r;  // kFound 和 kDeleted 都停
    }
    if (immutable_ != nullptr) {
      const LookupResult r = immutable_->Lookup(key, value);
      if (r != LookupResult::kNotFound) return r;
    }
    return LookupResult::kNotFound;
  }

  // ═══════════════════════════════════════════════════════════
  //  TODO 8（你写）：Rotate —— 冻结旧的，换一张新的
  //
  //  三步，顺序不能变：
  //
  //    ① if (immutable_ != nullptr) return false;   ← 上一张还没刷完
  //    ② active_->Freeze();
  //    ③ immutable_ = std::move(active_);
  //       active_    = std::make_unique<MemTable>(max_bytes_per_table_);
  //       ++rotate_count_;
  //       return true;
  //
  //  ★ ① 为什么必须先挡一下？
  //    冻结表是要占内存的。不挡的话，一阵写风暴就能在内存里堆出
  //    三张、五张、十张冻结表 —— 后台刷得再快也追不上，最后 OOM。
  //    所以「同一时刻最多一张 immutable」是一条硬约束，不是建议。
  //
  //  ★ ③ 里 std::move 之后 active_ 就是**空指针**了。
  //    如果忘了紧接着补一张新的，下一次 Put 直接解引用 nullptr 崩掉。
  //    这不是"可能忘"，是"一定会忘" —— 所以这两行必须挨着写，
  //    中间不许插别的东西。
  //
  //  ★ 为什么是 unique_ptr 不是 shared_ptr？
  //    "这张表归谁"必须唯一：要么归 active_，要么归 immutable_，
  //    要么归后台线程。不能同时归两家 —— 否则谁负责释放？
  //    shared_ptr 会让"同时持有"变得合法，这个 bug 就再没人拦得住。
  //    唯一所有权 = 编译器替你证明"只有一个主人"。
  //
  //  ★ 为什么返回 bool 而不是 Status？
  //    "有张表还没刷完"不是错误，是正常的背压信号（backpressure）。
  //    上层该做的是继续写 active_，不是报错。
  //    **凡是"预期之内且无需处理"的情况，别用错误码表达。**
  // ═══════════════════════════════════════════════════════════
  bool Rotate() {
    if(immutable_ != nullptr) return false;
    active_->Freeze();
    immutable_ = std::move(active_);
    active_ = std::make_unique<MemTable>(max_bytes_per_table_);
    ++rotate_count_;
    return true;
  }

  // 把冻结表的**所有权**交出去 —— 真实世界里这一步是刷盘线程干的。
  //
  // ★ 为什么是「返回 unique_ptr」而不是「当场 reset 掉」？
  //
  //   因为现在还没有 SSTable，直接 reset 等于把数据扔了 —— 而一个
  //   名叫 OnFlushComplete 的函数偷偷丢数据，是最恶劣的那种 API：
  //   调用方以为自己做了刷盘，实际上做的是删除。
  //
  //   所以这里只做唯一所有权该做的事：**转移**。
  //   谁接走，谁负责（Week 5 是刷盘线程，它会遍历 tree() 写成文件）。
  //   没人接走，数据就是被丢弃 —— 这个后果写在调用点，不藏进函数里。
  //
  // ★ 这也是 unique_ptr 的核心价值：**所有权必须显式转移**。
  //   shared_ptr 会让"两边都以为自己在管"变成合法状态，
  //   于是没人说得清谁该释放、什么时候释放。
  std::unique_ptr<MemTable> ReleaseImmutable() {
    return std::move(immutable_);
  }

  // ── 以下是测试和上层要用的只读信息，已经写好 ──────────────
  const MemTable& active() const { return *active_; }
  const MemTable& immutable() const { return *immutable_; }
  bool HasImmutable() const { return immutable_ != nullptr; }
  size_t RotateCount() const { return rotate_count_; }

 private:
  // active_ 写满了就换表。没满就什么也不做。
  void MaybeRotate() {
    if (active_->ShouldFlush()) {
      Rotate();
    }
  }

  std::unique_ptr<MemTable> active_;      // 唯一能写的那张
  std::unique_ptr<MemTable> immutable_;   // 冻结待刷盘的那张（可能为空）
  size_t max_bytes_per_table_;            // 换新表时用的容量
  size_t rotate_count_ = 0;               // 换过几次表（测试用）
};

}  // namespace lsmkv
