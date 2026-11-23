// ══════════════════════════════════════════════════════════════
//  MemTable 验收（11-08 建）
//
//  跑法（在 D:\code\lsm-kv 下）：
//    .\build.ps1 demo_memtable
//
//  这个文件不用你改。你要改的是 include/lsmkv/mem_table.h 里的 TODO。
//
//  [1] 转发：Put / Get / Delete 都能用
//  [2] 记账：写 100 条，字节数在合理区间
//  [3] 墓碑不免费：删 1000 个不存在的 key，内存照样涨
//  [4] 覆盖写：节点数不变，但字节数继续涨（只加不减）
//  [5] ShouldFlush：阈值语义（不是公式，是"该不该刷"这件事对不对）
//  [6] sizeof(Node) —— 估算是猜的吗？测一下就知道
//  [7] 与 std::map 对敲：2000 次随机写 / 删 / 查
//  [8] 冻结：只读、拒绝写、但内容和大小都定格、仍可遍历   ← 11-09
//
//  ★ 这个 demo 唯一不做的事：不验具体公式。
//    kMemTableEntryOverhead 取 128 还是 160，结果都该是绿的 ——
//    因为它只是一个估算常数，不是一个必须相等的值。
//    凡是"必须相等"的地方（比如节点数、读出来的值）才配写 ==。
// ══════════════════════════════════════════════════════════════

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "lsmkv/mem_table.h"

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

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // ── [1] 转发 ────────────────────────────────────────────────
  std::printf("[1] Put / Get / Delete still work\n");
  {
    lsmkv::MemTable m;
    Check("Put(\"a\") is OK", m.Put("a", "1").ok(), "forwarded to SkipList");
    std::string v;
    Check("Get(\"a\") -> 1", m.Get("a", &v) && v == "1", "forwarded to SkipList");
    Check("Get(\"zz\") -> miss", !m.Get("zz", &v), "nothing there");
    Check("Delete(\"a\") is OK", m.Delete("a").ok(), "writes a tombstone");
    Check("Get(\"a\") after Delete -> miss", !m.Get("a", &v),
          "the tombstone hides it");
    Check("EntryCount() == 1", m.EntryCount() == 1,
          "a tombstone is still an entry");
  }

  // ── [2] 记账 ────────────────────────────────────────────────
  std::printf("\n[2] does it count bytes at all\n");
  {
    lsmkv::MemTable m;
    for (int i = 0; i < 100; ++i) m.Put(KeyOf(i), "v");

    const size_t u = m.ApproximateMemoryUsage();
    const size_t raw = 100 * (6 + 1);  // 只算 key + value，忽略固定开销
    const size_t cap = 100 * (lsmkv::kMemTableEntryOverhead + 6 + 1);

    std::printf("  100 entries -> %zu bytes (raw payload alone is %zu)\n", u, raw);
    Check("it counts something", u > 0, "not the stub returning 0");
    Check("more than the raw payload", u > raw,
          "a record costs more than its key + value");
    Check("within a sane range", u <= cap,
          "not wildly over-counting");
  }

  // ── [3] 墓碑不免费 ──────────────────────────────────────────
  std::printf("\n[3] tombstones are not free either\n");
  {
    lsmkv::MemTable m;
    const int n = 1000;
    for (int i = 0; i < n; ++i) m.Delete(KeyOf(i));

    const size_t u = m.ApproximateMemoryUsage();
    const size_t floor_bytes = static_cast<size_t>(n) * lsmkv::kMemTableEntryOverhead;

    std::printf("  %d tombstones -> %zu bytes\n", n, u);
    Check("EntryCount() == 1000", m.EntryCount() == static_cast<size_t>(n),
          "every Delete on a missing key still creates a node");
    Check("each tombstone costs at least the overhead", u >= floor_bytes,
          u >= floor_bytes
              ? "value is empty, but the node is not"
              : "you only counted value.size() -- tombstones would OOM you");

    lsmkv::MemTable small(64 * 1024);  // 64KB
    int written = 0;
    while (!small.ShouldFlush() && written < 100000) {
      small.Delete(KeyOf(written));
      ++written;
    }
    char d[96];
    std::snprintf(d, sizeof(d), "%d tombstones filled 64KB", written);
    Check("enough tombstones trigger ShouldFlush", written > 100 && written < 100000, d);
  }

  // ── [4] 覆盖写：只加不减 ────────────────────────────────────
  std::printf("\n[4] overwriting the same keys\n");
  {
    lsmkv::MemTable m;
    for (int i = 0; i < 100; ++i) m.Put(KeyOf(i), std::string(20, 'a'));
    const size_t before = m.ApproximateMemoryUsage();
    const size_t count_before = m.EntryCount();

    for (int i = 0; i < 100; ++i) m.Put(KeyOf(i), std::string(20, 'b'));
    const size_t after = m.ApproximateMemoryUsage();

    std::printf("  %zu -> %zu bytes\n", before, after);
    Check("EntryCount() unchanged", m.EntryCount() == count_before,
          "overwrite, not append");
    Check("byte count keeps growing", after > before,
          "arena never reclaims; bytes_ counts everything ever written");
  }

  // ── [5] ShouldFlush 的语义 ──────────────────────────────────
  std::printf("\n[5] ShouldFlush: threshold semantics\n");
  {
    lsmkv::MemTable m(1000);  // 故意给一个很小的阈值
    const std::string big(100, 'x');

    size_t prev_bytes = 0;
    int n = 0;
    while (!m.ShouldFlush() && n < 10000) {
      prev_bytes = m.ApproximateMemoryUsage();
      m.Put(KeyOf(n), big);
      ++n;
    }
    const bool hit = m.ShouldFlush();

    char d1[96], d2[96], d3[96];
    std::snprintf(d1, sizeof(d1), "after %d puts, %zu bytes vs limit %zu", n,
                  m.ApproximateMemoryUsage(), m.max_bytes());
    std::snprintf(d2, sizeof(d2), "last state before flush: %zu bytes", prev_bytes);
    std::snprintf(d3, sizeof(d3), "%d puts of ~%zu bytes to fill 1000B", n,
                  lsmkv::kMemTableEntryOverhead + 6 + big.size());

    Check("ShouldFlush eventually turns true", hit, d1);
    Check("it was under the limit just before", prev_bytes < m.max_bytes(), d2);
    Check("it did not flush absurdly early", n > 1, d3);
    Check("it did not flush absurdly late", n < 100, d3);
  }

  // ── [6] 估算 vs 现实 ────────────────────────────────────────
  std::printf("\n[6] is the overhead a guess?\n");
  {
    char d[128];
    std::snprintf(d, sizeof(d), "sizeof(Node) = %zu, constant = %zu",
                  sizeof(lsmkv::Node), lsmkv::kMemTableEntryOverhead);
    std::printf("  %s\n", d);
    std::printf(
        "  (the constant deliberately ignores the heap array behind next[] and\n"
        "   leaves some headroom; it only has to be the right order of magnitude)\n");

    // 用一个 200 字节的 value，让固定开销不再淹没 payload，
    // 这样"估算 / 真实负载"这个比值才有意义
    lsmkv::MemTable m;
    const std::string val(200, 'v');
    for (int i = 0; i < 200; ++i) m.Put(KeyOf(i), val);

    const size_t est = m.ApproximateMemoryUsage();
    const size_t payload = 200 * (6 + 200);  // key + value，不算任何开销
    const double ratio = static_cast<double>(est) / static_cast<double>(payload);

    std::printf("  estimate = %zu, payload = %zu, ratio = %.2fx\n", est, payload,
                ratio);
    Check("estimate is between 1x and 2x the payload", ratio >= 1.0 && ratio <= 2.0,
          "an estimate that is off by more than 2x is not an estimate, it is a bug");
  }

  // ── [7] 与 std::map 对敲 ────────────────────────────────────
  std::printf("\n[7] 2000 random ops against std::map\n");
  {
    lsmkv::MemTable m;
    std::map<std::string, std::string> oracle;

    uint32_t s = 0x51ED270Bu;
    auto next = [&s]() {
      s ^= s << 13;
      s ^= s >> 17;
      s ^= s << 5;
      return s;
    };

    int n_put = 0, n_del = 0, n_get = 0, n_bad = 0;
    for (int i = 0; i < 2000; ++i) {
      const std::string key = KeyOf(static_cast<int>(next() % 300));
      const uint32_t op = next() % 3;
      if (op == 0) {
        const std::string val = "v" + std::to_string(i);
        if (!m.Put(key, val).ok()) ++n_bad;
        oracle[key] = val;
        ++n_put;
      } else if (op == 1) {
        if (!m.Delete(key).ok()) ++n_bad;
        oracle.erase(key);
        ++n_del;
      } else {
        std::string v;
        const bool hit = m.Get(key, &v);
        const auto it = oracle.find(key);
        const bool want = (it != oracle.end());
        if (hit != want || (want && v != it->second)) ++n_bad;
        ++n_get;
      }
    }

    char d[128];
    std::snprintf(d, sizeof(d), "%d puts / %d deletes / %d gets, %d bad", n_put,
                  n_del, n_get, n_bad);
    Check("no disagreement", n_bad == 0, d);

    int mismatch = 0;
    for (int i = 0; i < 300; ++i) {
      const std::string key = KeyOf(i);
      std::string v;
      const bool hit = m.Get(key, &v);
      const auto it = oracle.find(key);
      const bool want = (it != oracle.end());
      if (hit != want || (want && v != it->second)) ++mismatch;
    }
    char d2[80];
    std::snprintf(d2, sizeof(d2), "%d of 300 keys disagree", mismatch);
    Check("every key matches the oracle", mismatch == 0, d2);
  }

  // ── [8] 冻结 ────────────────────────────────────────────────
  std::printf("\n[8] freeze: read-only, not dead\n");
  {
    lsmkv::MemTable m;
    m.Put("a", "1");
    m.Put("b", "2");
    m.Delete("b");
    const size_t bytes_before = m.ApproximateMemoryUsage();
    const size_t count_before = m.EntryCount();

    Check("not frozen by default", !m.IsFrozen(), "a fresh table accepts writes");
    m.Freeze();
    Check("IsFrozen() after Freeze()", m.IsFrozen(), "one-way switch");

    const lsmkv::Status sp = m.Put("c", "3");
    Check("Put on a frozen table fails", !sp.ok(), sp.ToString().c_str());
    Check("the failure is InvalidArgument", sp.IsInvalidArgument(),
          "a usage bug, not an environment problem");

    const lsmkv::Status sd = m.Delete("a");
    Check("Delete is rejected by the same gate", !sd.ok() && sd.IsInvalidArgument(),
          "every write path must pass CheckWritable()");

    std::string v;
    Check("Get still works", m.Get("a", &v) && v == "1",
          "frozen means read-only, not dead");
    Check("tombstones stay hidden", !m.Get("b", &v),
          "the frozen table keeps its own semantics");
    Check("EntryCount() did not change", m.EntryCount() == count_before,
          "a rejected write must not sneak a node in");
    Check("byte count did not change", m.ApproximateMemoryUsage() == bytes_before,
          "the size of a frozen table is fixed, by definition");

    // 冻结后仍然可以把它整份读出来 —— 后台落盘就靠这个
    size_t seen = 0;
    for (int i = 0; i < 2; ++i) {
      const std::string k = (i == 0) ? "a" : "b";
      if (m.tree().DebugFind(k) != nullptr) ++seen;
    }
    Check("the frozen table is still fully traversable", seen == 2,
          "this is what the background flush thread will walk");
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
      "  [1] Put/Get broken   -> Put is still the stub `return Status::OK();`\n"
      "  [2] counts nothing   -> ApproximateMemoryUsage() still returns 0\n"
      "  [3] only value.size() -> count kMemTableEntryOverhead too; a tombstone\n"
      "                          with an empty value still costs ~128 bytes\n"
      "  [4] count went down  -> bytes_ must only grow; never subtract on overwrite\n"
      "  [5] never flushes    -> ShouldFlush() must be `usage >= max_bytes_`\n"
      "  [6] ratio out of range -> check the formula: overhead + key + value\n"
      "  [7] disagreement     -> MemTable must forward to the same tree you tested\n"
      "  [8] write succeeded  -> Put/Delete do not call CheckWritable() at the top\n"
      "  [8] wrong error code -> return Status::InvalidArgument, not IOError\n");
  return 1;
}
