// ══════════════════════════════════════════════════════════════
//  验收：数据能不能活过一次重启（2026-11-22）
//
//  跑法（在 D:\code\lsm-kv 下）：
//      .\build.ps1 demo_restart init      ← 第一次：写数据、删一半、退出
//      .\build.ps1 demo_restart           ← 第二次：重新打开，把数据读回来
//
//  ★ 为什么必须跑两次、而且中间进程要真的退出？
//
//    这是唯一能证明"SSTable 真的接上了"的办法。
//    只跑一次的话，数据全在内存里，你测的还是 MemTableSet，
//    SSTable 那部分一行都没被验证过 —— 测试通过，但什么都没证明。
//
//    **验证持久化，就必须让进程死一次。** 内存里活着的内存不能算数。
//
//  ★ 第二次跑为什么一个字节都不用重新写？
//    因为 DB::Open 会从 000001.sst 开始把文件读回来。
//    这个 demo 里 Get 命中的数据，全部来自磁盘，内存表是空的。
//    这就是 Week 6 的全部意义。
// ══════════════════════════════════════════════════════════════

#include <cstdio>
#include <string>

#include "lsmkv/db.h"
#include "lsmkv/status.h"

namespace {

int g_failed = 0;

void Check(const char* label, bool ok, const char* detail) {
  std::printf("  %-46s %s   %s\n", label, ok ? "PASS" : "FAIL", detail);
  if (!ok) ++g_failed;
}

const char* kDir = "build_win/restart_db";
const int kKeys = 8000;

std::string KeyOf(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "k%05d", i);
  return buf;
}

// 96 字节的值：足够把 1MB 的内存表喂满，又不至于让 demo 跑太久。
// 前 6 个字节带着序号，剩下用 '.' 填满 —— 读回来能逐字节比对。
std::string ValOf(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "v%05d", i);
  std::string v(buf);
  v.resize(96, '.');
  return v;
}

bool Muted(int i) { return i % 7 == 0; }  // 每 7 个删一个

// 把上一次留下的文件清掉，保证 init 是"从零开始"
//
// ★ 2026-11-23 补：现在库里多了 .wal，只清 .sst 是不够的。
//   留着旧日志的话，下一次 Open 会把上一轮的历史重放进新库里 ——
//   init 就不再是"从零开始"，而是"接着上一轮继续"。
//   凡是"清空目录"的函数，加了一种新文件就必须回来补一行。
void CleanDir() {
  for (int n = 1; n <= 64; ++n) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s/%06d.sst", kDir, n);
    std::remove(buf);
  }
  std::remove((std::string(kDir) + "/wal.log").c_str());
  std::remove((std::string(kDir) + "/wal.immutable.log").c_str());
}

int RunInit() {
  std::printf("=== phase 1: write, delete, and let the process die ===\n");

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
      delete db;
      return 1;
    }
  }
  std::printf("  wrote %d keys\n", kKeys);

  int muted = 0;
  for (int i = 0; i < kKeys; ++i) {
    if (!Muted(i)) continue;
    st = db->Delete(KeyOf(i));
    if (!st.ok()) {
      std::printf("  Delete(%s) failed: %s\n", KeyOf(i).c_str(), st.ToString().c_str());
      delete db;
      return 1;
    }
    ++muted;
  }
  std::printf("  deleted %d of them (every 7th)\n", muted);

  // ★ 这里 delete db 会走 ~LSMDB，把内存里剩下的表刷成文件。
  //   这一步是"没有 WAL"的代价：不刷，内存里那部分就永远没了。
  //   真实引擎不这么干 —— 它靠 WAL 保证"写进内存就等于不会丢"。
  delete db;
  std::printf("  closed. now run again WITHOUT 'init' to read it back.\n");
  return 0;
}

int RunVerify() {
  std::printf("=== phase 2: fresh process, nothing in memory ===\n");

  lsmkv::DB* db = nullptr;
  lsmkv::Status st = lsmkv::DB::Open(kDir, &db);
  if (!st.ok()) {
    std::printf("  Open failed: %s\n", st.ToString().c_str());
    return 1;
  }

  int got = 0, gone = 0, wrong = 0;
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
        if (wrong <= 3) {
          std::printf("  %s should be GONE but came back as '%s'\n", k.c_str(), v.c_str());
        }
      }
      continue;
    }

    if (!st.ok()) {
      ++wrong;
      if (wrong <= 3) std::printf("  %s should be here but: %s\n", k.c_str(), st.ToString().c_str());
    } else if (v != ValOf(i)) {
      ++wrong;
      if (wrong <= 3) std::printf("  %s has the wrong value\n", k.c_str());
    } else {
      ++got;
    }
  }

  std::printf("  %d alive / %d deleted / %d wrong\n", got, gone, wrong);

  Check("every surviving key came back with its value", got == kKeys - (kKeys + 6) / 7,
        "all read from .sst, memory was empty");
  Check("every deleted key stayed deleted", gone == (kKeys + 6) / 7,
        "tombstones survived the restart -- no resurrect");
  Check("nothing came back wrong", wrong == 0, "0 mismatches");

  // 边界情况：从来没写过的 key
  v.clear();
  Check("a key that never existed -> NotFound", db->Get("zzzzz", &v).IsNotFound(),
        "and it did not accidentally match something");

  delete db;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  const bool init = (argc > 1 && std::string(argv[1]) == "init");
  if (init) {
    CleanDir();
    RunInit();
  } else {
    RunVerify();
  }

  if (g_failed > 0) {
    std::printf("\n%d CHECK(S) FAILED\n", g_failed);
    return 1;
  }
  if (!init) std::printf("\nALL CHECKS PASSED\n");
  return 0;
}
