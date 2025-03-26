在 libtorrent 中，`mmap_disk_io`、`mmap_storage`、`torrent` 和 `file_storage` 这几个组件的关系是层级式的，各自负责不同的职责。下面详细解释它们的关系和交互方式：

---

## **1. 核心组件及其作用**
| 组件 | 作用 |
|------|------|
| **`mmap_disk_io`** | 全局磁盘 I/O 管理器，负责多个 torrent 的存储调度（读写、缓存、哈希校验等） |
| **`torrent`** | 代表一个具体的 BT 任务（种子），管理 peers、piece 下载顺序、状态等 |
| **`mmap_storage`** | 单个 torrent 的存储后端，负责文件读写、磁盘空间管理 |
| **`file_storage`** | 描述 torrent 的文件结构（文件名、大小、布局等），不涉及实际 I/O |

---

## **2. 为什么 `mmap_disk_io` 管理多个 `mmap_storage`？**
### **(1) 多任务并发管理**
- 一个 libtorrent 会话（`session`）可能同时下载多个 torrent。
- 每个 torrent 需要独立的存储对象（`mmap_storage`）管理其文件。
- `mmap_disk_io` 作为**全局磁盘 I/O 调度器**，集中管理所有 torrent 的存储对象。

### **(2) 资源共享**
- **`file_view_pool`（文件映射池）**：所有 `mmap_storage` 共享同一个文件打开/映射缓存，避免重复打开文件。
- **线程池**：`mmap_disk_io` 统一调度所有 torrent 的磁盘操作（如读、写、哈希），避免每个 torrent 独占线程。

### **(3) 统一优化**
- **I/O 合并**：将多个 torrent 的小块读写合并为顺序操作，提升磁盘吞吐量。
- **优先级调度**：根据 torrent 的优先级调整磁盘访问顺序。

---

## **3. 组件关系图**
```plaintext
+---------------------+
|     mmap_disk_io    |  (全局单例)
|---------------------|
| - m_torrents:       |  ▶ 存储所有 mmap_storage（按 storage_index_t 索引）
|   vector<mmap_storage> |
| - m_file_pool       |  ▶ 所有存储共享的文件映射池
+----------↑----------+
           |
           | 管理
           |
+----------↓----------+    +-------------------+
|       torrent       |    |    file_storage   |
|---------------------|    |-------------------|
| - storage_holder    |───▶| - 文件列表        |
| - download_priority |    | - 文件大小/偏移   |
+----------↑----------+    +-------------------+
           |
           | 持有
           |
+----------↓----------+
|    mmap_storage     |  (每个 torrent 一个实例)
|---------------------|
| - m_files           |───▶ 引用 torrent 的 file_storage
| - m_stat_cache      |     缓存文件状态（大小、修改时间）
| - m_file_priority   |     文件优先级设置
+---------------------+
```

---

## **4. 关键交互流程**
### **(1) 新增 Torrent**
1. `session` 创建一个新的 `torrent` 对象。
2. `torrent` 通过 `mmap_disk_io::new_torrent()` 申请存储：
   ```cpp
   storage_holder sh = disk_io.new_torrent(params, torrent_ptr);
   ```
3. `mmap_disk_io`：
   - 分配一个新的 `storage_index_t`（优先复用空闲槽位）。
   - 创建 `mmap_storage` 对象，并存入 `m_torrents`。
   - 返回 `storage_holder` 给 `torrent`。

### **(2) 文件读写**
1. `torrent` 需要读写数据时，调用 `mmap_storage::read()`/`write()`。
2. `mmap_storage`：
   - 通过 `file_storage` 查找文件路径和偏移。
   - 从 `m_file_pool` 获取文件映射（避免重复打开文件）。
   - 执行实际 I/O（可能委托给 `mmap_disk_io` 的线程池）。

### **(3) 移除 Torrent**
1. `torrent` 被销毁时，其 `storage_holder` 析构。
2. `mmap_disk_io`：
   - 回收 `storage_index_t` 到 `m_free_slots`。
   - 释放对应的 `mmap_storage` 对象。

---

## **5. 设计优势**
1. **解耦**：
   - `torrent` 只关心业务逻辑（如 peer 通信、piece 选择）。
   - `mmap_storage` 只关心文件存储。
   - `mmap_disk_io` 只关心调度和资源管理。

2. **性能优化**：
   - 共享 `file_view_pool` 减少文件打开开销。
   - 集中调度 I/O 请求，避免磁盘寻址抖动。

3. **扩展性**：
   - 可以替换 `mmap_storage` 为其他存储后端（如 SSD 优化版）。
   - 支持动态调整 torrent 优先级。

---

## **6. 总结**
- **`mmap_disk_io` 是中枢**：管理所有 torrent 的存储，协调 I/O 资源。
- **`mmap_storage` 是执行者**：每个 torrent 独占一个实例，处理具体文件操作。
- **`file_storage` 是元数据**：描述文件布局，被 `torrent` 和 `mmap_storage` 共享。

这种架构非常适合 BT 客户端的场景：**高并发、低延迟、资源高效复用**。