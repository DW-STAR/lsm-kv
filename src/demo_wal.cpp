// ══════════════════════════════════════════════════════════════
//  10-25 的验收程序：WAL 双写
//
//  这个文件**不需要你改**。它只负责把「你说的对不对」变成四个数。
//
//  跑法（PowerShell）：
//    g++ -std=c++17 -Iinclude src/demo_wal.cpp -o build_win/demo_wal.exe; if ($?) { .\build_win\demo_wal.exe }
//
//  四个检查：
//    ① size() == 4                      内存里四条
//    ② 文件字节数 == 四条编码之和        字节真的落盘了（且一条不多一条不少）
//    ③ 新对象 Load 后 size() == 4        重启能重建
//    ④ Get 内容对                         不只是条数对，内容也对
//
//  ⚠️ ② 失败但 ③ 通过，是很常见的一种组合，别当成"其实没事"：
//     ② 是在 a 还活着的时候查的 —— 它测的是「数据到内核了没有」（flush）。
//     ③ 之前 a 析构了，析构函数会替你 flush 一次 —— 它测的是「Load 逻辑对不对」。
//     现实中进程是被 kill -9 掉的，**没有析构函数帮你**。
//     所以 ② 才是 WAL 的真考题。
// ══════════════════════════════════════════════════════════════

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "lsmkv/append_only_log.h"
#include "lsmkv/record.h"

namespace {

const char* const kPath = "wal_demo.log";

long FileSize(const char* path) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in.is_open()) return -1;
  return static_cast<long>(in.tellg());
}

void Line() { std::cout << "--------------------------------------------------\n"; }

}  // namespace

int main() {
  // 每次从干净的盘开始，否则上一轮的记录会让 ② 对不上。
  std::remove(kPath);

  int failed = 0;

  // ── 阶段 1：写 ─────────────────────────────────────────────
  std::cout << "[1] write 4 records through the WAL\n";
  {
    lsmkv::AppendOnlyLog a(kPath);
    lsmkv::Status st = a.Open();
    if (!st.ok()) {
      std::cout << "    Open failed: " << st.ToString() << "\n";
      return 1;
    }

    const lsmkv::Status s1 = a.Append("name", "liupei");
    const lsmkv::Status s2 = a.Append("score", "130");
    const lsmkv::Status s3 = a.Append("name", "liupei_v2");
    const lsmkv::Status s4 = a.Delete("score");

    if (!s1.ok() || !s2.ok() || !s3.ok() || !s4.ok()) {
      std::cout << "    some Append/Delete returned an error\n";
      std::cout << "      s1=" << s1.ToString() << "  s2=" << s2.ToString()
                << "  s3=" << s3.ToString() << "  s4=" << s4.ToString() << "\n";
      return 1;
    }
    std::cout << "    all four calls returned OK\n";

    // ① 内存条数
    Line();
    const size_t mem = a.size();
    std::cout << "[2] check 1: memory count\n";
    std::cout << "    size() = " << mem << "   expect 4   "
              << (mem == 4 ? "PASS" : "FAIL") << "\n";
    if (mem != 4) ++failed;

    // ② 文件字节数（此刻 a 还活着，没有 Close —— 这正是"进程崩掉"的瞬间）
    Line();
    std::cout << "[3] check 2: bytes on disk (a is STILL OPEN, not closed)\n";

    const std::vector<lsmkv::Record> expect_records = {
        {lsmkv::RecordType::kPut, "name", "liupei"},
        {lsmkv::RecordType::kPut, "score", "130"},
        {lsmkv::RecordType::kPut, "name", "liupei_v2"},
        {lsmkv::RecordType::kDelete, "score", ""},
    };
    long expect_bytes = 0;
    for (const auto& r : expect_records) {
      const long n = static_cast<long>(lsmkv::Encode(r).size());
      expect_bytes += n;
      std::cout << "    " << (r.type == lsmkv::RecordType::kDelete ? "Delete" : "Put   ")
                << " key=" << r.key << "  ->  " << n << " bytes\n";
    }
    const long actual = FileSize(kPath);
    std::cout << "    expect total = " << expect_bytes << "\n";
    std::cout << "    actual file  = " << actual << "\n";
    if (actual == expect_bytes) {
      std::cout << "    PASS\n";
    } else {
      std::cout << "    FAIL  (difference = " << (expect_bytes - actual)
                << " bytes)\n";
      ++failed;
    }
  }  // <-- a 在这里析构，析构函数会 flush + close（真实崩溃没有这一步）

  // ── 阶段 2：模拟重启 ────────────────────────────────────────
  Line();
  std::cout << "[4] check 3: fresh object, Load the same file\n";

  lsmkv::AppendOnlyLog b(kPath);
  lsmkv::Status lst = b.Load();
  std::cout << "    Load status = " << lst.ToString() << "\n";
  const size_t loaded = b.size();
  std::cout << "    size() = " << loaded << "   expect 4   "
            << (loaded == 4 ? "PASS" : "FAIL") << "\n";
  if (!lst.ok() || loaded != 4) ++failed;

  // ── 阶段 3：内容正确性 ─────────────────────────────────────
  Line();
  std::cout << "[5] check 4: content after reload\n";

  std::string v;
  const bool got_name = b.Get("name", &v);
  const bool name_ok = got_name && v == "liupei_v2";
  std::cout << "    Get(\"name\")  -> " << (got_name ? v : "(not found)")
            << "   expect liupei_v2 (not liupei)   " << (name_ok ? "PASS" : "FAIL")
            << "\n";
  if (!name_ok) ++failed;

  v.clear();
  const bool got_score = b.Get("score", &v);
  const bool score_ok = !got_score;
  std::cout << "    Get(\"score\") -> " << (got_score ? v : "(not found)")
            << "   expect not found (tombstone)   " << (score_ok ? "PASS" : "FAIL")
            << "\n";
  if (!score_ok) ++failed;

  // 顺手把盘上的东西摊开给你看，跟 [3] 的每行对上
  Line();
  std::cout << "[6] what is actually inside the file\n";
  for (size_t i = 0; i < b.entries().size(); ++i) {
    const auto& r = b.entries()[i];
    std::cout << "    #" << i << " "
              << (r.type == lsmkv::RecordType::kDelete ? "Delete" : "Put   ")
              << " key=\"" << r.key << "\" value=\"" << r.value << "\"\n";
  }

  Line();
  if (failed == 0) {
    std::cout << "ALL 4 CHECKS PASSED\n";
    return 0;
  }
  std::cout << failed << " CHECK(S) FAILED\n";
  return 1;
}
