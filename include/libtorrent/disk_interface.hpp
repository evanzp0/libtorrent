/*

Copyright (c) 2014-2022, Arvid Norberg
Copyright (c) 2017-2018, Steven Siloti
Copyright (c) 2018, Alden Torres
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#ifndef TORRENT_DISK_INTERFACE_HPP
#define TORRENT_DISK_INTERFACE_HPP

#include "libtorrent/bdecode.hpp"

#include <string>
#include <memory>

#include "libtorrent/fwd.hpp"
#include "libtorrent/units.hpp"
#include "libtorrent/disk_buffer_holder.hpp"
#include "libtorrent/aux_/vector.hpp"
#include "libtorrent/aux_/export.hpp"
#include "libtorrent/storage_defs.hpp"
#include "libtorrent/time.hpp"
#include "libtorrent/sha1_hash.hpp"
#include "libtorrent/flags.hpp"
#include "libtorrent/session_types.hpp"

// OVERVIEW（概述）
//
// The disk I/O can be customized in libtorrent. In previous versions, the
// customization was at the level of each torrent. Now, the customization point
// is at the session level. All torrents added to a session will use the same
// disk I/O subsystem, as determined by the disk_io_constructor (in
// session_params).
// 在 libtorrent 中，磁盘 I/O 功能是可以自定义的。
// 在早期版本里，自定义是针对每个种子文件进行的。
// 而现在，自定义的层面提升到了会话级别。
// 添加到某个会话中的所有种子文件都会使用相同的磁盘 I/O 子系统，
// 该子系统由 disk_io_constructor（在 session_params 中）决定。
//
// This allows the disk subsystem to also customize threading and disk job
// management.
// 这种方式使得磁盘子系统还能够对线程和磁盘任务管理进行自定义设置。
//
// To customize the disk subsystem, implement disk_interface and provide a
// factory function to the session constructor (via session_params).
// 若要对磁盘子系统进行自定义，需要实现 disk_interface 接口，
// 并向会话构造函数（通过 session_params）提供一个工厂函数。
//
// Example use:
//
// .. include:: ../examples/custom_storage.cpp
// 	:code: c++
// 	:tab-width: 2
// 	:start-after: -- example begin
// 	:end-before: // -- example end
namespace libtorrent {

	struct disk_observer;
	struct counters;

	struct storage_holder;

	// 文件打开模式标志
	using file_open_mode_t = flags::bitfield_flag<std::uint8_t, struct file_open_mode_tag>;

	// internal
	// this is a bittorrent constant
	constexpr int default_block_size = 0x4000;

// 定义文件打开模式
namespace file_open_mode {
	// open the file for reading only
	// 只读模式（b0000_0000）
	constexpr file_open_mode_t read_only{};

	// open the file for writing only
	// 只写模式（b0000_0001）
	constexpr file_open_mode_t write_only = 0_bit;

	// open the file for reading and writing
	// 读写模式（b0000_0010）
	constexpr file_open_mode_t read_write = 1_bit;

	// the mask for the bits determining read or write mode
	// 用于确定读取或写入模式的位掩码（b0000_0011）
	constexpr file_open_mode_t rw_mask = read_only | write_only | read_write;

	// open the file in sparse mode (if supported by the filesystem).
	// 稀疏文件模式（b0000_0100）
	constexpr file_open_mode_t sparse = 2_bit;

	// don't update the access timestamps on the file (if
	// supported by the operating system and filesystem).
	// this generally improves disk performance.
	// 不更新访问时间（b0000_1000）
	constexpr file_open_mode_t no_atime = 3_bit;

	// When this is not set, the kernel is hinted that access to this file will
	// be made sequentially.
	// 随机访问（b0010_0000）
	constexpr file_open_mode_t random_access = 5_bit;

#if TORRENT_ABI_VERSION == 1
	// prevent the file from being opened by another process
	// while it's still being held open by this handle
	constexpr file_open_mode_t locked TORRENT_DEPRECATED = 6_bit;
#endif

	// the file is memory mapped
	// 内存映射模式（b1000_0000）
	constexpr file_open_mode_t mmapped = 7_bit;
}

	// this contains information about a file that's currently open by the
	// libtorrent disk I/O subsystem. It's associated with a single torrent.
	// 描述当前被 libtorrent 打开的文件状态，它与单个种子文件关联。
	struct TORRENT_EXPORT open_file_state
	{
		// the index of the file this entry refers to into the ``file_storage``
		// file list of this torrent. This starts indexing at 0.
		// file_index 是文件索引，它指向 torrent 的 `file_storage` 的文件列表 (torrent::m_torrent_file::m_files)。
		// 文件索引从 0 开始。
		file_index_t file_index;

		// ``open_mode`` is a bitmask of the file flags this file is currently
		// opened with. For possible flags, see file_open_mode_t.
		// `open_mode` 是文件打开模式标志
		//
		// Note that the read/write mode is not a bitmask. The two least significant bits are used
		// to represent the read/write mode. Those bits can be masked out using the ``rw_mask`` constant.
		//
		// 通常，位掩码允许任意组合（如 sparse | no_atime），
		// 但 read/write 模式（read_only、write_only、read_write）是互斥的（即文件不能同时是 read_only 和 write_only）。
		// 因此，read/write 模式 只用最低 2 位（bit 0 和 bit 1） 表示：
		// - read_only = 00（0x00）
		// - write_only = 01（0x01）
		// - read_write = 10（0x02）
		//
		// rw_mask 是一个掩码（b0000_0011），用于提取 open_mode 的最低 2 位:
		// ```cpp
		// file_open_mode_t mode = file.open_mode & rw_mask;
		// ```
		file_open_mode_t open_mode;

		// a (high precision) timestamp of when the file was last used.
		// 文件最后使用时间戳
		time_point last_use;
	};

	// 磁盘作业标志，用来控制磁盘操作行为
	using disk_job_flags_t = flags::bitfield_flag<std::uint8_t, struct disk_job_flags_tag>;

	// 磁盘接口 (disk_interface)
	// 全局磁盘 I/O 管理器，负责多个 torrent 的存储调度（读写、缓存、哈希校验等）
	//
	// The disk_interface is the customization point for disk I/O in libtorrent.
	// implement this interface and provide a factory function to the session constructor
	// use custom disk I/O. All functions on the disk subsystem (implementing
	// disk_interface) are called from within libtorrent's network thread. For
	// disk I/O to be performed in a separate thread, the disk subsystem has to
	// manage that itself.
	// disk_interface 是 libtorrent 中磁盘 I/O 的定制点。
	// 实现此接口，并向会话构造函数提供一个工厂函数，以使用自定义磁盘 I/O。
	// 磁盘子系统（实现 disk_interface）中的所有函数都在 libtorrent 的网络线程内被调用（即 session 的主线程）。
	// 若要让磁盘 I/O 在单独的线程中执行（以避免阻塞网络线程），磁盘子系统需要自己负责线程的创建、管理和同步。
	//
	// Although the functions are called ``async_*``, they do not technically
	// *have* to be asynchronous, but they support being asynchronous, by
	// expecting the result passed back into a callback. The callbacks must be
	// posted back onto the network thread via the io_context object passed into
	// the constructor. The callbacks will be run in the network thread.
	// 尽管这些函数名为 async_*，但从技术上讲，它们不一定必须是异步的，不过它们支持异步操作，
	// 其实现方式是通过回调函数来返回操作结果。
	// 回调必须返回网络线程（通过 io_context 提交），否则会破坏 libtorrent 的线程安全。
	// 回调函数将在网络线程（即 session 的主线程）中运行。
	// 通过 io_context 提交回调函数的例子：
	// ```cpp
	// void my_disk_io::async_read(handler, ...) {
    //     // 在后台线程执行读取，这里是直接开一个新线程，更好的是使用线程池处理，主线程添加读取任务，I/O线程池自动提取任务执行。
    //     std::thread([=] {
    //         char* buf = read_from_disk(...);
    //         // 回调必须回到网络线程！
    //         m_io_context.post([=] { handler(lt::disk_buffer_holder(*this, buf, r.length), error); });
    //     }).detach();
	// }
	//```
	//
	// libtorrent 的 默认磁盘子系统 (mmap_disk_io 或 posix_disk_io) 是 多线程设计：
	// - 网络线程（主线程）： 是 单线程 的，负责核心网络事件循环，
	//                      包括接收/发送 Peer 数据、调度任务，调用 disk_interface 方法（如 async_read/async_write）。
	// - 磁盘 I/O 线程（后台线程）：默认有一个 专用线程池 处理实际的文件读写、哈希计算等耗时操作，避免阻塞网络线程。
	struct TORRENT_EXPORT disk_interface
	{
		// force making a copy of the cached block, rather than getting a
		// reference to a block already in the cache. This is used the block is
		// expected to be overwritten very soon, by async_write()`, and we need
		// access to the previous content.
		// 强制对缓存块进行复制，而不是获取对已存在于缓存中的块的引用。
		// 当预计该块很快会被 async_write() 方法覆盖，并且我们需要访问其先前的内容时，会使用此操作。
		static constexpr disk_job_flags_t force_copy = 0_bit;

		// hint that there may be more disk operations with sequential access to
		// the file
		// 给出提示信息，表明对该文件可能会有更多的顺序访问磁盘操作。
		static constexpr disk_job_flags_t sequential_access = 3_bit;

		// don't keep the read block in cache. This is a hint that this block is
		// unlikely to be read again anytime soon, and caching it would be
		// wasteful.
		// 不要将读取的块保留在缓存中。
		// 这是一个提示信息，表明该块在短期内不太可能再次被读取，将其缓存起来会造成资源浪费。
		static constexpr disk_job_flags_t volatile_read = 4_bit;

		// compute a v1 piece hash. This is only used by the async_hash() call.
		// If this flag is not set in the async_hash() call, the SHA-1 piece
		// hash does not need to be computed.
		// 计算一个 v1 版本的片段哈希值。
		// 此操作仅在 async_hash() 调用时使用。如果在 async_hash() 调用中未设置此标志，
		// 则无需计算 SHA - 1 片段哈希值。
		static constexpr disk_job_flags_t v1_hash = 5_bit;

		// this flag instructs a hash job that we just completed this piece, and
		// it should be flushed to disk
		// 此标志指示一个哈希任务，表明我们刚刚完成了这个片段的处理，并且该片段应被刷新到磁盘。
		static constexpr disk_job_flags_t flush_piece = 7_bit;

		// 该函数定义了如何为新增的 torrent 创建存储后端。
		// - 核心作用：为每个新添加的 torrent 创建专属的存储对象（mmap_storage）
		// - 所有权管理：返回的 storage_holder 是一个 RAII（资源获取即初始化）包装器，
		//              存放 disk_interface 引用和 storage 的在 disk_interface 的 idx。
		// - 线程安全：允许存储操作在 session 移除 shared_ptr<torrent> 对象后仍能安全执行，
		//            disk_interface 也存放这 storage，storage 中存放着shared_ptr<torrent> 对象，
		//            当storage_holder 析构时，会调用 disk_interface::remove_torrent(storage_index_t) 
		//            从存储中移除 storage，从而真正释放 torrent 对象。
		//
		// this is called when a new torrent is added. The shared_ptr can be
		// used to hold the internal torrent object alive as long as there are
		// outstanding disk operations on the storage.
		// The returned storage_holder is an owning reference to the underlying
		// storage that was just created. It is fundamentally a storage_index_t
		// 当添加一个新的种子文件时会调用此函数。
		// 只要 storage 上还有未完成的磁盘操作，这个 shared_ptr 就可以用来保证 storage 内部的 torrent 对象存活。
		// 函数返回的 storage_holder 是对刚刚创建的底层 storage 的一个拥有所有权的引用，
		//（通过记录 storage 在 disk_io 中的 index）。
		// 从根本上来说，它是一个 storage_index_t 类型。
		virtual storage_holder new_torrent(storage_params const& p
			, std::shared_ptr<void> const& torrent) = 0;
		
		// 移除 torrent 的 storage
		//
		// remove the storage with the specified index. This is not expected to
		// delete any files from disk, just to clean up any resources associated
		// with the specified storage.
		// 移除具有指定索引的 storage。预计此操作不会从磁盘删除任何文件，
		// 仅清理与指定 storage 相关的任何资源
		virtual void remove_torrent(storage_index_t) = 0;

		// 异步从指定存储（对应一个 torrent）中读取数据块，完成后通过回调通知。
		// 它通过 disk_buffer_holder 直接管理内存缓冲区，实现零拷贝优化。
		//
		// perform a read or write operation from/to the specified storage
		// index and the specified request. When the operation completes, call
		// handler possibly with a disk_buffer_holder, holding the buffer with
		// the result. Flags may be set to affect the read operation. See
		// disk_job_flags_t.
		// 对指定 storage_index_t 和指定请求执行读或写操作。操作完成后，可能会调用 handler 函数，
		// 并传入一个 disk_buffer_holder 对象，该对象持有包含操作结果的缓冲区。
		// 可以设置标志来影响读操作，具体请参考 disk_job_flags_t。
		//
		// The disk_observer is a callback to indicate that
		// the store buffer/disk write queue is below the watermark to let peers
		// start writing buffers to disk again. When ``async_write()`` returns
		// ``true``, indicating the write queue is full, the peer will stop
		// further writes and wait for the passed-in ``disk_observer`` to be
		// notified before resuming.
		// disk_observer 是一个回调函数，用于指示存储缓冲区 / 磁盘写入队列已低于阈值，
		// 从而允许对等节点再次开始将缓冲区写入磁盘。
		// 当 async_write() 返回 true 时，表示写入队列已满，对等节点将停止进一步的写入操作，
		// 并等待传入的 disk_observer 被通知后再恢复写入。
		//
		// Note that for ``async_read``, the peer_request (``r``) is not
		// necessarily aligned to blocks (but it is most of the time). However,
		// all writes (passed to ``async_write``) are guaranteed to be block
		// aligned.
		// 请注意，对于 async_read 操作，peer_request（r）不一定与块对齐（尽管大多数情况下是对齐的）。
		// 然而，所有传递给 async_write 的写入操作都保证是块对齐的。
		virtual void async_read(storage_index_t storage, peer_request const& r
			, std::function<void(disk_buffer_holder, storage_error const&)> handler
			, disk_job_flags_t flags = {}) = 0;

		// 异步写入
		virtual bool async_write(storage_index_t storage, peer_request const& r
			, char const* buf, std::shared_ptr<disk_observer> o
			, std::function<void(storage_error const&)> handler
			, disk_job_flags_t flags = {}) = 0;

		// 计算指定 piece 的哈希
		//
		// Compute hash(es) for the specified piece. Unless the v1_hash flag is
		// set (in ``flags``), the SHA-1 hash of the whole piece does not need
		// to be computed.
		// 为指定的 piece 计算哈希值。除非在 flags 中设置了 v1_hash 标志，
		// 否则无需计算整个 piece 的 SHA-1 哈希值。
		//
		// The `v2` span is optional and can be empty, which means v2 hashes
		// should not be computed. If v2 is non-empty it must be at least large
		// enough to hold all v2 blocks in the piece, and this function will
		// fill in the span with the SHA-256 block hashes of the piece.
		// v2 范围（span）是可选的，可以为空，这意味着不应计算 v2 哈希值。
		// 如果 v2 不为空，那么它必须有足够的空间来存储该片段中所有 v2 块的 SHA-56 hash 值，
		// 并且此函数将使用该 piece 的 SHA-256 block hashes 填充该范围。
		virtual void async_hash(storage_index_t storage, piece_index_t piece, span<sha256_hash> v2
			, disk_job_flags_t flags
			, std::function<void(piece_index_t, sha1_hash const&, storage_error const&)> handler) = 0;

		// 计算单个 block 的 v2 哈希
		//
		// computes the v2 hash (SHA-256) of a single block. The block at
		// ``offset`` in piece ``piece``.
		virtual void async_hash2(storage_index_t storage, piece_index_t piece, int offset, disk_job_flags_t flags
			, std::function<void(piece_index_t, sha256_hash const&, storage_error const&)> handler) = 0;

		// 移动文件位置
		//
		// called to request the files for the specified storage/torrent be
		// moved to a new location. It is the disk I/O object's responsibility
		// to synchronize this with any currently outstanding disk operations to
		// the storage. Whether files are replaced at the destination path or
		// not is controlled by ``flags`` (see move_flags_t).
		// 调用此函数是为了请求将指定 storage/torrent 的相关文件移动到新的位置。
		// disk I/O object 有责任将此操作与当前对该 storage 进行的所有未完成磁盘操作进行同步。
		// 目标路径下的文件是否被替换由 flags 参数控制（参见 move_flags_t）。
		virtual void async_move_storage(storage_index_t storage, std::string p, move_flags_t flags
			, std::function<void(status_t, std::string const&, storage_error const&)> handler) = 0;

		// 释放文件
		//
		// This is called on disk I/O objects to request they close all open
		// files for the specified storage/torrent. If file handles are not
		// pooled/cached, it can be a no-op. For truly asynchronous disk I/O,
		// this should provide at least one point in time when all files are
		// closed. It is possible that later asynchronous operations will
		// re-open some of the files, by the time this completion handler is
		// called, that's fine.
		// 此操作会在磁盘 I/O 对象上被调用，用于请求关闭指定 storage/torrent 对应的所有已打开文件。
		// 如果文件句柄没有被 pooled/cached，那么该操作可以为空操作。
		// 对于真正的异步磁盘 I/O，此操作应至少确保在某一时刻所有文件都处于关闭状态。
		// 可能后续的异步操作会重新打开部分文件，当调用此完成处理程序时，出现这种情况是正常的。
		virtual void async_release_files(storage_index_t storage
			, std::function<void()> handler = std::function<void()>()) = 0;

		// 检查文件完整性
		// this is called when torrents are added to validate their resume data
		// against the files on disk. This function is expected to do a few things:
		// 当添加种子文件时会调用此函数，用于根据磁盘上的文件验证其恢复数据。此函数预期完成以下几件事：
		//
		// if ``links`` is non-empty, it contains a string for each file in the
		// torrent. The string being a path to an existing identical file. The
		// default behavior is to create hard links of those files into the
		// storage of the new torrent (specified by ``storage``). An empty
		// string indicates that there is no known identical file. This is part
		// of the "mutable torrent" feature, where files can be reused from
		// other torrents.
		// 如果 links 不为空，它为种子文件中的每个文件包含一个字符串。
		// 该字符串是一个指向现有相同文件的路径。
		// 默认行为是将这些文件创建硬链接到新种子文件的存储位置（由 storage 指定）。
		// 空字符串表示不存在已知的相同文件。
		// 这是 “可变种子文件” 功能的一部分，在该功能中，文件可以从其他种子文件中复用。
		//
		// The ``resume_data`` points the resume data passed in by the client.
		// resume_data 指向客户端传入的恢复数据。
		//
		// If the ``resume_data->flags`` field has the seed_mode flag set, all
		// files/pieces are expected to be on disk already. This should be
		// verified. Not just the existence of the file, but also that it has
		// the correct size.
		// 如果 resume_data->flags 字段设置了 seed_mode 标志，
		// 则预期所有文件 / 片段都已存在于磁盘上。这一点需要进行验证，
		// 不仅要验证文件是否存在，还要验证其大小是否正确。
		//
		// Any file with a piece set in the ``resume_data->have_pieces`` bitmask
		// should exist on disk, this should be verified. Pad files and files
		// with zero priority may be skipped.
		// resume_data->have_pieces 位掩码中设置了片段的任何文件都应该存在于磁盘上，
		// 这一点也需要进行验证。填充文件和优先级为零的文件可以跳过验证。
		virtual void async_check_files(storage_index_t storage
			, add_torrent_params const* resume_data
			, aux::vector<std::string, file_index_t> links
			, std::function<void(status_t, storage_error const&)> handler) = 0;

		// 停止 torrent
		//
		// This is called when a torrent is stopped. It gives the disk I/O
		// object an opportunity to flush any data to disk that's currently kept
		// cached. This function should at least do the same thing as
		// async_release_files().
		// 当一个种子文件停止下载或做种时会调用此函数。
		// 它为磁盘 I/O 对象提供了一个机会，将当前缓存中的任何数据刷新到磁盘上。
		// 此函数至少应执行与 async_release_files() 函数相同的操作。
		virtual void async_stop_torrent(storage_index_t storage
			, std::function<void()> handler = std::function<void()>()) = 0;

		// 重命名文件
		//
		// This function is called when the name of a file in the specified
		// storage has been requested to be renamed. The disk I/O object is
		// responsible for renaming the file without racing with other
		// potentially outstanding operations against the file (such as read,
		// write, move, etc.).
		// 当请求对指定存储中的某个文件进行重命名时，会调用此函数。
		// 磁盘 I/O 对象负责对文件进行重命名操作，
		// 同时要避免与其他可能正在对该文件进行的未完成操作（如读取、写入、移动等）发生竞态条件。
		virtual void async_rename_file(storage_index_t storage
			, file_index_t index, std::string name
			, std::function<void(std::string const&, file_index_t, storage_error const&)> handler) = 0;

		// 删除文件
		//
		// This function is called when some file(s) on disk have been requested
		// to be removed by the client. ``storage`` indicates which torrent is
		// referred to. See session_handle for ``remove_flags_t`` flags
		// indicating which files are to be removed.
		// e.g. session_handle::delete_files - delete all files
		// session_handle::delete_partfile - only delete part file.
		// 当客户端请求删除磁盘上的某些文件时，会调用此函数。
		// storage 参数指明了所涉及的是哪个种子文件。
		// 有关指示要删除哪些文件的 remove_flags_t 标志，请参考 session_handle。
		virtual void async_delete_files(storage_index_t storage, remove_flags_t options
			, std::function<void(storage_error const&)> handler) = 0;

		// 设置文件优先级
		//
		// This is called to set the priority of some or all files. Changing the
		// priority from or to 0 may involve moving data to and from the
		// partfile. The disk I/O object is responsible for correctly
		// synchronizing this work to not race with any potentially outstanding
		// asynchronous operations affecting these files.
		// 此函数用于设置部分或全部文件的优先级。
		// 将文件优先级设置为 0 或者从 0 更改优先级时，可能需要在部分文件（partfile）和正常存储之间移动数据。
		// 磁盘 I/O 对象需要正确同步此操作，避免与任何可能正在进行的、影响这些文件的异步操作产生竞态条件。
		//
		// ``prio`` is a vector of the file priority for all files. If it's
		// shorter than the total number of files in the torrent, they are
		// assumed to be set to the default priority.
		virtual void async_set_file_priority(storage_index_t storage
			, aux::vector<download_priority_t, file_index_t> prio
			, std::function<void(storage_error const&
				, aux::vector<download_priority_t, file_index_t>)> handler) = 0;

		// 清除指定的 piece 数据
		//
		// This is called when a piece fails the hash check, to ensure there are
		// no outstanding disk operations to the piece before blocks are
		// re-requested from peers to overwrite the existing blocks. The disk I/O
		// object does not need to perform any action other than synchronize
		// with all outstanding disk operations to the specified piece before
		// posting the result back.
		// 当一个片段的哈希校验失败时会调用此函数，
		// 以确保在向对等节点重新请求数据块来覆盖现有数据块之前，针对该片段没有未完成的磁盘操作。
		// 磁盘 I/O 对象除了在返回结果之前与针对指定片段的所有未完成磁盘操作进行同步之外，无需执行其他操作。
		virtual void async_clear_piece(storage_index_t storage, piece_index_t index
			, std::function<void(piece_index_t)> handler) = 0;

		// 更新统计计数器
		//
		// update_stats_counters() is called to give the disk storage an
		// opportunity to update gauges in the ``c`` stats counters, that aren't
		// updated continuously as operations are performed. This is called
		// before a snapshot of the counters are passed to the client.
		// 调用 update_stats_counters() 函数是为了让磁盘存储模块有机会更新 c counters 里，
		// 那些不会在操作执行时持续更新的指标。此函数会在将计数器的快照传递给客户端之前被调用。
		virtual void update_stats_counters(counters& c) const = 0;

		// 获取打开文件状态
		//
		// Return a list of all the files that are currently open for the
		// specified storage/torrent. This is is just used for the client to
		// query the currently open files, and which modes those files are open
		// in.
		// 返回指定 storage/torrent 当前已打开的所有文件的列表。
		// 此功能仅用于供客户端查询当前已打开的文件以及这些文件的打开模式。
		virtual std::vector<open_file_state> get_status(storage_index_t) const = 0;

		// 中止操作
		//
		// this is called when the session is starting to shut down. The disk
		// I/O object is expected to flush any outstanding write jobs, cancel
		// hash jobs and initiate tearing down of any internal threads. If
		// ``wait`` is true, this should be asynchronous. i.e. this call should
		// not return until all threads have stopped and all jobs have either
		// been aborted or completed and the disk I/O object is ready to be
		// destructed.
		// 当会话开始关闭时会调用此函数。
		// 期望磁盘 I/O 对象刷新所有未完成的写入任务，取消哈希计算任务，并启动内部线程的清理工作。
		// 如果 wait 为 true，则该操作应该是同步的。
		// 也就是说，在所有线程停止、所有任务要么被中止要么完成，并且磁盘 I/O 对象准备好被销毁之前，此调用不应返回。
		virtual void abort(bool wait) = 0;

		// 提交作业
		//
		// This will be called after a batch of disk jobs has been issues (via
		// the ``async_*`` ). It gives the disk I/O object an opportunity to
		// notify any potential condition variables to wake up the disk
		// thread(s). The ``async_*`` calls can of course also notify condition
		// variables, but doing it in this call allows for batching jobs, by
		// issuing the notification once for a collection of jobs.
		// 在一批磁盘作业被发出（通过 async_* 系列函数）之后会调用此函数。
		// 它为磁盘 I/O 对象提供了一个机会，使其能够通知任何潜在的条件变量，从而唤醒磁盘线程。
		// 当然，async_* 系列调用也能够通知条件变量，但在这个函数调用中进行通知，
		// 可以通过为一批作业仅发出一次通知来实现作业的批处理。
		virtual void submit_jobs() = 0;

		// 设置更新通知
		//
		// This is called to notify the disk I/O object that the settings have
		// been updated. In the disk io constructor, a settings_interface
		// reference is passed in. Whenever these settings are updated, this
		// function is called to allow the disk I/O object to react to any
		// changed settings relevant to its operations.
		// 调用此函数是为了通知磁盘 I/O 对象，设置已更新。
		// 在磁盘 I/O 的构造函数中，会传入一个 settings_interface 引用。
		// 每当这些设置被更新时，就会调用此函数，
		// 以便磁盘 I/O 对象能够对任何与其操作相关的更改设置做出反应。
		virtual void settings_updated() = 0;

		// hidden
		virtual ~disk_interface() {}
	};

	// 存储持有者
	// 这是一个 RAII (资源获取即初始化) 包装器，用于管理 torrent 存储的生命周期，
	// 当 storage_holder 析构时，会调用 disk_interface::remove_torrent(m_idx)，
	// 来确保当 torrent 被移除时，存储会被正确清理。
	// 
	// a unique, owning, reference to the storage of a torrent in a disk io
	// subsystem (class that implements disk_interface). This is held by the
	// internal libtorrent torrent object to tie the storage object allocated
	// for a torrent to the lifetime of the internal torrent object. When a
	// torrent is removed from the session, this holder is destructed and will
	// inform the disk object.
	// 这是对磁盘 I/O 子系统（实现 disk_interface 的类）中某个 torrent 存储的唯一、具有所有权的引用。
	// libtorrent 的内部 torrent 对象持有该引用，目的是将为某个 torrent 分配的存储对象与内部 torrent 对象的生命周期关联起来。
	// 当某个 torrent 从会话中移除时，这个持有者对象会被销毁，并且会通知磁盘对象。
	struct TORRENT_EXPORT storage_holder
	{
		storage_holder() = default;
		storage_holder(storage_index_t idx, disk_interface& disk_io)
			: m_disk_io(&disk_io)
			, m_idx(idx)
		{}
		~storage_holder()
		{
			if (m_disk_io) m_disk_io->remove_torrent(m_idx);
		}

		explicit operator bool() const { return m_disk_io != nullptr; }

		operator storage_index_t() const
		{
			TORRENT_ASSERT(m_disk_io);
			return m_idx;
		}

		void reset()
		{
			if (m_disk_io) m_disk_io->remove_torrent(m_idx);
			m_disk_io = nullptr;
		}

		storage_holder(storage_holder const&) = delete;
		storage_holder& operator=(storage_holder const&) = delete;

		storage_holder(storage_holder&& rhs) noexcept
			: m_disk_io(rhs.m_disk_io)
			, m_idx(rhs.m_idx)
		{
			rhs.m_disk_io = nullptr;
		}

		storage_holder& operator=(storage_holder&& rhs) noexcept
		{
			if (&rhs == this) return *this;
			if (m_disk_io) m_disk_io->remove_torrent(m_idx);
			m_disk_io = rhs.m_disk_io;
			m_idx = rhs.m_idx;
			rhs.m_disk_io = nullptr;
			return *this;
		}
	private:
		disk_interface* m_disk_io = nullptr;
		storage_index_t m_idx{0};
	};

} // namespace libtorrent

#endif
