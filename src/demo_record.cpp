// 演示：一条记录怎么变成字节，又怎么变回来。
//
// 跑起来你会看到两件事：
//   1. 编码后的十六进制 —— 那就是这条记录写进文件后的样子
//   2. roundtrip 测试 —— 把字节读回来，跟原对象比，是否完全一样
//
// 输出故意用英文 —— Windows 控制台是 GBK，中文会乱码（这个坑你踩过）。

#include <cstdio>
#include <iostream>
#include <string>

#include "lsmkv/record.h"

namespace {

// 把一串字节打成 "01 04 00 00 00 6e 61 6d 65" 这样，方便肉眼看。
void PrintHex(const std::string& bytes) {
  for (size_t i = 0; i < bytes.size(); ++i) {
    std::printf("%02x ", static_cast<unsigned char>(bytes[i]));
    if ((i + 1) % 8 == 0) {
      std::printf("\n");
    }
  }
  std::printf("\n");
}

void Show(const char* title, const lsmkv::Record& record) {
  std::cout << "--- " << title << " ---\n";
  const std::string bytes = lsmkv::Encode(record);
  if (bytes.empty()) {
    std::cout << "(empty)  Encode is not implemented yet\n\n";
    return;
  }
  std::cout << "encoded size: " << bytes.size() << " bytes\n";
  PrintHex(bytes);
  std::cout << "\n";
}

// 往返测试：Encode 出去，Decode 回来，看是不是跟原来一模一样。
// 这是所有编码代码最标准的验证方式 —— 只认「能不能还原」，别的都不算数。
void RoundTrip(const char* title, const lsmkv::Record& original) {
  const std::string bytes = lsmkv::Encode(original);

  lsmkv::Record restored;
  size_t consumed = 0;
  // 10-31：Decode 返回 Status 了（三种结局：OK / 读不全 / 校验不过）
  const lsmkv::Status st = lsmkv::Decode(bytes, 0, &restored, &consumed);
  if (!st.ok()) {
    std::cout << title << " roundtrip: FAIL   (Decode: " << st.ToString()
              << ")\n";
    return;
  }

  const bool same_type = (restored.type == original.type);
  const bool same_key = (restored.key == original.key);
  const bool same_value = (restored.value == original.value);
  const bool full = (consumed == bytes.size());

  if (same_type && same_key && same_value && full) {
    std::cout << title << " roundtrip: PASS   key=\"" << restored.key
              << "\"  value=\"" << restored.value << "\"  consumed="
              << consumed << "/" << bytes.size() << "\n";
  } else {
    std::cout << title << " roundtrip: FAIL   same_type=" << same_type
              << " same_key=" << same_key << " same_value=" << same_value
              << " consumed=" << consumed << "/" << bytes.size() << "\n";
    if (consumed == 5) {
      std::cout << "        (consumed=5 means Decode only got through type +"
                   " keylen. Steps 3, 4, 5 are still TODO.)\n";
    }
  }
}

}  // namespace

int main() {
  lsmkv::Record put;
  put.type = lsmkv::RecordType::kPut;
  put.key = "name";
  put.value = "liupei";

  lsmkv::Record del;
  del.type = lsmkv::RecordType::kDelete;
  del.key = "name";

  std::cout << "=== STEP 1: encode ===\n";
  std::cout << "(these are TWO DIFFERENT records, not encode-vs-decode)\n\n";
  Show("record A: Put(\"name\", \"liupei\")", put);
  Show("record B: Delete(\"name\")", del);

  std::cout << "Bytes 0-3 are the CRC32. Byte 4 IS the record type (01 / 02).\n";
  std::cout << "A has a 6-byte value, B has none (vlen = 00 00 00 00).\n";
  std::cout << "23 vs 17 bytes -- 4 more than the old 19/13, and that is the CRC.\n\n";

  std::cout << "=== STEP 2: decode back (roundtrip) ===\n";
  RoundTrip("Put   ", put);
  RoundTrip("Delete", del);

  std::cout << "\nDecode prints no hex. Its output is a Record again:\n";
  std::cout << "  key=\"name\", value=\"liupei\"   <- bytes in, object out.\n";
  std::cout << "Encode gives bytes, Decode gives fields. They look different\n";
  std::cout << "in shape because they are doing opposite things.\n";

  return 0;
}
