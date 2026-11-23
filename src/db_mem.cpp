// 第 2 版实现（2026-11-11）：DB 接口接上 MemTableSet。
//
// ── 换掉了什么 ────────────────────────────────────────────────
//
//   第 0 版（10-16）用 std::map 存数据：能跑、能测，但和 LSM 没有关系。
//   今晚换成 SkipList + 两张内存表，**而 include/lsmkv/db.h 一个字节都没改**。
//
//   这就是第一天「先定接口」的回报：实现整个换掉，调用方（test_basic、
//   main.cpp）一行都不用动。**接口是承诺，实现是私事。**
//
// ── 还差什么（按周排）────────────────────────────────────────
//
//   Week 4（明天）  immutable 交给后台线程写 SSTable
//   Week 5          Put 之前先写 WAL；Open 的时候重放 WAL
//   Week 6          Get 查完内存还要往下查 SSTable（新 → 旧）
//   Week 6+         compaction
//
//   现在这个版本的诚实描述是：**一个纯内存的有序 KV 库，
//   进程一退数据全没。** 但它已经是 LSM 的形状了。

#include <string>

#include "lsmkv/db.h"
#include "lsmkv/mem_table_set.h"
#include "lsmkv/status.h"

namespace lsmkv {

namespace {

// LSM 形状的 DB 实现：所有读写都落到 MemTableSet。
//
// ★ 为什么类名叫 LSMDB 而不是 MemDB？
//   上一版叫 MemDB，因为它的本质是"一个 map"。这一版虽然还是纯内存，
//   但结构已经是 LSM 的了（active / immutable / 将来有 SSTable），
//   名字跟着本质走，不跟着"有没有落盘"走。
class LSMDB : public DB {
 public:
  // ═══════════════════════════════════════════════════════════
  //  TODO 1（你写）：Put
  //
  //      return tables_.Put(key.ToString(), value.ToString());
  //
  //  ★ 为什么必须 ToString()，不能直接把 Slice 递进去？
  //
  //    因为 Slice **不拥有内存**（看 slice.h：它只有一个指针 + 一个长度）。
  //    它表达的是"借我看一眼这块数据"，用完就还，它不负责让它活着。
  //
  //    MemTable 要的是"存起来，以后还要能读出来"—— 它必须拥有自己那份
  //    内存。所以这里是每一次"从借变成拥有"的边界，必须复制。
  //
  //    ★ 一句话规则：**Slice 只活在调用的那一瞬间，活过这个函数的东西
  //      必须是 std::string。**
  //
  //    这条边界以后还会出现很多次（写 WAL、写 SSTable 都是），
  //    每次问一句：谁负责让这块字节活着？
  //
  //  ★ 为什么不在这里判空 key？
  //    test_basic 第 5 条明确要求空 key 能用。**规格里写了就得支持**，
  //    不要凭直觉加限制。
  // ═══════════════════════════════════════════════════════════
  Status Put(const Slice& key, const Slice& value) override {
    return tables_.Put(key.ToString(),value.ToString());
  }

  // ═══════════════════════════════════════════════════════════
  //  TODO 2（你写）：Get
  //
  //  两件事，第一件不能省：
  //
  //    ① if (value == nullptr) return Status::InvalidArgument(...);
  //    ② return tables_.Get(key.ToString(), value);
  //
  //  ★ 为什么 nullptr 要单独判，还返回 InvalidArgument？
  //
  //    因为空指针不是磁盘坏了，也不是 key 不存在 —— 是**调用方写错了**。
  //    上层看到 InvalidArgument 该做的是"改代码"，看到 IOError 该做的是
  //    "重试或降级"。混用枚举值，上层就永远做不出正确决策。
  //
  //    （这条判据你在 11-09 用过一次：往冻结表里写返回 InvalidArgument。
  //      同一个判断，第二次出现。）
  //
  //  ★ 注意 db.h 里的约定：「找不到时返回 NotFound，且不修改 *value」。
  //    MemTableSet::Get 满足这条 —— 它只在真的找到值时才写 *value。
  //    这种"约定写在注释里"的东西是全项目最容易悄悄破的地方，
  //    所以 test_basic 里有一条专门盯着它。
  // ═══════════════════════════════════════════════════════════
  Status Get(const Slice& key, std::string* value) override {
    if(value == nullptr){
      return Status::InvalidArgument("invalid value");
    }

    // ★ 2026-11-22：这里从 tables_.Get 改成三态 Lookup。
    //
    //   纯内存实现里，kDeleted 和 kNotFound 的结论确实一样 —— 都是"没有"。
    //   但这不是"可以合并"，是"这一层刚好没有更老的数据可查"。
    //   等它接上磁盘（db_sst.cpp），这两种就走向完全相反的两条路。
    const std::string k = key.ToString();
    if (tables_.Lookup(k, value) == LookupResult::kFound) {
      return Status::OK();
    }
    return Status::NotFound("not found: " + k);
  }

  // ═══════════════════════════════════════════════════════════
  //  TODO 3（你写）：Delete
  //
  //      return tables_.Delete(key.ToString());
  //
  //  ★ 这里不判"这个 key 在不在"，也不看返回值说"没删到"。
  //    删一个不存在的 key 也要写墓碑 —— 理由在 11-06 讲过：
  //    老版本可能不在你手里（在更下层），不写墓碑就永远删不掉。
  //
  //  ★ Delete 的 true 是"这条删除已被接受"，不是"确实删掉了一条"。
  //    语义见 skip_list.h 的 TODO 6。
  // ═══════════════════════════════════════════════════════════
  Status Delete(const Slice& key) override {
    return tables_.Delete(key.ToString());
  }

 private:
  MemTableSet tables_;  // 所有数据都在这里（两张内存表）
};

}  // namespace

Status DB::Open(const std::string& path, DB** dbptr) {
  // 内存版还无视 path。Week 5 它会变成：
  //   - 检查 / 创建目录
  //   - 打开 WAL，重放没落盘的记录（崩溃恢复就靠这一步）
  //   - 加载 MANIFEST，恢复 SSTable 列表
  (void)path;

  if (dbptr == nullptr) {
    // ★ 这里以前写的是 IOError —— 那是错的，现在有判据了就该改：
    //   传空指针是**调用方写错了**，不是环境问题。
    //   一份代码里错误分不清种类，是还没有判据时的常态；
    //   有了判据就要回头改，不能留着"反正也没人看"。
    return Status::InvalidArgument("DB::Open: dbptr is null");
  }

  // ★ 注意 test_basic 最后是 `delete db;`，而 db 是 `DB*`。
  //   如果 DB 的析构函数不是 virtual，这里只会调 ~DB，LSMDB 这一层
  //   的析构根本不会执行 → MemTableSet 里的两张表全部泄漏。
  //   这就是"基类必须有虚析构"这个面试题的现场。db.h 里已经有
  //   `virtual ~DB() = default;`，不用你加，但要知道它为什么在那儿。
  *dbptr = new LSMDB();
  return Status::OK();
}

}  // namespace lsmkv
