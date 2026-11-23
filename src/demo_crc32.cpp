// ══════════════════════════════════════════════════════════════
//  CRC32 验收（2026-10-26）—— 这个文件你不用改
//
//  跑法（PowerShell）：
//    g++ -std=c++17 -Wall -Wextra -Iinclude src/demo_crc32.cpp -o build_win/demo_crc32.exe; if ($?) { .\build_win\demo_crc32.exe }
//
//  测速度那个数想要好看，加 -O2：
//    g++ -std=c++17 -O2 -Iinclude src/demo_crc32.cpp -o build_win/demo_crc32.exe; if ($?) { .\build_win\demo_crc32.exe }
//
//  三部分：
//    [1] 四个标准向量 —— 算法对不对
//    [2] 改一个字节，Decode 还认不认 —— 校验有没有真的接进格式里
//    [3] 吞吐 —— 一个能拿去讲性能的数字
// ══════════════════════════════════════════════════════════════

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "lsmkv/crc32.h"
#include "lsmkv/record.h"

namespace {

int g_failed = 0;

void Check(const char* label, const char* input, unsigned long expect) {
  const unsigned long got = lsmkv::Crc32(input, std::strlen(input));
  const bool ok = (got == expect);
  std::printf("  %-58s got=0x%08lX  expect=0x%08lX  %s\n", label, got, expect,
              ok ? "PASS" : "FAIL");
  if (!ok) ++g_failed;
}

}  // namespace

int main() {
  // ── [1] 标准向量 ────────────────────────────────────────────
  std::printf("[1] standard test vectors (same values in every CRC-32 impl)\n");
  Check("CRC32(\"\")", "", 0x00000000UL);
  Check("CRC32(\"a\")", "a", 0xE8B7BE43UL);
  Check("CRC32(\"123456789\")", "123456789", 0xCBF43926UL);
  Check("CRC32(\"The quick brown fox jumps over the lazy dog\")",
        "The quick brown fox jumps over the lazy dog", 0x414FA339UL);

  // ── [2] 改一个 bit，Decode 还认不认 ─────────────────────────
  std::printf("\n[2] flip one byte inside the value -- can Decode still say no?\n");

  lsmkv::Record rec;
  rec.type = lsmkv::RecordType::kPut;
  rec.key = "name";
  rec.value = "liupei";
  const std::string bytes = lsmkv::Encode(rec);

  uint32_t stored_crc = 0;
  std::memcpy(&stored_crc, bytes.data(), 4);
  std::printf("  encoded    : %zu bytes (4 CRC + 1 type + 4 klen + key + 4 vlen + value)\n",
              bytes.size());
  std::printf("  stored CRC : 0x%08X\n", stored_crc);

  // 改最后一个字节 —— 它在 value 里面，所有长度字段一个都没动。
  // 也就是说：长度检查这一关完全拦不住它。
  std::string broken = bytes;
  broken[broken.size() - 1] =
      static_cast<char>(broken[broken.size() - 1] ^ 0x01);

  const uint32_t recomputed =
      lsmkv::Crc32(broken.data() + 4, broken.size() - 4);
  std::printf("  flipped the last byte (still inside value), lengths untouched\n");
  std::printf("  recomputed : 0x%08X\n", recomputed);
  std::printf("  CRC differs?  %s\n",
              stored_crc != recomputed ? "YES (fingerprint works)"
                                       : "NO  (something is very wrong)");
  if (stored_crc == recomputed) ++g_failed;

  // 关键一击：Decode 现在能不能拒绝它？
  size_t offset = 0;
  size_t consumed = 0;
  lsmkv::Record out;
  // 10-31：Decode 返回 Status，顺手把"为什么拒绝"打出来
  const lsmkv::Status st = lsmkv::Decode(broken, offset, &out, &consumed);
  const bool decoded = st.ok();
  std::printf("  Decode(broken) -> %s   %s\n", decoded ? "SUCCESS" : "REJECTED",
              decoded ? "FAIL -- corruption went straight through"
                      : "PASS -- the checksum caught it");
  if (!decoded) {
    std::printf("  reason: %s\n", st.ToString().c_str());
  }
  if (decoded) ++g_failed;

  // ── [3] 吞吐 ────────────────────────────────────────────────
  std::printf("\n[3] throughput over 8 MiB\n");

  const size_t kSize = 8u * 1024u * 1024u;
  std::vector<char> buf(kSize, 'A');

  // 先跑一遍热身（让表建好、缓存热起来），再计时
  volatile uint32_t sink = lsmkv::Crc32(buf.data(), buf.size());
  (void)sink;

  double best_ms = 1e9;
  for (int round = 0; round < 5; ++round) {
    const auto t0 = std::chrono::steady_clock::now();
    sink = lsmkv::Crc32(buf.data(), buf.size());
    const auto t1 = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(t1 - t0).count();
    if (ms < best_ms) best_ms = ms;
  }

  const double mb = static_cast<double>(kSize) / (1024.0 * 1024.0);
  if (best_ms < 0.001) {
    std::printf("  8 MiB in <0.001 ms  ->  Crc32 is not really doing work yet\n");
  } else {
    const double mb_per_s = mb / (best_ms / 1000.0);
    std::printf("  8 MiB in %.3f ms  ->  %.0f MB/s\n", best_ms, mb_per_s);
  }
  std::printf(
      "  (with -O2 this should be hundreds of MB/s; without it, lower.\n"
      "   remember this number -- you will need it when someone asks\n"
      "   \"how expensive is checking every record on startup?\")\n");

  std::printf("\n");
  if (g_failed == 0) {
    std::printf("ALL CHECKS PASSED\n");
    return 0;
  }
  std::printf("%d CHECK(S) FAILED\n", g_failed);
  return 1;
}
