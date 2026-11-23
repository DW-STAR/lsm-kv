// ══════════════════════════════════════════════════════════════
//  崩溃 / 损坏恢复 验收（2026-10-31）
//
//  跑法（在 D:\code\lsm-kv 下，PowerShell）：
//    g++ -std=c++17 -Wall -Wextra -Iinclude src/demo_corrupt.cpp -o build_win/demo_corrupt.exe; if ($?) { .\build_win\demo_corrupt.exe }
//
//  ★ 四组 case，期望值**各不相同** —— 这才是今天的考点。
//
//    1. 完好文件                → OK          加载 3 条
//    2. 尾部砍 5 字节           → OK          加载 2 条，truncated_bytes() == 12
//    3. 中间改 1 个 bit         → Corruption  加载 1 条，拒绝继续
//    4. 长度字段改成天文数字    → Corruption  加载 1 条（被上限检查抓住）
//
//  ★ 2 和 3 的区别，就是今天全部的工作：
//
//    2 是"进程崩在写操作中间"—— WAL 的日常，那半条记录从未被承诺过，正常。
//    3 是"数据内容变了"—— 绝不正常，后面全部不可信。
//
//    上一版代码这两种都走 else break，于是 3 会安静地丢掉后面所有数据。
//    现在一个返回 OK，一个返回 Corruption。
//
//  ★ case 2 有个必须看懂的细节：
//
//    被砍掉的第 3 条是 Delete("name")。丢掉它之后 Get("name") 会返回
//    "liupei" —— 被删的 key 回来了。**这不是 bug。**
//    那条 Delete 从来没有写成功过，客户端也没收到"删除成功"。
//    "已提交的写不丢"才是 WAL 的承诺，不是"所有发出过的写都不丢"。
//    但 case 3 里同样的"删除消失"就是 bug —— 因为那条 Delete 是写完的，
//    是磁盘把它改坏了。**同样现象，两种成因，两种处理。**
// ══════════════════════════════════════════════════════════════

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "lsmkv/append_only_log.h"
#include "lsmkv/record.h"
#include "lsmkv/status.h"

namespace {

int g_failed = 0;

void Check(const char* label, bool ok, const std::string& detail) {
  std::printf("  %-46s %s   %s\n", label, ok ? "PASS" : "FAIL",
              detail.c_str());
  if (!ok) ++g_failed;
}

bool WriteFile(const std::string& path, const std::string& data) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f.is_open()) return false;
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  f.close();
  return true;
}

// 造一份"三条记录"的日志，顺便记下每条记录的起始偏移（末尾多一个哨兵）。
struct Built {
  std::string bytes;
  std::vector<size_t> starts;
};

Built BuildLog() {
  Built b;
  const lsmkv::Record records[3] = {
      lsmkv::Record{lsmkv::RecordType::kPut, "name", "liupei"},
      lsmkv::Record{lsmkv::RecordType::kPut, "score", "100"},
      lsmkv::Record{lsmkv::RecordType::kDelete, "name", ""},
  };
  for (const auto& r : records) {
    b.starts.push_back(b.bytes.size());
    b.bytes += lsmkv::Encode(r);
  }
  b.starts.push_back(b.bytes.size());  // 哨兵 = 文件总长
  return b;
}

// 把一次 Load 的结果打出来，并做断言。
// expect_ok / expect_count / expect_trunc 是"期望值"。
void RunCase(const char* title, const std::string& path, bool expect_ok,
             size_t expect_count, size_t expect_trunc) {
  std::printf("\n=== %s ===\n", title);

  lsmkv::AppendOnlyLog log(path);
  const lsmkv::Status st = log.Load();

  std::printf("  status          : %s\n", st.ToString().c_str());
  std::printf("  loaded          : %zu record(s)\n", log.size());
  std::printf("  truncated_bytes : %zu\n", log.truncated_bytes());

  Check("status matches expectation", st.ok() == expect_ok,
        expect_ok ? "expected OK" : "expected Corruption");
  Check("record count matches", log.size() == expect_count,
        "expected " + std::to_string(expect_count));
  Check("truncated_bytes matches", log.truncated_bytes() == expect_trunc,
        "expected " + std::to_string(expect_trunc));

  if (st.ok()) {
    std::string v;
    if (log.Get("name", &v)) {
      std::printf("  Get(\"name\")     : %s\n", v.c_str());
    } else {
      std::printf("  Get(\"name\")     : NOT FOUND\n");
    }
  }
}

}  // namespace

int main() {
  const std::string kPath = "corrupt_test.log";
  const Built built = BuildLog();

  std::printf("three records, total %zu bytes, starts at", built.bytes.size());
  for (size_t i = 0; i + 1 < built.starts.size(); ++i) {
    std::printf(" %zu", built.starts[i]);
  }
  std::printf("\n");

  // ── case 1：完好 ────────────────────────────────────────────
  WriteFile(kPath, built.bytes);
  RunCase("case 1: intact file", kPath, /*ok=*/true, /*count=*/3,
          /*trunc=*/0);

  // ── case 2：砍掉尾部 5 字节（模拟 kill -9 打断写） ──────────
  // 第 3 条是 17 字节的 Delete，砍完剩 12 字节 —— 一条读不出来的残片。
  {
    std::string cut = built.bytes;
    cut.resize(cut.size() - 5);
    WriteFile(kPath, cut);
  }
  RunCase("case 2: tail truncated by 5 bytes (crash mid-write)", kPath,
          /*ok=*/true, /*count=*/2, /*trunc=*/12);

  // ── case 3：中间某条的内容被改 1 个 bit ─────────────────────
  // 改第 2 条的最后一个字节（在 value 里），所有长度字段都没动。
  // 也就是说：结构完全合法，长度检查一关都拦不住 —— 只有 CRC 能抓。
  {
    std::string bad = built.bytes;
    const size_t last_byte_of_2nd = built.starts[2] - 1;
    bad[last_byte_of_2nd] =
        static_cast<char>(bad[last_byte_of_2nd] ^ 0x01);
    WriteFile(kPath, bad);
  }
  RunCase("case 3: one bit flipped inside record #2's value", kPath,
          /*ok=*/false, /*count=*/1, /*trunc=*/0);

  // ── case 4：长度字段被改成天文数字 ───────────────────────────
  // 布局：offset+0..3 是 CRC，offset+4 是 type，offset+5..8 是 klen。
  // 把第 2 条的 klen 改成 0x7FFFFFFF。
  // 没有上限检查的话，这里会被当成"尾部残片"安静丢掉（盲区）。
  {
    std::string bad = built.bytes;
    const size_t klen_pos = built.starts[1] + 5;
    const uint32_t bogus = 0x7FFFFFFFu;
    std::memcpy(&bad[klen_pos], &bogus, 4);
    WriteFile(kPath, bad);
  }
  RunCase("case 4: key length overwritten with 0x7FFFFFFF", kPath,
          /*ok=*/false, /*count=*/1, /*trunc=*/0);

  std::printf("\n");
  if (g_failed == 0) {
    std::printf("ALL CHECKS PASSED\n");
    return 0;
  }
  std::printf("%d CHECK(S) FAILED\n", g_failed);
  return 1;
}
