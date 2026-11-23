// 最朴素的单元测试：不引第三方库，手写断言。
//
// 为什么不用 gtest？
//   这个项目的重点是"理解存储引擎"，不是"学习测试框架"。
//   等到第 4 周代码量上来了，再考虑换 gtest 也不迟。
//
// 现在的测试只覆盖内存版的语义。后面每加一层（WAL / SSTable / Compaction），
// 都应该在这里补上对应的用例 —— 尤其是"断电恢复"这种，
// 不写测试你根本不知道自己对不对。

#include <iostream>
#include <string>

#include "lsmkv/db.h"

using namespace lsmkv;

static int g_failed = 0;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      std::cerr << "  FAILED: " << #cond << "  (" << __FILE__ << ":"  \
                << __LINE__ << ")\n";                                 \
      ++g_failed;                                                     \
    }                                                                 \
  } while (0)

int main() {
  DB* db = nullptr;
  CHECK(DB::Open("./test_data", &db).ok());
  CHECK(db != nullptr);

  std::string v;

  // 1. 写入后能读回
  CHECK(db->Put("k1", "v1").ok());
  CHECK(db->Get("k1", &v).ok());
  CHECK(v == "v1");

  // 2. 不存在的 key 要返回 NotFound
  CHECK(db->Get("nope", &v).IsNotFound());

  // 3. 覆盖写：后写的赢
  CHECK(db->Put("k1", "v2").ok());
  CHECK(db->Get("k1", &v).ok());
  CHECK(v == "v2");

  // 4. 删除之后读不到
  CHECK(db->Delete("k1").ok());
  CHECK(db->Get("k1", &v).IsNotFound());

  // 5. 空 key / 空 value 不应该崩
  CHECK(db->Put("", "").ok());
  CHECK(db->Get("", &v).ok());
  CHECK(v == "");

  // 6. 二进制安全：value 里可以有 '\0'
  const std::string bin("a\0b", 3);
  CHECK(db->Put("bin", bin).ok());
  CHECK(db->Get("bin", &v).ok());
  CHECK(v.size() == 3);

  delete db;

  if (g_failed == 0) {
    std::cout << "all tests passed\n";
    return 0;
  }
  std::cerr << g_failed << " test(s) failed\n";
  return 1;
}
