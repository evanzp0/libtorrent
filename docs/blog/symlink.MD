### **`m_symlinks` 的内容与示例**

`m_symlinks` 是 `file_storage` 类中的一个成员变量，类型通常为 `std::vector<std::string>`，用于 **存储所有符号链接的目标路径**（即符号链接指向的文件或目录路径）。  

---

## **1. `m_symlinks` 的存储内容**
- **每个符号链接** 在 `m_files`（`file_storage` 的文件列表）中有一个对应的 `symlink_index`，指向 `m_symlinks` 中的某个位置。
- **`m_symlinks` 存储的是符号链接的原始目标路径**（可能是相对路径或绝对路径，但绝对路径会被修正）。
- **在 `sanitize_symlinks()` 处理后**，所有目标路径会被规范化，确保它们指向合法的 Torrent 内部文件或目录。

---

## **2. 示例 Torrent 文件结构**
假设 Torrent 包含以下文件：
```
/root/                    （目录）
/root/file.txt            （普通文件）
/root/dir1/               （目录）
/root/dir1/link_to_file → ../file.txt    （符号链接，指向文件）
/root/dir1/link_to_dir2 → ../dir2        （符号链接，指向目录）
/root/dir2/               （目录）
/root/dir2/invalid_link → /etc/passwd   （无效符号链接，指向系统文件）
```

---

## **3. `m_symlinks` 的初始化和处理**
### **（1）初始状态（解析 Torrent 文件后）**
`m_symlinks` 存储符号链接的原始目标：
```cpp
m_symlinks = {
    "../file.txt",    // /root/dir1/link_to_file 的目标
    "../dir2",       // /root/dir1/link_to_dir2 的目标
    "/etc/passwd"    // /root/dir2/invalid_link 的目标（绝对路径，非法）
};
```
对应的 `m_files` 条目：
```cpp
m_files = [
    { name: "file.txt", path_index: ..., flags: 普通文件 },
    { name: "link_to_file", path_index: ..., flags: 符号链接, symlink_index: 0 },
    { name: "link_to_dir2", path_index: ..., flags: 符号链接, symlink_index: 1 },
    { name: "invalid_link", path_index: ..., flags: 符号链接, symlink_index: 2 }
];
```

### **（2）`sanitize_symlinks()` 处理后的 `m_symlinks`**
经过校验和修正后：
```cpp
m_symlinks = {
    "../file.txt",    // 合法，保留（指向 Torrent 内部文件）
    "../dir2",        // 合法，保留（指向 Torrent 内部目录）
    "invalid_link"    // 非法绝对路径，改为指向自身（避免逃逸攻击）
};
```
- **`/etc/passwd` 被替换为 `invalid_link`**（指向自身，防止安全风险）。
- **`../file.txt` 和 `../dir2` 保留**，因为它们指向 Torrent 内部的有效文件/目录。

---

## **4. `dir_links` 的填充（仅存储指向目录的符号链接）**
在 `sanitize_symlinks()` 执行过程中，`dir_links` 会记录 **明确指向目录的符号链接**：
```cpp
dir_links = {
    { "/root/dir1/link_to_dir2", "/root/dir2" }  // 符号链接 → 目标目录
};
```
- **`link_to_file` 不会进入 `dir_links`**（因为它指向文件）。
- **`invalid_link` 也不会进入 `dir_links`**（因为它的目标被修正为指向自身）。

---

## **5. 关键总结**
| 变量 | 存储内容 | 示例 |
|------|----------|------|
| **`m_symlinks`** | 所有符号链接的目标路径（修正后） | `["../file.txt", "../dir2", "invalid_link"]` |
| **`dir_links`** | 仅存储 **指向目录的符号链接**（用于解析嵌套链接） | `{ "/root/dir1/link_to_dir2": "/root/dir2" }` |
| **`file_map`** | 所有文件的路径 → 索引（惰性初始化） | `{ "/root/file.txt": 0, ... }` |
| **`dir_map`** | 所有有效目录路径（惰性初始化） | `{ "/", "/root", "/root/dir1", "/root/dir2" }` |

### **6. 为什么需要 `m_symlinks`？**
1. **存储原始目标**：Torrent 文件可能包含符号链接的原始路径（如 `../file.txt`）。
2. **安全修正**：在加载时校验并修正非法目标（如 `/etc/passwd` → 改为指向自身）。
3. **支持嵌套解析**：结合 `dir_links` 可以处理复杂的符号链接链（如 `linkA → linkB → dir`）。

---

## **7. 最终 Torrent 的符号链接行为**
- **`/root/dir1/link_to_file`** → 实际指向 `/root/file.txt`（下载时会解析）。
- **`/root/dir1/link_to_dir2`** → 实际指向 `/root/dir2`（记录在 `dir_links`）。
- **`/root/dir2/invalid_link`** → 指向自身（防止逃逸攻击）。

这样设计既保证了安全性，又能正确解析合法的符号链接。