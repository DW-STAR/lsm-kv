// 今天（10-21）的任务：让字节活过进程退出。
//
// 到目前为止，你写的所有东西在程序结束那一刻就没了 ——
// Encode 出来的 std::string 在堆上，进程一退，操作系统全部回收。
// 今天第一次把字节搬到一个「关了程序还在」的地方：文件。
//
// ══════════════════════════════════════════════════════════════
//  规格（就这 4 步，顺序固定）
//
//   1. 打开 wal.log —— 追加模式 + 二进制模式
//   2. 把两条记录各自 Encode 出来的字节，原样写进去（不加任何分隔符）
//   3. 关掉
//   4. 打印：两条记录各多少字节、加起来多少、实际文件多大
//
//  验收：★ 关掉程序，再跑一次，文件里的字节数只增不减
//        ★ 用 PowerShell 的 Format-Hex 打开 wal.log，
//          第 0 个字节是 0x01（第一条是 Put），能说出为什么
//
// ══════════════════════════════════════════════════════════════
//  可用的积木 —— 自己决定怎么组合
//
//   std::ofstream out(路径, 模式);
//       模式 = std::ios::app | std::ios::binary
//
//   out.is_open()            打开了没有（用来判断失败）
//   out.write(地址, 字节数)   往文件里写一段裸字节
//   out.close()              关掉
//
//   bytes.data()             std::string 里字节的起始地址
//   bytes.size()             一共多少字节
//
// ══════════════════════════════════════════════════════════════
//  三行怎么写（逐词拆开读）
//
//  ── 第 1 行：打开 ─────────────────────────────────────────
//
//     std::ofstream out("wal.log", std::ios::app | std::ios::binary);
//     └─────┬─────┘ └┬┘ └───┬───┘  └──────────────┬──────────┘
//         类型名    变量名   文件路径              模式
//
//  · std::ofstream = output file stream = 「连到文件的 cout」
//    cout 你天天用，它本质上也是一个流，只是出口接的是屏幕。
//    ofstream 是同一类东西，出口接的是一个文件。写法和 cout 一模一样。
//
//  · 这一行同时干了两件事：声明变量 out + 打开文件。
//    C++ 的构造函数会在创建对象时自动跑，所以「声明即打开」——
//    你没看到 open 这个词，但它已经开了。
//    （也可以 out.open(...) 手动开，效果一样，不用两遍。）
//
//  · std::ios::app 里的 app = append = 追加。没有它默认 trunc = 清空。
//  · std::ios::binary 里的 binary = 二进制，Windows 上必须加。
//  · 中间那个 | 是「位或」，作用是把两个模式合并成一个 ——
//    因为模式本质是一堆开关位，| 就是把两个开关同时拨上去。
//
//  ── 第 2 行：写 ───────────────────────────────────────────
//
//     out.write(bytes.data(), bytes.size());
//
//  · 它的签名和 memcpy 一模一样：memcpy(目的地, 源地址, 字节数)。
//    write 只是把「目的地」固定成了「这个文件」，
//    所以你只需要给：「从哪读」+「读几个字节」。
//  · bytes.data() = 这串字节的起始地址
//  · bytes.size() = 一共几个字节
//  · 写完游标自动往后走，下一次 write 接在后面 —— 不用你管。
//
//  ── 第 3 行：关 ───────────────────────────────────────────
//
//     out.close();
//
//  · 关掉，把还压在缓冲区里的字节刷到磁盘。
//  · 不写也能过（析构时会自动关），但今天是学文件，显式写出来。
//    真实工程里「什么时候关」会变成一件要命的事（10-26 讲 fsync）。
//
// ══════════════════════════════════════════════════════════════
//  ⚠️ 三个坑，全是「少写一个词就全错」的类型
//
//  ① 不加 std::ios::app
//     std::ofstream 默认模式是 trunc —— 打开就清空。
//     你第二次运行时，第一次写的数据全部消失，而且不报错。
//
//  ② 不加 std::ios::binary
//     Windows 的文本模式会把 0x0A 翻译成 0x0D 0x0A。
//     你的字节里只要出现过 0x0A（value 里一个换行就够），
//     文件就比 Encode 的结果长，Decode 从此全部错位。
//
//  ③ 用 out << bytes.data() 而不是 out.write()
//     << 走的是「格式化输出」，把 char* 当 C 字符串处理 ——
//     写到第一个 0x00 就停。你的记录第 1 个字节是 04，第 2 个就是 00，
//     所以 << 只会写进去 2 个字节，剩下 17 个静默丢掉。
//
//     结论：写二进制一律 write()，不要 <<。
//
// ══════════════════════════════════════════════════════════════
//  ★ 规矩（和 record.h 一样）
//    卡住超过 40 分钟，把「想做什么 / 现在什么现象 / 你猜是什么原因」
//    三句话发我。我只指方向，不给代码。
// ══════════════════════════════════════════════════════════════

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "lsmkv/record.h"

namespace {

const char* const kLogPath = "wal.log";

// 文件现在有多大。文件不存在就返回 0。
// 注意 ios::ate = "打开后把读位置移到末尾"，这样 tellg() 给的就是文件大小。
long long FileSize(const char* path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f.is_open()) {
    return 0;
  }
  return static_cast<long long>(f.tellg());
}

}  // namespace

int main() {
  // 两条记录，和 demo_record 里的那两条一样。
  lsmkv::Record put;
  put.type = lsmkv::RecordType::kPut;
  put.key = "name";
  put.value = "liupei";

  lsmkv::Record del;
  del.type = lsmkv::RecordType::kDelete;
  del.key = "name";

  // ── STEP 1：编码（已验证过，直接用）────────────────────────
  const std::string bytes_a = lsmkv::Encode(put);
  const std::string bytes_b = lsmkv::Encode(del);

  std::printf("record A (%s = %s): %zu bytes\n", put.key.c_str(),
              put.value.c_str(), bytes_a.size());
  std::printf("record B (delete %s): %zu bytes\n", del.key.c_str(),
              bytes_b.size());
  std::printf("expect total: %zu bytes\n\n", bytes_a.size() + bytes_b.size());

  // ── STEP 1.5：先量一下文件现在多大（这段我写好了，你不用管）──
  // 为什么要「现在」量：因为验收要看的是「这一次写了多少」，
  // 而不是「文件总共多大」。第二次运行时文件里已经有 32 字节了，
  // 拿总数去比 32 会误判成 FAIL。
  const long long size_before = FileSize(kLogPath);

  // ── STEP 2：打开文件 ───────────────────────────────────────
  // TODO(你)：用 std::ofstream 打开 kLogPath
  //            模式里必须有 app 和 binary —— 为什么？往上翻坑 ①②。
  //            打开失败就直接 printf 一句然后 return 1。
  std::ofstream out(kLogPath,std::ios::app | std::ios::binary);
  if(!out.is_open()){
    return 1;
  }
  // ── STEP 3：写 ────────────────────────────────────────────
  // TODO(你)：先把 bytes_a 写进去，再把 bytes_b 写进去。
  //            顺序不能反 —— 反了之后读出来的"最新版本"就是错的。
  out.write(bytes_a.data(),static_cast<std::streamsize>(bytes_a.size()));
  out.write(bytes_b.data(),static_cast<std::streamsize>(bytes_b.size()));
  // ── STEP 4：关 ────────────────────────────────────────────
  // TODO(你)：关掉。然后 printf 一句"done"。
  out.close();
  printf("done\n");
  // ── STEP 5：验收（这段已经写好了，不用动）──────────────────
  // 核心不是"文件多大"，而是「这次比上次多了多少」。
  // 只要差值 == 32 就说明两件事同时成立：
  //   ① app 生效了（旧数据没被清掉）
  //   ② binary + write 用对了（没有被 0x0A 膨胀，也没有被 0x00 截断）
  const long long size_after = FileSize(kLogPath);
  const long long written = size_after - size_before;
  const long long expect =
      static_cast<long long>(bytes_a.size() + bytes_b.size());

  std::printf("\nfile size before : %lld bytes\n", size_before);
  std::printf("file size after  : %lld bytes\n", size_after);
  std::printf("this run wrote   : %lld bytes\n", written);
  std::printf("expected         : %lld bytes\n", expect);

  if (written == expect) {
    std::printf("verification: PASS\n");
    if (size_before > 0) {
      std::printf(
          "              and the old %lld bytes are STILL there -- "
          "that is ios::app doing its job\n",
          size_before);
    }
  } else if (written == 0) {
    std::printf(
        "verification: FAIL -- 0 bytes written. "
        "Pitfall 1: did you open without ios::app? (trunc wipes, then...)\n");
  } else if (written < expect) {
    std::printf(
        "verification: FAIL -- %lld bytes lost. "
        "Pitfall 2 (no binary) or 3 (used << instead of write, stopped at "
        "the first 0x00)\n",
        expect - written);
  } else {
    std::printf(
        "verification: FAIL -- %lld bytes too MANY. "
        "Pitfall 2: 0x0A got expanded to 0x0D 0x0A (text mode)\n",
        written - expect);
  }
  return 0;
}

// ══════════════════════════════════════════════════════════════
//  编译 & 运行（在 D:\code\lsm-kv 下）
//
//    g++ -std=c++17 -Iinclude src/demo_file.cpp -o build_win/demo_file.exe
//    ./build_win/demo_file.exe
//
//  看文件里到底是什么字节：
//
//    Format-Hex wal.log
//
//  再跑一次 demo_file.exe，再 Format-Hex 一次 ——
//  文件应该变成 64 字节（32 + 32），一条都没被清掉。
//  那一刻你就完成了今天真正要做的事：
//  数据第一次活过了进程的死亡。
// ══════════════════════════════════════════════════════════════
