/*

Copyright (c) 2016, 2019-2020, 2022, Arvid Norberg
Copyright (c) 2018, Steven Siloti
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

#ifndef TORRENT_POSIX_STORAGE
#define TORRENT_POSIX_STORAGE

#include "libtorrent/config.hpp"
#include "libtorrent/stat_cache.hpp"
#include "libtorrent/file_storage.hpp"
#include "libtorrent/storage_defs.hpp"
#include "libtorrent/aux_/storage_utils.hpp" // for iovec_t
#include "libtorrent/hex.hpp" // to_hex
#include "libtorrent/aux_/open_mode.hpp" // for aux::open_mode_t
#include "libtorrent/aux_/file_pointer.hpp"
#include "libtorrent/aux_/posix_part_file.hpp"
#include <memory>
#include <string>

namespace libtorrent {
namespace aux {

	struct session_settings;

	/**
	 * @brief POSIX文件存储实现类
	 * 
	 * 该类实现了基于POSIX文件系统的存储操作，用于管理torrent文件在磁盘上的存储和访问。
	 * 支持 Piece-based Storage、Partial Files / Partfile、File Priority 等特性。
	 * 
	 * - Partfile:
	 *   1. Libtorrent 将所有未完成块 集中存储在一个全局的临时文件中。
	 *      当某个文件的未下载完成时，该文件的所有 piece 数据会被追加到全局 partfile 中。
	 *      partfile 内部通过 逻辑映射 记录每个块属于哪个文件/偏移。
	 *   2. 当某个文件的所有块下载完成时，Libtorrent 会从 partfile 中提取该文件的全部块。
	 *      按顺序合并到最终的目标文件中，并删除 partfile 中对应的块数据。
	 */
	struct TORRENT_EXTRA_EXPORT posix_storage
	{
		explicit posix_storage(storage_params const& p);
		file_storage const& files() const;
		~posix_storage();

		int read(settings_interface const& sett
			, span<char> bufs
			, piece_index_t const piece, int const offset
			, storage_error& error);

		int write(settings_interface const& sett
			, span<char> bufs
			, piece_index_t const piece, int const offset
			, storage_error& error);

		bool has_any_file(storage_error& error);
		void set_file_priority(settings_interface const&
			, aux::vector<download_priority_t, file_index_t>& prio
			, storage_error& ec);
		bool verify_resume_data(add_torrent_params const& rd
			, aux::vector<std::string, file_index_t> const& links
			, storage_error& ec);

		void release_files();

		void delete_files(remove_flags_t options, storage_error& error);

		std::pair<status_t, std::string> move_storage(std::string const& sp
			, move_flags_t const flags, storage_error& ec);

		void rename_file(file_index_t const index, std::string const& new_filename, storage_error& ec);

		status_t initialize(settings_interface const&, storage_error& ec);

	private:

		file_pointer open_file(file_index_t idx, open_mode_t mode, std::int64_t offset
			, storage_error& ec);

		void need_partfile();
		bool use_partfile(file_index_t index) const;
		void use_partfile(file_index_t index, bool b);

		file_storage const& m_files;
		std::unique_ptr<file_storage> m_mapped_files;
		std::string m_save_path;
		stat_cache m_stat_cache;

		aux::vector<download_priority_t, file_index_t> m_file_priority;

		// this this is an array indexed by file-index. Each slot represents
		// whether this file has the part-file enabled for it. This is used for
		// backwards compatibility with pre-partfile versions of libtorrent. If
		// this vector is empty, the default is that files *do* use the partfile.
		// on startup, any 0-priority file that's found in it's original location
		// is expected to be an old-style (pre-partfile) torrent storage, and
		// those files have their slot set to false in this vector.
		// note that the vector is *sparse*, it's only allocated if a file has its
		// entry set to false, and only indices up to that entry.
		//
		// 这是一个由文件索引进行索引的数组。数组中的每个元素表示对应文件是否启用了 part-file（部分文件）功能。
		// 这是为了与旧版本（支持部分文件功能之前版本）的 libtorrent 保持向后兼容性。
		// 如果这个向量为空，默认情况下所有文件都 会 使用 part-file。
		// 在启动时，如果发现某个优先级为 0 的文件处于其原始位置，
		// 那么该文件会被视为旧格式（支持 part-file 功能之前的格式）的种子存储文件，
		// 并且在这个向量中，这些文件对应的元素会被设为 false。
		// 请注意，这个向量是 稀疏的，只有当某个文件对应的元素被设为 false 时，才会为该向量分配内存，
		// 并且只会分配到该元素对应的索引位置。
		aux::vector<bool, file_index_t> m_use_partfile;

		std::string m_part_file_name;
		std::unique_ptr<posix_part_file> m_part_file;
	};
}
}
#endif

