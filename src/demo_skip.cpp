// ══════════════════════════════════════════════════════════════
//  跳表验收（11-03 建，11-04 加 Insert 部分）
//
//  跑法（在 D:\code\lsm-kv 下）：
//    .\build.ps1 demo_skip
//
//  这个文件不用你改。你要改的是 include/lsmkv/skip_list.h 里的 TODO。
//
//  [1] 构造完的状态对不对（顺便测析构：块结束时对象会析构）
//  [2] 10 万次 RandomLevel 的分布
//  [3] 期望层数的实测值
//  [4] 基本插入：3 条、乱序插入，看 size_ / L0 顺序 / 三条不变量   ← 11-04
//  [5] 同 key 覆盖：节点数不变、值变成后写的那个                   ← 11-04
//  [6] 1 万条乱序插入：每 1000 条查一次不变量                      ← 11-04
//  [7] 抽 1000 个 key 校验值 + 打印各层节点数                      ← 11-04
//  [8] Get 的边界：空表 / 最小 / 最大 / 中间不存在 / 空 value       ← 11-05
//  [9] 与 std::map 对照：5000 次随机操作，两个实现必须一致          ← 11-05
//  [10] 1 万次查询：跳表 vs 线性扫描，比耗时                        ← 11-05
//  [11] Get 必须是 const 成员函数（编不过就是这里）                ← 11-05
//  [12] type 字段必须原样存进节点（走 DebugFind 看物理层）          ← 11-06
//  [13] Delete 墓碑：无条件写 / Get 屏蔽 / 不摘节点 / 幂等 / 复活    ← 11-06
//  [14] 与 std::map 对照，这次带删除                                ← 11-06
//
//  ★ 为什么非要测分布？
//
//    RandomLevel 写错的方式很隐蔽：比如退回固定返回 1，程序照样不崩、
//    功能照样能跑（只是变回普通链表，慢了 20 倍）。
//    只有看分布，才知道「抛硬币」真的在抛。
//
//  ★ 为什么非要 CheckInvariants？
//
//    Insert 写错的方式更隐蔽：某层少挂一根指针，查询只是偶尔漏一条数据，
//    程序照样跑完、照样打印「插入成功」。这种错靠肉眼看输出永远发现不了，
//    必须把不变量显式断言出来。
//    这就是「能跑」和「对」的区别。缓存里存个数字，比任何断言都硬。
// ══════════════════════════════════════════════════════════════

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "lsmkv/skip_list.h"

namespace {

int g_failed = 0;

void Check(const char* label, bool ok, const char* detail) {
  std::printf("  %-46s %s   %s\n", label, ok ? "PASS" : "FAIL", detail);
  if (!ok) ++g_failed;
}

// 某个层数的理论概率（百分数）
//
//   P(level = L) = 1 / 2^L         当 L < kMaxLevel
//   P(level = kMaxLevel) = 1 / 2^(kMaxLevel-1)     ← 截断累积到这里
//
// 推导：掷到第 L 层的条件，是前 L-1 次全是正面、第 L 次是反面。
double ExpectPercent(int level) {
  int power = (level == lsmkv::kMaxLevel) ? (lsmkv::kMaxLevel - 1) : level;
  return 100.0 / static_cast<double>(1 << power);
}

}  // namespace

int main() {
  // ★ 关掉 stdout 缓冲。
  //
  //   默认 stdout 是「全缓冲」：printf 只是往内存缓冲区里写，
  //   等到缓冲区满（4~8KB）或程序正常 return 时才一次性刷出去。
  //   如果程序中途崩溃（段错误），缓冲区里的内容【直接蒸发】——
  //   你看到的效果就是：明明 [1][2][3] 都该打印，屏幕上却一片空白。
  //
  //   这和 11-01 讲的「不 flush 就被 kill -9，数据全丢」是同一件事，
  //   只是那次丢的是文件内容，这次丢的是屏幕输出。
  //
  //   _IONBF = 不缓冲，每个 printf 立刻落盘。测试程序不差这点性能。
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // ── [1] 构造完的状态 ────────────────────────────────────────
  std::printf("[1] state right after construction\n");
  {
    lsmkv::SkipList list;
    Check("Level() == 1", list.Level() == 1, "a fresh list has only L0");
    Check("Size() == 0", list.Size() == 0, "no records yet");
  }  // ← 对象在这里析构。析构写错（比如 double delete）就在这一行炸。

  // ── [2] RandomLevel 的分布 ──────────────────────────────────
  std::printf("\n[2] distribution of RandomLevel over 100000 draws\n");

  const int kRound = 100000;
  lsmkv::SkipList list;
  int hist[lsmkv::kMaxLevel + 1] = {0};
  int out_of_range = 0;

  for (int i = 0; i < kRound; ++i) {
    const int lv = list.RandomLevel();
    if (lv < 1 || lv > lsmkv::kMaxLevel) {
      ++out_of_range;
      continue;
    }
    ++hist[lv];
  }

  Check("every result within [1, kMaxLevel]", out_of_range == 0,
        out_of_range == 0 ? "all in range" : "out of range!");

  std::printf("\n  %-6s %-9s %-9s %-9s %s\n", "level", "count", "actual%",
              "expect%", "verdict");
  std::printf("  %s\n", "------------------------------------------------------");

  // 前 5 层严格判（误差 < 3 个百分点）；后面几层样本太少，只打印不判定
  const int kStrictUpto = 5;
  const double kTolerance = 3.0;  // 百分点

  for (int lv = 1; lv <= lsmkv::kMaxLevel; ++lv) {
    const double actual = 100.0 * hist[lv] / kRound;
    const double expect = ExpectPercent(lv);
    const double diff = actual - expect;

    const char* verdict = "";
    if (lv <= kStrictUpto) {
      const bool ok = (diff < kTolerance && diff > -kTolerance);
      verdict = ok ? "ok" : "OFF";
      if (!ok) ++g_failed;
    }
    std::printf("  %-6d %-9d %-9.2f %-9.2f %s\n", lv, hist[lv], actual, expect,
                verdict);
  }

  // ── [3] 期望层数 ────────────────────────────────────────────
  std::printf("\n[3] is it really 2 levels on average?\n");

  double sum = 0.0;
  for (int lv = 1; lv <= lsmkv::kMaxLevel; ++lv) {
    sum += static_cast<double>(lv) * hist[lv];
  }
  const double mean = sum / kRound;
  std::printf("  mean level = %.3f   (theory: about 2.0)\n", mean);

  int highest = 0;
  for (int lv = lsmkv::kMaxLevel; lv >= 1; --lv) {
    if (hist[lv] > 0) {
      highest = lv;
      break;
    }
  }
  std::printf("  highest level seen = %d   (kMaxLevel = %d)\n", highest,
              lsmkv::kMaxLevel);
  Check("mean level is close to 2 (1.7 ~ 2.3)", mean > 1.7 && mean < 2.3,
        "geometric distribution with p=1/2");

  // ── [4] 基本插入 ────────────────────────────────────────────
  std::printf("\n[4] insert 3 records, out of order\n");
  {
    lsmkv::SkipList list;

    Check("Insert(\"a\") returns true", list.Insert("a", "1"), "accepted");
    list.Insert("c", "3");                      // 故意乱序插
    list.Insert("b", "2");
    Check("Size() == 3", list.Size() == 3, "a / b / c");

    const std::vector<std::string> l0 = list.LevelKeys(0);
    const std::vector<std::string> want({"a", "b", "c"});
    Check("L0 == [a b c]", l0 == want,
          l0 == want ? "L0 is sorted no matter the insert order"
                     : "L0 is not the sorted sequence");

    const std::string err = list.CheckInvariants();
    Check("3 invariants hold", err.empty(),
          err.empty() ? "L0 order / level_ / per-level chains" : err.c_str());

    std::printf("\n  level_ = %d, L0 = ", list.Level());
    for (size_t i = 0; i < l0.size(); ++i)
      std::printf("%s%s", i ? " -> " : "", l0[i].c_str());
    std::printf("\n");
  }  // ← 析构。这里崩了就是 double delete 或漏删

  // ── [5] 同 key 覆盖 ─────────────────────────────────────────
  //
  // ★ 这是今天语义上最容易写错的一处：见 skip_list.h 里 Insert 的注释。
  std::printf("\n[5] inserting the same key twice\n");
  {
    lsmkv::SkipList list;
    list.Insert("k", "v1");
    list.Insert("k", "v2");
    Check("Size() == 1", list.Size() == 1, "overwrite, not append");
    Check("value is the later one", list.DebugGet("k") == "v2",
          "last write wins");

    const std::string err = list.CheckInvariants();
    Check("invariants hold", err.empty(),
          err.empty() ? "no duplicate key in L0" : err.c_str());
  }

  // ── [6] 1 万条乱序插入 ──────────────────────────────────────
  std::printf("\n[6] 10000 inserts in random order\n");
  {
    const int n = 10000;
    std::vector<std::string> keys(static_cast<size_t>(n));
    std::vector<std::string> vals(static_cast<size_t>(n));
    std::vector<int> order(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "k%05d", i);
      keys[static_cast<size_t>(i)] = buf;
      vals[static_cast<size_t>(i)] = std::string(buf) + "_v";
      order[static_cast<size_t>(i)] = i;
    }

    // 打乱插入顺序：顺序插入测不出「前驱找错」的 bug
    uint32_t s = 0x9E3779B9u;
    for (int i = n - 1; i > 0; --i) {
      s = s * 1664525u + 1013904223u;
      const int j = static_cast<int>(s % static_cast<uint32_t>(i + 1));
      const int t = order[static_cast<size_t>(i)];
      order[static_cast<size_t>(i)] = order[static_cast<size_t>(j)];
      order[static_cast<size_t>(j)] = t;
    }

    lsmkv::SkipList big;
    std::string first_err;
    for (int i = 0; i < n; ++i) {
      const size_t idx = static_cast<size_t>(order[static_cast<size_t>(i)]);
      big.Insert(keys[idx], vals[idx]);
      if ((i + 1) % 1000 == 0 && first_err.empty())
        first_err = big.CheckInvariants();
    }
    Check("invariants hold after every 1000", first_err.empty(),
          first_err.empty() ? "checked 10 times" : first_err.c_str());
    Check("Size() == 10000", big.Size() == static_cast<size_t>(n),
          "no node lost, no node duplicated");

    // ── [7] 抽查 + 各层规模 ──────────────────────────────────
    std::printf("\n[7] read back 1000 of them, then look at the shape\n");
    int bad = 0;
    for (int i = 0; i < n; i += 10)
      if (big.DebugGet(keys[static_cast<size_t>(i)]) != vals[static_cast<size_t>(i)])
        ++bad;
    Check("1000 sampled values are correct", bad == 0,
          bad == 0 ? "linear scan on L0 (independent of Insert)" : "wrong value found");

    std::printf("\n  per-level node count (each level should be about half):\n");
    for (int lv = 0; lv < lsmkv::kMaxLevel; ++lv) {
      const size_t c = big.LevelKeys(lv).size();
      if (c == 0) break;
      std::printf("  L%-2d %7zu  %5.1f%%\n", lv, c, 100.0 * static_cast<double>(c) / n);
    }
    std::printf("  level_ = %d\n", big.Level());
  }  // ← 1 万个节点的析构，漏删会在这里被系统/工具发现

  // ── [8] Get 的边界 ──────────────────────────────────────────
  std::printf("\n[8] Get: edge cases\n");
  {
    lsmkv::SkipList list;
    std::string v = "unchanged";
    lsmkv::RecordType t = lsmkv::RecordType::kDelete;

    Check("empty list -> false", list.Get("a", &v, &t) == false,
          "nothing inserted yet");
    Check("out-params untouched on miss", v == "unchanged",
          "a miss must not write the out-params");

    list.Insert("b", "2");
    list.Insert("a", "1");
    list.Insert("c", "3");

    v.clear();
    t = lsmkv::RecordType::kDelete;
    const bool hit_b = list.Get("b", &v, &t);
    Check("hit the middle key", hit_b && v == "2" && t == lsmkv::RecordType::kPut,
          "b -> 2, and type is written too");

    v.clear();
    Check("hit the smallest key", list.Get("a", &v, &t) && v == "1", "a -> 1");
    v.clear();
    Check("hit the biggest key", list.Get("c", &v, &t) && v == "3", "c -> 3");

    Check("miss: smaller than all", !list.Get("0", &v, &t), "0 < a");
    Check("miss: bigger than all", !list.Get("z", &v, &t), "z > c");
    Check("miss: between two keys", !list.Get("aa", &v, &t), "a < aa < b");
    Check("miss: empty key", !list.Get("", &v, &t), "no empty key stored");

    Check("nullptr out-params are legal", list.Get("b", nullptr, nullptr),
          "existence check only");

    list.Insert("blank", "");
    v = "unchanged";
    Check("empty value is still a hit", list.Get("blank", &v, &t) && v.empty(),
          "this is why bool + out-param, not return string");
  }

  // ── [9] 与 std::map 对照 ────────────────────────────────────
  //
  // ★ 这叫 oracle 测试（参照实现测试）：不手写期望值，而是拿另一个
  //   实现（std::map）当答案册，随机喂同样的操作，两边结果必须一致。
  //   手写 case 只能覆盖你想到的情况，随机对敲能覆盖你没想到的。
  std::printf("\n[9] 5000 random operations, checked against std::map\n");
  {
    lsmkv::SkipList list;
    std::map<std::string, std::string> oracle;
    uint32_t s = 0x2718u;
    int inserted = 0;
    int looked = 0;
    std::string first_bad;

    for (int i = 0; i < 5000; ++i) {
      s = s * 1664525u + 1013904223u;
      char buf[32];
      std::snprintf(buf, sizeof(buf), "k%04d", static_cast<int>(s % 2000u));
      const std::string key(buf);

      if (((s >> 16) & 1u) != 0) {
        const std::string val = key + "#" + std::to_string(++inserted);
        list.Insert(key, val);
        oracle[key] = val;
      } else {
        std::string got;
        lsmkv::RecordType t;
        const bool hit = list.Get(key, &got, &t);
        const bool want = oracle.find(key) != oracle.end();
        ++looked;
        if (hit != want && first_bad.empty()) {
          first_bad = "key " + key + ": skip says " +
                      (hit ? "found" : "missing") + ", map says " +
                      (want ? "found" : "missing");
        }
      }
    }

    Check("5000 random ops: no disagreement", first_bad.empty(),
          first_bad.empty() ? "insert / overwrite / lookup all agree"
                            : first_bad.c_str());

    int bad = 0;
    std::string bad_detail;
    for (int i = 0; i < 2000; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "k%04d", i);
      const std::string key(buf);
      std::string got;
      const bool hit = list.Get(key, &got, nullptr);
      const std::map<std::string, std::string>::const_iterator it = oracle.find(key);
      const bool want = it != oracle.end();
      if (hit != want || (hit && got != it->second)) {
        ++bad;
        if (bad_detail.empty()) bad_detail = "key " + key + " returned the wrong value";
      }
    }
    Check("all 2000 keys match the oracle", bad == 0,
          bad == 0 ? "existence + value, every key" : bad_detail.c_str());
    Check("size_ agrees with the oracle", list.Size() == oracle.size(),
          "node count check");

    std::printf("  %d writes, %d distinct keys, %d lookups\n", inserted,
                static_cast<int>(oracle.size()), looked);
  }

  // ── [10] 性能对比 ───────────────────────────────────────────
  std::printf("\n[10] 10000 lookups: skip list vs linear scan on L0\n");
  {
    const int n = 10000;
    lsmkv::SkipList big;
    std::vector<std::string> keys(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "k%05d", i);
      keys[static_cast<size_t>(i)] = buf;
      big.Insert(keys[static_cast<size_t>(i)], "v");
    }

    long long sink = 0;
    int hits = 0;

    const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < 10000; ++r) {
      std::string v;
      if (big.Get(keys[static_cast<size_t>(r % n)], &v, nullptr)) {
        sink += static_cast<long long>(v.size());
        ++hits;
      }
    }
    const std::chrono::steady_clock::time_point t1 = std::chrono::steady_clock::now();

    for (int r = 0; r < 10000; ++r)
      sink += static_cast<long long>(big.DebugGet(keys[static_cast<size_t>(r % n)]).size());
    const std::chrono::steady_clock::time_point t2 = std::chrono::steady_clock::now();

    const double skip_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    const double lin_us = std::chrono::duration<double, std::micro>(t2 - t1).count();
    const double base = (skip_us > 0.01) ? skip_us : 0.01;

    std::printf("  skip list : %9.1f us   (10000 lookups)\n", skip_us);
    std::printf("  linear L0 : %9.1f us\n", lin_us);
    std::printf("  speedup   : %9.1fx\n", lin_us / base);
    std::printf("  (built without -O2; the gap widens a lot once optimised)\n");

    // ★ 必须先确认「真的查到了东西」，否则一个永远返回 false 的空壳
    //   会因为「什么都没干所以很快」而白捡一个 PASS —— 假通过。
    Check("every lookup actually found its key", hits == 10000,
          hits == 10000 ? "10000 / 10000 hits" : "Get returned false for existing keys");
    Check("skip list beats linear scan by >10x", lin_us > base * 10.0,
          "O(log n) vs O(n)");
    if (sink < 0) std::printf("  (checksum %lld)\n", sink);
  }

  // ── [11] Get 必须是 const ───────────────────────────────────
  //
  // ★ 这一段的唯一作用是【编译期检查】：
  //   如果你把 Get 写成非 const 成员函数，这行会报
  //     error: passing 'const lsmkv::SkipList' as 'this' argument
  //            discards qualifiers
  //   整个程序编不过 —— 那就去 Get 的声明末尾补一个 const。
  std::printf("\n[11] Get must be a const method\n");
  {
    lsmkv::SkipList list;
    list.Insert("x", "1");
    const lsmkv::SkipList& cref = list;
    std::string v;
    Check("const reference can call Get", cref.Get("x", &v, nullptr) && v == "1",
          "compiles only if Get is declared const");
  }

  // ── [12] type 字段必须原样存进节点 ──────────────────────────
  //
  // ★ 这段改用 DebugFind 而不是 Get —— 这是今天语义变化导致的必要修改，
  //   正好是个例子：**规格一变，测试必须跟着变，否则测试在骗你。**
  //
  //   [12] 验的是【物理记录】对不对（type 是不是 kDelete）。
  //   而今天之后，Get 的职责恰恰是把墓碑藏起来，返回 false。
  //   继续用 Get 的话，这段会因为「Get 按规矩办事」而变红 —— 那是测试错了。
  std::printf("\n[12] record type must survive Insert\n");
  {
    lsmkv::SkipList list;
    list.Insert("alive", "1", lsmkv::RecordType::kPut);
    list.Insert("dead", "", lsmkv::RecordType::kDelete);  // 墓碑：值可以为空

    const lsmkv::Node* d = list.DebugFind("dead");
    Check("the record physically exists", d != nullptr,
          "a kDelete record still occupies a node");
    Check("its type is kDelete",
          d != nullptr && d->type == lsmkv::RecordType::kDelete,
          d != nullptr && d->type == lsmkv::RecordType::kDelete
              ? "Insert forwarded `type` to NewNode"
              : "Insert hardcoded kPut instead of forwarding `type`");

    // 再写一次同 key，类型也必须跟着换回来（相当于"撤销删除"）
    list.Insert("dead", "back", lsmkv::RecordType::kPut);
    const lsmkv::Node* d2 = list.DebugFind("dead");
    Check("overwrite also replaces the type",
          d2 != nullptr && d2->type == lsmkv::RecordType::kPut,
          d2 != nullptr && d2->type == lsmkv::RecordType::kPut
              ? "the overwrite branch must assign both value and type"
              : "overwrite updated value but kept the old type");
  }

  // ── [13] Delete：墓碑 ──────────────────────────────────────
  //
  // ★ 这一整段只验一件事：【删除是写入，不是拆链表】。
  std::printf("\n[13] Delete: tombstones\n");
  {
    lsmkv::SkipList list;

    // ① 删一个从没存在过的 key —— 坑①（不许先查再删）的直接检验
    Check("Delete on a missing key returns true", list.Delete("ghost"),
          "accepted whether or not the key ever existed");

    const lsmkv::Node* g = list.DebugFind("ghost");
    Check("a tombstone was written anyway", g != nullptr,
          g != nullptr ? "no lookup-first, correct"
                       : "Delete skipped because the key was missing (pitfall 1)");
    Check("the tombstone's type is kDelete",
          g != nullptr && g->type == lsmkv::RecordType::kDelete,
          "compaction will look for exactly this later");
    Check("Size() counts the tombstone", list.Size() == 1,
          "a tombstone is a record too; it costs memory");
    Check("Get reports that key as absent", !list.Get("ghost", nullptr, nullptr),
          "to a reader, a tombstone means 'gone'");

    // ② 删一个确实存在的 key
    list.Insert("b", "2");
    std::string v;
    Check("b is readable before Delete", list.Get("b", &v, nullptr) && v == "2",
          "sanity check");
    Check("Delete on an existing key returns true", list.Delete("b"), "accepted");
    Check("Get reports b as absent", !list.Get("b", &v, nullptr),
          "Get must treat a tombstone as a miss");
    Check("Size() did not grow", list.Size() == 2,
          "the key already had a node; Delete overwrote it in place");

    const lsmkv::Node* nb = list.DebugFind("b");
    Check("b is still on the list, as a tombstone",
          nb != nullptr && nb->type == lsmkv::RecordType::kDelete,
          "nothing was unlinked -- that is the whole point");

    // ③ 幂等：同一个 key 删两次不该多出节点
    list.Delete("b");
    Check("deleting twice is idempotent", list.Size() == 2,
          "the 2nd Delete overwrites the 1st tombstone");

    // ④ 复活：删完再写，Get 必须又能查到
    list.Insert("b", "3");
    v.clear();
    Check("re-insert after Delete brings the key back",
          list.Get("b", &v, nullptr) && v == "3",
          "a later kPut overwrites the tombstone");

    // ⑤ 墓碑在结构上就是普通节点，链子不能坏
    const std::string err = list.CheckInvariants();
    Check("invariants still hold", err.empty(),
          err.empty() ? "a tombstone is a structurally normal node" : err.c_str());
  }

  // ── [14] 与 std::map 对照，这次带删除 ───────────────────────
  //
  // ★ 上一轮的 oracle 只做插入和查询。删除是最容易写出「静默错」的操作：
  //   墓碑没写、写错 type、Get 没屏蔽 —— 全都不会崩，只会答错。
  //   唯一能抓住的办法就是拿 std::map 当答案册，随机对敲。
  std::printf("\n[14] 5000 random ops (insert + delete), against std::map\n");
  {
    const int kOps = 5000;
    const int kKeys = 800;
    lsmkv::SkipList list;
    std::map<std::string, std::string> oracle;

    uint32_t s = 0x1234567u;
    auto next = [&s]() {
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      return s;
    };

    int n_put = 0, n_del = 0, n_get = 0, n_bad = 0;
    for (int i = 0; i < kOps; ++i) {
      char key[16];
      std::snprintf(key, sizeof(key), "k%04d", static_cast<int>(next() % kKeys));
      const uint32_t op = next() % 3;

      if (op == 0) {
        char val[24];
        std::snprintf(val, sizeof(val), "v%d", i);
        list.Insert(key, val);
        oracle[key] = val;
        ++n_put;
      } else if (op == 1) {
        list.Delete(key);
        oracle.erase(key);
        ++n_del;
      } else {
        std::string lv;
        const bool lf = list.Get(key, &lv, nullptr);
        const auto it = oracle.find(key);
        const bool of = (it != oracle.end());
        if (lf != of || (of && lv != it->second)) ++n_bad;
        ++n_get;
      }
    }

    char detail[128];
    std::snprintf(detail, sizeof(detail),
                  "%d puts / %d deletes / %d gets, %d disagreement(s)", n_put,
                  n_del, n_get, n_bad);
    Check("no disagreement during the run", n_bad == 0, detail);

    // 全量比对：800 个 key 的存在性和值，一个都不能错
    int mismatch = 0;
    size_t nodes = 0, tombs = 0;
    for (int k = 0; k < kKeys; ++k) {
      char key[16];
      std::snprintf(key, sizeof(key), "k%04d", k);

      // 物理层：这个 key 到底有没有节点，是不是墓碑
      const lsmkv::Node* n = list.DebugFind(key);
      if (n != nullptr) {
        ++nodes;
        if (n->type == lsmkv::RecordType::kDelete) ++tombs;
      }

      // 逻辑层：Get 的说法
      std::string lv;
      const bool lf = list.Get(key, &lv, nullptr);
      const auto it = oracle.find(key);
      const bool of = (it != oracle.end());
      if (lf != of || (of && lv != it->second)) ++mismatch;
    }

    char d2[96];
    std::snprintf(d2, sizeof(d2), "%d of %d keys disagree", mismatch, kKeys);
    Check("every key matches the oracle", mismatch == 0, d2);

    char d3[96];
    std::snprintf(d3, sizeof(d3), "%zu nodes walked, Size() = %zu", nodes,
                  list.Size());
    Check("Size() agrees with the physical node count", nodes == list.Size(), d3);

    const std::string err = list.CheckInvariants();
    Check("invariants survive mass deletion", err.empty(),
          err.empty() ? "2000+ deletes corrupted no level chain" : err.c_str());

    std::printf(
        "\n  records in list = %zu   live keys = %zu   tombstones = %zu\n"
        "  (Size() counts tombstones too -- they cost memory until compaction "
        "drops them)\n",
        list.Size(), oracle.size(), tombs);
  }

  // ── 结论 ────────────────────────────────────────────────────
  std::printf("\n");
  if (g_failed == 0) {
    std::printf("ALL CHECKS PASSED\n");
    return 0;
  }

  std::printf("%d CHECK(S) FAILED\n", g_failed);
  std::printf(
      "\nhow to read the failures:\n"
      "  [1]-[3] fail      -> RandomLevel() is still the stub, go write the loop\n"
      "  [4] Size() == 0   -> Insert() is still the stub `return false;`\n"
      "  [4] L0 wrong      -> the two pointer assignments are in the wrong order,\n"
      "                       or prevs[lv] was taken at the wrong level\n"
      "  [4] invariant msg -> read the message; it names which rule you broke\n"
      "  [5] Size() == 2   -> you appended instead of overwriting on equal key\n"
      "  [6] level_ wrong  -> you forgot to backfill prevs[] with head_ when\n"
      "                       new_level > level_\n"
      "  [8] all false     -> Get() is still the stub `return false;`\n"
      "  [8] out-params    -> a miss must NOT write the out-params\n"
      "  [8] nullptr crash -> guard with `if (value != nullptr)` before writing\n"
      "  [9] disagreement  -> Get and Insert disagree on what is in the list:\n"
      "                       most likely the search loop starts at the wrong level\n"
      "  [10] too slow     -> you are not using the upper levels; check that the\n"
      "                       loop starts at level_ - 1, not 0\n"
      "  compile error at [11]\n"
      "                    -> 'discards qualifiers': add `const` to Get's signature\n"
      "  [12] type wrong   -> Insert calls NewNode(..., RecordType::kPut, ...) with a\n"
      "                       hardcoded kPut; pass the `type` parameter through\n"
      "  [12] overwrite    -> the same-key branch only assigned value, not type\n"
      "  [13] Delete() returns false\n"
      "                    -> Delete is still the stub\n"
      "  [13] no tombstone  -> you looked the key up first and skipped when missing;\n"
      "                       Delete must write the tombstone unconditionally\n"
      "  [13] Get still hits\n"
      "                    -> Get does not screen tombstones yet: add\n"
      "                       `if (p->next[0]->type == RecordType::kDelete) return false;`\n"
      "                       BEFORE writing the out-params\n"
      "  [13] b not linked  -> you physically unlinked the node instead of rewriting it;\n"
      "                       a level chain is now broken (check [13] invariants msg)\n"
      "  [13] Size() grew   -> Delete added a node for a key that already had one\n"
      "  [14] disagreement  -> the tombstone is invisible to Get, or visible when it\n"
      "                       should not be; check the type test above\n");
  return 1;
}
