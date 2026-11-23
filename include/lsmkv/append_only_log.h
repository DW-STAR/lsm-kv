#pragma once

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "lsmkv/record.h"
#include "lsmkv/status.h"

namespace lsmkv {

// ─────────────────────────────────────────────────────────────
//  一个「只追加」的键值日志 —— 就是那个笔记本。
//
//  写：永远往末尾加一条，从不回头改
//  读：从后往前翻，第一条匹配的就是最新的
//  删：不是真删，而是再追加一条"墓碑"
//      （因为一旦允许"只追加"，就没办法把旧记录擦掉了）
//
//  这个类不到 40 行，但它就是整个 LSM 的最小骨架。
//  后面所有的东西（WAL / MemTable / SSTable）都是从这上面长出来的。
//
//  ── 历史 ──────────────────────────────────────────────────
//  10-24 已完成：entries_ 从 pair<string,string> 换成 Record，
//                 Load 返回 Status。三项验收全过。
// ─────────────────────────────────────────────────────────────

// ══════════════════════════════════════════════════════════════
//  10-25 任务：把这个类从「内存版日志」变成真正的 WAL
//
//  ★ 先看现在有多荒唐 —— 你现在有两条互不相干的链子：
//
//      链子 A（写）：Append(key, value) → entries_.push_back(...)
//                                          ↑ 只到这里，磁盘上什么都没发生
//
//      链子 B（读）：Load(path) ← 读文件 ← wal.log
//                                        ↑ 谁往这里写的？没人。是 demo_file
//                                          那个独立程序手工写进去的
//
//    也就是说：**这个类从来没写过文件，也从来没读过自己写的东西。**
//    两个 demo 各自跑通，但那不是同一套数据在流动。
//    今天要把两条链接成一圈：
//
//      Append(rec) ──① 编码写文件──→ wal.log
//                  └─② 塞进内存──→ entries_
//      Load(path)  ←─────────────── wal.log   （崩了重启，从盘上重建内存）
//
//    接成一圈之后，才叫「持久化」。接之前，你关机数据就没了。
//
//  ★ 什么是 WAL（Write-Ahead Log）？
//
//    「先把要做的事写进日志，再去改真正的数据。」
//    名字里的 **Ahead = 提前**，指的是"日志必须走在真正的状态前面"。
//    数据库崩了，用日志把没做完的事重放一遍，状态就回来了。
//
//    所以今天代码里那个顺序问题（先写文件还是先写内存），
//    不是"风格偏好"，而是 WAL 这个名字的定义条款：
//    **先落盘，后改内存。反了就不叫 WAL，叫"内存缓存偶尔落盘"。**
//
//  ★ 顺序反了会死成什么样（这张表你自己再推一遍，别背）
//
//    | 做法                | 崩在两步中间时         | 后果                |
//    |---------------------|------------------------|---------------------|
//    | 先写内存，再写文件  | 内存里有了，盘上没有   | 重启后数据**永久消失** |
//    |                     |                        | 但客户端当时被告知"写成功" |
//    | 先写文件，再写内存  | 盘上有了，内存里没有   | 重启 Load 一跑，数据**自己回来** |
//
//    第二种的"损失"只是**当前进程的视角**落后了一下；
//    第一种的损失是**数据本身没了**。前者能自动修复，后者不能。
//    **在存储里，"能不能自愈"比"少丢一点"重要一个数量级。**
// ══════════════════════════════════════════════════════════════

class AppendOnlyLog {
 public:
  // ★ 为什么路径从构造函数传进来？
  //   这个类现在要自己管文件了，它必须知道文件在哪。
  //   以前它是纯内存的，所以不需要 —— 加这个参数本身就是"升级"的证据。
  explicit AppendOnlyLog(std::string path) : path_(std::move(path)) {}
  AppendOnlyLog(const AppendOnlyLog&) = delete;             // 禁止拷贝
  AppendOnlyLog& operator=(const AppendOnlyLog&) = delete;  // 禁止拷贝赋值
  // ★ 为什么 Open 单独一个函数，不塞进构造函数？
  //
  //   C++ 的构造函数**没有返回值**，所以它想报告失败只有一个办法：抛异常。
  //   而 status.h 开头写了，存储代码全程不用异常（错误用 Status 表达）。
  //   两条规矩打架，解法就是：构造函数只做不会失败的事（存下路径），
  //   真正会失败的（开文件）挪到一个返回 Status 的 Open() 里。
  //
  //   这条原则有名字：**构造函数不应该有可能失败的工作。**
  //   （真实工程里同样处理：LevelDB 的 DB::Open 是静态函数返回 Status，
  //     构造函数是私有的。就是为了绕开"构造函数不能失败"这件事。）
  Status Open() {
    // app  = 追加模式：文件已存在就从末尾接着写，不存在就创建。
    //        ⚠️ 千万别用默认模式 —— 默认会**清空**原文件，
    //        数据库第一次重启就把自己的历史抹掉了。
    // binary = 原样字节，别做换行符转换（Windows 会把 0x0A 变成 0x0D 0x0A，
    //        你的长度前缀就全错位了）。
    out_.open(path_, std::ios::app | std::ios::binary);
    if (!out_.is_open()) {
      return Status::IOError("cannot open " + path_ + ": " + std::strerror(errno));
    }
    return Status::OK();
  }

  // ───────────────────────────────────────────────────────────
  //  今天你要写的就一个函数：WriteRecord（在下面 private 区）
  //
  //  Append / Delete 我已经写好 —— 它们现在只干一件事：
  //  造一条 Record，交给 WriteRecord。这样写盘逻辑**全项目只有一份**，
  //  想错也只能错在一个地方，改也只改一处。
  //  （这就是"重复代码是 bug 的温床"的具体样子：两份拷贝，
  //    改了一份忘了另一份，半年后你会花两天找这个 bug。）
  // ───────────────────────────────────────────────────────────

  // 追加一条记录。key 已经存在也不用管 —— 直接再写一条就行。
  // "后写的赢"这件事，是在 Get 的时候用"从后往前找"实现的。
  //
  // ★ 返回类型从 void 变成 Status：写盘会失败（磁盘满、文件被删、只读）。
  //   以前只动内存，push_back 失败基本只有内存耗尽，可以不管；
  //   现在碰硬件了，**必须让调用方知道成没成**。
  Status Append(const std::string& key, const std::string& value) {
    return WriteRecord(Record{RecordType::kPut, key, value});
  }

  // 删除 = 追加一条墓碑记录。
  //
  // 注意：这里绝对不能去 entries_ 里 erase 或者标记某条记录 ——
  // 那样就破坏了"只追加"这个前提，整个设计的地基就没了。
  Status Delete(const std::string& key) {
    return WriteRecord(Record{RecordType::kDelete, key, ""});
  }

  // 读取：从最后一条往前找，第一条 key 匹配的就是最新版本。
  // 如果找到的是墓碑，直接返回 false —— 相当于"这个 key 不存在"。
  bool Get(const std::string& key, std::string* value) const {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
      if (it->key == key) {
        if (it->type == RecordType::kDelete) return false;
        *value = it->value;
        return true;
      }
    }
    return false;
  }

  // 日志里一共有多少条记录。
  // 注意：删除也会让这个数字 +1 —— 因为删除也是一次"写"。
  size_t size() const { return entries_.size(); }

  // 给 demo 用的只读访问。真实工程里不会这么暴露，这是教学项目。
  const std::vector<Record>& entries() const { return entries_; }

  // ══════════════════════════════════════════════════════════════
  //  Load —— 2026-10-31 版：区分「尾部残片」和「中间损坏」
  //
  //  ★ 上一版的 bug（就是 10-30 第五关撞到的 resurrection）
  //
  //    上一版是一个 else break：
  //        if (Decode(...)) push_back;
  //        else             break;      ← 两种完全不同的失败共用一个出口
  //
  //    结果：改坏最后一条 Delete → Decode 失败 → break → 那条被丢掉
  //    → 前面那个 Put 变成最新版本 → **被删的 key 复活了**。
  //    Get("name") 又返回 "liupei"。数据没丢，但它错了，而且不报警。
  //
  //  ★ 现在怎么分
  //
  //    | 现象                        | 成因                    | 处理          |
  //    |-----------------------------|-------------------------|---------------|
  //    | 字段读不全（short_by > 0）  | 进程写到一半被砍（正常）| 截断尾部，返回 OK |
  //    | 读全了但 CRC 不对           | 内容变了（绝不正常）    | 返回 Corruption  |
  //
  //  ★ 为什么"截断"返回 OK 而不是错误？
  //
  //    崩溃恢复是 WAL 的**日常**，不是异常。日志设计里那半条记录
  //    本来就**从未被承诺过**（写它的进程没等到 flush 返回就死了）。
  //    如果这里返回错误，上层通常会拒绝启动 —— 一次 kill -9 就把
  //    数据库变成打不开，那还叫什么崩溃恢复。
  //    所以：丢掉残片、正常启动、但**把截断了多少字节记下来**
  //    （truncated_bytes_）。**正常路径不等于无声无息。**
  //
  //  ★ 为什么中间损坏必须报 Corruption？
  //
  //    因为从这条往后的所有记录，你都**没能力验证**了 ——
  //    文件只有从前往后顺序解析这一条路，一跳过就失去了同步点。
  //    带着不可信的数据继续跑，等于让用户以为数据库是好的。
  //    **宁可不启动，也不能让引擎带着它自己都不信的数据对外服务。**
  //
  //  ★ 为什么截断时不去改文件（truncate 到最后一个好记录的末尾）？
  //
  //    因为恢复路径上的任何写操作，都可能让你从「崩过一次」变成
  //    「崩过两次」。这条路径必须尽可能只读。
  //    LevelDB 也是这么处理的：重新打开时以追加模式继续写，
  //    残片永远留在文件里，但再也不会被读到。
  // ══════════════════════════════════════════════════════════════
  Status Load() {
    errno = 0;
    std::ifstream in(path_, std::ios::binary);
    if (!in.is_open()) {
      if (errno == ENOENT) return Status::OK();
      return Status::IOError(std::string("open failed: "));
    }
    std::string all;
    char chunk[1024];
    while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0) {
      all.append(chunk, static_cast<size_t>(in.gcount()));
    }

    entries_.clear();
    truncated_bytes_ = 0;

    size_t offset = 0;
    while (offset < all.size()) {
      size_t consumed = 0;
      size_t short_by = 0;
      Record record;

      const Status st = Decode(all, offset, &record, &consumed, &short_by);
      if (st.ok()) {
        entries_.push_back(record);
        offset += consumed;
        continue;
      }

      if (short_by > 0) {
        // 字段读不全：头部声称的长度越过了文件末尾。
        // 最常见的成因就是「写到一半被砍」—— 截断，正常启动。
        truncated_bytes_ = all.size() - offset;
        return Status::OK();
      }

      // 读全了但校验不过：内容变了。后面已经不可信，拒绝继续。
      return Status::Corruption("corrupted record at offset " +
                                std::to_string(offset) + ": " +
                                st.ToString());
    }
    return Status::OK();
  }

  // 上一次 Load 在文件尾部丢掉了多少字节（0 表示没丢）。
  // 崩溃恢复时这个数一般正好等于「没写完的那条记录的已有长度」。
  size_t truncated_bytes() const { return truncated_bytes_; }

  // 关文件。真正需要它的场合不多（进程退出时析构函数会替你做），
  // 但 demo 里要在"同一个进程内换另一个对象读这个文件"时，
  // 一定要先把这边关掉 —— 否则数据还在缓冲区里，对面读到的是空的。
  void Close() { out_.close(); }

  // 这个日志对应的文件路径。
  //
  // ★ 为什么需要它（2026-11-23 加）：
  //   接上 DB 之后，日志和内存表是**成对**存在的 —— 一张表刷盘完成，
  //   它对应的那份日志就可以删了。删除需要知道路径，而路径是日志自己的事，
  //   不该让 DB 那边再算一遍（算一遍就是两处维护同一份命名规则）。
  const std::string& path() const { return path_; }

 private:
  // ══════════════════════════════════════════════════════════════
  //  ★ 今天唯一的 TODO：WriteRecord
  //
  //  它要做四件事，**顺序不能换**：
  //
  //    ① 把 rec 编码成字节串        （Encode 你会了）
  //    ② 把那串字节写进文件          （write）
  //    ③ 把用户态缓冲区推给内核      （flush）
  //    ④ 前面三步都成功了，才 entries_.push_back(rec)
  //
  //  ★ 为什么必须有 ③ flush？—— 这是今天第二个"底层概念"
  //
  //    你调用 out_.write(...) 的时候，**数据并没有到磁盘**。
  //    ofstream 内部有一块自己管理的缓冲区（默认几 KB），
  //    write 只是把字节从你的 std::string 拷进这块缓冲区，
  //    真正的系统调用（write(2)）要等缓冲区满了才发一次。
  //    这么做是为了减少系统调用次数 —— 每次进内核都要保存现场，
  //    一个字节一个字节地写会慢几百倍。
  //
  //    代价是：**数据卡在用户态缓冲区里，进程一崩就跟着没了。**
  //    而 WAL 的全部意义就是"崩了也能恢复"，所以这里必须主动 flush，
  //    把缓冲区的数据推给内核。
  //
  //    再往下还有一层（现在不做，知道有这回事就行）：
  //    flush 只是把数据交给了内核，**内核的页缓存里还躺着**，
  //    真正写到盘上要 fsync(2)。断电的话内核缓存也会丢。
  //    工业级 WAL 是 write + fsync 的组合，代价是 fsync 极慢
  //    （机械盘一次 5~10ms）。LevelDB 用 `sync = false` 当默认值，
  //    靠操作系统的刷盘策略兜底 —— 这就是"性能 vs 安全"的真实取舍点。
  //
  //  ★ 为什么 ② ③ 失败了就不能进 ④？
  //
  //    进了就变成"内存说有、磁盘说没有"的**假数据**：
  //    Get 查得到，客户端以为存住了，重启一次凭空消失。
  //    这种 bug 最难查 —— 因为它在"写成功的瞬间"是完全正常的。
  //    **宁可让这次写失败并报错，也不能让内存和磁盘各说一套。**
  //
  //  ── 可用的积木 ──────────────────────────────────────────────
  //    Encode(rec)                           → std::string 字节串
  //    out_.write(ptr, n)                    写 n 个字节，n 是 std::streamsize
  //    out_.flush()                          把缓冲区推给内核
  //    static_cast<std::streamsize>(x)        size_t → streamsize（有符号）
  //    out_ 能不能用：直接 `if (!out_)` 判断
  //        （ofstream 有隐式 bool 转换，等价于 !fail()，
  //          写失败 / 磁盘满 都会让它变成 false）
  //
  //  ── 验收（必须给出数字，不许只说"跑通了"）───────────────────
  //    跑 src/demo_wal.cpp，四个数都要对上：
  //      ① 3 次 Append + 1 次 Delete 后，size() == 4
  //      ② 文件字节数 == 四条记录编码后长度之和（程序会算给你看）
  //      ③ 换一个全新的对象 Load 同一个文件 → size() == 4
  //      ④ 新对象 Get("name") 拿到 "liupei_v2"，Get("score") 返回 false
  //
  //  ④ 是今天的核心：**条数对不代表数据对**。
  //    只验 size() 的话，把 key/value 写乱序也能"通过"。
  //    永远要验一个"内容层面的正确性"，否则就是在做废法③。
  // ══════════════════════════════════════════════════════════════
  Status WriteRecord(const Record& rec) {
    std::string encoded = Encode(rec);
    out_.write(encoded.data(),static_cast<std::streamsize>(encoded.size()));
    if(!out_) return Status::IOError("Write failed");
    out_.flush();
    if(!out_) return Status::IOError("Flush failed");
    entries_.push_back(rec);
    return Status::OK();
  }

  std::string path_;
  // 上次 Load 在文件尾部丢掉的字节数（0 = 没丢）。崩溃恢复的"体检报告"。
  size_t truncated_bytes_ = 0;
  // 常驻的文件句柄。为什么不在每次写的时候 open/close？
  // open 是一次系统调用，要查目录、分配内核对象、加载元数据，
  // 比一次 write 贵一两个数量级。逐条写日志的场合，
  // 每次开关文件会让吞吐掉一个数量级 —— 这是"打开一次、复用到底"的典型场景。
  std::ofstream out_;
  // 内存里的那份数据。和磁盘上的记录同构：type + key + value。
  std::vector<Record> entries_;
};

}  // namespace lsmkv
