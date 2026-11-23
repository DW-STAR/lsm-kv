// ══════════════════════════════════════════════════════════════
//  tests/test_skip.cpp —— 你的第一个测试文件（11-07 写）
//
//  这个文件由你写。
//  目标不是「证明代码是对的」，是【让错误实现活不下去】。
//
//  ── 跑法 ──────────────────────────────────────────────────
//    .\build.ps1 tskip      对你的实现跑一遍（应该全绿）
//    .\build.ps1 mutants    拿你的测试去跑 6 个【故意写错】的实现
//
//  验收标准：mutants 跑出 6 / 6。
//  漏一个 = 有一整类 bug 能穿过你的测试活下来，上线后就是线上事故。
//
//  ── 靶子 ──────────────────────────────────────────────────
//  只告诉你「改的是哪个函数」，不给具体改法。你得自己想它可能怎么错：
//
//      m1  Insert
//      m2  RandomLevel
//      m3  Insert
//      m4  Insert
//      m5  Delete
//      m6  Get
//
//  ★ 一条提示（不是答案）：
//    m1 / m2 / m3 这三种，Get / Insert / Delete 的【行为全都是对的】。
//    你只测「存进去能不能读出来」，一条都抓不住。
//    想抓它们，得用上看结构的工具：Level() / LevelKeys() / CheckInvariants()。
//
//  ── 可用的工具（SkipList 的公开接口）──────────────────────
//    Insert(key, value[, type])  写；同 key 覆盖
//    Delete(key)                 删；写墓碑，不摘节点
//    Get(key, &value, &type)     读；墓碑视为不存在；出参可传 nullptr
//    Size()                      物理记录数（含墓碑）
//    Level()                     当前实际最高层
//    LevelKeys(lv)               导出第 lv 层的 key 序列（lv=0 是全部）
//    DebugFind(key)              沿 L0 找，返回整条记录（含墓碑），未找到返回 nullptr
//    CheckInvariants()           三条不变量，返回空串表示全对，否则返回第一处违例
//    RandomLevel()               抛一次硬币（1/2 概率升一层）
//
//  ★ 别改 main、别改 CHECK 宏。只写 Case01 ~ Case06 的函数体。
// ══════════════════════════════════════════════════════════════

#include <cstdio>
#include <string>
#include <vector>

#include "lsmkv/skip_list.h"

namespace {

int g_failed = 0;
int g_checks = 0;
const char* g_case = "";

#define CHECK(cond)                                        \
  do {                                                     \
    ++g_checks;                                            \
    if (!(cond)) {                                         \
      std::printf("  FAIL  [%s]  %s\n", g_case, #cond);    \
      ++g_failed;                                          \
    }                                                      \
  } while (0)

// ── 六个 case，一个抓一类 bug ──────────────────────────────

void Case01() {
  g_case = "Case01";
  lsmkv::SkipList list;
  for(int i = 0;i < 100;++ i){
    list.Insert(std::to_string(1000 + i),"LJ");
  }
  CHECK(list.CheckInvariants().empty());

  for(int lv = 0;lv < list.Level();++ lv){
    std::vector<std::string> expected;
    for(int i = 0;i < 100;++ i){
      const lsmkv::Node* found = list.DebugFind(std::to_string(1000 + i));
      if(found != nullptr && found->next.size() > lv){
        expected.push_back(std::to_string(1000 + i));
      }
    }
    CHECK(!expected.empty());
    CHECK(list.LevelKeys(lv) == expected);
  }
}

void Case02() {
  g_case = "Case02";
  bool level_check = true;
  lsmkv::SkipList list;
  for(int i = 0;i < 100;++ i){
    int lev = list.RandomLevel();
    if(lev < 1 || lev > lsmkv::kMaxLevel){
      level_check = false;
    }
    CHECK(level_check);
  }
}

void Case03() {
  g_case = "Case03";
}

void Case04() {
  g_case = "Case04";
}

void Case05() {
  g_case = "Case05";
}

void Case06() {
  g_case = "Case06";
}

}  // namespace

int main() {
  Case01();
  Case02();
  Case03();
  Case04();
  Case05();
  Case06();

  if (g_failed > 0) {
    std::printf("%d check(s) failed (out of %d)\n", g_failed, g_checks);
    return 1;
  }
  if (g_checks == 0) {
    std::printf("0 checks ran -- Case01..Case06 are still empty.\n");
    std::printf("go write them, then run: .\\build.ps1 mutants\n");
    return 0;
  }
  std::printf("all checks passed (%d assertions)\n", g_checks);
  return 0;
}
