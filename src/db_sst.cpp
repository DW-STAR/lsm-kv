// ══════════════════════════════════════════════════════════════
//  第 4 版实现（2026-11-23）：DB 接上 WAL —— 崩溃也不丢数据
//
//  ── 这一版补的是什么 ────────────────────────────────────────
//
//  之前那一版，数据的安全性靠 ~LSMDB() 里强行刷盘。
//  那是一个**错误的依赖**：析构函数不保证被调用（kill -9、断电、_Exit、
//  异常栈展开被打断），而"数据不丢"这件事不能建立在一个不保证执行的动作上。
//
//  现在改成：
//
//      Put ─→ ① 写 WAL（flush 到 OS）─→ ② 写 MemTable ─→ ③ 满了就刷盘
//                │
//                └─ 崩在任何一步之后，重启重放 WAL 都能回来
//
//  **只要 ① 返回成功，这条数据就不会丢。** 内存表刷不刷、析构跑不跑，
//  都只影响"启动快慢"，不影响"数据在不在"。
//
//  ── 全篇最需要想清楚的一件事：WAL 和内存表是【成对】的 ──────
//
//  一份 WAL 记录的是"某一张内存表的全部写入"。
//  所以它只能在那张表**落盘之后**才能删 —— 早一天删就是丢数据。
//
//  而 rotate 那一瞬间，这张表的记录在旧 WAL 里，新表还一条都没写。
//  这就是唯一安全的切换点：
//
//      rotate 之前：  旧 WAL ←→ active(旧)
//      rotate 之后：  旧 WAL ←→ immutable(旧)     ← 可以刷盘了
//                     新 WAL ←→ active(新，空的)  ← 从这个文件重新开始记
//
//  ★ 为什么"新 active 是空的"这个前提这么重要？
//    如果 rotate 之后新表已经开始写了，那新旧两张表的记录就混在同一个
//    文件里，删也不是、留也不是 —— 删了丢新数据，留着永远删不掉。
//    所以切换必须发生在**新表还空着**的那一刻，一晚都不行。
//
//  ── 还差什么（诚实清单）──────────────────────────────────────
//
//    fsync       WAL 每次 Append 只 flush 到 OS，没有 fsync 到盘片。
//                kill -9 不丢（数据已在页缓存），但**断电会丢最后几条**。
//                真实引擎用 group commit：攒一批、一次 fsync。
//    MANIFEST    靠"从 000001.sst 逐个探测"。中间缺一个文件，后面的读不到。
//    compaction  .sst 只增不减，墓碑占的空间永不回收。
//    后台线程    刷盘是同步的，Put 会等文件写完。
// ══════════════════════════════════════════════════════════════

#include <cerrno>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lsmkv/append_only_log.h"
#include "lsmkv/db.h"
#include "lsmkv/mem_table.h"
#include "lsmkv/mem_table_set.h"
#include "lsmkv/record.h"
#include "lsmkv/skip_list.h"
#include "lsmkv/sstable.h"
#include "lsmkv/status.h"

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

namespace lsmkv {

namespace {

// 内存表阈值。
//
// ★ 为什么这里写 1MB，而不是 mem_table.h 里的默认 4MB？
//   为了让 demo 几秒钟就能把一张表写满、真的触发一次刷盘。
//   4MB 要写三万多条，1MB 只要七千多条 —— 逻辑完全一样，
//   但你能在一屏里看到 Rotate → Flush 发生。
//   **测试里把规模调到"刚好能看见现象"，比照抄生产参数有用。**
constexpr size_t kMemTableBytesForDemo = 1u << 20;

// 文件名：000001.sst / 000001.wal、000002.sst / 000002.wal、……
//
// ★ 为什么补零到 6 位？
//   因为要靠文件名排序来确定"谁新谁旧"。
//   不补零的话 "10.sst" < "9.sst"（字符串逐字符比较），顺序就错乱了。
//   **凡是把序号编进文件名，就必须保证字典序 == 数值序。**
//
// ★ 为什么 sst 和 wal 各自独立编号，不共用一个序列？
//   因为它们描述的是两件不同的事：sst 是"第几份落盘数据"，
//   wal 是"第几份未落盘的写入记录"。共用编号会逼你维护
//   "这个号是给谁的"这种状态，而这种状态一旦算错，
//   表现就是"删了不该删的日志"或"覆盖了不该覆盖的文件"。
std::string PathWithNumber(const std::string& dir, uint64_t number, const char* ext) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%06llu%s",
                static_cast<unsigned long long>(number), ext);
  if (dir.empty()) return std::string(buf);
  return dir + "/" + buf;
}

// ★ sst 用编号，wal 用固定名 —— 为什么两者不一样？
//
//   这是被一个真实的 bug 逼出来的（2026-11-23 晚）。
//
//   一开始 wal 也叫 000001.wal / 000002.wal，Open 时按编号从小到大试，
//   试不到就停。然后 demo_restart 直接炸了：3908 alive / 3601 wrong。
//
//   因为 000001.wal 在第一次刷盘成功后被删掉了，而 000002.wal 还在。
//   Open 从 1 开始试，第一个就找不到 → break → **一个日志都没重放**。
//   后半段 3445 条写入和 1143 条墓碑全在 000002.wal 里，没人看得见。
//
//   ★ 教训：**"按编号探测"要求编号连续，而删除恰恰会破坏连续性。**
//     这个前提在 sst 上暂时成立（我们还不删 sst），
//     在 wal 上从一开始就不成立（刷完就删）。同一个偷懒办法，
//     换一个地方就是 bug —— 复用"经验"之前先问它依赖什么前提。
//
//   固定名的好处是它和系统的约束天然对齐：
//   同一时刻最多一张 immutable → 最多两个 wal 文件 → 固定两个名字就够。
//   不需要编号，也就不需要"编号必须连续"这个脆弱前提。
std::string SstPath(const std::string& dir, uint64_t number) {
  return PathWithNumber(dir, number, ".sst");
}

// 当前 active 表的日志。所有新写入都追加到这里。
std::string WalPath(const std::string& dir) {
  return dir.empty() ? std::string("wal.log") : dir + "/wal.log";
}

// 冻结表的日志。rotate 时由 wal.log 改名而来，落盘成功后删除。
// 它存在 = "有一张表还没落盘"，这是崩溃恢复最重要的一个信号。
std::string WalImmutablePath(const std::string& dir) {
  return dir.empty() ? std::string("wal.immutable.log")
                     : dir + "/wal.immutable.log";
}

int MakeDir(const std::string& path) {
#ifdef _WIN32
  return _mkdir(path.c_str());
#else
  return mkdir(path.c_str(), 0755);
#endif
}

// 这个文件在不在。用 fopen 探测而不是 stat：
// 两者都能回答"在不在"，但 fopen 顺便验证了"能不能读"。
bool FileExists(const std::string& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (f == nullptr) return false;
  std::fclose(f);
  return true;
}

// ══════════════════════════════════════════════════════════════
//  LSMDB —— 真正同时管内存和磁盘的那一层
//
//  ★ 和 db_mem.cpp 里的 LSMDB 是同一个名字、不同的实现。
//    这不是重名 bug：两版实现各自编译成各自的 exe，从不进同一个程序。
//    它们共用同一个 db.h —— 接口不变，实现整个换掉，调用方一行不改。
//    这就是第 0 版"先定接口"到底换来了什么。
// ══════════════════════════════════════════════════════════════
class LSMDB : public DB {
 public:
  explicit LSMDB(std::string dir) : dir_(std::move(dir)) {}

  // ═══════════════════════════════════════════════════════════
  //  析构：什么都不做
  //
  //  ★ 上一版这里是"强行把内存刷成文件"，因为那时候没有 WAL，
  //    不刷就等于丢。现在有了 WAL，这个依赖被彻底拿掉了：
  //
  //      数据在不在  ← 由 WAL 保证（Append 成功就是承诺）
  //      启动快不快  ← 由 sst 决定（没有就重放日志，慢一点，但一定对）
  //
  //    **这是一个很重要的降级：安全性不再依赖一个"不保证被调用"的函数。**
  //    上一版那个"忘了写析构 → 3601 条数据复活"的 bug，
  //    在有 WAL 之后根本不是 bug —— 最坏情况只是下次启动慢一点。
  //
  //  ★ 那为什么不顺便刷一下让它更快？
  //    因为刷盘要配套"删掉对应的 WAL"，而删 WAL 依赖"新表已经就位"这个前提。
  //    析构的时候已经没有"新表"了，硬做只会把在线的安全逻辑搞出分叉。
  //    宁可慢一点，也不要两条代码路径。
  // ═══════════════════════════════════════════════════════════
  ~LSMDB() override { CloseWal(); }

  // ═══════════════════════════════════════════════════════════
  //  Put —— 顺序是这一整版的核心，三步一步都不能换
  //
  //    ① wal_->Append(k, v)     写日志，flush 到 OS
  //    ② tables_.Put(k, v)      改内存
  //    ③ MaybeFlushImmutable()  表满了就落盘
  //
  //  ★ 为什么 ① 失败了要立刻 return，不能"记个日志继续写内存"？
  //
  //    因为 ① 失败意味着"这条数据没有任何地方记得它"。
  //    继续写内存的话，用户拿到 OK，数据活到进程结束 —— 一次假成功。
  //    **WAL 存在的全部意义就是"返回成功 = 真的不会丢"，
  //      它一旦开始说谎，整套机制就白做了。**
  //
  //  ★ 为什么 ① 成功了、② 失败了却可以直接返回错误，不用回滚日志？
  //
  //    因为多写一条日志是**无害的**：重启重放时它会被重新写进内存，
  //    结果和"当时成功了"一样。日志允许有冗余，不允许有缺失。
  //    （这条不对称是故意设计的，不是没想到回滚。）
  // ═══════════════════════════════════════════════════════════
  Status Put(const Slice& key, const Slice& value) override {
    const std::string k = key.ToString();
    const std::string v = value.ToString();

    const Status ws = wal_->Append(k, v);
    if (!ws.ok()) return ws;

    const Status st = tables_.Put(k, v);
    if (!st.ok()) return st;

    return MaybeFlushImmutable();
  }

  Status Delete(const Slice& key) override {
    const std::string k = key.ToString();

    // 墓碑也要进日志。否则重启后"a 被删了"这件事没人记得，
    // 重放出来的只有那条 Put —— 删掉的数据自己爬回来，只是这次跨了重启。
    const Status ws = wal_->Delete(k);
    if (!ws.ok()) return ws;

    const Status st = tables_.Delete(k);
    if (!st.ok()) return st;

    return MaybeFlushImmutable();
  }

  // ═══════════════════════════════════════════════════════════
  //  Get —— 整条查找链
  //
  //    ① 内存：active → immutable（MemTableSet::Lookup 干这个）
  //         kFound    → 直接给值，磁盘根本不用碰
  //         kDeleted  → 直接 NotFound，**磁盘也不许碰**
  //         kNotFound → 落到 ②
  //
  //    ② 磁盘：从新到旧
  //         每张表用 FindRecord（三态），不是 Get（两态）
  //         墓碑同样终结，理由和内存层一模一样
  //
  //    ③ 都没命中 → NotFound
  //
  //  ★ 为什么 ② 必须用 FindRecord 而不是 SSTable::Get？
  //    同一个理由，只是换了一层：SSTable::Get 会把墓碑压成 NotFound，
  //    于是"这张表里有墓碑"和"这张表里没有"分不清。
  //    分不清的后果是继续去查更老的表 —— 那里躺着旧值 —— 又活了。
  //    **同一个 bug 会在每一层重新出现一次，因为每一层的返回值都要缩水。**
  // ═══════════════════════════════════════════════════════════
  Status Get(const Slice& key, std::string* value) override {
    if (value == nullptr) return Status::InvalidArgument("invalid value");
    const std::string k = key.ToString();

    // ── ① 内存
    const LookupResult r = tables_.Lookup(k, value);
    if (r == LookupResult::kFound) return Status::OK();
    if (r == LookupResult::kDeleted) return Status::NotFound("not found: " + k);

    // ── ② 磁盘：ssts_ 是"旧 → 新"，所以从后往前走
    for (size_t i = ssts_.size(); i > 0; --i) {
      Record rec;
      const Status st = ssts_[i - 1]->FindRecord(k, &rec);
      if (st.IsNotFound()) continue;  // 这张表里没有，问更老的
      if (!st.ok()) return st;        // Corruption 之类，必须往上抛
      if (rec.type == RecordType::kDelete) {
        return Status::NotFound("not found (deleted on disk): " + k);
      }
      *value = std::move(rec.value);
      return Status::OK();
    }

    // ── ③ 真没有
    return Status::NotFound("not found: " + k);
  }

  // ── 下面这些是 DB::Open 用的接线口，正常读写路径不该碰 ──────

  uint64_t next_number() const { return next_number_; }
  void SetNextNumber(uint64_t n) { next_number_ = n; }
  void AddSSTable(std::unique_ptr<SSTable> sst) { ssts_.push_back(std::move(sst)); }

  Status OpenWal(const std::string& path) {
    if (wal_ != nullptr) wal_->Close();
    wal_ = std::make_unique<AppendOnlyLog>(path);
    return wal_->Open();
  }

  // 重放一批日志记录。
  //
  // ★ 为什么用 PutDirect 而不是 tables_.Put？
  //   因为 tables_.Put 在表满时会自动 Rotate —— 而重放阶段一旦发生 Rotate，
  //   "旧 WAL 属于哪张表"就说不清了：重放出来的记录会同时落在
  //   immutable 和新 active 里，但它们在磁盘上还是同一个日志文件。
  //   之后刷盘 immutable、删掉这个日志 —— 新 active 里的数据就一起没了。
  //
  //   **重放是一次"重演历史"，不该在这个阶段改变系统的结构。**
  //   等重放完，让下一次真实的 Put 去触发换表，那时候的日志归属是清楚的。
  Status ReplayRecords(const std::vector<Record>& records) {
    for (const Record& rec : records) {
      const Status st = (rec.type == RecordType::kDelete)
                            ? tables_.DeleteDirect(rec.key)
                            : tables_.PutDirect(rec.key, rec.value);
      if (!st.ok()) return st;
    }
    return Status::OK();
  }

 private:
  void CloseWal() {
    if (wal_ != nullptr) wal_->Close();
  }

  // ═══════════════════════════════════════════════════════════
  //  MaybeFlushImmutable —— 冻结表落盘，并且处理它的日志
  //
  //  ★ 为什么"切日志"必须在"刷盘"之前？反过来会怎样？
  //
  //    反过来的话，刷盘那几十毫秒里新来的写还会继续进旧日志。
  //    等刷完去删旧日志，就把那些新写的一起删了 —— 而它们还没落盘。
  //    顺序必须是：先划清界限，再清理旧账。
  //
  //  ★ 为什么用 rename，而不是"新建一个带新编号的文件"？
  //    见上面 WalPath 那段注释 —— 编号方案在这里真的塌过一次。
  //    rename 还有一个好处：它是原子的。崩溃在 rename 前后，
  //    要么是 wal.log 里全都有，要么是 imm 里有旧数据 + wal.log 空，
  //    两种状态都能正确恢复。不会出现"两个文件都缺一半"。
  // ═══════════════════════════════════════════════════════════
  Status MaybeFlushImmutable() {
    if (!tables_.HasImmutable()) return Status::OK();

    const std::string cur = WalPath(dir_);
    const std::string imm = WalImmutablePath(dir_);

    // ① 把当前日志改名成"冻结表的日志"，同一个路径重新开一个空文件
    wal_->Close();
    std::remove(imm.c_str());  // 清掉上一次可能残留的（正常情况已经被删了）
    if (std::rename(cur.c_str(), imm.c_str()) != 0) {
      const Status re = wal_->Open();
      if (!re.ok()) return re;
      return Status::IOError("wal: cannot rename " + cur + " -> " + imm);
    }
    const Status opened = wal_->Open();
    if (!opened.ok()) return opened;

    // ② 刷盘
    const Status flushed = FlushImmutable();
    if (!flushed.ok()) {
      // 刷失败了：imm 日志**留着**，不删。重启会重放它，数据还在。
      // 这就是"删除必须晚于成功"的具体样子。
      return flushed;
    }

    // ③ 落盘成功，这份日志的历史使命结束
    std::remove(imm.c_str());
    return Status::OK();
  }

  // ═══════════════════════════════════════════════════════════
  //  FlushImmutable —— 把冻结的那张内存表倒进一个 .sst 文件
  //
  //  五步：
  //    ① 把所有权接过来（ReleaseImmutable）
  //    ② 建文件
  //    ③ 按 key 顺序遍历这张表，一条条 Add
  //    ④ Finish（写索引区 + footer，fsync）
  //    ⑤ 打开它、挂进 ssts_ —— 这最后一步不能省：
  //       刚写完的数据如果不在内存里的 SSTable 对象里，
  //       下一次 Get 就查不到它（文件在那儿，但没人知道）。
  //
  //  ★ 为什么必须按 key 顺序写？
  //    SSTable 的二分靠"记录有序 + 索引锚点有序"。乱序写进去，
  //    FindRecord 会漏。而我们能保证有序，是因为底下的 SkipList 就是有序的
  //    —— **MemTable 用跳表而不是哈希表，价值在这里兑现。**
  //
  //  ★ 失败了会怎样？
  //    imm 是个 unique_ptr，函数一返回就析构 —— 这张表的数据就真的没了。
  //    但**日志还在**（调用方不会删），所以重启能重放回来。
  //    "内存丢了、日志还在"是可以恢复的；"内存还在、日志删了"才叫丢。
  // ═══════════════════════════════════════════════════════════
  Status FlushImmutable() {
    std::unique_ptr<MemTable> imm = tables_.ReleaseImmutable();
    if (imm == nullptr) return Status::OK();  // 没有待刷的表，不是错误

    const std::string path = SstPath(dir_, next_number_);
    SSTableBuilder builder(path);
    if (!builder.IsOpen()) {
      return Status::IOError("sstable: cannot create: " + path);
    }

    for (SkipList::Iterator it = imm->tree().Begin(); it.Valid(); it.Next()) {
      Record rec;
      rec.type  = it.type();
      rec.key   = it.key();
      rec.value = it.value();
      const Status st = builder.Add(rec);
      if (!st.ok()) return st;
    }

    const Status fin = builder.Finish();
    if (!fin.ok()) return fin;

    std::unique_ptr<SSTable> sst;
    const Status op = SSTable::Open(path, &sst);
    if (!op.ok()) return op;

    ssts_.push_back(std::move(sst));
    ++next_number_;  // ★ 一定要在成功之后才加。失败了下次重写这个号。
    return Status::OK();
  }

  std::string dir_;
  MemTableSet tables_{kMemTableBytesForDemo};
  std::vector<std::unique_ptr<SSTable>> ssts_;  // 旧 → 新（Get 从后往前扫）

  uint64_t next_number_ = 1;  // 下一份 .sst 的编号

  // 唯一的日志句柄，永远指向 wal.log。
  // ★ 注意它只有一个：rotate 时我们把文件改名、复用同一个对象，
  //   而不是"新建一个对象代表新日志"。对象越少，所有权越清楚。
  std::unique_ptr<AppendOnlyLog> wal_;
};

}  // namespace

// ══════════════════════════════════════════════════════════════
//  DB::Open —— 找目录、加载 sst、重放 WAL、打开当前日志
//
//  ★ 顺序为什么是"先 sst、后 wal"？
//    因为它们代表不同的时间。sst 是"很久以前就落定的历史"，
//    wal 是"上次进程死之前还没来得及落盘的部分"。
//    先摆好历史，再把没落地的补上去 —— 后到的覆盖先到的，
//    正好就是"越新的越晚写"这个真实顺序。
//    反过来的话，重放出来的新数据会被老 sst 盖掉。
//
//  ★ 这里用了最笨的加载方式：从 000001 开始试，打不开就停。
//    它有一个明确的缺陷：中间少一个文件，后面的全部读不到。
//    正解是一个记录"我现在有哪些文件"的清单（LevelDB 叫 MANIFEST），
//    那是独立的一课，塞在这里会糊掉。
// ══════════════════════════════════════════════════════════════
Status DB::Open(const std::string& path, DB** dbptr) {
  if (dbptr == nullptr) {
    return Status::InvalidArgument("DB::Open: dbptr is null");
  }

  // ① 目录不存在就建。已存在（EEXIST）不算错。
  if (!path.empty() && MakeDir(path) != 0 && errno != EEXIST) {
    return Status::IOError("DB::Open: cannot create directory: " + path);
  }

  auto db = std::make_unique<LSMDB>(path);

  // ② 加载已有的 SSTable
  uint64_t n = 1;
  for (;; ++n) {
    const std::string p = SstPath(path, n);
    if (!FileExists(p)) break;

    std::unique_ptr<SSTable> sst;
    const Status st = SSTable::Open(p, &sst);
    if (!st.ok()) return st;  // 文件在但读不了 = 真的坏了，必须报
    db->AddSSTable(std::move(sst));
  }
  db->SetNextNumber(n);

  // ③ 重放日志：先 immutable（更老），再 wal.log（更新）
  //
  //    ★ 顺序为什么不能反？
  //      因为"后写的赢"。imm 是上一次 rotate 之前的历史，
  //      wal.log 是那之后新写的。先放历史、再放新的，新的才能盖住旧的。
  //      反过来就是"把旧值当成最新值" —— 又是 resurrect，只是换了个触发条件。
  //
  //    ★ imm 文件重放完之后为什么不删？
  //      因为它对应的数据现在只在内存里，还没落盘。删了就没有第二份了。
  //      它会一直留到下一次 rotate 时被覆盖 —— 那才是它真正安全消失的时刻。
  auto replay_one = [&db](const std::string& p) -> Status {
    if (!FileExists(p)) return Status::OK();
    AppendOnlyLog log(p);
    Status s = log.Load();          // Load 会把尾部残片截掉，那是正常情况
    if (!s.ok()) return s;          // CRC 对不上才是真损坏
    s = db->ReplayRecords(log.entries());
    log.Close();
    return s;
  };

  Status rs = replay_one(WalImmutablePath(path));
  if (!rs.ok()) return rs;
  rs = replay_one(WalPath(path));
  if (!rs.ok()) return rs;

  // ④ 打开 wal.log 继续追加（不存在就创建）
  rs = db->OpenWal(WalPath(path));
  if (!rs.ok()) return rs;

  *dbptr = db.release();
  return Status::OK();
}

}  // namespace lsmkv
