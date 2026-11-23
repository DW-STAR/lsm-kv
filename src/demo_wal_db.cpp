// ══════════════════════════════════════════════════════════════
//  验收：WAL 到底有没有兜住数据（2026-11-23）
//
//  跑法（在 D:\code\lsm-kv 下）：
//      .\build.ps1 demo_wal_db crash    ← 写数据，然后**跳过所有析构**直接退出
//      .\build.ps1 demo_wal_db          ← 新进程打开，把数据读回来
//
//  ── 这个 demo 和 demo_restart 的区别 ────────────────────────
//
//  demo_restart 是 `delete db` 正常关闭：析构函数跑了。
//  这个 demo 是 `std::_Exit()`：**一个析构函数都不跑**，文件不关、缓冲不刷、
//  栈上对象全部悬空 —— 它模拟的是 kill -9 / 断电 / 异常栈被打断。
//
//  ★ 为什么必须这么测？
//    因为"正常关闭后数据还在"这件事，靠的是"我调了 delete db"。
//    而真实世界里，进程死掉的方式大多数时候轮不到你调 delete。
//    **一个只在优雅退出时成立的安全性，不叫安全性。**
//
//  ★ 为什么这里只写 100 条，故意不触发刷盘？
//    因为一旦触发了刷盘，数据就进了 .sst，"数据还在"就可能来自 sst，
//    证明不了 WAL 有没有用。
//    100 条 × 约 230 字节 ≈ 23KB，远小于 1MB 的阈值 → 一张 .sst 都不会生成。
//    所以下面 Verify 阶段读出来的每一个字节，都只能来自 WAL 重放。
// ══════════════════════════════════════════════════════════════

#include <cstdio>
#include <cstdlib>
#include <string>

#include "lsmkv/db.h"
#include "lsmkv/status.h"

namespace {

int g_failed = 0;

void Check(const char* label, bool ok, const char* detail) {
  std::printf("  %-46s %s   %s\n", label, ok ? "PASS" : "FAIL", detail);
  if (!ok) ++g_failed;
}

const char* kDir = "build_win/wal_db";
const int kKeys = 100;

std::string KeyOf(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%04d", i);
  return buf;
}

std::string ValOf(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "v%04d", i);
  std::string v(buf);
  v.resize(64, '.');  // 撑到 64 字节，让 100 条也不至于小得离谱
  return v;
}

// 每 10 个删一个 = 10 条墓碑，90 条存活
bool Muted(int i) { return i % 10 == 0; }
const int kMuted = 10;

bool FileExists(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  std::fclose(f);
  return true;
}

void CleanDir() {
  for (int n = 1; n <= 64; ++n) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s/%06d.sst", kDir, n);
    std::remove(buf);
  }
  std::remove((std::string(kDir) + "/wal.log").c_str());
  std::remove((std::string(kDir) + "/wal.immutable.log").c_str());
}

int RunCrash() {
  std::printf("=== phase 1: write, then die without running any destructor ===\n");

  lsmkv::DB* db = nullptr;
  lsmkv::Status st = lsmkv::DB::Open(kDir, &db);
  if (!st.ok()) {
    std::printf("  Open failed: %s\n", st.ToString().c_str());
    return 1;
  }

  for (int i = 0; i < kKeys; ++i) {
    st = db->Put(KeyOf(i), ValOf(i));
    if (!st.ok()) {
      std::printf("  Put(%s) failed: %s\n", KeyOf(i).c_str(), st.ToString().c_str());
      std::_Exit(1);
    }
  }
  for (int i = 0; i < kKeys; ++i) {
    if (!Muted(i)) continue;
    st = db->Delete(KeyOf(i));
    if (!st.ok()) {
      std::printf("  Delete(%s) failed: %s\n", KeyOf(i).c_str(), st.ToString().c_str());
      std::_Exit(1);
    }
  }

  std::printf("  wrote %d keys, deleted %d\n", kKeys, kMuted);
  std::printf("  now calling std::_Exit(0) -- ~LSMDB never runs\n");

  // ★ 不 delete db，直接走 _Exit：
  //   析构函数一个都不跑，ofstream 不 close，栈上对象全部不清理。
  //   这正是 kill -9 的样子。
  std::fflush(stdout);
  std::_Exit(0);
}

int RunVerify() {
  std::printf("=== phase 2: new process, nothing in memory ===\n");

  // 先确认这一轮真的没有 .sst —— 否则下面所有 PASS 都不算数
  const bool has_sst = FileExists(std::string(kDir) + "/000001.sst");
  const bool has_wal = FileExists(std::string(kDir) + "/wal.log");
  std::printf("  on disk: 000001.sst = %s, wal.log = %s\n",
              has_sst ? "yes" : "no", has_wal ? "yes" : "no");

  lsmkv::DB* db = nullptr;
  lsmkv::Status st = lsmkv::DB::Open(kDir, &db);
  if (!st.ok()) {
    std::printf("  Open failed: %s\n", st.ToString().c_str());
    return 1;
  }

  int alive = 0, gone = 0, wrong = 0;
  std::string v;
  for (int i = 0; i < kKeys; ++i) {
    const std::string k = KeyOf(i);
    v.clear();
    st = db->Get(k, &v);

    if (Muted(i)) {
      if (st.IsNotFound()) {
        ++gone;
      } else {
        ++wrong;
        if (wrong <= 3) std::printf("  %s should be GONE but came back\n", k.c_str());
      }
      continue;
    }

    if (!st.ok() || v != ValOf(i)) {
      ++wrong;
      if (wrong <= 3) std::printf("  %s should be here and correct\n", k.c_str());
    } else {
      ++alive;
    }
  }

  std::printf("  %d alive / %d deleted / %d wrong\n", alive, gone, wrong);

  Check("no .sst exists (so nothing could come from disk)", !has_sst,
        "everything below must have come from the WAL");
  Check("the WAL file is there", has_wal, "wal.log survived the crash");
  Check("all 90 surviving keys came back", alive == kKeys - kMuted,
        "byte-for-byte, after a crash with no destructor");
  Check("all 10 tombstones still took effect", gone == kMuted,
        "deletes were logged too, not just puts");
  Check("nothing came back wrong", wrong == 0, "0 mismatches");

  delete db;  // 这一次正常关闭，不影响上面的结论
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  const bool crash = (argc > 1 && std::string(argv[1]) == "crash");
  if (crash) {
    CleanDir();
    RunCrash();  // 这个函数不会返回
  } else {
    RunVerify();
  }

  if (g_failed > 0) {
    std::printf("\n%d CHECK(S) FAILED\n", g_failed);
    return 1;
  }
  std::printf("\nALL CHECKS PASSED\n");
  return 0;
}
