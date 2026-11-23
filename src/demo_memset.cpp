// ══════════════════════════════════════════════════════════════
//  MemTableSet 验收（11-10 建）
//
//  跑法（在 D:\code\lsm-kv 下）：
//    .\build.ps1 demo_memset
//
//  这个文件不用你改。你要改的是：
//    include/lsmkv/mem_table.h       TODO 6  (Lookup，三态)
//    include/lsmkv/mem_table_set.h   TODO 7  (Lookup，两级查找链，三态出口)
//                                    TODO 8  (Rotate，冻结 + 换表)
//
//  [1] 三态：kNotFound / kFound / kDeleted 能被分开报出来
//  [2] Rotate：冻结 → 换表 → 旧表还在、新表是空的
//  [3] 跨表读：只在老表里的 key，照样读得到
//  [4] 新赢：两张表都有同一个 key，新的那个赢
//  [5] 墓碑终结：老表有值、新表有墓碑 → 必须 NotFound   ★ 今晚的核心
//  [6] 墓碑在旧表：删完之后冻结，照样读不到
//  [7] 背压：上一张冻结表还没刷完，不许再冻结一张；交还所有权后可继续
//  [8] 自动换表：写满阈值自己换，不用手动喊
//  [9] 与 std::map 对敲：2000 次随机写/删/查，中途换一次表，
//      之后 1800 步全程在「两张表同时在」的状态下跑
//  [10] 背压堵死时 active 会一直涨 —— 这一段记录的是【现在的缺口】，
//       不是 bug。看它之前先读函数里的注释。
//
//  ★ 这个 demo 的设计原则和 demo_memtable 一样：
//    凡是"必须相等"的地方（读出来的值、有没有命中）才写 ==；
//    凡是估算、容量、次数之类的地方，只判区间。
// ══════════════════════════════════════════════════════════════

#include <cstdio>
#include <map>
#include <memory>
#include <string>

#include "lsmkv/mem_table.h"
#include "lsmkv/mem_table_set.h"

namespace {

int g_failed = 0;

void Check(const char* label, bool ok, const char* detail) {
  std::printf("  %-46s %s   %s\n", label, ok ? "PASS" : "FAIL", detail);
  if (!ok) ++g_failed;
}

std::string KeyOf(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%05d", i);
  return buf;
}

// 只给 [9] 用的确定性随机数。用 rand() 的话换台机器序列就变了，
// 出问题时没法复现 —— 测试里凡是随机的东西都必须可重现。
struct Rng {
  unsigned int s = 20261110u;
  unsigned int Next() {
    s = s * 1103515245u + 12345u;
    return (s >> 16) & 0x7fff;
  }
};

// 用户视角：三态压成两态。
//
// ★ 为什么这个函数在 demo 里，而不在 MemTableSet 里？
//   因为"压平"是【调用方的选择】，不是存储引擎的义务。
//   MemTableSet 一旦提供两态接口，就等于替所有调用方做了这个选择 ——
//   而有的调用方（DB::Get 在内存 miss 时）根本不能压。
//   把压平的动作留在用得着它的那一层，别下沉。
bool ReadAsUser(const lsmkv::MemTableSet& s, const std::string& k,
                std::string* v) {
  return s.Lookup(k, v) == lsmkv::LookupResult::kFound;
}

const char* NameOf(lsmkv::LookupResult r) {
  switch (r) {
    case lsmkv::LookupResult::kNotFound: return "kNotFound";
    case lsmkv::LookupResult::kFound:    return "kFound";
    case lsmkv::LookupResult::kDeleted:  return "kDeleted";
  }
  return "???";
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // ── [1] 三态 ────────────────────────────────────────────────
  std::printf("[1] Lookup: three states, not two\n");
  {
    lsmkv::MemTable m;
    std::string v = "untouched";

    auto r0 = m.Lookup("a", &v);
    Check("empty table -> kNotFound", r0 == lsmkv::LookupResult::kNotFound,
          "no record at all");

    m.Put("a", "1");
    auto r1 = m.Lookup("a", &v);
    Check("after Put -> kFound", r1 == lsmkv::LookupResult::kFound,
          "a real value");
    Check("the value came out", v == "1", "value is 1");

    m.Delete("a");
    auto r2 = m.Lookup("a", &v);
    Check("after Delete -> kDeleted", r2 == lsmkv::LookupResult::kDeleted,
          "a tombstone, not an absence");
    Check("kDeleted != kNotFound", r2 != r0,
          "this is the whole point of a third state");
    Check("but Get() still hides it", !m.Get("a", &v),
          "user view: deleted means gone");

    std::printf("  (got %s / %s / %s)\n", NameOf(r0), NameOf(r1), NameOf(r2));
  }

  // ── [2] Rotate ──────────────────────────────────────────────
  std::printf("\n[2] Rotate: freeze the old, open a new\n");
  {
    lsmkv::MemTableSet s;
    s.Put("a", "1");
    Check("starts with no immutable", !s.HasImmutable(), "only the active table");

    const bool ok = s.Rotate();
    Check("Rotate() returned true", ok, "nothing was in the way");
    Check("now there is an immutable", s.HasImmutable(), "the old active");

    // ★ 注意这里的写法：先判 HasImmutable() 再去碰 immutable()。
    //   immutable() 是解引用指针，没表的时候它返回的引用是空的 ——
    //   直接调就是野指针，程序会直接崩，而不是给你一条 FAIL。
    //   测试代码里"访问可能不存在的东西"和业务代码一样危险。
    const bool has_imm = s.HasImmutable();
    Check("a fresh active took its place", s.active().EntryCount() == 0,
          "empty, ready for new writes");
    Check("the old table kept its data",
          has_imm && s.immutable().EntryCount() == 1, "freezing is not clearing");
    Check("is the old table frozen", has_imm && s.immutable().IsFrozen(),
          "one-way switch, set by Freeze()");

    std::string v;
    Check("writes still work on the new table", s.Put("b", "2").ok(),
          "the writer never waited");
    Check("RotateCount() == 1", s.RotateCount() == 1, "counted once");
  }

  // ── [3] 跨表读 ──────────────────────────────────────────────
  std::printf("\n[3] read across tables\n");
  {
    lsmkv::MemTableSet s;
    s.Put("a", "1");
    s.Rotate();
    s.Put("b", "2");

    std::string v;
    Check("read(a) -> 1 (from the immutable)", ReadAsUser(s, "a", &v) && v == "1",
          "the key only exists in the old table");
    Check("read(b) -> 2 (from the active)", ReadAsUser(s, "b", &v) && v == "2",
          "the key only exists in the new table");
    Check("read(zz) -> kNotFound",
          s.Lookup("zz", &v) == lsmkv::LookupResult::kNotFound,
          "neither table has it");
  }

  // ── [4] 新赢 ────────────────────────────────────────────────
  std::printf("\n[4] both tables have the key -- the newer one wins\n");
  {
    lsmkv::MemTableSet s;
    s.Put("a", "old");
    s.Rotate();
    s.Put("a", "new");

    std::string v;
    Check("read(a) -> new", ReadAsUser(s, "a", &v) && v == "new",
          "active is newer than immutable");
    Check("the old value is still on disk-in-waiting",
          s.HasImmutable() && s.immutable().EntryCount() == 1,
          "we did not overwrite the old table");
  }

  // ── [5] 墓碑终结（今晚的核心）────────────────────────────────
  std::printf("\n[5] a tombstone in the new table ends the search\n");
  {
    lsmkv::MemTableSet s;
    s.Put("a", "old");
    s.Rotate();      // "a" 现在躺在 immutable 里，值还是 "old"
    s.Delete("a");    // active 里多一条墓碑

    std::string v = "untouched";
    const lsmkv::LookupResult r = s.Lookup("a", &v);

    std::printf("  immutable has a=old, active has a=<tombstone>\n");
    Check("Lookup(a) -> kDeleted", r == lsmkv::LookupResult::kDeleted,
          "not kNotFound -- stopped here, did not fall through");
    Check("the old value did NOT resurrect", v != "old",
          "this is the bug this check exists for");
    Check("the user view is still 'gone'", !ReadAsUser(s, "a", &v),
          "three states inside, two outside");
    Check("the old value is still physically there",
          s.HasImmutable() && s.immutable().EntryCount() == 1,
          "we hid it, we did not rewrite history");
    Check("the tombstone is physically in the active table",
          s.active().EntryCount() == 1, "one record: the tombstone");
  }

  // ── [6] 墓碑在旧表 ──────────────────────────────────────────
  std::printf("\n[6] a tombstone in the old table\n");
  {
    lsmkv::MemTableSet s;
    s.Put("a", "1");
    s.Delete("a");
    s.Rotate();      // 墓碑随整张表一起冻结

    std::string v;
    Check("Lookup(a) -> kDeleted", s.Lookup("a", &v) == lsmkv::LookupResult::kDeleted,
          "the tombstone survived the freeze");
    Check("the tombstone is still an entry",
          s.HasImmutable() && s.immutable().EntryCount() == 1,
          "freezing does not drop tombstones either");
  }

  // ── [7] 背压 ────────────────────────────────────────────────
  std::printf("\n[7] backpressure: only one immutable at a time\n");
  {
    lsmkv::MemTableSet s;
    s.Put("a", "1");
    Check("first Rotate() -> true", s.Rotate(), "the slot was free");
    Check("second Rotate() -> false", !s.Rotate(),
          "the previous table has not been flushed yet");
    Check("still exactly one rotation", s.RotateCount() == 1,
          "the refused call changed nothing");
    Check("writes still go through", s.Put("b", "2").ok(),
          "backpressure is not an error");

    std::unique_ptr<lsmkv::MemTable> flushed = s.ReleaseImmutable();
    Check("ReleaseImmutable() handed over a real table", flushed != nullptr,
          "ownership moved out, it was not copied");
    Check("the handed-over table still holds the data",
          flushed != nullptr && flushed->EntryCount() == 1,
          "releasing is not clearing");
    Check("the slot is free again", !s.HasImmutable(), "immutable_ is null now");
    Check("now Rotate() -> true", s.Rotate(), "the slot opened up");

    // 连着换 5 次：验机制不是一次性的，也验每次交出来的表都带着
    // 那一刻的数据 —— 所有权转对了，数据才不会跟着指针一起消失。
    int rounds = 0, handed_over_with_data = 0;
    for (int i = 0; i < 5; ++i) {
      if (s.HasImmutable()) s.ReleaseImmutable();  // 清掉上一轮留下的那张
      s.Put(KeyOf(i), "x");
      if (!s.Rotate()) break;
      std::unique_ptr<lsmkv::MemTable> t = s.ReleaseImmutable();
      ++rounds;
      if (t != nullptr && t->EntryCount() == 1) ++handed_over_with_data;
    }
    Check("5 rotate + release cycles in a row",
          rounds == 5 && handed_over_with_data == 5,
          "the mechanism is repeatable, no state left behind");
  }

  // ── [8] 自动换表 ────────────────────────────────────────────
  std::printf("\n[8] ShouldFlush triggers a rotate by itself\n");
  {
    // key 6 字节 + value 10 字节 + 固定开销 128 ≈ 144 字节/条
    // 阈值 400 → 第 3 条写完就该换表
    lsmkv::MemTableSet s(400);
    s.Put("a", "0123456789");
    s.Put("b", "0123456789");
    Check("2 puts is not enough to rotate", s.RotateCount() == 0,
          "288 bytes < 400");
    s.Put("c", "0123456789");
    Check("the 3rd put rotated", s.RotateCount() == 1 && s.HasImmutable(),
          "432 bytes >= 400");
    Check("the new table is empty", s.active().EntryCount() == 0,
          "the overflow went to the old table, not the new one");

    // 把这一张交出去，槽位才空得出来。现在没有 SSTable 接盘，
    // 所以它会被丢掉 —— [8] 只数换表次数、不验数据存活，不影响结论。
    std::unique_ptr<lsmkv::MemTable> dropped = s.ReleaseImmutable();
    (void)dropped;

    s.Put("d", "0123456789");
    s.Put("e", "0123456789");
    s.Put("f", "0123456789");
    Check("and again on the next table", s.RotateCount() == 2,
          "the mechanism is not one-shot");
  }

  // ── [9] 与 std::map 对敲 ────────────────────────────────────
  std::printf("\n[9] 2000 random ops against std::map, one rotate in the middle\n");
  {
    const int kKeys = 300;
    lsmkv::MemTableSet s;
    std::map<std::string, std::string> oracle;
    Rng rng;

    int puts = 0, dels = 0, gets = 0, bad = 0, two_tables_ops = 0;

    for (int i = 0; i < 2000; ++i) {
      // 第 200 步换一次表，然后【再也不刷】：让双表状态持续 1800 步。
      //
      // ★ 这里故意不调 ReleaseImmutable。因为现在还没有 SSTable 接盘，
      //   交出去就等于把这批数据扔掉 —— 对拍会立刻炸出来。
      //   这恰恰说明它叫"释放所有权"而不是"刷盘"：名字不能骗人。
      if (i == 200) s.Rotate();
      if (s.HasImmutable()) ++two_tables_ops;

      const std::string k = KeyOf(static_cast<int>(rng.Next() % kKeys));
      const unsigned int op = rng.Next() % 100;

      if (op < 45) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "v%u", rng.Next());
        s.Put(k, buf);
        oracle[k] = buf;
        ++puts;
      } else if (op < 75) {
        s.Delete(k);
        oracle.erase(k);
        ++dels;
      } else {
        std::string v;
        const bool found = (s.Lookup(k, &v) == lsmkv::LookupResult::kFound);
        ++gets;
        auto it = oracle.find(k);
        const bool expect_found = (it != oracle.end());
        if (found != expect_found) {
          ++bad;
          if (bad <= 3) {
            std::printf("  MISMATCH on %s: store says %s, oracle says %s\n",
                        k.c_str(), found ? "found" : "not found",
                        expect_found ? "found" : "not found");
          }
        } else if (expect_found && v != it->second) {
          ++bad;
          if (bad <= 3) {
            std::printf("  MISMATCH on %s: got '%s', want '%s'\n",
                        k.c_str(), v.c_str(), it->second.c_str());
          }
        }
      }
    }

    std::printf("  %d puts / %d deletes / %d gets, %d of them with two tables\n",
                puts, dels, gets, two_tables_ops);
    Check("no disagreement with the oracle", bad == 0,
          "0 bad -- including every delete that crossed a rotate");
    Check("the two-table path was actually exercised", two_tables_ops > 1000,
          "more than half the ops saw an immutable table");
    Check("rotations happened", s.RotateCount() == 1,
          "one rotate, then 1800 ops lived with two tables");

    // 最后把所有 key 全查一遍：这是 [9] 真正的总结算
    int final_bad = 0;
    for (int i = 0; i < kKeys; ++i) {
      const std::string k = KeyOf(i);
      std::string v;
      const bool found = (s.Lookup(k, &v) == lsmkv::LookupResult::kFound);
      auto it = oracle.find(k);
      const bool expect_found = (it != oracle.end());
      if (found != expect_found) {
        ++final_bad;
      } else if (expect_found && v != it->second) {
        ++final_bad;
      }
    }
    Check("every key matches the oracle", final_bad == 0,
          "0 of 300 keys disagree");
  }

  // ── [10] 换不动的时候 ──────────────────────────────────────
  std::printf("\n[10] when nothing can be flushed, the limit stops meaning anything\n");
  {
    // 阈值 400 字节 ≈ 3 条记录。先写到触发换表，让 immutable 把槽位占住。
    lsmkv::MemTableSet s(400);
    for (int i = 0; i < 4; ++i) s.Put(KeyOf(i), "0123456789");
    Check("rotate happened once", s.RotateCount() == 1,
          "the slot is now taken by an unflushed table");

    // 再猛写 40 条。Rotate 会一直失败，写会一直成功。
    for (int i = 0; i < 40; ++i) s.Put(KeyOf(100 + i), "0123456789");

    const size_t used = s.active().ApproximateMemoryUsage();
    const size_t cap = s.active().max_bytes();
    std::printf("  active holds %zu bytes, but its own limit is %zu\n", used, cap);
    Check("no second rotate was allowed", s.RotateCount() == 1,
          "immutable never left, so the slot never opened");
    Check("active grew far past its limit", used > 10 * cap,
          "ShouldFlush() is true and nobody can act on it");

    std::printf("  ^ 这不是 bug，是一个【还没做的决定】。\n");
    std::printf("    active 写满、immutable 又刷不走的时候，写入方只有三条路：\n");
    std::printf("      a) 继续往里塞   —— 现在的行为，上限等于没有\n");
    std::printf("      b) 阻塞等待     —— LevelDB 选这个（write stall）\n");
    std::printf("      c) 直接返回错误 —— 客户端收到写失败\n");
    std::printf("    选哪个取决于【下游多久能缓解】。明天有后台线程了才谈得上选。\n");
  }

  std::printf("\n");
  if (g_failed == 0) {
    std::printf("ALL CHECKS PASSED\n");
    return 0;
  }  std::printf("%d CHECK(S) FAILED\n", g_failed);
  return 1;
}
