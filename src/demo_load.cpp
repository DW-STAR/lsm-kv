// 验收工具（10-24）：重构之后，三件事必须同时成立。
//
// 这个文件**不用你改**。你要改的是 include/lsmkv/append_only_log.h。
//
// 编译 & 运行（在 D:\code\lsm-kv 下）：
//
//    g++ -std=c++17 -Iinclude src/demo_load.cpp -o build_win/demo_load.exe
//    ./build_win/demo_load.exe
//
//    g++ -std=c++17 -Iinclude src/demo_log.cpp  -o build_win/demo_log.exe
//    ./build_win/demo_log.exe
//
// 期望输出：
//
//    === case 1: load a real file ===
//    file size: 32 bytes
//    status: OK
//    loaded 2 records
//      #0 Put    name = liupei
//      #1 Delete name =
//    Get("name") -> NOT FOUND (last record is a tombstone)
//
//    === case 2: load a file that does not exist ===
//    status: OK   <- must be OK, not IOError
//    loaded 0 records
//    case 2 PASS
//
//    === case 3: demo_log still works ===
//    ...
//
// 三个必须能回答的问题（新规则要求的，答不出就是白做）：
//
//    1. 为什么 case 2 返回 OK 而不是 IOError？
//       提示：如果返回 IOError，调用方通常会怎么做？那个做法会造成什么后果？
//    2. 换 Record 之后，代码变多还是变少了？为什么？
//    3. 如果用户真的存了字符串 "__DELETED__"，改之前和改之后分别会怎样？

#include <cstdio>
#include <fstream>
#include <string>

#include "lsmkv/append_only_log.h"
#include "lsmkv/record.h"
#include "lsmkv/status.h"

namespace {

const char* TypeName(lsmkv::RecordType t) {
  switch (t) {
    case lsmkv::RecordType::kPut:
      return "Put";
    case lsmkv::RecordType::kDelete:
      return "Delete";
  }
  return "??";
}

long long FileSize(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    return -1;
  }
  return static_cast<long long>(f.tellg());
}

void Dump(const lsmkv::AppendOnlyLog& log) {
  const auto& all = log.entries();
  for (size_t i = 0; i < all.size(); ++i) {
    std::printf("  #%zu %-6s %s = %s\n", i, TypeName(all[i].type),
                all[i].key.c_str(), all[i].value.c_str());
  }
}

}  // namespace

int main() {
  // ── case 1：正常文件 ─────────────────────────────────────
  std::printf("=== case 1: load a real file ===\n");
  const long long size = FileSize("wal.log");
  if (size < 0) {
    std::printf("wal.log not found. Run demo_file.exe first.\n");
    return 1;
  }
  std::printf("file size: %lld bytes\n", size);

  // 10-26：构造函数要路径了，Load 不再收参数（用它自己那个 path_）
  lsmkv::AppendOnlyLog log("wal.log");
  lsmkv::Status st = log.Load();
  std::printf("status: %s\n", st.ToString().c_str());
  if (!st.ok()) {
    std::printf("  Load failed -- nothing to check.\n");
    return 1;
  }
  std::printf("loaded %zu records\n", log.size());
  Dump(log);

  std::string value;
  if (log.Get("name", &value)) {
    std::printf("Get(\"name\") -> %s\n", value.c_str());
  } else {
    std::printf("Get(\"name\") -> NOT FOUND (last record is a tombstone)\n");
  }

  // ── case 2：文件不存在 ───────────────────────────────────
  std::printf("\n=== case 2: load a file that does not exist ===\n");
  // 这个对象一出生就指向一个不存在的路径 —— 专门测"打不开但不是错"
  lsmkv::AppendOnlyLog fresh("no_such_dir/never.log");
  lsmkv::Status st2 = fresh.Load();
  std::printf("status: %s   <- must be OK, not IOError\n",
              st2.ToString().c_str());
  std::printf("loaded %zu records\n", fresh.size());
  if (!st2.ok() || fresh.size() != 0) {
    std::printf("case 2 FAIL\n");
    return 1;
  }
  std::printf("case 2 PASS\n");

  // ── case 3：提醒 ─────────────────────────────────────────
  std::printf(
      "\n=== case 3: demo_log still works ===\n"
      "run ./build_win/demo_log.exe -- its output must be unchanged.\n"
      "if it crashed or changed, you broke the OUTSIDE, not just the "
      "inside.\n");

  return 0;
}
