// ══════════════════════════════════════════════════════════════
//  迭代器验收（11-15 建）
//
//  跑法（在 D:\code\lsm-kv 下）：
//    .\build.ps1 titer
//
//  [1] TODO 9：Begin + Next 能按 key 升序走完全表，条数一致
//  [2] TODO 13：Seek 的七种情况
//
//  ★ [2] 现在会是红的（Seek 还是 `return Begin()`）。
//    这不是失败，是 TODO 13 的清单。
// ══════════════════════════════════════════════════════════════

#include <cstdio>
#include <string>
#include <vector>

#include "lsmkv/skip_list.h"

namespace {

int g_failed = 0;

void Eq(const char* what, const std::string& got, const std::string& want) {
  const bool ok = (got == want);
  std::printf("  %-34s %s   got='%s' want='%s'\n", what, ok ? "PASS" : "FAIL",
              got.c_str(), want.c_str());
  if (!ok) ++g_failed;
}

void Check(const char* label, bool ok, const char* detail) {
  std::printf("  %-34s %s   %s\n", label, ok ? "PASS" : "FAIL", detail);
  if (!ok) ++g_failed;
}

}  // namespace

int main() {
  using lsmkv::SkipList;

  std::printf("=== 迭代器验收 ===\n\n");

  // ── [1] TODO 9：顺序遍历 ──────────────────────────────────
  std::printf("[1] Begin + Next 走完全表\n");
  {
    SkipList t;
    for (const char* k : {"m", "c", "x", "a", "q"}) t.Insert(k, std::string("v-") + k);

    std::vector<std::string> got;
    size_t guard = 0;
    for (SkipList::Iterator it = t.Begin(); it.Valid(); it.Next()) {
      got.push_back(it.key());
      if (++guard > t.Size() + 8) break;  // Next() 没动就别挂了
    }

    std::string joined;
    for (const std::string& s : got) joined += s + " ";
    Eq("按 key 升序", joined, "a c m q x ");
    Check("条数 == Size()", got.size() == t.Size(), "5 条");
    Check("value 跟着一起出来",
          [&] {
            for (SkipList::Iterator it = t.Begin(); it.Valid(); it.Next()) {
              if (it.key() == "q") return it.value() == "v-q";
            }
            return false;
          }(),
          "q 的 value 是 v-q");
  }

  // ── [2] TODO 13：Seek ─────────────────────────────────────
  std::printf("\n[2] Seek：第一个 key >= target\n");
  {
    SkipList t;
    for (const char* k : {"a", "c", "e", "g"}) t.Insert(k, "1");

    auto at = [&](const char* key) {
      SkipList::Iterator it = t.Seek(key);
      return it.Valid() ? it.key() : std::string("<end>");
    };

    Eq("Seek(a) 正好命中最小", at("a"), "a");
    Eq("Seek(b) 落到第一个更大的", at("b"), "c");
    Eq("Seek(c) 正好命中", at("c"), "c");
    Eq("Seek(f) 落到 g", at("f"), "g");
    Eq("Seek(z) 走到头", at("z"), "<end>");
    Eq("Seek(\"\") 从头开始", at(""), "a");

    SkipList empty;
    Eq("空表 Seek 走不到东西", empty.Seek("a").Valid() ? "valid" : "<end>", "<end>");
  }

  // ── [3] 墓碑也在遍历里（顺序是从 L0 走的，不屏蔽任何东西）──
  std::printf("\n[3] 墓碑也会被倒出来\n");
  {
    SkipList t;
    t.Insert("a", "1");
    t.Insert("b", "2");
    t.Delete("b");

    int seen = 0;
    int deleted = 0;
    for (SkipList::Iterator it = t.Begin(); it.Valid(); it.Next()) {
      ++seen;
      if (it.type() == lsmkv::RecordType::kDelete) ++deleted;
    }
    Check("2 条都在（含 1 条墓碑）", seen == 2 && deleted == 1, "seen=2, deleted=1");
    std::printf("  ↑ 落盘必须保留墓碑，否则老 SSTable 里的旧值会复活\n");
  }

  std::printf("\n─────────────────────────────────────────────\n");
  if (g_failed == 0) {
    std::printf("ALL CHECKS PASSED\n");
  } else {
    std::printf("%d FAILED\n", g_failed);
  }
  return g_failed == 0 ? 0 : 1;
}
