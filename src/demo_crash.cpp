// ══════════════════════════════════════════════════════════════
//  崩溃现场模拟（2026-11-01）
//
//  为什么需要这个文件？
//
//    上周的 demo_corrupt 是拿字符串 resize 造出来的残片 —— 那是"伪造现场"。
//    今天要做的，是让进程**真的死在写操作中间**，再看引擎能不能自己爬起来。
//
//    在 Windows 上没法按电源键，但我们有更狠的工具：std::_Exit(0)。
//
//  ★ _Exit(0) 和 return 0 / exit(0) 有什么区别？
//
//      return 0   → 正常退出：局部对象析构 → ofstream 析构时 flush → 数据出去
//      exit(0)    → 正常退出：同上（还会跑 atexit 注册的函数）
//      _Exit(0)   → **立刻死**：不跑析构，不 flush 缓冲区，不跑 atexit
//
//    _Exit 之后进程没了，但**操作系统还活着**，内核页缓存里的数据当然也在。
//    这正是 kill -9 的样子（kill -9 也是"不给你收尾的机会"）。
//
//  ★ 用法（在 D:\code\lsm-kv 下）
//
//    .\build_win\demo_crash.exe dirty
//        写一条记录进 ofstream 的缓冲区，**不 flush**，然后 _Exit
//        → 缓冲区跟着进程一起蒸发
//
//    .\build_win\demo_crash.exe clean
//        写一条记录，**flush**，然后 _Exit
//        → 数据已经交给内核，进程死也不影响
//
//    .\build_win\demo_crash.exe size
//        只看文件现在多大
//
//    .\build_win\demo_crash.exe bulk 1000 347
//        用真正的 AppendOnlyLog 写 1000 条，写到第 347 条**之前** _Exit
//
//    .\build_win\demo_crash.exe halflast 1000 500
//        写 500 条完整的，第 500 条只写一半就 _Exit
//        → 制造一条真实的残记录（比 demo_corrupt 手工 resize 真实）
//
//    .\build_win\demo_crash.exe verify
//        Load 一遍，报告：状态 / 条数 / 截断字节 / 前缀是否完整
//
//  ★ 今晚要验证的两件事
//
//    ① dirty 之后文件是 0 字节，clean 之后是 23 字节
//       → flush 的作用是"把数据从用户态缓冲区交给内核"，
//         它挡的是"进程死"，挡不住"机器断电"
//
//    ② bulk / halflast 之后 verify 依然 OK
//       → 崩溃恢复是日常，不是异常
// ══════════════════════════════════════════════════════════════

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

#include "lsmkv/append_only_log.h"
#include "lsmkv/record.h"
#include "lsmkv/status.h"

namespace {

const char* const kPath = "crash_test.log";

long long FileSize() {
  std::ifstream f(kPath, std::ios::binary | std::ios::ate);
  if (!f.is_open()) return -1;
  return static_cast<long long>(f.tellg());
}

// 保证 _Exit 之前 printf 的内容真的吐出来了。
// 因为 _Exit 不刷新 stdout，不手动 flush 的话你可能什么都看不到。
void FlushOut() { std::fflush(stdout); }

// ── dirty：写进缓冲区，不 flush，直接死 ─────────────────────
void ModeDirty() {
  std::remove(kPath);
  // ⚠️ 注意这里**故意不加作用域**：
  //    如果写成 { std::ofstream f(...); ... } 这样，离开大括号时
  //    f 会析构，析构就会 flush —— 那这一课就白做了。
  std::ofstream f(kPath, std::ios::binary | std::ios::trunc);
  lsmkv::Record r{lsmkv::RecordType::kPut, "name", "liupei"};
  const std::string enc = lsmkv::Encode(r);
  f.write(enc.data(), static_cast<std::streamsize>(enc.size()));
  std::printf("[dirty] pushed %zu bytes into the ofstream buffer\n",
              enc.size());
  std::printf("[dirty] did NOT call flush()\n");
  std::printf("[dirty] calling _Exit(0) -- destructors will not run\n");
  FlushOut();
  std::_Exit(0);
}

// ── clean：写进缓冲区，flush，再死 ──────────────────────────
void ModeClean() {
  std::remove(kPath);
  std::ofstream f(kPath, std::ios::binary | std::ios::trunc);
  lsmkv::Record r{lsmkv::RecordType::kPut, "name", "liupei"};
  const std::string enc = lsmkv::Encode(r);
  f.write(enc.data(), static_cast<std::streamsize>(enc.size()));
  f.flush();  // ← 唯一的一行差别
  if (!f) {
    std::printf("[clean] flush FAILED\n");
    FlushOut();
    return;
  }
  std::printf("[clean] pushed %zu bytes, then flush() -- data is in the kernel\n",
              enc.size());
  std::printf("[clean] calling _Exit(0) -- destructors will not run\n");
  FlushOut();
  std::_Exit(0);
}

// ── bulk：写 n 条，写到第 crash_at 条之前死 ─────────────────
void ModeBulk(int n, int crash_at) {
  std::remove(kPath);

  const auto t0 = std::chrono::steady_clock::now();

  {
    lsmkv::AppendOnlyLog log(kPath);
    const lsmkv::Status st = log.Open();
    if (!st.ok()) {
      std::printf("[bulk] Open failed: %s\n", st.ToString().c_str());
      FlushOut();
      return;
    }
    for (int i = 0; i < n; ++i) {
      if (i == crash_at) {
        const auto t1 = std::chrono::steady_clock::now();
        const double ms =
            std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::printf("[bulk] %d records written in %.3f ms\n", i, ms);
        std::printf("[bulk] about to write #%d -- _Exit now (simulated kill -9)\n", i);
        FlushOut();
        std::_Exit(0);
      }
      char k[32];
      char v[32];
      std::snprintf(k, sizeof(k), "k%05d", i);
      std::snprintf(v, sizeof(v), "v%05d", i);
      const lsmkv::Status s = log.Append(k, v);
      if (!s.ok()) {
        std::printf("[bulk] append failed at #%d: %s\n", i, s.ToString().c_str());
        FlushOut();
        return;
      }
    }
  }

  const auto t1 = std::chrono::steady_clock::now();
  const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  std::printf("[bulk] all %d records written in %.3f ms (%.1f us each)\n", n,
              ms, ms * 1000.0 / n);
}

// ── halflast：k 条完整的，第 k 条只写一半 ───────────────────
void ModeHalfLast(int n, int k) {
  (void)n;
  std::remove(kPath);
  {
    lsmkv::AppendOnlyLog log(kPath);
    if (!log.Open().ok()) {
      std::printf("[halflast] Open failed\n");
      FlushOut();
      return;
    }
    for (int i = 0; i < k; ++i) {
      char kk[32];
      char vv[32];
      std::snprintf(kk, sizeof(kk), "k%05d", i);
      std::snprintf(vv, sizeof(vv), "v%05d", i);
      if (!log.Append(kk, vv).ok()) {
        std::printf("[halflast] append failed at #%d\n", i);
        FlushOut();
        return;
      }
    }
  }  // ← 这里正常析构，前 k 条完整落盘

  // 第 k 条：只写一半，然后死。这才是真实的"崩在写操作中间"。
  lsmkv::Record r{lsmkv::RecordType::kPut, "k99999", "v99999"};
  const std::string enc = lsmkv::Encode(r);
  const size_t half = enc.size() / 2;
  std::ofstream f(kPath, std::ios::binary | std::ios::app);
  f.write(enc.data(), static_cast<std::streamsize>(half));
  f.flush();
  std::printf("[halflast] %d complete records on disk\n", k);
  std::printf("[halflast] record #%d is %zu bytes, wrote only %zu of them\n", k,
              enc.size(), half);
  std::printf("[halflast] calling _Exit(0) now\n");
  FlushOut();
  std::_Exit(0);
}

// ── verify：Load 一遍，看引擎怎么想 ─────────────────────────
void ModeVerify() {
  const long long sz = FileSize();
  if (sz < 0) {
    std::printf("[verify] %s does not exist -- run dirty/clean/bulk first\n",
                kPath);
    return;
  }
  std::printf("[verify] file size     : %lld bytes\n", sz);

  lsmkv::AppendOnlyLog log(kPath);
  const lsmkv::Status st = log.Load();
  std::printf("[verify] Load status   : %s\n", st.ToString().c_str());
  std::printf("[verify] records       : %zu\n", log.size());
  std::printf("[verify] truncated     : %zu bytes\n", log.truncated_bytes());

  if (!st.ok()) {
    std::printf("[verify] VERDICT      : engine refused to load\n");
    return;
  }

  // 前缀校验：第 i 条必须是 k%05d / v%05d。
  // 只验条数是不够的 —— 条数对、内容乱序也能"通过"。
  size_t bad = 0;
  for (size_t i = 0; i < log.size(); ++i) {
    char ek[32];
    char ev[32];
    std::snprintf(ek, sizeof(ek), "k%05zu", i);
    std::snprintf(ev, sizeof(ev), "v%05zu", i);
    const lsmkv::Record& r = log.entries()[i];
    if (r.type != lsmkv::RecordType::kPut || r.key != ek || r.value != ev) {
      if (bad < 3) {
        std::printf(
            "[verify]   #%zu MISMATCH: got key=\"%s\" value=\"%s\", expected "
            "%s/%s\n",
            i, r.key.c_str(), r.value.c_str(), ek, ev);
      }
      ++bad;
    }
  }
  if (bad == 0) {
    std::printf("[verify] prefix check  : %zu records, all in order -- PASS\n",
                log.size());
  } else {
    std::printf("[verify] prefix check  : %zu bad -- FAIL\n", bad);
  }
}

void Usage() {
  std::printf(
      "usage:\n"
      "  demo_crash.exe dirty             write one record, no flush, _Exit\n"
      "  demo_crash.exe clean             write one record, flush, _Exit\n"
      "  demo_crash.exe bulk N CRASH_AT   write N records, _Exit before #CRASH_AT\n"
      "  demo_crash.exe halflast N K      K complete records + half of #K\n"
      "  demo_crash.exe verify            Load the file and check the prefix\n"
      "  demo_crash.exe size              print file size only\n");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage();
    return 1;
  }
  const std::string mode = argv[1];

  if (mode == "dirty") {
    ModeDirty();
    return 0;
  }
  if (mode == "clean") {
    ModeClean();
    return 0;
  }
  if (mode == "size") {
    const long long sz = FileSize();
    std::printf("file size: %lld bytes\n", sz);
    return 0;
  }
  if (mode == "verify") {
    ModeVerify();
    return 0;
  }
  if (mode == "bulk") {
    if (argc < 4) {
      Usage();
      return 1;
    }
    ModeBulk(std::atoi(argv[2]), std::atoi(argv[3]));
    return 0;
  }
  if (mode == "halflast") {
    if (argc < 4) {
      Usage();
      return 1;
    }
    ModeHalfLast(std::atoi(argv[2]), std::atoi(argv[3]));
    return 0;
  }

  Usage();
  return 1;
}
