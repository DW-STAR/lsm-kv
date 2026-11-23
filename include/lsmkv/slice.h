#pragma once

#include <cstddef>
#include <cstring>
#include <string>

namespace lsmkv {

// Slice 是一个「只读的字节视图」——它不拥有内存，只记录"起点 + 长度"。
//
// 为什么要这个东西？
//   LevelDB 里 key 和 value 会被传来传去，如果每层都用 std::string，
//   每一次传参都是一次堆分配 + 一次拷贝。Slice 让传参退化成一个指针
//   加一个长度，零拷贝。
//
// 面试追问点（准备好回答）：
//   - Slice 和 std::string_view 的区别是什么？（C++17 有 string_view 了，
//     为什么 LevelDB 还要自己造一个？答案：LevelDB 2011 年就有了，
//     string_view 是 2017 年才进标准的；另外 Slice 支持内嵌 '\0' 的二进制数据，
//     可以直接 ToString() 出 std::string）
//   - 用 Slice 有什么坑？（它不持有内存。如果它指向的字符串被销毁了，
//     Slice 就悬空了——这是个典型的生命周期陷阱）
class Slice {
 public:
  Slice() : data_(""), size_(0) {}
  Slice(const char* s) : data_(s), size_(std::strlen(s)) {}  // NOLINT
  Slice(const char* d, size_t n) : data_(d), size_(n) {}
  Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}  // NOLINT

  const char* data() const { return data_; }
  size_t size() const { return size_; }
  bool empty() const { return size_ == 0; }

  // 唯一会真正分配内存的地方，需要拷贝出去时才调用
  std::string ToString() const { return std::string(data_, size_); }

  // 允许像访问数组一样用：s[0]、s[1] ...
  char operator[](size_t n) const { return data_[n]; }

  // 比较（按字节序）
  int compare(const Slice& other) const;

 private:
  const char* data_;
  size_t size_;
};

inline int Slice::compare(const Slice& other) const {
  const size_t min_len = (size_ < other.size_) ? size_ : other.size_;
  int r = std::memcmp(data_, other.data_, min_len);
  if (r == 0) {
    if (size_ < other.size_) {
      r = -1;
    } else if (size_ > other.size_) {
      r = +1;
    }
  }
  return r;
}

inline bool operator==(const Slice& a, const Slice& b) {
  return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

inline bool operator!=(const Slice& a, const Slice& b) { return !(a == b); }

}  // namespace lsmkv
