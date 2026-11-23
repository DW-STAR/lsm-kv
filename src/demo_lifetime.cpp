// ══════════════════════════════════════════════════════════════
//  内存从哪来、又到哪去（11-12 建）
//
//  跑法（在 D:\code\lsm-kv 下）：
//    .\build.ps1 demo_lifetime
//
//  这个文件不用你改，看完就懂。
//
//  回答三个问题：
//    · "字段"是什么
//    · 函数返回时到底发生了什么，为什么内存会被释放
//    · 为什么指针还在、内容却已经不是你的了（悬空）
//
//  [1] 对象在哪，它管的字节在哪（两处不是一个地方）
//  [2] 堆内存被归还之后，同一块地址会被别人拿走
//  [3] 栈内存被回收之后，同一块空间会被别人写掉
// ══════════════════════════════════════════════════════════════

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// ── [3] 用：把一个局部数组的地址偷偷存到全局 ────────────────
const char* g_stack_addr = nullptr;

void StashStackAddress() {
  char buf[64];
  std::strcpy(buf, "this text lives on the stack frame");
  g_stack_addr = buf;  // 函数返回后，这块空间就不再属于任何人了
  std::printf("    inside : buf at %p   \"%s\"\n",
              static_cast<const void*>(buf), buf);
}

// 再调用一个函数，让它用掉同一块栈空间
void Clobber() {
  char buf[64];
  std::memset(buf, 'X', sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  std::printf("    Clobber(): its own buf at %p\n",
              static_cast<const void*>(buf));
}

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // ── [1] 对象在哪，字节在哪 ──────────────────────────────────
  std::printf("[1] the object and its bytes are two different places\n");
  {
    std::string short_s = "hi";
    std::printf("  short (\"hi\"):\n");
    std::printf("    &short_s      = %p   <- the object itself\n",
                static_cast<const void*>(&short_s));
    std::printf("    short_s.data()= %p   <- the bytes\n",
                static_cast<const void*>(short_s.data()));
    std::printf("    distance      = %+ld bytes  -> inside the object (SSO)\n",
                static_cast<long>(short_s.data() - reinterpret_cast<const char*>(&short_s)));

    std::string long_s = "a string long enough to be allocated on the heap";
    std::printf("  long (%zu bytes):\n", long_s.size());
    std::printf("    &long_s       = %p   <- the object itself (still on the stack)\n",
                static_cast<const void*>(&long_s));
    std::printf("    long_s.data() = %p   <- the bytes (on the heap, far away)\n",
                static_cast<const void*>(long_s.data()));
    std::printf("  ^ 对象在栈上，字节在堆上。**两处不同的内存，两个不同的死法。**\n");
  }

  // ── [2] 堆：归还之后，地址会被别人拿走 ──────────────────────
  std::printf("\n[2] after the heap block is freed, someone else gets the address\n");
  {
    char* buf = new char[64];
    std::strcpy(buf, "hello, this is my data");
    const char* addr = buf;

    std::printf("    my buffer      : %p   \"%s\"\n",
                static_cast<const void*>(buf), buf);

    delete[] buf;  // 归还给分配器。此刻起，这块内存不属于我了。

    char* again = new char[64];  // 别人来要一块 64 字节
    std::strcpy(again, "XXXX someone else's data");
    std::printf("    a later buffer : %p   \"%s\"\n",
                static_cast<const void*>(again), again);
    std::printf("    same address?  : %s\n", (again == addr) ? "YES" : "no");
    std::printf("  ^ 我的指针 %p 一个字节都没变，但它现在指着【别人的数据】。\n",
                static_cast<const void*>(addr));
    delete[] again;
  }

  // ── [3] 栈：回收之后，空间会被别人写掉 ──────────────────────
  std::printf("\n[3] after the stack frame is gone, someone else reuses the space\n");
  {
    StashStackAddress();
    std::printf("    after  : the address is %p, content is still \"%s\"\n",
                static_cast<const void*>(g_stack_addr), g_stack_addr);
    std::printf("    (it looks fine -- because nobody has reused the space YET)\n");

    Clobber();
    std::printf("    after  : the same address now says  \"%s\"\n", g_stack_addr);
    std::printf("  ^ 内容变了。这段内存从来没有被删除过，它只是不再属于你。\n");
  }

  std::printf("\n");
  std::printf("so what does \"free\" actually mean?\n");
  std::printf("  it does NOT wipe the bytes. it only means \"this block is\n");
  std::printf("  available again\". the old values usually sit there for a while,\n");
  std::printf("  which is exactly why use-after-free bugs look like they work.\n");
  return 0;
}
