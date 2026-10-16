/* file:// 后端：本地目录（键 → "<dir>/<ns>/<key>"；写**原子**：tmp + rename，防半截存档）。
 * 默认根由运行时给出（<场景目录>/.maque）；裸 file:// 的兜底默认见 storage.cpp（XDG 数据目录）。 */
#include "../storage_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace maque::storage {

/* 目录 → 该目录下的文件（相对路径），排序保证确定性。 */
static std::vector<std::string> list_files(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || fec) continue;
        std::error_code rel_ec;
        const fs::path rel = fs::relative(it->path(), dir, rel_ec);
        if (rel_ec) continue;
        out.push_back(rel.generic_string());
    }
    std::sort(out.begin(), out.end());
    return out;
}

int file_apply(const std::string& dir, const Request& req, Response& out) {
    const fs::path root(dir);
    const fs::path target = root / rel_path(req.ns, req.key);
    std::error_code ec;

    switch (req.op) {
        case Op::Get: {
            std::ifstream f(target, std::ios::binary);
            if (!f) return MQ_STORAGE_ERR_NOTFOUND;
            f.seekg(0, std::ios::end);
            const std::streamoff n = f.tellg();
            f.seekg(0, std::ios::beg);
            if (n < 0) return MQ_STORAGE_ERR_IO;
            out.value.resize((size_t)n);
            if (n) f.read((char*)out.value.data(), n);
            return f.good() || f.eof() ? MQ_STORAGE_OK : MQ_STORAGE_ERR_IO;
        }
        case Op::Put: {
            fs::create_directories(target.parent_path(), ec);
            if (ec) return MQ_STORAGE_ERR_IO;
            /* 原子写：先写同目录临时文件，再 rename（同文件系统内 rename 原子） */
            const fs::path tmp = target.string() + ".tmp";
            {
                std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
                if (!f) return MQ_STORAGE_ERR_IO;
                if (req.size) f.write((const char*)req.data, (std::streamsize)req.size);
                f.flush();
                if (!f) { std::error_code rm; fs::remove(tmp, rm); return MQ_STORAGE_ERR_IO; }
            }
            fs::rename(tmp, target, ec);
            if (ec) { std::error_code rm; fs::remove(tmp, rm); return MQ_STORAGE_ERR_IO; }
            return MQ_STORAGE_OK;
        }
        case Op::Remove: {
            if (!fs::exists(target, ec) || ec) return MQ_STORAGE_ERR_NOTFOUND;
            fs::remove(target, ec);
            return ec ? MQ_STORAGE_ERR_IO : MQ_STORAGE_OK;
        }
        case Op::Keys: {
            out.json = keys_json(list_files(root / req.ns));
            return MQ_STORAGE_OK;
        }
    }
    return MQ_STORAGE_ERR_ARG;
}

} // namespace maque::storage
