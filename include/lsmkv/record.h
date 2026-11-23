#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "lsmkv/crc32.h"
#include "lsmkv/status.h"

namespace lsmkv {

// 单条记录的长度上限。超过这个数，就不可能是"文件末尾的残片"了。
//
// ★ 为什么需要它？
//
//   假如磁盘把某个 key 的长度字段（klen = 4）改成了 0x7FFFFFFF，
//   Decode 顺着这个错长度往前读，一路读到文件末尾发现不够 ——
//   表现和"进程写到一半被砍"一模一样，Load 会当成残片安静丢掉。
//   这就是"长度字段损坏伪装成尾部截断"的盲区。
//
//   加一个上限，把离谱的长度直接判成损坏，盲区就被压到
//   「损坏后的值恰好落在 [剩余字节数, 上限] 之间」这一小块。
//   不能彻底消灭（只靠记录自己的长度字段做不到），但把
//   "随便改成一个大数就能骗过校验"变成"改得越离谱越容易被抓"。
//
// ★ 64 MiB 是拍的。真实工程按业务最大 value 定，LevelDB 不用这个，
//   它靠 32KB 块边界当独立锚点 —— 那是另一套解法，等做 SSTable 再说。
inline constexpr size_t kMaxRecordSize = 64u * 1024u * 1024u;

// ══════════════════════════════════════════════════════════════
//  记录格式 —— 2026-10-30 版（加 CRC）
//
//  ★ 变更是怎么来的
//
//    10-20 你从零重写了 Encode / Decode，格式是 5 段：
//        [type][klen][key][vlen][value]
//
//    但 Decode 只能检查「长度会不会超出缓冲区」。它能保证结构没崩，
//    保证不了内容没变 —— 磁盘上某个字节坏了，照样"成功"解析，
//    读出一个错的 value，而且一句错都不报（静默错误）。
//
//    10-26 你写好了 Crc32。今天把它塞进记录头。
//
//  ★ 新格式
//
//     CRC        类型      key 长度      key      value 长度     value
//   ┌──────┬──────┬────────────┬──────────┬────────────┬───────────┐
//   │  4B  │  1B  │     4B     │  klen B  │     4B     │  vlen B   │
//   └──────┴──────┴────────────┴──────────┴────────────┴───────────┘
//   └────┘ └───────────────── 被 CRC 保护的部分 ────────────────────┘
//
//   就多了最前面 4 个字节。别的全是原来的。
//
//  ★ 三个必须想清楚的问题（想不清就别动键盘）
//
//  ① CRC 为什么放在最前面，不放在最后面？
//
//     读的时候你手上要先有一个"预期值"，才知道后面那一段该校验成什么。
//     放最后，你得先把整条解析完、找到末尾，才拿到 CRC —— 那 4 个字节
//     的位置本身又依赖前面的长度字段，而长度字段可能是坏的。
//     **先拿到校验值，再校验内容** —— 顺序反了整个机制立不起来。
//
//  ② CRC 校验哪一段？为什么不含它自己？
//
//     只校验 type..value。因为 CRC 字段本身如果坏了，你没法用它来验证
//     自己 —— 那等于用一个可能错的值去对另一个值，对不上你也不知道是谁错。
//     **校验值不保护自己，这是所有校验和的通例。**
//
//     （那 CRC 字段坏了能不能发现？能 —— 算出来的和读到的必然不等。
//       只是你分不清是 body 坏了还是 crc 字段坏了。这叫"检测到了，
//       但定位不了"，在存储里完全可以接受。）
//
//  ③ 加了 CRC，Decode 有没有变强？
//
//     变强了，而且是从"能不能读"升级到"能不能信"。
//     以前：字节不够 → 失败；字节够 → 成功（内容对不对，不知道）
//     现在：字节不够 → 失败；字节够但 CRC 对不上 → 失败（内容坏了）
//     **第二种失败，是这个项目第一次有能力说"这条数据不可信"。**
// ══════════════════════════════════════════════════════════════

// 「这条是存还是删」单独占一个字节。
enum class RecordType : uint8_t {
  kPut = 1,     // 写入
  kDelete = 2,  // 删除（墓碑）
};

struct Record {
  RecordType type = RecordType::kPut;
  std::string key;
  std::string value;
};

// 把 32 位整数追加到 dst 末尾，占 4 字节（原样搬内存，x86 是小端）。
inline void PutFixed32(std::string* dst, uint32_t v) {
  char buf[4];
  std::memcpy(buf, &v, 4);
  dst->append(buf, 4);
}

// 从 *p 读 4 字节，返回这个数，并把 *p 往后推 4。
// ⚠️ 注意它的失败语义：字节不够时**返回 0 且不移动游标**。
//    所以调用它之前必须先自己确认「p + 4 <= limit」，
//    否则你会拿到一个假的 0，还以为读到了真值。
inline uint32_t GetFixed32(const char** p, const char* limit) {
  if (*p + 4 > limit) {
    return 0;
  }
  uint32_t v;
  std::memcpy(&v, *p, 4);
  *p += 4;
  return v;
}

// ─────────────────────────────────────────────────────────────
//  Encode —— 2026-10-30 完成（CRC 塞进来）
//
//  两段式：先拼「被保护的部分」body，算完 CRC 再把 4 字节贴在最前面。
//
//  ★ 为什么一定要先有完整的 body 才能算 CRC？
//
//    CRC 是"整段字节的指纹"。少一个字节、多一个字节，指纹完全不同。
//    所以顺序**只能**是：拼完 → 算 → 贴前面。
//    想"边拼边算"是做不到的 —— 除非你改成流式计算（CRC 本身支持增量，
//    但那要维护中间状态，为了几十字节不值得）。
//
//  ★ 记住这个结构：`[校验值][内容]`。
//    后面做 SSTable 的 block、做 Manifest 文件，全是这个形状。
//    它不是这条记录的发明，是存储格式的通用范式。
// ─────────────────────────────────────────────────────────────
inline std::string Encode(const Record& record) {
  // ① 先拼出"被保护的那一段"（原有逻辑，一个字没改）
  std::string body;
  body.push_back(static_cast<char>(record.type));
  PutFixed32(&body, static_cast<uint32_t>(record.key.size()));
  body.append(record.key);
  PutFixed32(&body, static_cast<uint32_t>(record.value.size()));
  body.append(record.value);
  // ② 算 CRC，贴到 body 前面
  std::string out;
  uint32_t v = Crc32(body);
  PutFixed32(&out,v);
  out += body;
  return out;
}

// ─────────────────────────────────────────────────────────────
//  Decode —— 2026-10-31 版：返回值从 bool 升级成 Status
//
//  ★ 为什么要改？
//
//    加 CRC 之前，Decode 只有一种失败：「字节不够」。一个 bool 装得下。
//    加 CRC 之后，「字节不够」变成了一种**可能正常**的情况 ——
//    进程写到一半被 kill -9，文件末尾就是半条记录，这叫崩溃恢复的正常路径。
//
//    而「读全了但 CRC 对不上」是另一种失败，它**永远不正常**：
//    数据内容变了，后面全部不可信。
//
//    这两种要做的处理**方向相反**（一个丢掉继续、一个报警停机），
//    一个 bool 装不下。**返回值的信息量必须够调用方做判断。**
//    （这就是 10-24 把 Load 从 bool 改成 Status 的同一个理由，在下一层重演。）
//
//  ★ short_by 是什么？为什么不让 Decode 自己判断"该不该报警"？
//
//    它是个可选出参，只在「字节不够」时填：填的是**还差多少字节**。
//        short_by > 0   → 字节不够（可能是正常残片，也可能是损坏）
//        short_by == 0  → 字节够，但 CRC 对不上（一定是损坏）
//
//    Decode 只能说这么多，因为**它不知道 buf 是不是文件的全部**。
//    同一个 buf，可能是整个 wal.log，也可能是它的前 200 字节。
//    "缓冲区到头了"不等于"文件到头了"。
//    只有 Load 知道 offset 是不是最后一个有效位置 —— 所以判断权在 Load。
//
//    **下层报告事实，上层做决定。** 这就是分层最实际的用处。
//
//  ★ 已知盲区（必须知道，别以为自己写完美了）
//
//    如果损坏恰好落在长度字段，且改成的值落在
//    「文件剩余字节数」到「kMaxRecordSize」之间，Decode 会报"字节不够"，
//    Load 会当残片丢掉 —— 后面那几条好记录跟着陪葬，且不报警。
//
//    只靠记录自己的长度字段，这个盲区灭不掉（这正是之前讲过的
//    desynchronization：跳过坏记录需要长度，而长度本身可能是坏的）。
//    正解是引入独立锚点 —— LevelDB 把 WAL 切成 32KB 块，块边界就是锚点。
//    那是后面做 SSTable 时的事。
// ─────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────
//  ReportShortBy —— 小工具，只干一件事：把"还差多少字节"写进出参
//
//  三个入参都是 Decode 本来就有的东西，看懂了 Decode 就懂它：
//    need  = 这一小段需要多少字节（CRC 要 4，type 要 1，key 要 klen，…）
//    p     = 现在读到哪儿了
//    limit = 缓冲区末尾（注意：是"最后一个字节的**下一个**位置"）
//
//  ★ limit 为什么不是最后一个字节？
//
//    因为遍历区间是左闭右开 [base, limit)。
//    缓冲区有 10 个字节 → base 指第 0 个，limit 指第 10 个（不存在的位置）。
//    好处：判断"够不够"只要一个比较 p + need > limit，
//          判断"读完没有"只要 p == limit。不用到处写 ±1。
//
//  ★ have 为什么不写成 (p + need) - limit？
//
//    两种写法结果一样，但 have 这种两步骤谁都看得懂：
//        还剩几个 = 末尾 - 现在      → limit - p
//        还差几个 = 需要 - 还剩      → need - have
//    而 (p + need) - limit 要先在脑子里把 p 挪到 need 之后，再往回减。
//
//  ★ 指针相减出来的是什么？
//
//    `limit - p` 不是"两个地址相减"，而是"中间隔了几个 char"。
//    因为 char 是 1 字节，所以这个数**就是字节数**，正是我们要的。
//    （如果元素是 4 字节的 int，`q - p` 就是"隔了几个 int"，要乘 4 才是字节。）
// ─────────────────────────────────────────────────────────────
inline void ReportShortBy(size_t* short_by, size_t need, const char* p,
                          const char* limit) {
  if (short_by == nullptr) return;  // 调用方不关心，什么都不用做
  const size_t have = static_cast<size_t>(limit - p);  // 还剩几个字节
  // 正常情况下 need > have 一定成立（调用前刚比较过）。
  // 加这个三元只是防止将来有人不检查就调用，导致 size_t 减成天文数字。
  *short_by = (need > have) ? (need - have) : 0;
}

// ─────────────────────────────────────────────────────────────
//  Decode —— 2026-10-31 版
//
//  ★ 一句话：short_by 回答「还差几个字节才够读完这条记录」。
//
//        0    = 没缺，字节够（那失败就只能是 CRC 对不上）
//        > 0  = 缺这么多字节
//
//  ★ 为什么需要它？
//
//    失败有两种，Load 要分开处理：
//        字节不够 → 可能只是崩溃残片（正常）→ 丢掉，继续用后面的
//        CRC 不对 → 内容真的坏了（异常）    → 停，报 Corruption
//
//    光看"失败了"分不出是哪一种，short_by 就是那个区分标记。
//
//  ★ 为什么用 size_t* 不用 size_t&？
//
//    因为它是可选的：调用方可以传 nullptr 表示"我不关心"。
//    引用不能为"空"，指针可以 —— 这是它俩最实际的分工。
//    所以每处都得先判 if (short_by != nullptr) 再写，否则解空指针会崩。
//
//  ★ 为什么这个数要 Decode 算，不在 Load 里算？
//
//    因为只有 Decode 知道"我现在读到第几个字段、这个字段要多长"。
//    Load 手里只有一个总长度，它不知道一条记录内部长什么样。
//
//  ★ 已知盲区
//
//    长度字段自己坏了（klen 从 4 被改成 40）时，Decode 报的是"缺字节"，
//    看上去和残片一模一样 —— 后面那几条好记录会跟着被丢掉。
//    kMaxRecordSize 挡得住离谱值，挡不住"看着合理"的值。
//    根治要靠分块（每块自带边界），做 SSTable 时解决。
// ─────────────────────────────────────────────────────────────
inline Status Decode(const std::string& buf, size_t offset, Record* out,
                     size_t* consumed, size_t* short_by = nullptr) {
  if (short_by != nullptr) *short_by = 0;

  // offset 已经越过缓冲区末尾：连第一条都不存在。不要让它去算 base。
  if (offset >= buf.size()) {
    if (short_by != nullptr) *short_by = 4;  // 至少还缺一个 CRC
    return Status::Corruption("truncated: offset past end of buffer");
  }

  const char* base = buf.data() + offset;
  const char* limit = buf.data() + buf.size();
  const char* p = base;

  // ① 读 CRC
  //    先拿到"预期值"，后面才知道 body 该校验成什么。
  if (p + 4 > limit) {
    ReportShortBy(short_by, 4, p, limit);
    return Status::Corruption("truncated in crc");
  }
  const uint32_t stored_crc = GetFixed32(&p, limit);  // ← 占位，等你填
  const char* body_begin = p;     // ← 占位，等你填
 
  // ② 老逻辑（10-20 写的，原样保留）
  if (p + 1 > limit) {
    ReportShortBy(short_by, 1, p, limit);
    return Status::Corruption("truncated in type");
  }
  out->type = static_cast<RecordType>(*p);
  ++p;

  if (p + 4 > limit) {
    ReportShortBy(short_by, 4, p, limit);
    return Status::Corruption("truncated in key length");
  }
  const uint32_t klen = GetFixed32(&p, limit);

  // 长度离谱 → 一定不是残片。这是上面说的"上限检查"。
  if (klen > kMaxRecordSize) {
    return Status::Corruption("insane key length");
  }

  if (p + klen > limit) {
    ReportShortBy(short_by, klen, p, limit);
    return Status::Corruption("truncated in key");
  }
  out->key.assign(p, klen);
  p += klen;

  if (p + 4 > limit) {
    ReportShortBy(short_by, 4, p, limit);
    return Status::Corruption("truncated in value length");
  }
  const uint32_t vlen = GetFixed32(&p, limit);

  if (vlen > kMaxRecordSize) {
    return Status::Corruption("insane value length");
  }

  if (p + vlen > limit) {
    ReportShortBy(short_by, vlen, p, limit);
    return Status::Corruption("truncated in value");
  }
  out->value.assign(p, vlen);
  p += vlen;

  // ③ 校验 CRC
  //    走到这里说明结构全读通了 —— 失败就只可能是内容变了。
  //    short_by 保持 0，调用方据此知道"这不是残片，是坏数据"。
  if (stored_crc != Crc32(body_begin, static_cast<size_t>(p - body_begin))) {
    return Status::Corruption("checksum mismatch");
  }
  *consumed = static_cast<size_t>(p - base);
  return Status::OK();
}

// ══════════════════════════════════════════════════════════════
//  验收（10-31 版）
//
//  ① demo_record.exe      两条 roundtrip PASS，23 / 17 字节
//  ② demo_crc32.exe       4 向量 PASS；改 1 字节 → REJECTED，
//                         reason: checksum mismatch（Status 第一次说出原因）
//  ③ demo_wal.exe         4 条 / 88 字节 / 重启内容对
//  ④ demo_corrupt.exe     四种结局各就各位：
//                           完好          → OK         3 条
//                           尾部砍 5 字节 → OK         2 条，truncated=12
//                           改 1 bit      → Corruption 1 条
//                           klen 改天文数 → Corruption 1 条
//
//  ★ 这里已经没有任何 TODO 了。record.h 从此是"稳定地基"，
//    后面做 MemTable / SSTable 都建立在它上面 —— 除非有充分理由，别再动格式。
//    格式每动一次，磁盘上的老文件就多一批读不了的。
// ══════════════════════════════════════════════════════════════

}  // namespace lsmkv
