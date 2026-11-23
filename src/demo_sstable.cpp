// ══════════════════════════════════════════════════════════════
//  SSTable 验收（11-14 建）—— Week 5 第一天
//
//  跑法（在 D:\code\lsm-kv 下）：
//    .\build.ps1 demo_sstable
//
//  这个文件不用你改。你要改的是：
//    include/lsmkv/skip_list.h   TODO 9   (Iterator 的 key/value/type/Next)
//    include/lsmkv/sstable.h     TODO 10  (SSTableBuilder::Add)
//                                TODO 11  (SSTableBuilder::Finish)
//                                TODO 12  (SSTable::Open)
//
//  [1] 倒出来：把 MemTable 按序走一遍，10 条（含 2 个墓碑）进了 builder
//  [2] footer：data_end / count / magic 三个字段对得上
//  [3] 读回：10 条逐条比对 key / value / type，一字不差   ★ roundtrip
//  [4] 查：值类的查得到；墓碑在用户视角是"没有"，
//          但在引擎视角（FindRecord）看得见 —— 屏蔽是上层的事
//  [5] 乱序被拒：写一条倒退的 key 必须失败，且文件里不能多出这条
//  [6] 代价：5000 条 vs 40000 条，同样查 200 次。
//          这个倍率就是"没有索引"的价格 —— 下次做索引的理由
//  [7] 半成品：没 Finish 就析构 → Open 必须报 Corruption，不能装成好文件
//
//  ★ 你先跑一遍。现在应该全是红的 —— [1] 甚至会直接告诉你
//    "iterator does not advance"（因为 Next 还没写）。
//    这不是失败，这是清单。
// ══════════════════════════════════════════════════════════════

#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "lsmkv/mem_table.h"
#include "lsmkv/sstable.h"

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

std::string ValOf(int i) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "value-for-k%05d", i);
  return buf;
}

// ══════════════════════════════════════════════════════════════
//  把一张内存表整个倒进 builder
//
//  ★ 这个十几行的循环，就是 Week 5 开头那个问题的答案：
//    「内存里的东西怎么写进文件」—— 一个迭代器 + 一个 Add，没有魔法。
//
//  ★ 注意 guard 那一小段：它在防"Next() 没实现"。
//    一个不前进的迭代器会让这个 for 变成死循环，程序挂住，
//    你只能看到光标在闪，不知道卡在哪。
//    所以凡是"遍历一个可能是自己写错的链表"，都要有防环保护
//    （SkipList::LevelKeys 里那句 `if (out.size() > 200000) break;` 是同一个东西）。
// ══════════════════════════════════════════════════════════════
lsmkv::Status DumpTable(const lsmkv::MemTable& table, lsmkv::SSTableBuilder* builder,
                        size_t* dumped) {
  size_t n = 0;
  lsmkv::SkipList::Iterator it = table.tree().Begin();
  while (it.Valid()) {
    lsmkv::Record rec;
    rec.key = it.key();
    rec.value = it.value();
    rec.type = it.type();

    const lsmkv::Status st = builder->Add(rec);
    if (!st.ok()) return st;

    ++n;
    it.Next();

    // 走过的条数不应该超过表里的物理条数。超了说明 Next() 没动。
    if (n > table.EntryCount() + 8) {
      return lsmkv::Status::Corruption(
          "iterator does not advance (Iterator::Next implemented?)");
    }
  }
  *dumped = n;
  return lsmkv::Status::OK();
}

// ══════════════════════════════════════════════════════════════
//  造一张固定内容的表：10 条值 + 2 条墓碑
// ══════════════════════════════════════════════════════════════
void FillSampleTable(lsmkv::MemTable* t) {
  for (int i = 1; i <= 10; ++i) {
    t->Put(KeyOf(i), ValOf(i));
  }
  t->Delete(KeyOf(3));   // 墓碑：key 还在，值是空的
  t->Delete(KeyOf(7));
}

// 计时用的「黑洞」：把结果写进一个 volatile 变量。
//
// ★ 为什么非要这么干？
//   写成 `sst.Get(k, &v);` 而 v 之后没人读，编译器有权认为这次调用
//   对程序输出没有任何影响，直接把它删掉 —— 于是你测出来的是「空循环」的时间。
//   症状就是耗时报 0.00 ms，看着飞快，其实一次查询都没执行。
//
//   volatile 让这个写入成为「有外部可见副作用」的操作，编译器删不掉。
//   **测性能的时候，被测的代码必须真的执行，否则数字是假的。**
volatile size_t g_time_sink = 0;

// 查 200 次，返回总耗时（微秒）
double TimeLookups(const lsmkv::SSTable& sst, const std::vector<std::string>& keys) {
  std::string v;
  const auto t0 = std::chrono::steady_clock::now();
  for (const std::string& k : keys) {
    v.clear();
    if (sst.Get(k, &v).ok()) g_time_sink += v.size();
  }
  const auto t1 = std::chrono::steady_clock::now();
  return std::chrono::duration<double, std::micro>(t1 - t0).count();
}

}  // namespace

int main() {
  std::printf("=== SSTable 验收（Week 5 第一天）===\n\n");

  // ── [1] 从 MemTable 有序倒出来 ────────────────────────────
  std::printf("[1] 从 MemTable 有序倒出\n");
  lsmkv::MemTable table(1u << 20);  // 1MB，够放这点数据
  FillSampleTable(&table);

  const std::string path1 = "build_win/sst_sample.sst";
  lsmkv::SSTableBuilder builder1(path1);
  size_t dumped = 0;
  {
    const lsmkv::Status st = DumpTable(table, &builder1, &dumped);
    if (!st.ok()) {
      std::printf("  dump failed: %s\n", st.ToString().c_str());
      std::printf("  （可能是 TODO 9 的 Next() 还没写，也可能是 TODO 10 还没写）\n\n");
      ++g_failed;
    }
  }
  {
    char detail[160];
    std::snprintf(detail, sizeof(detail), "memtable has %zu records, dumped %zu",
                  table.EntryCount(), dumped);
    Check("条数一致", dumped == table.EntryCount() && dumped == 10, detail);
  }

  // 顺着迭代器再走一遍，确认 key 严格递增（这就是"有序"的实证）
  {
    bool ordered = true;
    std::string prev;
    int seen = 0;
    for (lsmkv::SkipList::Iterator it = table.tree().Begin(); it.Valid(); it.Next()) {
      if (seen > 0 && !(prev < it.key())) ordered = false;
      prev = it.key();
      ++seen;
      if (seen > 100) break;  // 防死循环
    }
    char detail[96];
    std::snprintf(detail, sizeof(detail), "walked %d, strictly increasing=%s", seen,
                  ordered ? "yes" : "no");
    Check("迭代顺序严格递增", ordered && seen == 10, detail);
  }

  // ── [2] footer 三个字段 ───────────────────────────────────
  std::printf("\n[2] footer\n");
  lsmkv::Status fst = builder1.Finish();
  {
    char detail[160];
    std::snprintf(detail, sizeof(detail), "finish=%s, file=%llu bytes",
                  fst.ok() ? "OK" : fst.ToString().c_str(),
                  static_cast<unsigned long long>(builder1.file_size()));
    Check("Finish 成功且文件非空", fst.ok() && builder1.file_size() > 24, detail);
  }

  std::unique_ptr<lsmkv::SSTable> sst1;
  {
    const lsmkv::Status st = lsmkv::SSTable::Open(path1, &sst1);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "open=%s", st.ok() ? "OK" : st.ToString().c_str());
    Check("Open 成功", st.ok() && sst1 != nullptr, detail);
    if (!st.ok() || sst1 == nullptr) {
      std::printf("\n打不开文件，后面全部跳过。先把 TODO 10/11/12 写完。\n");
      std::printf("failed: %d\n", g_failed);
      return 1;
    }
  }
  {
    char detail[192];
    std::snprintf(detail, sizeof(detail), "count=%llu, data_end=%llu, size=%llu",
                  static_cast<unsigned long long>(sst1->record_count()),
                  static_cast<unsigned long long>(sst1->data_end()),
                  static_cast<unsigned long long>(sst1->file_size()));
    Check("count == 10", sst1->record_count() == 10, detail);
  }
  {
    // v2：文件 = [数据区][索引区][footer]，三段必须正好填满，一个字节不多不少。
    //
    // ★ 这条断言比 v1 的「data_end + 24 == size」强得多：索引项是变长的，
    //   index_bytes 是把解析出来的每一项按 [klen][key][offset] 重新算一遍得到的。
    //   只要解析错位一个字节、少解一项、或者 klen 读错，这个等式立刻不成立。
    uint64_t index_bytes = 0;
    for (const lsmkv::IndexEntry& e : sst1->index()) {
      index_bytes += 4 + e.key.size() + 8;
    }
    const uint64_t sum = sst1->data_end() + index_bytes + lsmkv::kFooterSize;
    const bool ok = sum == sst1->file_size();
    char detail[224];
    std::snprintf(detail, sizeof(detail),
                  "data %llu + index %llu + footer %llu = %llu, file = %llu",
                  static_cast<unsigned long long>(sst1->data_end()),
                  static_cast<unsigned long long>(index_bytes),
                  static_cast<unsigned long long>(lsmkv::kFooterSize),
                  static_cast<unsigned long long>(sum),
                  static_cast<unsigned long long>(sst1->file_size()));
    Check("三段正好填满文件", ok, detail);
  }

  // ── [2b] 索引区：解析出来的东西对不对 ────────────────────
  std::printf("\n[2b] 索引区\n");
  {
    const uint64_t n = sst1->record_count();
    const uint64_t want = (n + lsmkv::kIndexInterval - 1) / lsmkv::kIndexInterval;
    char d1[160];
    std::snprintf(d1, sizeof(d1), "%zu anchors for %llu records (interval %zu)",
                  sst1->index().size(), static_cast<unsigned long long>(n),
                  lsmkv::kIndexInterval);
    Check("锚点数 == ceil(count / 16)", sst1->index().size() == want, d1);
  }
  {
    // 每个锚点存的 offset，必须正好指向那条 key 自己。
    //
    // ★ 这是「写方和读方对表」的核心检查：Builder 记的是"这条记录将要落的位置"，
    //   Open 解析出来的 offset 再去 Decode，key 必须完全一致。
    //   差一条、差一个字节，这里都会红。
    bool all_ok = true;
    std::string bad_key;
    uint64_t bad_off = 0;
    for (const lsmkv::IndexEntry& e : sst1->index()) {
      lsmkv::Record r;
      size_t consumed = 0;
      const lsmkv::Status st = lsmkv::Decode(sst1->data(), e.offset, &r, &consumed);
      if (!st.ok() || r.key != e.key) {
        all_ok = false;
        bad_key = e.key;
        bad_off = e.offset;
        break;
      }
    }
    char d2[224];
    if (all_ok) {
      std::snprintf(d2, sizeof(d2), "all %zu anchors decode back to their own key",
                    sst1->index().size());
    } else {
      std::snprintf(d2, sizeof(d2), "anchor \"%s\"@%llu decodes to something else",
                    bad_key.c_str(), static_cast<unsigned long long>(bad_off));
    }
    Check("锚点 offset 指向正确的记录", all_ok, d2);
  }

  // ── [3] roundtrip：12 条逐条比对 ──────────────────────────
  std::printf("\n[3] 读回（roundtrip）\n");
  {
    int matched = 0;
    int expected_deleted = 0;
    for (int i = 1; i <= 10; ++i) {
      lsmkv::Record rec;
      const lsmkv::Status st = sst1->FindRecord(KeyOf(i), &rec);
      if (!st.ok()) continue;
      const bool want_deleted = (i == 3 || i == 7);
      if (want_deleted) {
        if (rec.type == lsmkv::RecordType::kDelete && rec.value.empty()) ++matched;
        ++expected_deleted;
      } else if (rec.type == lsmkv::RecordType::kPut && rec.value == ValOf(i)) {
        ++matched;
      }
    }
    char detail[128];
    std::snprintf(detail, sizeof(detail), "matched %d / 10 (其中墓碑 %d 条)", matched,
                  expected_deleted);
    Check("10 条 key/value/type 全部一致", matched == 10, detail);
  }
  {
    // 文件里不该出现第 11 条
    lsmkv::Record rec;
    const lsmkv::Status st = sst1->FindRecord(KeyOf(11), &rec);
    Check("不存在的 key 查不到", !st.ok(), st.ok() ? "found?! " : "NotFound as expected");
  }

  // ── [4] 墓碑：两种视角 ────────────────────────────────────
  std::printf("\n[4] 墓碑的两种视角\n");
  {
    std::string v;
    const lsmkv::Status st = sst1->Get(KeyOf(3), &v);
    Check("Get(k00003) 查不到（用户视角）", !st.ok(), st.ok() ? "returned a value?!" : "NotFound");
  }
  {
    lsmkv::Record rec;
    const lsmkv::Status st = sst1->FindRecord(KeyOf(3), &rec);
    const bool ok = st.ok() && rec.type == lsmkv::RecordType::kDelete;
    Check("FindRecord(k00003) 看得见墓碑（引擎视角）", ok,
          ok ? "type=kDelete" : "看不到 —— 墓碑在落盘时被吞了？");
  }
  {
    std::string v;
    const lsmkv::Status st = sst1->Get(KeyOf(5), &v);
    Check("Get(k00005) 正常返回", st.ok() && v == ValOf(5), v.c_str());
  }

  // ── [5] 乱序守卫 ──────────────────────────────────────────
  std::printf("\n[5] 乱序写入必须被拒\n");
  {
    const std::string path5 = "build_win/sst_order.sst";
    lsmkv::SSTableBuilder b(path5);
    lsmkv::Record a;
    a.key = "m";
    a.value = "1";
    lsmkv::Record c;
    c.key = "a";  // 倒退
    c.value = "2";
    const lsmkv::Status s1 = b.Add(a);
    const lsmkv::Status s2 = b.Add(c);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "first=%s, second=%s", s1.ok() ? "OK" : "rej",
                  s2.ok() ? "OK(!!)" : "rejected");
    Check("倒退的 key 被拒且计数不变", s1.ok() && !s2.ok() && b.record_count() == 1,
          detail);
    b.Finish();
  }
  {
    // 同一个 key 也不许出现两次
    const std::string path5b = "build_win/sst_dup.sst";
    lsmkv::SSTableBuilder b(path5b);
    lsmkv::Record a;
    a.key = "x";
    a.value = "1";
    const lsmkv::Status s1 = b.Add(a);
    const lsmkv::Status s2 = b.Add(a);
    Check("重复 key 被拒", s1.ok() && !s2.ok(), s2.ok() ? "accepted a duplicate" : "rejected");
    b.Finish();
  }

  // ── [6] 有了索引之后，查一条还是 O(N) 吗 ──────────────────
  std::printf("\n[6] 查一条要多久（有索引 vs 线性）\n");
  {
    const int kSmall = 5000;
    const int kBig = 40000;

    double small_us = 0.0;
    double big_us = 0.0;
    int small_ok = 0;
    int big_ok = 0;

    for (int round = 0; round < 2; ++round) {
      const int n = (round == 0) ? kSmall : kBig;
      lsmkv::MemTable t(1u << 30);  // 大阈值，别让它自己想着冻结
      for (int i = 1; i <= n; ++i) t.Put(KeyOf(i), ValOf(i));

      char pathbuf[64];
      std::snprintf(pathbuf, sizeof(pathbuf), "build_win/sst_%d.sst", n);
      lsmkv::SSTableBuilder b(pathbuf);
      size_t out = 0;
      lsmkv::Status st = DumpTable(t, &b, &out);
      if (!st.ok()) {
        std::printf("  dump %d failed: %s\n", n, st.ToString().c_str());
        ++g_failed;
        break;
      }
      b.Finish();

      std::unique_ptr<lsmkv::SSTable> sst;
      st = lsmkv::SSTable::Open(pathbuf, &sst);
      if (!st.ok()) {
        std::printf("  open %d failed: %s\n", n, st.ToString().c_str());
        ++g_failed;
        break;
      }

      // 查 200 个 key，均匀撒在文件里（最坏情况接近全扫）
      std::vector<std::string> qs;
      qs.reserve(200);
      for (int i = 0; i < 200; ++i) {
        qs.push_back(KeyOf(1 + (i * (n - 1)) / 199));
      }

      const double us = TimeLookups(*sst, qs);
      int hit = 0;
      for (const std::string& k : qs) {
        std::string v;
        if (sst->Get(k, &v).ok()) ++hit;
      }

      if (round == 0) {
        small_us = us;
        small_ok = hit;
      } else {
        big_us = us;
        big_ok = hit;
      }

      char detail[192];
      std::snprintf(detail, sizeof(detail), "%d 条, 文件 %llu KB, 200 次查 %d 命中, %.2f ms",
                    n, static_cast<unsigned long long>(sst->file_size() / 1024), hit,
                    us / 1000.0);
      std::printf("  %s\n", detail);
    }

    if (small_us > 0.0 && big_us > 0.0) {
      const double ratio = big_us / small_us;
      char detail[192];
      std::snprintf(detail, sizeof(detail), "%.3f ms → %.3f ms，%.2f 倍（数据量 8 倍）",
                    small_us / 1000.0, big_us / 1000.0, ratio);
      // ★ 断言反过来了。
      //   没索引时：数据 8 倍 → 耗时 ~8 倍（线性），判 ratio > 3。
      //   有索引后：数据 8 倍 → 耗时基本不变（对数），应该判 ratio < 2。
      //   这一行从「证明它慢」变成「证明它不随数据量线性增长」，
      //   是整个 [6] 段落唯一真正的验收点。
      Check("数据量 8 倍 → 耗时基本不变（不再是线性）", ratio < 2.0, detail);
      std::printf("  ↑ 索引生效的证据：8 倍数据只多几次二分比较，不再多扫 8 倍记录。\n");
    }
    Check("5000 条全命中", small_ok == 200, "200 / 200");
    Check("40000 条全命中", big_ok == 200, "200 / 200");
  }

  // ── [7] 半成品不能装成好文件 ──────────────────────────────
  std::printf("\n[7] 没 Finish 的文件\n");
  {
    const std::string path7 = "build_win/sst_half.sst";
    {
      lsmkv::SSTableBuilder b(path7);  // 出作用域就析构，不调 Finish
      lsmkv::Record r;
      r.key = "only";
      r.value = "one";
      b.Add(r);
    }
    std::unique_ptr<lsmkv::SSTable> half;
    const lsmkv::Status st = lsmkv::SSTable::Open(path7, &half);
    char detail[192];
    std::snprintf(detail, sizeof(detail), "open=%s",
                  st.ok() ? "OK(!!)" : st.ToString().c_str());
    Check("Open 必须失败（不是合法 SSTable）", !st.ok(), detail);
  }

  std::printf("\n─────────────────────────────────────────────\n");
  if (g_failed == 0) {
    std::printf("ALL CHECKS PASSED\n");
  } else {
    std::printf("%d FAILED\n", g_failed);
  }
  return g_failed == 0 ? 0 : 1;
}
