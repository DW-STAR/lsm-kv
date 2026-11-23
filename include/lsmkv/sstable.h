#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <io.h>  // _commit / _fileno
#else
#include <unistd.h>  // fsync / fileno
#endif

#include "lsmkv/record.h"
#include "lsmkv/status.h"

namespace lsmkv {

// ══════════════════════════════════════════════════════════════
//  SSTable —— 2026-11-14（Week 5 第一天）
//
//  ── 【术语】SSTable = Sorted String Table（有序字符串表）。
//     读作 "S-S-table"。LSM 里的"磁盘表"，官方的叫法。
//
//  ── 【定义】一个文件，里面是一堆记录，**按 key 从小到大排好**。
//     就这么简单。它是一个【不可变】文件：写完之后再也不改。
//
//  ── 【为什么是「有序」这两个字】
//
//     因为 MemTable 用的是 SkipList —— 它本来就把数据按 key 排好了。
//     倒出来天然有序，我们一分额外力气都不用花。
//     而这一份"顺手的秩序"，是后面所有性能的来源：
//
//        · 查一条不用扫全表 —— 可以二分（下次做索引）
//        · 合并两个文件不用建哈希表 —— 两边从头对撞就行（compaction）
//        · 同一个 key 的新旧版本必然挨着 —— 谁新谁旧一目了然
//
//     ★ 所以「有序」不是格式的装饰，是这个数据结构存在的理由。
//       一旦写进一条乱序的记录，上面三件事全部失效 ——
//       而且文件照样能读、能查得到（只要全扫），错得极其安静。
//       所以 Add 里必须有一道检查，见 TODO 10。
//
//  ── 【和 WAL 的区别，一句话】
//
//       WAL 是「先记下来，崩了能重放」，它只要求顺序追加；
//       SSTable 是「排好序存起来，以后要能查」，它要求有序 + 可定位。
//       同一个 Encode 格式，两种完全不同的用途。
//
//  ── 【文件布局 v1（今晚版，没有索引）】
//
//      ┌────────────────────────────────┐ 0
//      │ record 0                       │
//      │ record 1                       │   每条都是 record.h 的 Encode 格式
//      │ ...                            │   [CRC 4B][type 1B][klen 4B][key]
//      │ record N-1                     │   [vlen 4B][value]
//      ├────────────────────────────────┤ data_end
//      │ footer                         │ 固定 24 字节，见下
//      └────────────────────────────────┘ 文件尾
//
//      footer 布局：
//        [0..8)    uint64 data_end    数据区最后一个字节之后的位置
//        [8..16)   uint64 count       记录条数
//        [16..24)  uint64 magic       印章，确认这是个 SSTable
//
//  ── 【为什么 footer 必须固定长度、必须放在最后】
//
//     打开文件时你只知道"文件有多长"，不知道里面有什么。
//     想找到数据的边界，就得先读 footer；想读 footer，
//     就必须知道它从哪儿开始 —— 只有"最后 24 字节"这个说法不依赖任何前置信息。
//
//     ★ 这是所有"从后往前读"的格式的通例：**尾巴必须是定长的**。
//       一旦 footer 变长，你就得先读一个变长的头才知道 footer 在哪，
//       而那又需要先读 footer —— 死循环。
//       （record.h 把 CRC 放最前面，是同一个思路的镜像：先拿到锚点。）
//
//  ★ 今晚这个版本【没有索引】。查一条 key 要把整个文件线性扫一遍。
//    这不是偷懒，是刻意的第一步：先证明「写出去能原样读回来」，
//    再谈「怎么少读一点」。demo 的 [6] 会把全扫有多慢实测给你看，
//    那个数字就是下次做索引的理由。
// ══════════════════════════════════════════════════════════════

// 文件末尾的印章。随便挑的一个 8 字节常数，只要全项目唯一即可。
//
// ★ 它挡的是什么？挡的是「有人把 wal.log 或者一张图片当成 SSTable 打开」。
//   没有它，那种情况会表现成一堆莫名其妙的 Corruption，很难查。
//   有了它，第一句话就能说清"这不是 SSTable 文件"。
// ══════════════════════════════════════════════════════════════
//  ★ 11-17 v2：magic 拆成「家族 + 版本」两半
//
//    0x4C534D53_53543032
//    └────┬───┘ └──┬───┘
//       家族     版本
//      "LSMS"   "ST02"
//
//  ★ 为什么要拆？
//    拆开之后，读到 v1 的文件可以精确说出「版本不对」，而不是笼统的
//    「magic 不对」。这两种错误要做的事完全不同：
//
//      · 家族不对 → 这不是我的文件（可能传错了路径、传了 wal.log）
//      · 版本不对 → 这是我的文件，但我读不懂（要兼容，或者让它重建）
//
//    **报错的价值在于它能区分多少种情况。** 一句话盖住两种故障，
//    等于把诊断工作丢给下一个人。
// ══════════════════════════════════════════════════════════════
inline constexpr uint64_t kSSTableMagicFamily = 0x4C534D5300000000ULL;  // "LSMS"
inline constexpr uint64_t kSSTableMagic = 0x4C534D5353543032ULL;        // "LSMS" + "ST02"

// 取 magic 高 32 位（家族部分）用的掩码。
// 加它就是为了一句话：`magic & 掩码` 能单独把家族拎出来比。
inline constexpr uint64_t kSSTableMagicFamilyMask = 0xFFFFFFFF00000000ULL;

// footer 定长 40 字节 = 五个 uint64（v1 是 24 字节 = 三个）。
//
// ★ v2 比 v1 多了两样东西：数据区后面的一段索引，和 footer 里的两个新字段。
//   记录本身的格式【一个字节都没变】—— 这就是分层的价值：
//   加索引不需要动 record.h。
inline constexpr size_t kFooterSize = 40;

// ══════════════════════════════════════════════════════════════
//  索引：每 16 条记录记一个锚点
//
//  ★ 为什么是「每 N 条」而不是「每一条」？
//    每条都记，索引本身就和数据一样大了，等于把整个文件又存一份。
//    每 16 条记一个：索引是数据量的 1/16，而查询最多扫 16 条。
//
//    **这就是「稀疏」两个字的全部含义：用少量锚点换 log(N) 的定位。**
//
//  ★ 代价是什么？
//    查询一条记录，除了二分 log(N/16) 次，还要在块内线性扫最多 16 条。
//    16 这个数是权衡：调小 → 索引变大但扫得快；调大 → 索引小但扫得慢。
//    LevelDB 按「每 4KB 一个块」来切，本质是同一个东西，只是用字节数而不是条数。
// ══════════════════════════════════════════════════════════════
inline constexpr size_t kIndexInterval = 16;

// 一条索引项：这一块的第一个 key + 它在 data 区的起始偏移。
struct IndexEntry {
  std::string key;
  uint64_t offset = 0;
};

// ── 64 位定长读写（record.h 里只有 32 位的，这里补上）────────────
//
// ★ 为什么这里要 8 字节而不是 4 字节？
//   4 字节最大 4GB。单机 SSTable 一般不会到 4GB，但 offset 用 32 位
//   会在"算着算着就溢出"的时候静默出错，而 8 字节的成本是 4 个字节。
//   **长度/偏移这类字段，宁可宽一档，不要刚好够用。**
inline void PutFixed64(std::string* dst, uint64_t v) {
  char buf[8];
  std::memcpy(buf, &v, 8);
  dst->append(buf, 8);
}

inline uint64_t GetFixed64(const char** p, const char* limit) {
  if (*p + 8 > limit) return 0;
  uint64_t v;
  std::memcpy(&v, *p, 8);
  *p += 8;
  return v;
}

// magic 打成 16 位十六进制，只为让错误信息里能直接看出「读到了什么」。
//
// ★ 为什么不直接把 magic 丢进错误信息？
//   0x4C534D5353543032 这种数字人眼根本对不上，你还得自己去查对照表。
//   **报错是给人看的，不是给编译器看的。**
inline std::string ToHexU64(uint64_t v) {
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llX", static_cast<unsigned long long>(v));
  return std::string(buf);
}

// 把 magic 的低 32 位当成 4 个 ASCII 字符取出来（"ST02"）。
// 不可打印的字节换成 '?' —— 文件被改坏时 magic 会是随机值，别让控制字符
// 直接打到终端上。
inline std::string MagicVersion(uint64_t magic) {
  const uint32_t v = static_cast<uint32_t>(magic & 0xFFFFFFFFULL);
  std::string s;
  for (int i = 24; i >= 0; i -= 8) {
    const unsigned char c = static_cast<unsigned char>((v >> i) & 0xFF);
    s.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '?');
  }
  return s;
}

// 把 FILE* 的缓冲区真正推给操作系统。
//
// ★ 这是第三层：你的 std::string → FILE 缓冲 → 内核 page cache → 设备。
//   fflush 只做到第二层，_commit / fsync 才推到第四层。
//   （demo_flush_cost 里量过的那个 2ms，就是这一句的时间。）
inline int SyncFile(std::FILE* f) {
#ifdef _WIN32
  return _commit(_fileno(f));  // <io.h>
#else
  return ::fsync(fileno(f));
#endif
}

// ══════════════════════════════════════════════════════════════
//  SSTableBuilder —— 写的一方
//
//  ── 【用法】
//
//      SSTableBuilder b("build_win/000001.sst");
//      b.Add(record0);        // 必须 key 递增
//      b.Add(record1);
//      ...
//      b.Finish();            // 写 footer + 落盘 + 关文件
//
//  ── 【为什么是 Builder 模式，不是「一把梭 WriteAll(vector)」】
//
//    因为上游是 MemTable 的迭代器 —— 数据是一条一条来的，
//    你没法先收集成一个 vector 再写（那等于在内存里又存一份）。
//    Builder 就是"边来边写"的形状：来一条，写一条，只留一个
//    上一条的 key 用来检查顺序。
//
//    ★ 这也解释了为什么 Builder 要记住 last_key_：
//      有序是【跨记录】的约束，只检查单条记录查不出来，
//      必须记住上一条才知道"这一条有没有倒退"。
// ══════════════════════════════════════════════════════════════
class SSTableBuilder {
 public:
  explicit SSTableBuilder(const std::string& path) : path_(path) {
    file_ = std::fopen(path.c_str(), "wb");
  }

  // 没 Finish 就析构：关文件，但【不补写 footer】。
  //
  // ★ 于是这个文件打开时会报 Corruption —— 这是**故意的**。
  //   半成品 SSTable 绝不能长得像个好文件：一个"看起来完整、其实少了几条"
  //   的表比一个明确读不开的表危险一百倍。
  //   要么整份交出去，要么明确作废，没有中间状态。
  ~SSTableBuilder() {
    if (file_ != nullptr) std::fclose(file_);
  }

  SSTableBuilder(const SSTableBuilder&) = delete;
  SSTableBuilder& operator=(const SSTableBuilder&) = delete;

  bool IsOpen() const { return file_ != nullptr; }
  size_t record_count() const { return count_; }

  // ═══════════════════════════════════════════════════════════
  //  TODO 10（你写）：Add —— 追加一条记录
  //
  //  五步，顺序不能变：
  //
  //    ① 没打开 / 已经 Finish 过 → 返回错误
  //         if (file_ == nullptr) return Status::IOError("sstable: not open: " + path_);
  //
  //    ② 【有序守卫】不是第一条，且这条 key <= 上一条 → 拒绝
  //         if (count_ > 0 && record.key <= last_key_) {
  //           return Status::InvalidArgument("sstable: key order violated: " + record.key);
  //         }
  //
  //    ③ 编码成字节：  const std::string buf = Encode(record);
  //
  //    ④ 写进文件，并检查"真的写完了"
  //         const size_t n = std::fwrite(buf.data(), 1, buf.size(), file_);
  //         if (n != buf.size()) return Status::IOError("sstable: short write");
  //
  //    ⑤ 记账：偏移要涨、条数要加、last_key_ 要更新
  //         offset_ += buf.size();
  //         ++count_;
  //         last_key_ = record.key;
  //         return Status::OK();
  //
  //  ★★ ② 是今晚最重要的一行，想清楚再写 ★★
  //
  //     不检查会怎样？文件照样写成功、照样能读回来、测试照样全绿 ——
  //     唯一的症状是「查某几条数据时查不到」，而且只在数据量大的时候出现。
  //     因为一旦无序，后面所有依赖"有序"的东西（二分索引、compaction 归并）
  //     都会给出**错误答案而不是报错**。
  //
  //     **宁可写失败，不要写出一个静默错误的文件。**
  //     这条原则在存储里没有例外。
  //
  //  ★ 为什么用 `<=` 而不是 `<`？
  //     同一个 key 出现两次就是无序（也是上游有 bug 的信号）。
  //     MemTable 里一个 key 只会有一条（SkipList 覆盖），
  //     所以这里出现重复 = 上游搞错了，必须挡住而不是"随便留一个"。
  //
  //  ★ 为什么 fwrite 之后要查 n？缓冲区满了、磁盘满了，fwrite 会短写。
  //     不查的话你会在"只写了一半"的情况下返回 OK，然后 offset_ 也记错，
  //     后面全部错位。**每个 I/O 都要检查返回值，这不是强迫症。**
  // ═══════════════════════════════════════════════════════════
  Status Add(const Record& record) {
    if(file_ == nullptr) return Status::IOError("sstable not open" + path_);
    if(count_ > 0 && record.key <= last_key_){
      return Status::InvalidArgument("sstable: key order violated: " + record.key);
    }
    // ═══════════════════════════════════════════════════════════
    //  TODO 14（你写）：记一个索引锚点 —— 三行
    //
    //  在这条记录真正写进文件【之前】，判断它是不是"每 16 条的第一个"：
    //
    //      if (count_ % kIndexInterval == 0) {
    //        index_.push_back({record.key, offset_});
    //      }
    //
    //  ★ 两个细节，错一个就全歪：
    //
    //    ① 用 offset_，不是 offset_ + buf.size()
    //       offset_ 是"这条记录将要写入的位置"，也就是这一块的起点。
    //       索引里存的是块的开头，不是块里某一条的位置。
    //
    //    ② 必须在 fwrite 之前记（此时 offset_ 还没涨）
    //       写在 offset_ += 之后，记下的就成了下一条的位置，整条索引错位一格。
    //       —— 这种"差一条"的 bug 不崩、不报错，只是某些 key 查不到。
    //
    //  ★ 为什么用 count_ % 16 == 0，不另开一个计数器？
    //    count_ 本来就是只增的累计数，取模就够了。
    //    **能用已有状态算出来的东西，不要再存一份** —— 两份数据迟早不同步。
    // ═══════════════════════════════════════════════════════════
    if (count_ % kIndexInterval == 0) {
        index_.push_back({record.key, offset_});  
    }

    std::string buf = Encode(record);
    const size_t n = std::fwrite(buf.data(), 1, buf.size(), file_);
    if (n != buf.size()) return Status::IOError("sstable: short write");
    offset_ += buf.size();
    ++count_;
    last_key_ = record.key;
    return Status::OK();
  }

  // ═══════════════════════════════════════════════════════════
  //  TODO 11（你写）：Finish —— 收尾，让文件变成"完整的"
  //
  //  四步，顺序不能变：
  //
  //    ① 没打开 → 报错
  //
  //    ② 拼 footer（24 字节，顺序就是上面的布局）
  //         std::string footer;
  //         PutFixed64(&footer, offset_);        // data_end
  //         PutFixed64(&footer, count_);         // count
  //         PutFixed64(&footer, kSSTableMagic);  // magic
  //       然后 fwrite 进文件，同样检查返回值。
  //
  //    ③ 三层都要刷
  //         std::fflush(file_);              // 用户态缓冲 → 内核
  //         SyncFile(file_);                 // 内核 → 设备
  //         std::fclose(file_);              // 关文件
  //         file_ = nullptr;                 // ★ 立刻置空
  //
  //    ④ offset_ += kFooterSize;
  //
  //  ★ 为什么 file_ 关完必须立刻置 nullptr？
  //     不置空的话，之后误调 Add 会往一个已关闭的 FILE* 上写 ——
  //     那是未定义行为，可能崩、也可能"成功"写进一个已经不属于你的缓冲区。
  //     **悬空指针的定义就是这样来的：指针还在，指向的东西已经没了。**
  //     置空是最便宜的保险：至少下一次调用会走 "not open" 分支。
  //
  //  ★ 为什么 fflush 和 SyncFile 都要？
  //     fflush 只把 C 库的缓冲区交给操作系统，数据还在 page cache 里。
  //     这时候断电，数据就没了 —— 但文件看起来是"写完了"的。
  //     这就是「写成功」和「落盘」的区别，Week 4 量过那个 2ms。
  //
  //  ★ 顺序能不能换成先关文件再 commit？
  //     不能。fclose 之后 FILE* 就没了，你再也拿不到它的 fd。
  // ═══════════════════════════════════════════════════════════
  Status Finish() {
    if (file_ == nullptr) {
      return Status::IOError("sstable: not open: " + path_);
    }

    // 数据区到此为止；索引紧接着它开始。
    //
    // ★ 这两个值现在【必然相等】（中间什么都没有）。
    //   那为什么还记两份？
    //     ① 交叉验证 —— 不相等就说明文件被改过或有空洞
    //     ② 将来 data 和 index 中间会插东西（布隆过滤器、统计信息），
    //        到那天它们就分开了，格式不用再改一次
    const uint64_t data_end = offset_;
    const uint64_t index_offset = offset_;

    // ═══════════════════════════════════════════════════════════
    //  TODO 15（你写）：把索引区写进去
    //
    //  对 index_ 里每一条，编码成 [klen 4B][key][offset 8B]，追加到文件：
    //
    //      for (const IndexEntry& e : index_) {
    //        std::string buf;
    //        PutFixed32(&buf, static_cast<uint32_t>(e.key.size()));
    //        buf.append(e.key);
    //        PutFixed64(&buf, e.offset);
    //
    //        const size_t n = std::fwrite(buf.data(), 1, buf.size(), file_);
    //        if (n != buf.size()) return Status::IOError("sstable: short write (index)");
    //
    //        offset_ += buf.size();
    //      }
    //
    //  ★ 为什么 key 前面要记 klen？因为 key 是变长的。
    //    定长就得截断（错）或者留一大片空白（浪费）。
    //    「变长 + 长度前缀」和 record.h 里的一条记录是同一个套路。
    //
    //  ★ 为什么写完要 offset_ += ？
    //    因为下面的 footer 要记「整个文件到哪儿结束」。
    //    offset_ 不跟着涨，你写出来的 footer 就会认为索引不存在。
    //
    //  ★ 为什么索引项里不存 value？
    //    因为索引的作用是「定位」，不是「取数」。
    //    定位到块之后，value 从数据区里读 —— 读一份就够了，不要存两份。
    //    （B+ 树的非叶子节点也只存 key 不存 value，同一个道理。）
    // ═══════════════════════════════════════════════════════════
    for(const IndexEntry& e:index_){
      std::string buf;
      PutFixed32(&buf,static_cast<uint32_t>(e.key.size()));
      buf.append(e.key);
      PutFixed64(&buf,e.offset);
      const size_t n = std::fwrite(buf.data(),1,buf.size(),file_);
      if(n != buf.size()) return Status::IOError("sstable short write (index)");
      offset_ += buf.size();
    }

    std::string footer;
    PutFixed64(&footer, data_end);
    PutFixed64(&footer, index_offset);
    PutFixed64(&footer, static_cast<uint64_t>(index_.size()));
    PutFixed64(&footer, static_cast<uint64_t>(count_));
    PutFixed64(&footer, kSSTableMagic);

    const size_t nf = std::fwrite(footer.data(), 1, footer.size(), file_);
    if (nf != footer.size()) {
      return Status::IOError("sstable: short write (footer)");
    }
    offset_ += kFooterSize;

    std::fflush(file_);
    SyncFile(file_);
    std::fclose(file_);
    file_ = nullptr;

    return Status::OK();
  }

  // 已写入的字节数（含 footer）。测试用。
  uint64_t file_size() const { return offset_; }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  std::FILE* file_ = nullptr;
  uint64_t offset_ = 0;   // 已经写了多少字节 = 下一条记录该写在哪
  size_t count_ = 0;      // 已经写了多少条
  std::string last_key_;  // 上一条的 key，只为了检查顺序
  // 锚点表：每 16 条记录的第一条（key + 它在该文件里的偏移）。
  // ★ 它在内存里只占很小一块（N/16 条），所以可以一直留着，直到 Finish 才落盘。
  std::vector<IndexEntry> index_;
};

// ══════════════════════════════════════════════════════════════
//  SSTable —— 读的一方
//
//  ── 【今晚的实现方式，以及它为什么"不对"】
//
//    今晚 Open 会把【整个文件读进内存】，然后 Get 在这块内存里线性扫。
//    这是最笨的做法，真实引擎里完全不可接受：
//
//      · 一个 SSTable 可能 2MB ~ 几百 MB，全部读进内存 = 内存爆掉
//      · 每查一条 key 都要扫整个文件 = O(N)，几万条就是几毫秒
//
//    为什么要先这么写？因为**今晚要验证的不是性能，是字节格式**。
//    写出去 → 读回来 → 每个字节都对得上，这件事成立了，
//    后面的索引、分块、mmap 才有意义。
//
//    ★ 先把数据搬对，再谈怎么少搬。顺序反了会同时抓两只兔子。
//
//  ── 【下次要改的三件事】
//
//     ① 索引：每 N 条记一个 (key → offset)，先在索引里二分，再进文件
//     ② 分块：数据按 4KB 块切，每块自带校验 —— 也是 record.h 里
//        那个"长度字段坏了无法定位"盲区的正解
//     ③ 按需读：只 pread 需要的那几 KB，不把整个文件读进内存
// ══════════════════════════════════════════════════════════════
class SSTable {
 public:
  // ═══════════════════════════════════════════════════════════
  //  TODO 12（你写）：Open —— 打开一个 SSTable
  //
  //  ① 把整个文件读进 data_（用 fread 循环，块大小 8192 就行）
  //         std::FILE* f = std::fopen(path.c_str(), "rb");
  //         if (f == nullptr) return Status::IOError("sstable: cannot open: " + path);
  //         char buf[8192];
  //         size_t n;
  //         while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) self->data_.append(buf, n);
  //         std::fclose(f);
  //
  //  ② 文件太短（连 footer 都装不下）→ Corruption
  //
  //  ③ 读 footer，检查 magic
  //         const char* p = self->data_.data() + self->data_.size() - kFooterSize;
  //         const char* limit = self->data_.data() + self->data_.size();
  //         self->data_end_ = GetFixed64(&p, limit);
  //         self->count_    = GetFixed64(&p, limit);
  //         const uint64_t magic = GetFixed64(&p, limit);
  //         if (magic != kSSTableMagic) return Status::Corruption("sstable: bad magic: " + path);
  //
  //  ④ 检查 data_end 没越界（data_end_ <= data_.size() - kFooterSize）
  //
  //  ⑤ *out = std::move(self); return Status::OK();
  //
  //  ★ 为什么要检查 magic 而不是"字节数对得上就算成功"？
  //    因为"对得上"是巧合的概率远比你想象的高。
  //    一个明确的印章，把"可能是别的文件"这种模糊情况变成一条确定的错误。
  //
  //  ★ 为什么先检查长度再读 footer？
  //    文件只有 10 个字节时，`data_.data() + size - 24` 会算到
  //    缓冲区【前面】去 —— 那不是"读到垃圾"，是直接越界（UB）。
  //    **所有"从末尾往回退 N 字节"的代码，前面都必须有一句 size >= N。**
  // ═══════════════════════════════════════════════════════════
  static Status Open(const std::string& path, std::unique_ptr<SSTable>* out) {
    // ── ① 入参
    if (out == nullptr) return Status::InvalidArgument("sstable: out is null");

    // ── ② 读整个文件
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return Status::IOError("sstable: cannot open: " + path);

    auto self = std::make_unique<SSTable>();
    char buf[8192];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
      self->data_.append(buf, n);
    }
    // ★ fread 返回 0 有两种原因：读到文件尾，或者读出错。
    //   只看 `while (n > 0)` 区分不出来 —— 出错时你会拿着半份数据继续走。
    const bool read_failed = (std::ferror(f) != 0);
    std::fclose(f);
    if (read_failed) return Status::IOError("sstable: read error: " + path);

    const uint64_t file_size = self->data_.size();

    // ── ③ 长度必须装得下 footer（否则下面那次指针相减会跑到缓冲区前面去）
    if (file_size < kFooterSize) {
      return Status::Corruption("sstable: file is " + std::to_string(file_size) +
                                " bytes but footer needs " +
                                std::to_string(kFooterSize) + ": " + path);
    }

    const char* const base = self->data_.data();
    const char* const limit = base + file_size;
    const uint64_t footer_start = file_size - kFooterSize;

    // ── ④ 读 footer 五个字段（顺序必须和 Finish 里 PutFixed64 的顺序严格一致）
    const char* p = limit - kFooterSize;
    self->data_end_             = GetFixed64(&p, limit);
    const uint64_t index_offset = GetFixed64(&p, limit);
    const uint64_t index_count  = GetFixed64(&p, limit);
    self->count_                = GetFixed64(&p, limit);
    const uint64_t magic        = GetFixed64(&p, limit);

    // ── ⑤ magic：先分家族，再分版本
    //
    // ★ 为什么要两级？
    //   「不是我的文件」和「是我的文件但版本读不懂」要采取的动作完全不同：
    //     前者 → 你路径传错了，去查调用方
    //     后者 → 该做格式迁移，或者让它重建成新版本
    //   一句话盖住两种情况，等于把诊断工作丢给下一个人。
    if ((magic & kSSTableMagicFamilyMask) != kSSTableMagicFamily) {
      return Status::Corruption("sstable: not an sstable file (magic 0x" +
                                ToHexU64(magic) + "): " + path);
    }
    if (magic != kSSTableMagic) {
      return Status::NotSupported("sstable: format version \"" + MagicVersion(magic) +
                                  "\" is not supported (this build reads \"" +
                                  MagicVersion(kSSTableMagic) + "\"): " + path);
    }

    // ── ⑥ 三段边界必须自洽：data | index | footer
    //
    //   违规情况只有两种：重叠、越界。这里不要求 data_end == index_offset ——
    //   将来 data 和 index 中间会插布隆过滤器，那天它们就该不相等。
    //   **检查只挡真故障，不挡「和我想的不一样」。**
    if (self->data_end_ > index_offset) {
      return Status::Corruption("sstable: data_end " + std::to_string(self->data_end_) +
                                " overlaps index at " + std::to_string(index_offset) +
                                ": " + path);
    }
    if (index_offset > footer_start) {
      return Status::Corruption("sstable: index_offset " + std::to_string(index_offset) +
                                " runs past footer at " + std::to_string(footer_start) +
                                ": " + path);
    }

    // ── ⑦ index_count 是文件里的数，属于【不可信输入】
    //
    //   不先校验就 reserve，一个被改成 0xFFFFFFFF 的字段能让进程直接
    //   bad_alloc。每一项至少 4(klen) + 0(空 key) + 8(offset) = 12 字节，
    //   拿它当上限比一比就够了。
    const uint64_t index_area = footer_start - index_offset;
    if (index_count > index_area / 12) {
      return Status::Corruption("sstable: index_count " + std::to_string(index_count) +
                                " cannot fit in " + std::to_string(index_area) +
                                " bytes: " + path);
    }

    // ── ⑧ 解析索引区
    const char* q = base + index_offset;
    const char* const qlimit = base + footer_start;

    self->index_.reserve(static_cast<size_t>(index_count));
    for (uint64_t i = 0; i < index_count; ++i) {
      // ★ 这两个检查不能省。GetFixed32 / GetFixed64 越界时【返回 0 且不推游标】——
      //   它们不是校验函数，是搬运函数。指望它们报错，你会拿到一个假的 0，
      //   然后在原地死循环。
      if (q + 4 > qlimit) {
        return Status::Corruption("sstable: index entry " + std::to_string(i) +
                                  " truncated (no klen): " + path);
      }
      const uint32_t klen = GetFixed32(&q, qlimit);
      if (q + klen + 8 > qlimit) {
        return Status::Corruption("sstable: index entry " + std::to_string(i) +
                                  " overruns index area (klen=" +
                                  std::to_string(klen) + "): " + path);
      }

      IndexEntry e;
      e.key.assign(q, klen);
      q += klen;
      e.offset = GetFixed64(&q, qlimit);

      if (e.offset >= self->data_end_) {
        return Status::Corruption("sstable: index entry " + std::to_string(i) +
                                  " points to " + std::to_string(e.offset) +
                                  ", outside the data area: " + path);
      }
      // 索引本身也必须有序 —— 二分的前提。乱序的索引不会报错，
      // 只会让某些 key 查不到，和 Add 里那道有序守卫是同一个理由。
      if (!self->index_.empty() && !(self->index_.back().key < e.key)) {
        return Status::Corruption("sstable: index keys out of order at entry " +
                                  std::to_string(i) + " (\"" +
                                  self->index_.back().key + "\" then \"" + e.key +
                                  "\"): " + path);
      }
      self->index_.push_back(std::move(e));
    }

    // ── ⑨ 交出去
    *out = std::move(self);
    return Status::OK();
  }

  // ── 查一条记录（引擎视角：墓碑也如实交出来）──────────────
  //
  // ★ 这个函数是【已经写好】的，你不用改 —— 但必须读一遍。
  //   它把 SSTable 的"笨"暴露得很清楚：从文件头一路 Decode 下去，
  //   碰到 key 相等就返回。有序的话可以在 key 比目标大的时候提前停，
  //   但本质上还是 O(N)。
  //
  // ★ 注意它用的是 Decode 的 consumed 出参往前走，而不是自己算长度。
  //   长度是 Encode 时定的规矩，只有 Decode 知道怎么解释 ——
  //   调用方自己算一遍，就是在两个地方维护同一份格式定义，
  //   改格式时必然漏一处。**格式的解析权归负责解析的那个函数。**
  Status FindRecord(const std::string& key, Record* out) const {
    if(index_.empty()){
      return Status::NotFound("sstable: cannot find" + key);
    }
    size_t l = 0,r = index_.size();
    while(l < r){
      // ★ l + (r - l) / 2 而不是 (l + r) / 2：
      //   后者在 l、r 都很大时会让 l + r 先溢出。这里两个 size_t 不可能
      //   真的到 2^63，但这是二分查找的标准写法，写成习惯就不用每次想了。
      //   （之前写的 l + r >> 1 还有第二个问题：+ 的优先级高于 >>，
      //     实际算的是 (l + r) >> 1，能跑对，但读的人要多想一秒。）
      size_t mid = l + (r - l) / 2;
      if(index_[mid].key <= key){
        l = mid + 1;
      }
      else r = mid;
    }

    if(l == 0){
      return Status::NotFound("sstable: cannot find" + key);
    }

    const size_t anchor = l - 1;
    size_t offset = static_cast<size_t>(index_[anchor].offset);
    const size_t block_end =
      anchor + 1 < index_.size()
          ? static_cast<size_t>(index_[anchor + 1].offset)
          : static_cast<size_t>(data_end_);

    while(offset < block_end){
      Record rec;
      size_t consumed = 0;
      const Status st = Decode(data_,offset,&rec,&consumed);

      if(!st.ok()){
        return Status::Corruption("sstable: decode wrong" + key);
      }
      if(rec.key == key){
        *out = std::move(rec);
        return Status::OK();
      }
      if(key < rec.key){
        break;
      }
      offset += consumed;
    }
    return Status::NotFound("sstable: cannot find" + key);
  }

  // ── 查一条值（用户视角：墓碑 = 不存在）──────────────────
  Status Get(const std::string& key, std::string* value) const {
    Record rec;
    Status st = FindRecord(key, &rec);
    if (!st.ok()) return st;
    if (rec.type == RecordType::kDelete) {
      return Status::NotFound("sstable: deleted: " + key);
    }
    if (value != nullptr) *value = std::move(rec.value);
    return Status::OK();
  }

  uint64_t record_count() const { return count_; }
  uint64_t data_end() const { return data_end_; }
  uint64_t file_size() const { return data_.size(); }
  const std::string& data() const { return data_; }

  // 索引区（Open 时解析出来的）。测试和 FindRecord 都从这里读。
  const std::vector<IndexEntry>& index() const { return index_; }

 private:
  std::string data_;            // 整个文件的内容（今晚的偷懒做法，见上面说明）
  uint64_t data_end_ = 0;       // 数据区末尾
  uint64_t count_ = 0;          // footer 里记的条数
  // 索引区，一条一项。★ 它和 Builder 里的 index_ 是【两份不同的数据】：
  //   Builder 那份是「写的时候攒的」，落盘之后就没了；
  //   这份是「读的时候从文件里解出来的」。
  //   中间唯一的连接是那 48 个字节 —— 一份写进去，一份读出来，对得上才算格式正确。
  std::vector<IndexEntry> index_;
};

}  // namespace lsmkv
