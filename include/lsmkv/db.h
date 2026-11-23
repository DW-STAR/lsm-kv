#pragma once

#include <string>

#include "lsmkv/slice.h"
#include "lsmkv/status.h"

namespace lsmkv {

// ─────────────────────────────────────────────────────────────
//  存储引擎的对外接口。
//
//  今晚只实现内存版；后面几周会换成真正的 LSM 实现，
//  但这个文件不会变 —— 这就是"先定接口"的价值：
//  实现可以整个重写，调用方一行都不用改。
// ─────────────────────────────────────────────────────────────
class DB {
 public:
  virtual ~DB() = default;

  // 打开（或创建）一个位于 path 的数据库。
  // 成功时把 new 出来的实例写进 *dbptr，调用方负责 delete。
  static Status Open(const std::string& path, DB** dbptr);

  // 写入：key -> value。已存在的 key 会被覆盖。
  virtual Status Put(const Slice& key, const Slice& value) = 0;

  // 读取：找到时把值写进 *value 并返回 OK；
  //      找不到时返回 NotFound，且不修改 *value。
  virtual Status Get(const Slice& key, std::string* value) = 0;

  // 删除。注意：LSM 里删一个 key 不是真的抹掉，
  // 而是写一条"墓碑"（tombstone），等 Compaction 时才真正清掉。
  virtual Status Delete(const Slice& key) = 0;

  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

 protected:
  DB() = default;
};

}  // namespace lsmkv
