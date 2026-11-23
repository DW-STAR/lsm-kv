// ══════════════════════════════════════════════════════════════
//  Slice 到底是什么（11-11 建）
//
//  跑法（在 D:\code\lsm-kv 下）：
//    .\build.ps1 demo_slice
//
//  这个文件不用你改，看完就懂。
//
//  ★ 一句话：**Slice 是"借"，std::string 是"拥有"。**
//
//    整件事只有一个维度：**谁负责让这块字节活着。**
//    不是"字符串 vs 字节数组"，不是"快 vs 慢"。
//
//  [1] 它有多大：两个字段，比 std::string 小一半
//  [2] 它省的是什么：传参不构造、不分配（短 key 有限，长 key 才现原形）
//  [3] 它为什么必须带长度：内嵌 '\0' 的数据
//  [4] 它的代价：不能活过它指向的那个字符串
// ══════════════════════════════════════════════════════════════

#include <chrono>
#include <cstdio>
#include <string>

#include "lsmkv/slice.h"

namespace {

// volatile 是为了防止编译器把这五次加法优化掉（优化掉就测不出东西了）
volatile size_t g_sink = 0;

// 两种"接收一个 key"的写法。注意：函数体一模一样，差别只在参数类型。
void TakeString(const std::string& s) { g_sink += s.size(); }
void TakeSlice(lsmkv::Slice s) { g_sink += s.size(); }

// 同一段代码跑三遍取最快的一次。
// ★ 为什么要三遍？demo_flush_cost 那次你已经见过了：同一份代码两次差 5 倍，
//   那是 page cache / 调度 / 频率。任何耗时结论都必须多次跑。
template <typename F>
long long BestOf3(F f) {
  long long best = -1;
  for (int r = 0; r < 3; ++r) {
    const auto t0 = std::chrono::steady_clock::now();
    f();
    const auto t1 = std::chrono::steady_clock::now();
    const long long us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    if (best < 0 || us < best) best = us;
  }
  return best;
}

// 故意返回一个指向"已经销毁的局部变量"的 Slice。
// 这就是 Slice 唯一的、也是致命的坑。函数体里我们不读它。
lsmkv::Slice MakeSliceFromLocal() {
  std::string local =
      "this string dies the moment this function returns .................";
  return lsmkv::Slice(local);
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // ── [1] 它有多大 ────────────────────────────────────────────
  std::printf("[1] what is inside a Slice\n");
  std::printf("  sizeof(std::string) = %zu bytes\n", sizeof(std::string));
  std::printf("  sizeof(Slice)       = %zu bytes  (const char* + size_t)\n",
              sizeof(lsmkv::Slice));
  std::printf("  a std::string also has to own its buffer; a Slice owns nothing\n");

  // ── [2] 它省的是什么 ────────────────────────────────────────
  std::printf("\n[2] cost of passing a key, 2,000,000 times (best of 3)\n");
  {
    const int kIters = 2000000;

    // 短 key：8 字节。std::string 会把它塞进"小字符串优化"（SSO）的
    // 内部缓冲区，不访问堆 —— 所以两边差距不大。
    const char* kShort = "key00012";
    const long long str_short =
        BestOf3([&] { for (int i = 0; i < kIters; ++i) TakeString(kShort); });
    const long long slc_short =
        BestOf3([&] { for (int i = 0; i < kIters; ++i) TakeSlice(kShort); });

    // 长 key：64 字节，超过 SSO 的容量，std::string 每次都要 malloc/free。
    const char* kLong =
        "kkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkkk";
    const long long str_long =
        BestOf3([&] { for (int i = 0; i < kIters; ++i) TakeString(kLong); });
    const long long slc_long =
        BestOf3([&] { for (int i = 0; i < kIters; ++i) TakeSlice(kLong); });

    const double rs = static_cast<double>(str_short) / static_cast<double>(slc_short);
    const double rl = static_cast<double>(str_long) / static_cast<double>(slc_long);

    std::printf("  short key (8 B, fits in SSO)\n");
    std::printf("    std::string : %6lld ms\n", str_short / 1000);
    std::printf("    Slice       : %6lld ms   -> %.1fx\n", slc_short / 1000, rs);
    std::printf("  long key (64 B, heap)\n");
    std::printf("    std::string : %6lld ms\n", str_long / 1000);
    std::printf("    Slice       : %6lld ms   -> %.1fx\n", slc_long / 1000, rl);

    std::printf("  ★ 短 key 也差 %.1f 倍：SSO 免掉了 malloc，但「造一个对象」\n", rs);
    std::printf("    这件事本身还要初始化字段、拷字节、再析构一次。\n");
    std::printf("    长 key 差 %.1f 倍 —— 那是真的走进堆了。\n", rl);
    std::printf("    所以 Slice 省掉的不是「分配」，是「**为了传个参数先造一个对象**」。\n");
    std::printf("    每次传参：std::string %lld ns，Slice %lld ns（短 key）。\n",
                str_short * 1000 / kIters, slc_short * 1000 / kIters);
    std::printf("    折到 10 万 QPS：每 10 万次请求省下 %lld us，约 %.1f%% 的 CPU。\n",
                (str_short - slc_short) / 20,
                (str_short - slc_short) / 20 / 10000.0);
    std::printf("    ^ 这点钱救不了 QPS，别指望它。Slice 的价值在别处：\n");
    std::printf("      它让「我只是借一下」变成接口上能写出来的东西，\n");
    std::printf("      于是复制只发生在唯一该发生的那一处（ToString）。\n");
  }

  // ── [3] 它为什么必须带长度 ──────────────────────────────────
  std::printf("\n[3] why a Slice carries its own length\n");
  {
    const std::string bin("a\0b", 3);
    const lsmkv::Slice from_string(bin);
    const lsmkv::Slice from_cstr("a\0b");  // NOLINT: 故意演示 strlen 截断

    std::printf("  from std::string(\"a\\0b\", 3) : size = %zu\n", from_string.size());
    std::printf("  from \"a\\0b\" (char*)          : size = %zu\n", from_cstr.size());
    std::printf("  round trip ToString()       : size = %zu\n",
                from_string.ToString().size());
    std::printf("  ^ c-string 在第一个 \\0 就断了。Slice 带长度，所以不会。\n");
    std::printf("    test_basic 第 6 条（二进制安全）赌的就是这个。\n");
  }

  // ── [4] 它的代价 ────────────────────────────────────────────
  std::printf("\n[4] the price: a Slice must not outlive what it points at\n");
  {
    const lsmkv::Slice dead = MakeSliceFromLocal();
    std::printf("  the slice returned by the function still says size = %zu\n",
                dead.size());
    std::printf("  but that memory was freed when the function returned.\n");
    std::printf("  calling dead.ToString() now would read freed memory (UB).\n");
    std::printf("  the fix is one character: return std::string, not Slice.\n");
    std::printf("  ^ 这就是为什么 MemTable 只收 std::string：\n");
    std::printf("    它要把数据留到几个月后，Slice 做不到这件事。\n");
  }

  std::printf("\n");
  std::printf("so:  Slice = borrow (call it, use it, drop it within the call)\n");
  std::printf("     std::string = own (store it, keep it alive)\n");
  std::printf("     crossing from borrow to own costs one copy -- that copy is\n");
  std::printf("     ToString(), and it belongs at the boundary, exactly once.\n");
  return 0;
}
