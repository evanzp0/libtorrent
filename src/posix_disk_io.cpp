/*

Copyright (c) 2016, 2019-2022, Arvid Norberg
Copyright (c) 2017-2018, Steven Siloti
Copyright (c) 2020, Alden Torres
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

#include "libtorrent/config.hpp"
#include "libtorrent/posix_disk_io.hpp"
#include "libtorrent/disk_interface.hpp"
#include "libtorrent/aux_/disk_buffer_pool.hpp"
#include "libtorrent/io_context.hpp"
#include "libtorrent/performance_counters.hpp"
#include "libtorrent/aux_/session_settings.hpp"
#include "libtorrent/aux_/path.hpp"
#include "libtorrent/aux_/numeric_cast.hpp"
#include "libtorrent/aux_/posix_storage.hpp"
#include "libtorrent/stat_cache.hpp"
#include "libtorrent/file_storage.hpp"
#include "libtorrent/hasher.hpp"
#include "libtorrent/add_torrent_params.hpp"
#include "libtorrent/aux_/storage_free_list.hpp"

#include <vector>

namespace libtorrent {

namespace {

	using aux::posix_storage;

} // anonymous namespace

	struct TORRENT_EXTRA_EXPORT posix_disk_io final
		: disk_interface
	{
		posix_disk_io(io_context& ios, settings_interface const& sett, counters& cnt)
			: m_settings(sett)
			, m_buffer_pool(ios)
			, m_stats_counters(cnt)
			, m_ios(ios)
		{
			settings_updated();
		}

		void settings_updated() override
		{
			m_buffer_pool.set_settings(m_settings);
		}

		/**
		 * 创建新的 torrent 存储，分配存储索引
		 * 
		 * @param params 包含创建存储所需的所有参数(如文件路径、文件大小等)
		 * 
		 * @return 返回 storage_holder 对象，用于管理存储的生命周期
		 */
		storage_holder new_torrent(storage_params const& params
			, std::shared_ptr<void> const&) override
		{
			// make sure we can remove this torrent without causing a memory
			// allocation, by causing the allocation now instead
			// 使用 m_free_slots (空闲槽位管理器)获取一个可用索引，
			// 如果没有空闲槽位，则返回 m_torrents.end_index() 表示需要新增。
			storage_index_t const idx = m_free_slots.new_index(m_torrents.end_index());

			// 创建一个新的 posix_storage 对象
			auto storage = std::make_unique<posix_storage>(params);

			if (idx == m_torrents.end_index()) 
				// 如果索引是新的(end_index)，则添加到 m_torrents 向量末尾
				m_torrents.emplace_back(std::move(storage));
			else
				// 如果是重用现有槽位，则替换指定位置的存储对象
				m_torrents[idx] = std::move(storage);

			return storage_holder(idx, *this);
		}

		/**
		 * // 移除指定索引的 torrent storage
		 */
		void remove_torrent(storage_index_t const idx) override
		{
			// 释放指定索引位置的存储对象
			m_torrents[idx].reset();

			// 将索引加入空闲槽位列表，供后续new_torrent复用
			m_free_slots.add(idx);
		}

		void abort(bool) override {}

		/**
		 * @brief 异步读取指定存储块的数据
		 * 
		 * @param storage 存储索引，标识要操作的torrent存储位置
		 * @param r 读取文件数据依据的 peer_request 参数，包含piece索引、偏移量和长度等信息
		 * @param handler 读取完成后的回调函数，该回调函数的参数为：接收数据缓冲区、错误信息
		 * @param flags 磁盘作业标志位(当前未使用)
		 * 
		 * @return void 异步操作无直接返回值，结果通过回调函数返回
		 * 
		 * @note 该函数执行流程：
		 * 1. 从缓冲池分配内存缓冲区
		 * 2. 执行同步读取操作
		 * 3. 通过 io_context 异步返回结果
		 * 4. 自动更新读取统计计数器
		 * 
		 * @note 回调函数将在 io_context 所在的线程执行
		 */
		void async_read(storage_index_t storage, peer_request const& r
			, std::function<void(disk_buffer_holder block, storage_error const& se)> handler
			, disk_job_flags_t) override
		{
			// 从缓冲池分配缓冲区，默认 16 KiB 的块
			disk_buffer_holder buffer = disk_buffer_holder(m_buffer_pool, 
				m_buffer_pool.allocate_buffer("send buffer"), 
				default_block_size);

			// 缓冲区分配失败处理
			storage_error error;
			if (!buffer)
			{
				error.ec = errors::no_memory;
				error.operation = operation_t::alloc_cache_piece;

				// 通过 io_context 异步返回错误，错误最终在 peer_connection::on_disk_read_complete 被处理
				post(m_ios, 
					[this, error, h = std::move(handler)] {
						h(disk_buffer_holder(m_buffer_pool, nullptr, 0), error);
					}
				);
				return;
			}

			// 记录操作开始时间(用于性能统计)
			time_point const start_time = clock_type::now();

			// 准备数据读取的缓冲区视图
			span<char> const buf = {buffer.data(), r.length};
			
			// 根据 peer_request 的要求，通过 posix_storage 读取数据到 buf
			m_torrents[storage]->read(m_settings, buf, r.piece, r.start, error);

			// 成功读取时的统计更新
			if (!error.ec)
			{
				// 计算读取耗时(微秒)
				std::int64_t const read_time = total_microseconds(clock_type::now() - start_time);

				// 更新各种统计计数器
				m_stats_counters.inc_stats_counter(counters::num_blocks_read);				// 读取块数
				m_stats_counters.inc_stats_counter(counters::num_read_ops);					// 读取操作次数
				m_stats_counters.inc_stats_counter(counters::disk_read_time, read_time);	// 读取总时间
				m_stats_counters.inc_stats_counter(counters::disk_job_time, read_time);		// 磁盘作业总时
			}

			// 通过 io_context 异步返回结果
			post(m_ios, 
				[h = std::move(handler), b = std::move(buffer), error] () mutable { 
					h(std::move(b), error); 
				}
			);
		}

		/**
		 * @brief 异步写入数据块到存储中
		 * 
		 * 该函数将指定的数据异步写入到 torrent 存储中，并统计写入操作的性能指标。
		 * 操作完成后通过回调函数通知调用方结果。
		 * 
		 * @param storage 存储索引，标识要写入的目标 torrent 存储
		 * @param r 写入请求信息，包含块位置和大小等
		 * @param buf 要写入的数据缓冲区指针
		 * @param 磁盘观察者对象(未使用)
		 * @param handler 写入完成后的回调函数
		 * @param 磁盘作业标志(未使用)
		 * @return bool 总是返回 false，表示操作已异步处理
		 */
		bool async_write(storage_index_t storage, peer_request const& r
			, char const* buf, std::shared_ptr<disk_observer>
			, std::function<void(storage_error const&)> handler
			, disk_job_flags_t) override
		{
			// 准备数据视图（避免拷贝，直接引用原始缓冲区）
			span<char> const b = { const_cast<char*>(buf), r.length };

			// 记录操作开始时间（用于计算写入耗时）
			time_point const start_time = clock_type::now();

			// 执行实际同步写入操作
			storage_error error;
			m_torrents[storage]->write(m_settings, b, r.piece, r.start, error);

			if (!error.ec)
			{
				// 计算写入耗时（微秒级精度）
				std::int64_t const write_time = total_microseconds(clock_type::now() - start_time);

				// 更新各种统计指标
				m_stats_counters.inc_stats_counter(counters::num_blocks_written);			// 写入块数+1
				m_stats_counters.inc_stats_counter(counters::num_write_ops);				// 写入操作次数+1
				m_stats_counters.inc_stats_counter(counters::disk_write_time, write_time);	// 累计写入时间
				m_stats_counters.inc_stats_counter(counters::disk_job_time, write_time);	// 累计磁盘作业时间
			}

			// 通过 io_context 异步返回结果，使用 post 确保回调在网络线程中执行
			post(m_ios, [=, h = std::move(handler)]{ h(error); });

			// 历史遗留返回值，始终返回false（无实际意义）
			return false;
		}

		/**
		 * @brief 计算指定 piece 的哈希值（支持同时计算 v1 和 v2），在创建 .torrent 文件时会使用。
		 * 
		 * @param storage 存储索引，标识目标torrent存储
		 * @param piece 需要校验的piece索引
		 * @param block_hashes [v2专用] 输出参数，存储每个16KB块的SHA-256哈希
		 * @param flags 控制标志，指定是否计算v1/v2哈希
		 * @param handler 完成回调，返回piece索引、v1哈希和错误信息
		 */
		void async_hash(
			storage_index_t storage,           
			piece_index_t const piece,         
			span<sha256_hash> block_hashes,    
			disk_job_flags_t flags,
			std::function<void(
				piece_index_t, 			// 原 piece 索引
				sha1_hash const&,		// v1 哈希结果
				storage_error const&	// 错误信息
			)> handler
		) override
		{
			// 记录开始时间用于性能统计
			time_point const start_time = clock_type::now();

			// 解析标志位决定校验模式
			bool const v1 = bool(flags & disk_interface::v1_hash);
			bool const v2 = !block_hashes.empty();

			// 分配临时缓冲区（默认16KB块大小），用于存放 v1 hash
			disk_buffer_holder buffer = disk_buffer_holder(m_buffer_pool, m_buffer_pool.allocate_buffer("hash buffer"), default_block_size);
			storage_error error;
			// 内存分配失败处理
			if (!buffer)
			{
				error.ec = errors::no_memory;
				error.operation = operation_t::alloc_cache_piece;
				post(m_ios, [=, h = std::move(handler)]{ h(piece, sha1_hash{}, error); });
				return;
			}

			// v1 SHA-1哈希计算器
			hasher ph;

			posix_storage* st = m_torrents[storage].get();

			// 获取piece尺寸信息
			int const piece_size = v1 ? st->files().piece_size(piece) : 0;		// v1 piece大小
			int const piece_size2 = v2 ? st->files().piece_size2(piece) : 0;	// v2 piece大小
			int const blocks_in_piece = v1 ? (piece_size + default_block_size - 1) / default_block_size : 0; // v1块数，向上取整
			int const blocks_in_piece2 = v2 ? st->files().blocks_in_piece2(piece) : 0;	// v2块数

			TORRENT_ASSERT(!v2 || int(block_hashes.size()) >= blocks_in_piece2);

			// 分块读取和哈希计算 ---------

			int offset = 0;
			int const blocks_to_read = std::max(blocks_in_piece, blocks_in_piece2);
			for (int i = 0; i < blocks_to_read; ++i)
			{
				// 当前块是否需要v2计算
				bool const v2_block = i < blocks_in_piece2; 

				// 计算当前块的有效长度
				// - 正常情况（当前块不是最后一个块），piece_size - offset >= default_block_size，所以 len = default_block_size（16KB）。
				// - 最后一个块（剩余数据不足 16KB），比如 piece_size = 260KB，offset = 256KB，剩余 260 - 256 = 4KB。此时 len = std::min(16KB, 4KB) = 4KB，避免读取越界。
				auto const len = v1 ? std::min(default_block_size, piece_size - offset) : 0;
				auto const len2 = v2_block ? std::min(default_block_size, piece_size2 - offset) : 0;

				// span<char> b 是对 buffer 的一个切片，它并不重新分配内存，而是限制当前操作的读写范围。
				// 如果当前块是 最后一个块，len 或 len2 可能小于 default_block_size（比如只剩 4KB 数据）。
				span<char> const b = {
					buffer.data(), 
					std::max(len, len2) // 确保缓冲区足够大，能同时满足 v1 和 v2 的计算需求。
				};

				// 从存储读取数据块
				int const ret = st->read(m_settings, b, piece, offset, error);
				offset += default_block_size;

				// 读取失败或EOF
				if (ret <= 0) break;

				// v1哈希更新（累积计算）
				if (v1)
					ph.update(b.first(std::min(ret, len))); // 正常读取（ret == len），读取失败或 EOF（ret < len）

				// v2块哈希计算（独立计算每个块）
				if (v2_block)
					block_hashes[i] = hasher256(b.first(std::min(ret, len2))).final();
			}

			// 最终化v1哈希
			sha1_hash const hash = v1 ? ph.final() : sha1_hash();

			// 更新统计计数器（成功时）
			if (!error.ec)
			{
				// 计算本次哈希操作的耗时（微秒）
				std::int64_t const read_time = total_microseconds(clock_type::now() - start_time);

				// 1. 记录回读块数量（用于统计缓存命中率等）
				// 在BitTorrent客户端中，"回读"（read-back）指的是 从磁盘重新读取已下载但未被缓存的数据块。这种情况通常发生在：
				// - 缓存失效：之前下载的数据块已被移出内存缓存
				// - 哈希校验：需要重新读取数据块以验证完整性
				// - 做种上传：为其他peer提供数据时需要从磁盘读取
				// 结合num_blocks_read可计算缓存命中率：缓存命中率 = 1 - (num_read_back / num_blocks_read)
				m_stats_counters.inc_stats_counter(counters::num_read_back, blocks_to_read);
				// 2. 记录实际读取的块数量（用于I/O吞吐量统计）
				m_stats_counters.inc_stats_counter(counters::num_blocks_read, blocks_to_read);
				// 3. 记录读操作次数（用于磁盘负载评估）
				m_stats_counters.inc_stats_counter(counters::num_read_ops, blocks_to_read);
				// 4. 记录哈希计算耗时（用于性能分析）
				m_stats_counters.inc_stats_counter(counters::disk_hash_time, read_time);
				// 5. 记录总任务耗时（用于整体性能监控）
				m_stats_counters.inc_stats_counter(counters::disk_job_time, read_time);
			}

			// 异步返回结果
			post(m_ios, [=, h = std::move(handler)]{ h(piece, hash, error); });
		}

		/**
		 * @brief 异步计算指定数据块的SHA-256哈希值（BitTorrent v2协议专用）
		 * 
		 * @param storage 存储索引，标识目标torrent
		 * @param piece 数据块索引
		 * @param offset 块内偏移量（字节）
		 * @param flags 磁盘操作标志位（保留参数）
		 * @param handler 异步回调函数，包含三个参数：
		 *                - piece_index_t: 原始数据块索引
		 *                - sha256_hash: 计算出的哈希值
		 *                - storage_error: 错误信息
		 * 
		 * @note 函数特性：
		 * 1. 采用16KB固定缓冲区（0x4000字节）
		 * 2. 自动处理边界条件（末块截断）
		 * 3. 内置性能统计（耗时/吞吐量）
		 * 4. 线程安全（通过IOService派发结果）
		 * 
		 * @warning 内存分配失败会立即触发错误回调
		 * 
		 * @par 典型调用流程：
		 * 1. 分配缓冲区
		 * 2. 读取磁盘数据
		 * 3. 计算SHA-256
		 * 4. 更新统计信息
		 * 5. 异步返回结果
		 * 
		 * @see async_hash() v1版本哈希函数
		 * @see posix_storage::read() 底层读取实现
		 */
		void async_hash2(storage_index_t storage, piece_index_t const piece, int offset, disk_job_flags_t
			, std::function<void(piece_index_t, sha256_hash const&, storage_error const&)> handler) override
		{
			// 记录起始时间点（用于耗时统计）
			time_point const start_time = clock_type::now();

			// 申请16KB对齐的内存缓冲区，用于存放 v2 hash
			disk_buffer_holder buffer = disk_buffer_holder(m_buffer_pool, m_buffer_pool.allocate_buffer("hash buffer"), 0x4000);

			// 内存分配失败处理
			storage_error error;
			if (!buffer)
			{
				error.ec = errors::no_memory;
				error.operation = operation_t::alloc_cache_piece;
				post(m_ios, [=, h = std::move(handler)]{ h(piece, sha256_hash{}, error); });
				return;
			}

			// 获取对应 torrent 的存储接口
			posix_storage* st = m_torrents[storage].get();
			int const piece_size = st->files().piece_size2(piece);

			// 计算有效数据长度（防止越界），取最小值：默认块大小 vs 剩余数据长度
			std::ptrdiff_t const len = std::min(default_block_size, piece_size - offset);

			// SHA-256哈希计算器实例
			hasher256 ph;

			// 创建受限数据切片
			span<char> const b = {buffer.data(), len};

			// 执行同步磁盘读取
			int const ret = st->read(m_settings, b, piece, offset, error);

			// 仅当成功读取时才更新哈希
			if (ret > 0)
				ph.update(b.first(ret));

			// 最终化哈希值
			sha256_hash const hash = ph.final();

			// 更新指标
			if (!error.ec)
			{
				std::int64_t const read_time = total_microseconds(clock_type::now() - start_time);

				m_stats_counters.inc_stats_counter(counters::num_read_back);	// 回读计数
				m_stats_counters.inc_stats_counter(counters::num_blocks_read);	// 块读取计数
				m_stats_counters.inc_stats_counter(counters::num_read_ops);		// 操作次数
				m_stats_counters.inc_stats_counter(counters::disk_hash_time, read_time);	// 纯哈希耗时（µs）
				m_stats_counters.inc_stats_counter(counters::disk_job_time, read_time);		// 总耗时（µs）
			}

			// 通过 io_context 异步返回结果
			post(m_ios, [=, h = std::move(handler)]{ h(piece, hash, error); });
		}

		/**
		 * @brief 异步移动或重命名种子文件的存储位置
		 * 
		 * 该函数用于将指定种子的存储目录整体迁移到新路径，支持异步回调返回操作结果。
		 * 典型场景：用户手动更改下载保存路径，或客户端自动整理文件存储结构。
		 * 
		 * @param storage 存储索引，用于定位目标种子
		 * @param p 目标路径字符串，可以是绝对路径或相对路径
		 * @param flags 移动标志位，控制具体操作行为：
		 *              - move_flags_t::overwrite_existing 覆盖已存在文件
		 *              - move_flags_t::fail_if_exist 目标存在时失败
		 *              - move_flags_t::dont_replace 保留原文件（默认）
		 * @param handler 操作结果回调函数，包含三个参数：
		 *                - status_t 操作状态码
		 *                - std::string 最终生效的存储路径（可能与输入不同）
		 *                - storage_error 错误详细信息
		 * 
		 * @warning 重要限制：
		 * - 移动过程中会暂停该种子的所有I/O操作
		 * - 对多文件种子（即包含文件夹的种子）必须确保目标路径存在
		 * - Windows系统下可能需要管理员权限才能跨磁盘移动
		 */
		void async_move_storage(storage_index_t const storage, std::string p
			, move_flags_t const flags
			, std::function<void(status_t, std::string const&, storage_error const&)> handler) override
		{
			// 获取对应的存储对象
			posix_storage* st = m_torrents[storage].get();

			// 准备错误收集器和状态码
			storage_error ec;
			status_t ret;

			// 执行实际的存储移动操作
    		// 注意：返回的路径 p 可能被调整（如添加了数字后缀解决冲突）
			std::tie(ret, p) = st->move_storage(p, flags, ec);

			// 通过 io_context 异步返回结果
			post(m_ios, [=, h = std::move(handler)]{ h(ret, p, ec); });
		}

		/**
		 * @brief 异步释放指定种子的文件句柄和资源
		 * 
		 * 该函数用于主动释放种子关联的文件系统资源，通常在以下场景调用：
		 * - 种子暂停下载时减少资源占用
		 * - 客户端退出前清理资源
		 * - 做种时遇到磁盘错误需要重置状态
		 * 
		 * @param storage 存储索引，标识目标种子
		 * @param handler 操作完成后的回调函数（可为空）
		 * 
		 * @note 资源释放行为：
		 * - 关闭所有打开的文件描述符
		 * - 清空内存缓存（如有）
		 * - 保持文件内容完整性（不会删除文件）
		 */
		void async_release_files(storage_index_t storage, std::function<void()> handler) override
		{
			posix_storage* st = m_torrents[storage].get();

			// 同步执行文件资源释放， 立即关闭文件句柄和清理缓存
			st->release_files();

			if (!handler) return;
			post(m_ios, [=]{ handler(); });
		}

		/**
		 * @brief 异步删除指定种子关联的物理文件
		 * 
		 * 该函数用于永久移除磁盘上与种子关联的所有数据文件，适用于：
		 * - 用户手动删除下载任务
		 * - 自动清理已完成种子
		 * - 磁盘空间回收操作
		 * 
		 * @param storage 存储索引，标识目标种子
		 * @param options 删除选项标志位，可选值：
		 *                - remove_flags_t::files      删除数据文件
		 *                - remove_flags_t::partfile   删除部分下载文件
		 *                - remove_flags_t::directories 删除空目录
		 * @param handler 操作结果回调，携带错误信息
		 * 
		 * @warning 危险操作
		 * - 删除后不可恢复（不同于移到回收站）
		 * - 会终止该种子的所有活动
		 * 
		 * @note 实际行为取决于存储实现：
		 * - 多文件种子：递归删除整个目录树
		 * - 单文件种子：仅删除目标文件
		 * - 支持保留部分文件（通过options控制）
		 */
		void async_delete_files(storage_index_t storage, remove_flags_t const options
			, std::function<void(storage_error const&)> handler) override
		{
			storage_error error;
			posix_storage* st = m_torrents[storage].get();
			st->delete_files(options, error);
			post(m_ios, [=, h = std::move(handler)]{ h(error); });
		}

		/**
		 * @brief 异步校验种子文件完整性
		 * 
		 * 该函数用于验证本地文件与种子元数据的匹配情况，主要场景：
		 * - 启动时检查已下载文件的完整性
		 * - 恢复下载时校验续传数据有效性
		 * - 手动触发文件重新校验
		 * 
		 * @param storage 存储索引，标识目标种子
		 * @param resume_data 续传数据指针（可为空）
		 * @param links 文件硬链接路径列表（可选）
		 * @param handler 校验结果回调，包含：
		 *                - status_t 校验状态
		 *                - storage_error 错误信息
		 * 
		 * @note 校验逻辑：
		 * 1. 初始化存储系统
		 * 2. 验证续传数据有效性
		 * 3. 根据策略决定是否需要完整校验
		 * 
		 * @warning 性能影响：
		 * - 完整校验会扫描所有文件内容
		 * - 大种子校验可能耗时较长
		 */
		void async_check_files(storage_index_t storage
			, add_torrent_params const* resume_data
			, aux::vector<std::string, file_index_t> links
			, std::function<void(status_t, storage_error const&)> handler) override
		{
			posix_storage* st = m_torrents[storage].get();

			// 处理空续传数据情况
			add_torrent_params tmp;
			add_torrent_params const* rd = resume_data ? resume_data : &tmp;

			storage_error error;

			// 核心校验逻辑（立即执行的 lambda）
			status_t const ret = [&]
			{
				// 初始化存储
				auto const ret_flag = st->initialize(m_settings, error);
				if (error) return status_t::fatal_disk_error | ret_flag;

				// 验证续传数据
				bool const verify_success = st->verify_resume_data(*rd
					, std::move(links), error);

				// 检查是否跳过校验（根据设置）
				if (m_settings.get_bool(settings_pack::no_recheck_incomplete_resume))
					return status_t::no_error | ret_flag;

				if (!aux::contains_resume_data(*rd))
				{
					// if we don't have any resume data, we still may need to trigger a
					// full re-check, if there are *any* files.
					// 无续传数据时，存在的任何文件，都要完整校验
					storage_error ignore;
					return ((st->has_any_file(ignore))
						? status_t::need_full_check
						: status_t::no_error)
						| ret_flag;
				}

				// 根据验证结果返回状态
				return (verify_success
					? status_t::no_error
					: status_t::need_full_check) // 当快速验证失败时，要求后续流程执行全量哈希校验
					| ret_flag;
			}();

			post(m_ios, [error, ret, h = std::move(handler)]{ h(ret, error); });
		}

		void async_rename_file(storage_index_t const storage
			, file_index_t const idx
			, std::string name
			, std::function<void(std::string const&, file_index_t, storage_error const&)> handler) override
		{
			posix_storage* st = m_torrents[storage].get();
			storage_error error;
			st->rename_file(idx, name, error);
			post(m_ios, [idx, error, h = std::move(handler), n = std::move(name)] () mutable
				{ h(std::move(n), idx, error); });
		}

		void async_stop_torrent(storage_index_t, std::function<void()> handler) override
		{
			if (!handler) return;
			post(m_ios, std::move(handler));
		}

		/**
		 * 设置文件下载优先级
		 */
		void async_set_file_priority(storage_index_t const storage
			, aux::vector<download_priority_t, file_index_t> prio
			, std::function<void(storage_error const&
				, aux::vector<download_priority_t, file_index_t>)> handler) override
		{
			posix_storage* st = m_torrents[storage].get();
			storage_error error;
			st->set_file_priority(m_settings, prio, error);
			post(m_ios, [p = std::move(prio), h = std::move(handler), error] () mutable
				{ h(error, std::move(p)); });
		}

		void async_clear_piece(storage_index_t, piece_index_t index
			, std::function<void(piece_index_t)> handler) override
		{
			post(m_ios, [=, h = std::move(handler)]{ h(index); });
		}

		void update_stats_counters(counters&) const override {}

		std::vector<open_file_state> get_status(storage_index_t) const override
		{ return {}; }

		void submit_jobs() override {}

	private:

		aux::vector<std::unique_ptr<posix_storage>, storage_index_t> m_torrents;

		// slots that are unused in the m_torrents vector
		aux::storage_free_list m_free_slots;

		settings_interface const& m_settings;

		// disk cache
		aux::disk_buffer_pool m_buffer_pool;

		counters& m_stats_counters;

		// callbacks are posted on this
		io_context& m_ios;
	};

	TORRENT_EXPORT std::unique_ptr<disk_interface> posix_disk_io_constructor(
		io_context& ios, settings_interface const& sett, counters& cnt)
	{
		return std::make_unique<posix_disk_io>(ios, sett, cnt);
	}
}

