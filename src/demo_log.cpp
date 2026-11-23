// 这个程序用来演示「只追加日志」的行为。
// Delete 已经由你实现了，所以现在跑出来是最终答案。
//
// 注意：输出故意用英文。
// Windows 控制台默认是 GBK 编码，源文件是 UTF-8，
// 直接输出中文会变成乱码 —— 这是 Windows 上写 C++ 的一个老坑，
// 记住这一条能省你以后半小时。（注释用中文没问题，注释不进二进制。）

#include <iostream>
#include <string>

#include "lsmkv/append_only_log.h"

int main() {
  // 10-25 起：这个类要自己管文件了，所以得告诉它文件在哪。
  // 这个 demo 只关心内存行为，文件名随便起一个，跑完可以删。
  lsmkv::AppendOnlyLog log("demo_log.wal");
  if (!log.Open().ok()) {
    std::cout << "open demo_log.wal failed\n";
    return 1;
  }

  log.Append("name", "liupei");
  log.Append("score", "130");
  log.Append("name", "liupei_v2");  // 同一个 key 写第二遍：后写的赢

  std::string v;

  std::cout << "--- after 3 appends ---\n";
  if (log.Get("name", &v)) {
    std::cout << "name  = " << v << "   (expect liupei_v2, not liupei)\n";
  }
  if (log.Get("score", &v)) {
    std::cout << "score = " << v << "\n";
  }

  std::cout << "\n--- now call Delete(\"score\") ---\n";
  log.Delete("score");
  log.Delete("nobody");

  std::cout << "score still readable? -> "
            << (log.Get("score", &v)
                    ? "YES   (Delete is still empty -- go fill the TODO)"
                    : "NO    (Delete works!)")
            << "\n";
  std::cout << "entries in log: " << log.size()
            << "   (note: a delete also counts as a write)\n";

  return 0;
}
