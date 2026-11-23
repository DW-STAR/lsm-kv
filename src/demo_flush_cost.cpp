// ══════════════════════════════════════════════════════════════
//  刷盘的代价 —— 11-09
//
//  跑法（在 D:\code\lsm-kv 下）：
//    .\build.ps1 demo_flush_cost
//
//  ★ 这个 demo 不测任何"我们的代码"，它测的是【机器】。
//
//    目的只有一个：回答一个问题 ——
//
//        MemTable 写满了，为什么不直接把内容倒进文件、然后清空继续写？
//
//    答案不是"那样不优雅"，是"那样会卡"。卡多久，这里给数字。
//
//  ★ 三个层次，成本差着数量级，这是整个 LSM 设计的物理基础：
//
//      ① 写进用户态缓冲区   —— 纯 memcpy，几个时钟周期
//      ② 写进内核（flush）  —— 一次系统调用，微秒级
//      ③ 真写到盘上（fsync）—— 等硬件，毫秒级（机械盘 5~10ms）
//
//    差三个数量级。所以"什么时候把数据推下去"是存储引擎里
//    最核心的一个决策，没有之一。
// ══════════════════════════════════════════════════════════════

#include <io.h>

#include <chrono>
#include <cstdio>
#include <string>

namespace {

const char* kPath = "build_win/_flush_test.bin";

using Clock = std::chrono::steady_clock;

double MsSince(Clock::time_point t0) {
  const auto d = Clock::now() - t0;
  return std::chrono::duration<double, std::milli>(d).count();
}

// 往文件里写 n 条长度为 rec.size() 的记录。
// flush_each = 每条都 fflush（推给内核）
// commit_each = 每条都 _commit（真写到盘上，等价于 fsync）
double Run(const std::string& rec, size_t n, bool flush_each, bool commit_each) {
  FILE* f = std::fopen(kPath, "wb");
  if (f == nullptr) {
    std::printf("  cannot open %s\n", kPath);
    return -1.0;
  }
  const auto t0 = Clock::now();
  for (size_t i = 0; i < n; ++i) {
    std::fwrite(rec.data(), 1, rec.size(), f);
    if (flush_each) std::fflush(f);
    if (commit_each) _commit(_fileno(f));
  }
  if (!flush_each) std::fflush(f);
  const double ms = MsSince(t0);
  std::fclose(f);
  return ms;
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  const size_t kTotal = 4u << 20;           // 4 MB，和默认 MemTable 容量一致
  const size_t kRecSize = 100;              // 一条记录约 100 字节
  const size_t kRecs = kTotal / kRecSize;   // 约 4.2 万条

  std::printf("total payload : %zu bytes (4 MB)\n", kRecs * kRecSize);
  std::printf("record size   : %zu bytes, %zu records\n\n", kRecSize, kRecs);

  // ── ① 攒成一大块，一次写 ────────────────────────────────────
  const std::string big(kRecs * kRecSize, 'x');
  const double t_block = Run(big, 1, true, false);
  std::printf("[A] one 4MB fwrite + flush          %8.2f ms\n", t_block);

  // ── ② 4 万次小写，靠 stdio 缓冲合并 ─────────────────────────
  const std::string rec(kRecSize, 'x');
  const double t_buf = Run(rec, kRecs, false, false);
  std::printf("[B] %zu fwrites, stdio buffers them %8.2f ms\n", kRecs, t_buf);

  // ── ③ 4 万次小写，每条都 flush（推给内核）───────────────────
  const double t_flush = Run(rec, kRecs, true, false);
  std::printf("[C] %zu fwrites + flush each        %8.2f ms\n", kRecs, t_flush);

  // ── ④ 每条都 flush + fsync（真落盘）────────────────────────
  //    太多条会跑很久，取 2000 条再按比例外推
  const size_t kCommitRecs = 2000;
  const double t_commit = Run(rec, kCommitRecs, true, true);
  const double per_commit_ms = t_commit / static_cast<double>(kCommitRecs);
  const double t_commit_extrapolated = per_commit_ms * static_cast<double>(kRecs);
  std::printf("[D] %zu fwrites + flush + fsync each   %8.2f ms  (%.3f ms each)\n",
              kCommitRecs, t_commit, per_commit_ms);
  std::printf("    -> extrapolated to %zu records:  %8.2f ms  (%.1f s)\n",
              kRecs, t_commit_extrapolated, t_commit_extrapolated / 1000.0);

  // ── 结论 1：缓冲到底省了什么 ───────────────────────────────
  std::printf("\n--- what the buffering buys you ---\n");
  std::printf("  B vs C : %9.1fx   (batching syscalls)\n",
              t_flush > 0.001 ? t_flush / t_buf : 0.0);
  std::printf("  A vs C : %9.1fx   (one big sequential write)\n",
              t_flush > 0.001 ? t_flush / t_block : 0.0);
  std::printf("  D vs B : %9.1fx   (touching the disk every record)\n",
              t_buf > 0.001 ? t_commit_extrapolated / t_buf : 0.0);

  // ── 结论 2：吞吐上限 —— 这才是决定设计的那组数 ─────────────
  //
  //  不要看"总耗时"，看"一秒钟最多能塞进去多少条"。
  const double sync_per_record = 1000.0 / per_commit_ms;   // 每条都 fsync
  const double one_big_flush = static_cast<double>(kRecs) / (t_block + per_commit_ms) * 1000.0;
  std::printf("\n--- throughput ceiling (this is the number that decides the design) ---\n");
  std::printf("  fsync on every record      : %12.0f records/s\n", sync_per_record);
  std::printf("  one fsync per 4MB block    : %12.0f records/s\n", one_big_flush);
  std::printf("  -> %0.0f times apart\n", one_big_flush / sync_per_record);
  std::printf("  your target is 100k QPS. the first line is %.0fx short of it.\n",
              100000.0 / (sync_per_record > 0.5 ? sync_per_record : 0.5));

  // ── 结论 3：阻塞窗口与尾延迟 ───────────────────────────────
  const double window_ms = t_block + per_commit_ms;
  std::printf("\n--- the blocking window ---\n");
  std::printf("  one 4MB flush blocks the writer for about %.1f ms\n", window_ms);
  std::printf("  a normal write takes microseconds -> the tail latency jumps ~3 orders\n");
  const int qps_list[2] = {100000, 1000000};
  for (int i = 0; i < 2; ++i) {
    const double queued = window_ms / 1000.0 * static_cast<double>(qps_list[i]);
    std::printf("    at %8d QPS, %8.0f writes sit behind that single flush\n",
                qps_list[i], queued);
  }

  // ── 结论 4：所以 LSM 怎么分工 ──────────────────────────────
  std::printf("\n--- so the design is ---\n");
  std::printf("  WAL       : flush every record (sequential, no fsync by default)\n");
  std::printf("              -> this is what [C] measures. it is the price of durability\n");
  std::printf("  MemTable  : accumulate in memory, never touch the disk on the write path\n");
  std::printf("  SSTable   : one 4MB sequential write + one fsync, off the critical path\n");
  std::printf("              -> this is what [A] measures. thousands of times fewer syscalls\n");
  std::printf("\n  a frozen memtable is flushed by a BACKGROUND thread, so the\n");
  std::printf("  writer never waits for those %.1f ms.\n", window_ms);
  std::printf("  that is the whole reason Freeze() exists.\n");

  std::remove(kPath);

  std::printf("\n(note: these numbers depend on your disk and file cache.\n");
  std::printf(" run it twice -- the second run is usually faster, that is the\n");
  std::printf(" page cache, not your code improving.)\n");
  return 0;
}
