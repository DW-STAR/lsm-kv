// ══════════════════════════════════════════════════════════════
//  所有权入门：裸指针 / unique_ptr / std::move（11-11 建）
//
//  跑法（在 D:\code\lsm-kv 下）：
//    .\build.ps1 demo_ownership
//
//  这个文件不用你改，看完就懂。
//
//  ★ 为什么需要这个 demo？
//    11-10 的 Rotate() 里突然出现了 std::unique_ptr 和 std::move，
//    而前面三周一个指针都没露过面。不是风格突变，是被逼的 ——
//    内存里第一次出现了"一张表在运行时换主人"这件事。
//
//  [1] 裸指针：delete 得你自己记，忘一次就漏
//  [2] unique_ptr：析构函数替你记
//  [3] 它为什么禁止拷贝：两个主人会 delete 两次
//  [4] std::move 到底做了什么：地址一个字节都没变
//  [5] move 之后旧指针是空的 —— 碰它就是段错误
// ══════════════════════════════════════════════════════════════

#include <cstdio>
#include <memory>
#include <type_traits>

namespace {

// 一个会"报户口"的对象：造出来 +1，销毁 -1。
// 有了它，泄漏和重复释放都变成肉眼可见的数字。
struct Tracked {
  static int alive;
  int id;

  explicit Tracked(int i) : id(i) {
    ++alive;
    std::printf("    + create #%d        alive=%d\n", id, alive);
  }
  ~Tracked() {
    --alive;
    std::printf("    - destroy #%d       alive=%d\n", id, alive);
  }

  Tracked(const Tracked&) = delete;
  Tracked& operator=(const Tracked&) = delete;
};

int Tracked::alive = 0;

}  // namespace

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  // ── [1] 裸指针 ──────────────────────────────────────────────
  std::printf("[1] raw pointer: you have to remember delete\n");
  {
    Tracked* leak = nullptr;
    {
      Tracked* a = new Tracked(1);
      Tracked* b = new Tracked(2);
      leak = b;              // 留个把手，方便后面手动补
      delete a;              // 这个记得
      // delete b;           // 这个忘了
      std::printf("  leaving the scope...\n");
    }
    std::printf("  alive=%d  <-- #2 is still alive, and nobody owns it\n",
                Tracked::alive);

    delete leak;             // 手动补一刀，否则后面全是脏账
    std::printf("  cleaned up by hand: alive=%d\n", Tracked::alive);
  }

  // ── [2] unique_ptr ──────────────────────────────────────────
  std::printf("\n[2] unique_ptr: the destructor remembers for you\n");
  {
    auto p = std::make_unique<Tracked>(3);
    (void)p;
    std::printf("  leaving the scope...\n");
  }
  std::printf("  alive=%d  <-- it deleted itself, you did not write delete\n",
              Tracked::alive);

  // ── [3] 为什么不能拷贝 ──────────────────────────────────────
  std::printf("\n[3] copy is forbidden, move is allowed\n");
  std::printf("  copy-constructible : %s\n",
              std::is_copy_constructible<std::unique_ptr<Tracked>>::value
                  ? "yes" : "no");
  std::printf("  move-constructible : %s\n",
              std::is_move_constructible<std::unique_ptr<Tracked>>::value
                  ? "yes" : "no");
  std::printf("  if copy were allowed:\n");
  std::printf("      q = p;  -> two pointers, one object, two deletes\n");
  std::printf("      first delete is correct, second one is a heap corruption\n");
  std::printf("  so the compiler refuses it.  -- 下面的代码是编译不过的：\n");
  std::printf("      std::unique_ptr<Tracked> q = p;   // error\n");
  std::printf("      std::unique_ptr<Tracked> q = std::move(p);  // ok\n");

  // ── [4] std::move 到底做了什么 ──────────────────────────────
  std::printf("\n[4] std::move does not move a single byte\n");
  {
    auto p = std::make_unique<Tracked>(4);
    Tracked* before = p.get();

    std::printf("  before: p points to %p\n", static_cast<void*>(before));
    auto q = std::move(p);   // 唯一的差别：这一行允许"偷"
    std::printf("  after : p = %p   q = %p\n",
                static_cast<void*>(p.get()),
                static_cast<void*>(q.get()));
    std::printf("  same address as before: %s\n",
                (q.get() == before) ? "yes" : "no");
    std::printf("  object created or destroyed: %s\n",
                (Tracked::alive == 1) ? "no (alive is still 1)" : "yes");
    std::printf("  so what changed? **who owns it**, not where it lives\n");
    std::printf("  leaving the scope...\n");
  }
  std::printf("  alive=%d\n", Tracked::alive);

  // ── [5] move 之后旧指针是空的 ───────────────────────────────
  std::printf("\n[5] after the move, the source pointer is null\n");
  {
    auto p = std::make_unique<Tracked>(5);
    auto q = std::move(p);
    (void)q;

    std::printf("  p is %s\n", p ? "non-null" : "null");
    if (p) {
      std::printf("  #%d\n", p->id);
    } else {
      std::printf("  p->id would dereference null -> 0xC0000005\n");
      std::printf("  this is exactly why Rotate() must make a new table\n");
      std::printf("  immediately after the move\n");
    }
  }

  std::printf("\n");
  if (Tracked::alive == 0) {
    std::printf("ALL OBJECTS DESTROYED  (alive=0)\n");
    return 0;
  }
  std::printf("LEAK: alive=%d\n", Tracked::alive);
  return 1;
}
