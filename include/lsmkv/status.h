#pragma once

#include <string>
#include <utility>

namespace lsmkv {

// Status 是 LevelDB 风格的错误处理：不抛异常，所有可能失败的操作
// 都返回一个 Status，调用方必须显式检查。
//
// 为什么存储引擎不用异常？
//   1. 存储代码到处是 C 接口（write/fsync/mmap），它们的错误本来就是
//      用返回值表达的，硬包成异常反而多一层胶水。
//   2. 异常让控制流不可预测。在"写完 WAL 但还没更新内存状态"这种
//      中间态抛出异常，很容易留下不一致。
//   3. 异常安全（strong exception guarantee）在持锁代码里极难保证。
//
// 面试追问点：Status 这种设计有什么缺点？
//   → 错误容易被忽略（忘了检查 .ok()）。所以 LevelDB 用了
//     __attribute__((warn_unused_result)) 之类的手段，我们这里先不做。
class Status {
 public:
  enum Code {
    kOk = 0,
    kNotFound,      // Get 一个不存在的 key
    kIOError,       // 磁盘读写失败
    kCorruption,    // 数据块损坏（校验和不匹配）
    kNotSupported,  // 尚未实现的功能
    // 调用方用法错了 —— 不是环境问题，是代码问题。
    // 例：往一张已经冻结（只读）的 MemTable 里写。
    // LevelDB 里这个码叫 InvalidArgument，我们跟它对齐。
    kInvalidArgument,
  };

  Status() : code_(kOk) {}
  Status(Code code, std::string msg) : code_(code), msg_(std::move(msg)) {}

  static Status OK() { return Status(); }
  static Status NotFound(const std::string& m) { return Status(kNotFound, m); }
  static Status IOError(const std::string& m) { return Status(kIOError, m); }
  static Status Corruption(const std::string& m) {
    return Status(kCorruption, m);
  }
  static Status NotSupported(const std::string& m) {
    return Status(kNotSupported, m);
  }
  static Status InvalidArgument(const std::string& m) {
    return Status(kInvalidArgument, m);
  }

  bool ok() const { return code_ == kOk; }
  bool IsNotFound() const { return code_ == kNotFound; }
  bool IsIOError() const { return code_ == kIOError; }
  bool IsCorruption() const { return code_ == kCorruption; }
  bool IsInvalidArgument() const { return code_ == kInvalidArgument; }
  Code code() const { return code_; }

  std::string ToString() const { return ok() ? std::string("OK") : msg_; }

 private:
  Code code_;
  std::string msg_;
};

}  // namespace lsmkv
