#include <iostream>
#include <string>

#include "lsmkv/db.h"

int main() {
    lsmkv::DB* db = nullptr;
    lsmkv::Status s = lsmkv::DB::Open("./data", &db);
    if (!s.ok()) {
      std::cerr << "open failed: " << s.ToString() << "\n";
      return 1;
    }

    db->Put("name", "ove");
    db->Put("project", "lsm-kv");

    std::string v;
    if (db->Get("name", &v).ok()) {
      std::cout << "name = " << v << "\n";
    }

    if (db->Get("missing", &v).IsNotFound()) {
      std::cout << "missing -> NotFound (expected)\n";
    }

    db->Delete("project");
    std::cout << "after delete, project -> "
              << (db->Get("project", &v).ok() ? "found" : "gone") << "\n";

    delete db;
    return 0;
}
