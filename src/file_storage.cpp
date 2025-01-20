/*

Copyright (c) 2008-2022, Arvid Norberg
Copyright (c) 2009, Georg Rudoy
Copyright (c) 2016-2018, 2020, Alden Torres
Copyright (c) 2017-2019, Steven Siloti
Copyright (c) 2022, Konstantin Morozov
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

#include "libtorrent/file_storage.hpp"
#include "libtorrent/string_util.hpp" // for allocate_string_copy
#include "libtorrent/utf8.hpp"
#include "libtorrent/index_range.hpp"
#include "libtorrent/aux_/path.hpp"
#include "libtorrent/aux_/numeric_cast.hpp"
#include "libtorrent/disk_interface.hpp" // for default_block_size
#include "libtorrent/aux_/merkle.hpp"
#include "libtorrent/aux_/throw.hpp"

#include "libtorrent/aux_/disable_warnings_push.hpp"
#include <boost/crc.hpp>
#include "libtorrent/aux_/disable_warnings_pop.hpp"

#include <cstdio>
#include <cinttypes>
#include <algorithm>
#include <functional>
#include <set>
#include <atomic>

#if defined(TORRENT_WINDOWS) || defined(TORRENT_OS2)
#define TORRENT_SEPARATOR '\\'
#else
#define TORRENT_SEPARATOR '/'
#endif

using namespace std::placeholders;

namespace libtorrent {

	constexpr file_flags_t file_storage::flag_pad_file;
	constexpr file_flags_t file_storage::flag_hidden;
	constexpr file_flags_t file_storage::flag_executable;
	constexpr file_flags_t file_storage::flag_symlink;

#if TORRENT_ABI_VERSION == 1
	constexpr file_flags_t file_storage::pad_file;
	constexpr file_flags_t file_storage::attribute_hidden;
	constexpr file_flags_t file_storage::attribute_executable;
	constexpr file_flags_t file_storage::attribute_symlink;
#endif

	file_storage::file_storage() = default;
	file_storage::~file_storage() = default;

	// even though this copy constructor and the copy assignment
	// operator are identical to what the compiler would have
	// generated, they are put here to explicitly make them part
	// of libtorrent and properly exported by the .dll.
	file_storage::file_storage(file_storage const&) = default;
	file_storage& file_storage::operator=(file_storage const&) & = default;
	file_storage::file_storage(file_storage&&) noexcept = default;
	file_storage& file_storage::operator=(file_storage&&) & = default;

	void file_storage::reserve(int num_files)
	{
		m_files.reserve(num_files);
	}

	int file_storage::piece_size(piece_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= piece_index_t(0) && index < end_piece());
		if (index == last_piece())
		{
			std::int64_t const size_except_last
				= (num_pieces() - 1) * std::int64_t(piece_length());
			std::int64_t const size = total_size() - size_except_last;
			TORRENT_ASSERT(size > 0);
			TORRENT_ASSERT(size <= piece_length());
			return int(size);
		}
		else
			return piece_length();
	}

constexpr aux::path_index_t aux::file_entry::no_path;
constexpr aux::path_index_t aux::file_entry::path_is_absolute;

namespace {

	bool compare_file_offset(aux::file_entry const& lhs
		, aux::file_entry const& rhs)
	{
		return lhs.offset < rhs.offset;
	}

}

	int file_storage::piece_size2(piece_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= piece_index_t{} && index < end_piece());
		TORRENT_ASSERT(max_file_offset / piece_length() > static_cast<int>(index));
		// find the file iterator and file offset
		aux::file_entry target;
		TORRENT_ASSERT(max_file_offset / piece_length() > static_cast<int>(index));
		target.offset = aux::numeric_cast<std::uint64_t>(std::int64_t(piece_length()) * static_cast<int>(index));
		TORRENT_ASSERT(!compare_file_offset(target, m_files.front()));

		auto const file_iter = std::upper_bound(
			m_files.begin(), m_files.end(), target, compare_file_offset);

		TORRENT_ASSERT(file_iter != m_files.begin());
		if (file_iter == m_files.end()) return piece_size(index);

		// this static cast is safe because the resulting value is capped by
		// piece_length(), which fits in an int
		return static_cast<int>(
			std::min(static_cast<std::uint64_t>(piece_length()), file_iter->offset - target.offset));
	}

	int file_storage::blocks_in_piece2(piece_index_t const index) const
	{
		// the number of default_block_size in a piece size, rounding up
		return (piece_size2(index) + default_block_size - 1) / default_block_size;
	}

	int file_storage::blocks_per_piece() const
	{
		return (m_piece_length + default_block_size - 1) / default_block_size;
	}

	// path is supposed to include the name of the torrent itself.
	// or an absolute path, to move a file outside of the download directory
	/**
	 * 生成 m_paths, 并更新 file_entry 的 path_index 字段，它有 3 种取值：
	 * 1. file_entry::path_is_absolute，表明 path 是一个绝对路径，
	 * 2. file_entry::no_path，表明单个文件, path 就是文件名
	 * 3. 其他值，表明该值是一个索引，指向 m_paths 中的一个路径。
	 * 
	 * @param path 文件路径，可以是绝对路径或相对路径，相对路径不能以 "/" 开头。
	 * @param set_name 是否 path 字段包含了文件名
	 */
	void file_storage::update_path_index(aux::file_entry& e
		, std::string const& path, bool const set_name)
	{
		if (is_complete(path))
		{
			TORRENT_ASSERT(set_name);
			e.set_name(path);
			e.path_index = aux::file_entry::path_is_absolute;
			return;
		}

		TORRENT_ASSERT(path[0] != '/');

		// split the string into the leaf filename
		// and the branch path
		string_view leaf;
		string_view branch_path;

		// branch_path 不能是 "/" 开头
		std::tie(branch_path, leaf) = rsplit_path(path);

		if (branch_path.empty())
		{
			// 如果 path 包含文件名，则 leaf 就是该文件名
			if (set_name) e.set_name(leaf);
			e.path_index = aux::file_entry::no_path;
			return;
		}

		// if the path *does* contain the name of the torrent (as we expect)
		// strip it before adding it to m_paths
		//
		// 规范化 branch_path，branch path 不能以 "/" 开头，如果有则移除。
		if (lsplit_path(branch_path).first == m_name)
		{
			branch_path = lsplit_path(branch_path).second;
			// strip duplicate separators
			while (!branch_path.empty() && (branch_path.front() == TORRENT_SEPARATOR
#if defined(TORRENT_WINDOWS) || defined(TORRENT_OS2)
				|| branch_path.front() == '/'
#endif
				))
				branch_path.remove_prefix(1);
			e.no_root_dir = false;
		}
		else
		{
			e.no_root_dir = true;
		}

		// 查找或生成新的 item（不会以 "/" 开头） 并添加到 m_paths 中。
		e.path_index = get_or_add_path(branch_path);
		if (set_name) e.set_name(leaf);
	}

	/** 
	 * 在 m_paths 中查找或添加 path，返回 path 在 m_paths 中的索引
	 * 
	 * @param path 路径，不能以 "/" 开头。
	*/
	aux::path_index_t file_storage::get_or_add_path(string_view const path)
	{
		// do we already have this path in the path list?
		auto const p = std::find(m_paths.rbegin(), m_paths.rend(), path);

		if (p == m_paths.rend())
		{
			// no, we don't. add it
			auto const ret = m_paths.end_index();

			// 确保 path 不以 '/' 开头。
			TORRENT_ASSERT(path.size() == 0 || path[0] != '/');

			// 在 m_paths 中添加一个新的路径，并返回其索引。
			m_paths.emplace_back(path.data(), path.size());

			return ret;
		}
		else
		{
			// yes we do. use it
			return aux::path_index_t{aux::numeric_cast<std::uint32_t>(
				p.base() - m_paths.begin() - 1)};
		}
	}

#if TORRENT_ABI_VERSION == 1
	file_entry::file_entry(): offset(0), size(0)
		, mtime(0), pad_file(false), hidden_attribute(false)
		, executable_attribute(false)
		, symlink_attribute(false)
	{}

	file_entry::~file_entry() = default;
#endif // TORRENT_ABI_VERSION

namespace aux {

	file_entry::file_entry()
		: offset(0)
		, symlink_index(not_a_symlink)
		, no_root_dir(false)
		, size(0)
		, name_len(name_is_owned)
		, pad_file(false)
		, hidden_attribute(false)
		, executable_attribute(false)
		, symlink_attribute(false)
	{}

	file_entry::~file_entry()
	{
		if (name_len == name_is_owned) delete[] name;
	}

	file_entry::file_entry(file_entry const& fe)
		: offset(fe.offset)
		, symlink_index(fe.symlink_index)
		, no_root_dir(fe.no_root_dir)
		, size(fe.size)
		, name_len(fe.name_len)
		, pad_file(fe.pad_file)
		, hidden_attribute(fe.hidden_attribute)
		, executable_attribute(fe.executable_attribute)
		, symlink_attribute(fe.symlink_attribute)
		, root(fe.root)
		, path_index(fe.path_index)
	{
		bool const borrow = fe.name_len != name_is_owned;
		set_name(fe.filename(), borrow);
	}

	file_entry& file_entry::operator=(file_entry const& fe) &
	{
		if (&fe == this) return *this;
		offset = fe.offset;
		size = fe.size;
		path_index = fe.path_index;
		symlink_index = fe.symlink_index;
		pad_file = fe.pad_file;
		hidden_attribute = fe.hidden_attribute;
		executable_attribute = fe.executable_attribute;
		symlink_attribute = fe.symlink_attribute;
		no_root_dir = fe.no_root_dir;
		root = fe.root;

		// if the name is not owned, don't allocate memory, we can point into the
		// same metadata buffer
		bool const borrow = fe.name_len != name_is_owned;
		set_name(fe.filename(), borrow);

		return *this;
	}

	file_entry::file_entry(file_entry&& fe) noexcept
		: offset(fe.offset)
		, symlink_index(fe.symlink_index)
		, no_root_dir(fe.no_root_dir)
		, size(fe.size)
		, name_len(fe.name_len)
		, pad_file(fe.pad_file)
		, hidden_attribute(fe.hidden_attribute)
		, executable_attribute(fe.executable_attribute)
		, symlink_attribute(fe.symlink_attribute)
		, name(fe.name)
		, root(fe.root)
		, path_index(fe.path_index)
	{
		fe.name_len = 0;
		fe.name = nullptr;
	}

	file_entry& file_entry::operator=(file_entry&& fe) & noexcept
	{
		if (&fe == this) return *this;
		offset = fe.offset;
		size = fe.size;
		path_index = fe.path_index;
		symlink_index = fe.symlink_index;
		pad_file = fe.pad_file;
		hidden_attribute = fe.hidden_attribute;
		executable_attribute = fe.executable_attribute;
		symlink_attribute = fe.symlink_attribute;
		no_root_dir = fe.no_root_dir;

		if (name_len == name_is_owned) delete[] name;

		name = fe.name;
		root = fe.root;
		name_len = fe.name_len;

		fe.name_len = 0;
		fe.name = nullptr;
		return *this;
	}

	// if borrow_string is true, don't take ownership over n, just
	// point to it.
	// if borrow_string is false, n will be copied and owned by the
	// file_entry.
	/**
	 *  给 file_entry 的 name 属性赋值
	 * 
	 * @param n – 当前文件名或目录名（不含上级目录）
	 * @param borrow_string – 是否借用 n 的内存。如果为 true, 则 n 不会被拷贝；如果为 false, n 会被拷贝。
	 */
	void file_entry::set_name(string_view n, bool const borrow_string)
	{
		// free the current string, before assigning the new one
		if (name_len == name_is_owned) delete[] name;
		if (n.empty())
		{
			TORRENT_ASSERT(borrow_string == false);
			name = nullptr;
		}
		else if (borrow_string)
		{
			// we have limited space in the length field. truncate string
			// if it's too long
			if (n.size() >= name_is_owned) n = n.substr(name_is_owned - 1);

			name = n.data();
			name_len = aux::numeric_cast<std::uint64_t>(n.size());
		}
		else
		{
			name = allocate_string_copy(n);
			name_len = name_is_owned;
		}
	}

	string_view file_entry::filename() const
	{
		if (name_len != name_is_owned) return {name, std::size_t(name_len)};
		return name ? string_view(name) : string_view();
	}

} // aux namespace

#if TORRENT_ABI_VERSION == 1

	void file_storage::add_file_borrow(char const* filename, int filename_len
		, std::string const& path, std::int64_t file_size, file_flags_t file_flags
		, char const* filehash, std::int64_t mtime, string_view symlink_path)
	{
		TORRENT_ASSERT(filename_len >= 0);
		add_file_borrow({filename, std::size_t(filename_len)}, path, file_size
			, file_flags, filehash, mtime, symlink_path);
	}

	void file_storage::add_file(file_entry const& fe, char const* filehash)
	{
		file_flags_t flags = {};
		if (fe.pad_file) flags |= file_storage::flag_pad_file;
		if (fe.hidden_attribute) flags |= file_storage::flag_hidden;
		if (fe.executable_attribute) flags |= file_storage::flag_executable;
		if (fe.symlink_attribute) flags |= file_storage::flag_symlink;

		add_file_borrow({}, fe.path, fe.size, flags, filehash, fe.mtime
			, fe.symlink_path);
	}
#endif // TORRENT_ABI_VERSION

	void file_storage::rename_file(file_index_t const index
		, std::string const& new_filename)
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		update_path_index(m_files[index], new_filename);
	}

#if TORRENT_ABI_VERSION == 1
	file_storage::iterator file_storage::file_at_offset_deprecated(std::int64_t offset) const
	{
		// find the file iterator and file offset
		aux::file_entry target;
		TORRENT_ASSERT(offset <= max_file_offset);
		target.offset = aux::numeric_cast<std::uint64_t>(offset);
		TORRENT_ASSERT(!compare_file_offset(target, m_files.front()));

		auto file_iter = std::upper_bound(
			begin_deprecated(), end_deprecated(), target, compare_file_offset);

		TORRENT_ASSERT(file_iter != begin_deprecated());
		--file_iter;
		return file_iter;
	}

	file_storage::iterator file_storage::file_at_offset(std::int64_t offset) const
	{
		return file_at_offset_deprecated(offset);
	}
#endif

	file_index_t file_storage::file_index_at_offset(std::int64_t const offset) const
	{
		TORRENT_ASSERT_PRECOND(offset >= 0);
		TORRENT_ASSERT_PRECOND(offset < m_total_size);
		TORRENT_ASSERT(offset <= max_file_offset);
		// find the file iterator and file offset
		aux::file_entry target;
		target.offset = aux::numeric_cast<std::uint64_t>(offset);
		TORRENT_ASSERT(!compare_file_offset(target, m_files.front()));

		auto file_iter = std::upper_bound(
			m_files.begin(), m_files.end(), target, compare_file_offset);

		TORRENT_ASSERT(file_iter != m_files.begin());
		--file_iter;
		return file_index_t{int(file_iter - m_files.begin())};
	}

	file_index_t file_storage::file_index_at_piece(piece_index_t const piece) const
	{
		return file_index_at_offset(static_cast<int>(piece) * std::int64_t(piece_length()));
	}

	file_index_t file_storage::file_index_for_root(sha256_hash const& root_hash) const
	{
		// TODO: maybe it would be nice to have a better index here
		for (file_index_t const i : file_range())
		{
			if (root(i) == root_hash) return i;
		}
		return file_index_t{-1};
	}

	piece_index_t file_storage::piece_index_at_file(file_index_t f) const
	{
		return piece_index_t{aux::numeric_cast<int>(file_offset(f) / piece_length())};
	}

#if TORRENT_ABI_VERSION <= 2
	char const* file_storage::file_name_ptr(file_index_t const index) const
	{
		return m_files[index].name;
	}

	int file_storage::file_name_len(file_index_t const index) const
	{
		if (m_files[index].name_len == aux::file_entry::name_is_owned)
			return -1;
		return m_files[index].name_len;
	}
#endif

	std::vector<file_slice> file_storage::map_block(piece_index_t const piece
		, std::int64_t const offset, std::int64_t size) const
	{
		TORRENT_ASSERT_PRECOND(piece >= piece_index_t{0});
		TORRENT_ASSERT_PRECOND(piece < end_piece());
		TORRENT_ASSERT_PRECOND(num_files() > 0);
		TORRENT_ASSERT_PRECOND(size >= 0);
		std::vector<file_slice> ret;

		if (m_files.empty()) return ret;

		// find the file iterator and file offset
		aux::file_entry target;
		TORRENT_ASSERT(max_file_offset / m_piece_length > static_cast<int>(piece));
		target.offset = aux::numeric_cast<std::uint64_t>(static_cast<int>(piece) * std::int64_t(m_piece_length) + offset);
		TORRENT_ASSERT_PRECOND(std::int64_t(target.offset) <= m_total_size - size);
		TORRENT_ASSERT(!compare_file_offset(target, m_files.front()));

		// in case the size is past the end, fix it up
		if (std::int64_t(target.offset) > m_total_size - size)
			size = m_total_size - std::int64_t(target.offset);

		auto file_iter = std::upper_bound(
			m_files.begin(), m_files.end(), target, compare_file_offset);

		TORRENT_ASSERT(file_iter != m_files.begin());
		--file_iter;

		std::int64_t file_offset = target.offset - file_iter->offset;
		for (; size > 0; file_offset -= file_iter->size, ++file_iter)
		{
			TORRENT_ASSERT(file_iter != m_files.end());
			if (file_offset < std::int64_t(file_iter->size))
			{
				file_slice f{};
				f.file_index = file_index_t(int(file_iter - m_files.begin()));
				f.offset = file_offset;
				f.size = std::min(std::int64_t(file_iter->size) - file_offset, std::int64_t(size));
				TORRENT_ASSERT(f.size <= size);
				size -= f.size;
				file_offset += f.size;
				ret.push_back(f);
			}

			TORRENT_ASSERT(size >= 0);
		}
		return ret;
	}

#if TORRENT_ABI_VERSION == 1
	file_entry file_storage::at(int index) const
	{
		return at_deprecated(index);
	}

	aux::file_entry const& file_storage::internal_at(int const index) const
	{
		TORRENT_ASSERT(index >= 0);
		TORRENT_ASSERT(index < int(m_files.size()));
		return m_files[file_index_t(index)];
	}

	file_entry file_storage::at_deprecated(int index) const
	{
		TORRENT_ASSERT_PRECOND(index >= 0 && index < int(m_files.size()));
		file_entry ret;
		aux::file_entry const& ife = m_files[index];
		ret.path = file_path(index);
		ret.offset = ife.offset;
		ret.size = ife.size;
		ret.mtime = mtime(index);
		ret.pad_file = ife.pad_file;
		ret.hidden_attribute = ife.hidden_attribute;
		ret.executable_attribute = ife.executable_attribute;
		ret.symlink_attribute = ife.symlink_attribute;
		if (ife.symlink_index != aux::file_entry::not_a_symlink)
			ret.symlink_path = symlink(index);
		ret.filehash = hash(index);
		return ret;
	}
#endif // TORRENT_ABI_VERSION

	int file_storage::num_files() const noexcept
	{ return int(m_files.size()); }

	// returns the index of the one-past-end file in the file storage
	file_index_t file_storage::end_file() const noexcept
	{ return m_files.end_index(); }

	file_index_t file_storage::last_file() const noexcept
	{ return --m_files.end_index(); }

	index_range<file_index_t> file_storage::file_range() const noexcept
	{ return m_files.range(); }

	index_range<piece_index_t> file_storage::piece_range() const noexcept
	{ return {piece_index_t{0}, end_piece()}; }

	peer_request file_storage::map_file(file_index_t const file_index
		, std::int64_t const file_offset, int const size) const
	{
		TORRENT_ASSERT_PRECOND(file_index < end_file());
		TORRENT_ASSERT(m_num_pieces >= 0);

		peer_request ret{};
		if (file_index >= end_file())
		{
			ret.piece = end_piece();
			ret.start = 0;
			ret.length = 0;
			return ret;
		}

		std::int64_t const offset = file_offset + this->file_offset(file_index);

		if (offset >= total_size())
		{
			ret.piece = end_piece();
			ret.start = 0;
			ret.length = 0;
		}
		else
		{
			ret.piece = piece_index_t(int(offset / piece_length()));
			ret.start = int(offset % piece_length());
			ret.length = size;
			if (offset + size > total_size())
				ret.length = int(total_size() - offset);
		}
		return ret;
	}

#ifndef BOOST_NO_EXCEPTIONS
	void file_storage::add_file(std::string const& path, std::int64_t const file_size
		, file_flags_t const file_flags, std::time_t const mtime, string_view const symlink_path
		, char const* root_hash)
	{
		error_code ec;
		add_file_borrow(ec, {}, path, file_size, file_flags, nullptr, mtime
			, symlink_path, root_hash);
		if (ec) aux::throw_ex<system_error>(ec);
	}

	void file_storage::add_file_borrow(string_view filename
		, std::string const& path, std::int64_t const file_size
		, file_flags_t const file_flags, char const* filehash
		, std::int64_t const mtime, string_view const symlink_path
		, char const* root_hash)
	{
		error_code ec;
		add_file_borrow(ec, filename, path, file_size
			, file_flags, filehash, mtime, symlink_path, root_hash);
		if (ec) aux::throw_ex<system_error>(ec);
	}
#endif // BOOST_NO_EXCEPTIONS

	void file_storage::add_file(error_code& ec, std::string const& path
		, std::int64_t const file_size, file_flags_t const file_flags, std::time_t const mtime
		, string_view symlink_path, char const* root_hash)
	{
		add_file_borrow(ec, {}, path, file_size, file_flags, nullptr, mtime
			, symlink_path, root_hash);
	}

	/** 
	 * 将文件加入到 file_storage::m_files 属性中 (v1，v2 )
	 * 
	 * @param filename 当前文件名，不含目录名
	 * @param path 文件路径，包含了目录和文件名的完整路径。
	 * 			   如果 torrent 内是单文件（文件路径中不能含有目录名），则 path 为文件名；
	 * 			   如果 torrent 内是多文件，则 path 为 torrent_name + file_tree 中各级目录 + 文件名。
	 * @param file_size 文件大小, 对应该文件的 "length" 字段
	 * @param file_flags 文件属性，对应该文件的 "attr" 字段(bep47)
	 * @param filehash v2 该值为 nullptr; v1 该值对应该文件的 sha1 字段? (bep47)
	 * @param mtime 文件修改时间，对应该文件的 "mtime" 字段(libtorrent 自定义字段)
	 * @param symlink_path 符号链接指向的路径，对应对应该文件的 "symlink path" 字段(bep47)
	 * @param root_hash 该文件的 merkle 根 hash，对应对应该文件的 "pieces root" 字段
	*/
	void file_storage::add_file_borrow(error_code& ec, string_view filename
		, std::string const& path, std::int64_t const file_size
		, file_flags_t const file_flags, char const* filehash
		, std::int64_t const mtime, string_view const symlink_path
		, char const* root_hash)
	{
		TORRENT_ASSERT_PRECOND(file_size >= 0);
		TORRENT_ASSERT_PRECOND(!is_complete(filename));

		if (file_size > max_file_size)
		{
			ec = make_error_code(boost::system::errc::file_too_large);
			return;
		}

		if (max_file_offset - m_total_size < file_size)
		{
			ec = make_error_code(errors::torrent_invalid_length);
			return;
		}

		if (!filename.empty())
		{
			if (filename.size() >= (1 << 12))
			{
				ec = make_error_code(boost::system::errc::filename_too_long);
				return;
			}
		}
		else if (lt::filename(path).size() >= (1 << 12))
		{
			ec = make_error_code(boost::system::errc::filename_too_long);
			return;
		}

		// 文件的 path 中没有目录名，说明整个 torrent 内只有一个文件（如果是多文件的话，path 中至少有一个 torrent_name 作为目录）。
		// 注意：设定 m_name 的值，一旦设定就不会修改了
		if (!has_parent_path(path)) 
		{
			// torrent 内只有一个文件，且第一次调用 add_file_borrow 时，则将 m_name 设为 path（一个文件名）。
			// 否则断言就会失败，因为第二次调用，如果能进这个分支 m_files.empty() 为 false。

			// you have already added at least one file with a
			// path to the file (branch_path), which means that
			// all the other files need to be in the same top
			// directory as the first file.
			TORRENT_ASSERT_PRECOND(m_files.empty());
			m_name = path;
		}
		else
		{
			// torrent 有多个文件，且第一次调用 add_file_borrow 时，将 m_name 设为 path 的第一段目录名。
			if (m_files.empty()) {
				m_name = lsplit_path(path).first.to_string();
			}
		}
		// 上面绕了半天，其实就是将 ".torrent" 文件的 "name" 字段值，赋值给 file_storage 对象的 m_name 属性

		// files without a root_hash are assumed to be v1, except symlinks. They
		// don't have a root hash and can be either v1 or v2
		//
		// 这个 if 如果 true ，说明当前是一个真实的文件
		if (symlink_path.empty() && file_size > 0)
		{
			bool const v2 = (root_hash != nullptr);
			// This condition is true of all files we've added so far have been
			// symlinks. i.e. this is the first "real" file we're adding.
			// or if m_total_size == 0, all files we've added so far have been
			// empty (which also are are v1/v2-ambigous)
			//
			// 这里的判断作用是：如果root_hash 存在, 且第一次处理真实文件，则 m_v2 就是 true（当前种子是 v2）
			if (m_files.size() == m_symlinks.size() || m_total_size == 0)
			{
				m_v2 = v2;
			}
			else if (m_v2 != v2)
			{
				// you cannot mix v1 and v2 files when building torrent_storage. Either
				// all files are v1 or all files are v2
				ec = m_v2 ? make_error_code(errors::torrent_missing_pieces_root)
					: make_error_code(errors::torrent_inconsistent_files);
				return;
			}
		}

		// 下面两行是在 m_files 这个 list 末尾添加一个空 item，
		// 然后通过 m_files.back() 返回这个 item 的引用，这样之后可以填充这个 item 。
		m_files.emplace_back();
		aux::file_entry& e = m_files.back();

		// the last argument specified whether the function should also set
		// the filename. If it does, it will copy the leaf filename from path.
		// if filename is empty, we should copy it. If it isn't, we're borrowing
		// it and we can save the copy by setting it after this call to
		// update_path_index().
		//
		// 更新 e.path_index 字段
		update_path_index(e, path, filename.empty());

		// filename is allowed to be empty, in which case we just use path
		// filename 不为空，则将 filename (文件名，不含目录名)赋值为 file_entry.name。
		if (!filename.empty())
			e.set_name(filename, true);

		e.size = aux::numeric_cast<std::uint64_t>(file_size);
		e.offset = aux::numeric_cast<std::uint64_t>(m_total_size); // 添加第一个文件时，m_total_size 为 0。
		e.pad_file = bool(file_flags & file_storage::flag_pad_file);
		e.hidden_attribute = bool(file_flags & file_storage::flag_hidden);
		e.executable_attribute = bool(file_flags & file_storage::flag_executable);
		e.symlink_attribute = bool(file_flags & file_storage::flag_symlink);
		e.root = root_hash;

		if (filehash)
		{
			if (m_file_hashes.size() < m_files.size()) m_file_hashes.resize(m_files.size());
			m_file_hashes[last_file()] = filehash;
		}

		if (!symlink_path.empty()
			&& m_symlinks.size() < aux::file_entry::not_a_symlink - 1)
		{
			e.symlink_index = m_symlinks.size();
			m_symlinks.emplace_back(symlink_path.to_string());
		}
		else
		{
			e.symlink_attribute = false;
		}

		if (mtime)
		{
			if (m_mtime.size() < m_files.size()) m_mtime.resize(m_files.size());
			m_mtime[last_file()] = std::time_t(mtime);
		}

		m_total_size += e.size; // 更新 m_total_size，下一个 file_entry 的 offset 就是当前 m_total_size 的大小。

		// when making v2 torrents, pad the end of each file (if necessary) to
		// ensure it ends on a piece boundary.
		// we do this at the end of files rather in-front of files to conform to
		// the BEP52 reference implementation
		//
		// 根据 BEP52 中升级参考的描述，混合种子的文件如果没有对齐块边界，就会在末尾添加 .pad 文件。
		//
		// 这样处理估计是因为支持 v1 和 v2 的应用，对用同一个混合种子，要建两个不同的下载 群，
		// 当一个 piece 从 v1 群里去请求了，就不用从 v2 群里去请求了，
		// 如果 v1 中的文件没对齐，可能会产生一个 piece 跨两个文件的情况，那么这个 piece 是无法在 v2 群中进行请求的。 
		if (m_v2 && (m_total_size % piece_length()) != 0)
		{
			auto const pad_size = piece_length() - (m_total_size % piece_length());
			TORRENT_ASSERT(int(pad_size) != piece_length());
			TORRENT_ASSERT(int(pad_size) > 0);
			if (m_total_size > max_file_offset - pad_size)
			{
				ec = make_error_code(errors::torrent_invalid_length);
				return;
			}

			m_files.emplace_back();
			// e is invalid from here down!
			auto& pad = m_files.back();
			pad.size = static_cast<std::uint64_t>(pad_size);
			TORRENT_ASSERT(m_total_size <= max_file_offset);
			TORRENT_ASSERT(m_total_size > 0);
			pad.offset = static_cast<std::uint64_t>(m_total_size);
			pad.path_index = get_or_add_path(".pad");
			char name[30];
			std::snprintf(name, sizeof(name), "%" PRIu64
				, pad.size);
			pad.set_name(name);
			pad.pad_file = true;
			m_total_size += pad_size;
		}
	}

	// this is here for backwards compatibility with hybrid torrents created
	// with libtorrent 2.0.0-2.0.7, which would not add tail-padding
	void file_storage::remove_tail_padding()
	{
		file_index_t f = end_file();
		while (f > file_index_t{0})
		{
			--f;
			// empty files and symlinks are skipped
			if (file_size(f) == 0) continue;
			if (pad_file_at(f))
			{
				m_total_size -= file_size(f);
				m_files.erase(m_files.begin() + int(f));
				while (f < end_file())
				{
					m_files[f].offset = static_cast<std::uint64_t>(m_total_size);
					TORRENT_ASSERT(m_files[f].size == 0);
					++f;
				}
			}
			// if the last non-empty file isn't a pad file, don't do anything
			return;
		}
		// nothing found
	}

	sha1_hash file_storage::hash(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t{} && index < end_file());
		if (index >= m_file_hashes.end_index()) return sha1_hash();
		return sha1_hash(m_file_hashes[index]);
	}

	sha256_hash file_storage::root(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t{} && index < end_file());
		if (m_files[index].root == nullptr) return sha256_hash();
		return sha256_hash(m_files[index].root);
	}

	char const* file_storage::root_ptr(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t{} && index < end_file());
		return m_files[index].root;
	}

	std::string file_storage::symlink(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t{} && index < end_file());
		aux::file_entry const& fe = m_files[index];
		if (fe.symlink_index == aux::file_entry::not_a_symlink)
			return {};

		TORRENT_ASSERT(fe.symlink_index < int(m_symlinks.size()));

		auto const& link = m_symlinks[fe.symlink_index];

		std::string ret;
		ret.reserve(m_name.size() + link.size() + 1);
		ret.assign(m_name);
		append_path(ret, link);
		return ret;
	}

	std::string const& file_storage::internal_symlink(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t{} && index < end_file());
		aux::file_entry const& fe = m_files[index];
		TORRENT_ASSERT(fe.symlink_index < int(m_symlinks.size()));

		return m_symlinks[fe.symlink_index];
	}

	std::time_t file_storage::mtime(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t{} && index < end_file());
		if (index >= m_mtime.end_index()) return 0;
		return m_mtime[index];
	}

namespace {

		/**
		 * 将字符串 str 中的每个字符转换为小写，并更新到 crc 计算器中。
		 */
		template <class CRC>
		void process_string_lowercase(CRC& crc, string_view str)
		{
			for (char const c : str)
				// 与 0xff 进行按位与操作，是为了确保只处理一个字节。
				crc.process_byte(to_lower(c) & 0xff);
		}

		/**
		 * 将 str 路径的每一部分（即路径中的每个目录或文件名）的CRC校验和单独作为一个 item 存储在 table 中。
		 * table 是 Set 类型，存储时会自动去重。
		 * 
		 * @example
		 * 
		 * crc 初始值为 CRC("abc/")。
		 * str = "def/ghi"，则：
		 * 
		 * 结束时 table 中将包含以下CRC值：
		 * 
		 * CRC("abc/def")（在遇到 / 时插入）。
		 * CRC("abc/def/ghi")（遍历结束后插入）。
		 */
		template <class CRC>
		void process_path_lowercase(
			std::unordered_set<std::uint32_t>& table
			, CRC crc, string_view str)
		{
			if (str.empty()) return;
			for (char const c : str)
			{
				// 如果字符 c 是路径分隔符（TORRENT_SEPARATOR），则将当前的CRC校验和（通过 crc.checksum() 获取）插入到 table 中。
				// 注意 第一次循环时，m_name 的最后一个字符，此时不应该插入 table 中。

				if (c == TORRENT_SEPARATOR)
					// 调用 crc.checksum() 会返回当前的CRC值，但通常不会重置CRC计算器的状态
					table.insert(crc.checksum());

				// 与 0xff 进行按位与操作，是为了确保只处理一个字节。
				crc.process_byte(to_lower(c) & 0xff);
			}
			table.insert(crc.checksum());
		}
	}

	/**
	 * 获取所有路径的CRC32哈希值
	 * 
	 * @param table 用于存储所有路径的CRC32哈希值。
	 */
	void file_storage::all_path_hashes(
		std::unordered_set<std::uint32_t>& table) const
	{
		boost::crc_optimal<32, 0x1EDC6F41, 0xFFFFFFFF, 0xFFFFFFFF, true, true> crc;

		if (!m_name.empty())
		{
			// 将 m_name 字符串转换为小写，并更新到 crc 计算器中(这时还没计算校验码)。
			process_string_lowercase(crc, m_name);

			// 使用断言确保 m_name 的最后一个字符不是路径分隔符（TORRENT_SEPARATOR），避免路径格式错误
			TORRENT_ASSERT(m_name[m_name.size() - 1] != TORRENT_SEPARATOR);
			
			// 在 CRC 计算中添加路径分隔符，此时 crc(["{m_name}/"]])
			crc.process_byte(TORRENT_SEPARATOR);
		}

		for (auto const& p : m_paths)
			// 注意：crc 是 copy 方式传入，所以每次调用时传入的都是 crc([m_name])，并且还没计算校验码
			process_path_lowercase(table, crc, p); 
	}

	/**
	 * 根据文件的路径信息生成一个 CRC32 校验值，用于唯一标识文件的路径
	 */
	std::uint32_t file_storage::file_path_hash(file_index_t const index
		, std::string const& save_path) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		aux::file_entry const& fe = m_files[index];

		boost::crc_optimal<32, 0x1EDC6F41, 0xFFFFFFFF, 0xFFFFFFFF, true, true> crc;

		if (fe.path_index == aux::file_entry::path_is_absolute)
		{
			process_string_lowercase(crc, fe.filename());
		}
		else if (fe.path_index == aux::file_entry::no_path)
		{
			if (!save_path.empty())
			{
				process_string_lowercase(crc, save_path);
				TORRENT_ASSERT(save_path[save_path.size() - 1] != TORRENT_SEPARATOR);
				// 不含文件的 path，末尾加 "/"
				crc.process_byte(TORRENT_SEPARATOR);
			}
			process_string_lowercase(crc, fe.filename());
		}
		else if (fe.no_root_dir)
		{
			if (!save_path.empty())
			{
				process_string_lowercase(crc, save_path);
				TORRENT_ASSERT(save_path[save_path.size() - 1] != TORRENT_SEPARATOR);
				crc.process_byte(TORRENT_SEPARATOR);
			}
			std::string const& p = m_paths[fe.path_index];
			if (!p.empty())
			{
				process_string_lowercase(crc, p);
				TORRENT_ASSERT(p[p.size() - 1] != TORRENT_SEPARATOR);
				crc.process_byte(TORRENT_SEPARATOR);
			}
			process_string_lowercase(crc, fe.filename());
		}
		else
		{
			if (!save_path.empty())
			{
				process_string_lowercase(crc, save_path);
				TORRENT_ASSERT(save_path[save_path.size() - 1] != TORRENT_SEPARATOR);
				crc.process_byte(TORRENT_SEPARATOR);
			}
			process_string_lowercase(crc, m_name);
			TORRENT_ASSERT(m_name.size() > 0);
			TORRENT_ASSERT(m_name[m_name.size() - 1] != TORRENT_SEPARATOR);
			crc.process_byte(TORRENT_SEPARATOR);

			std::string const& p = m_paths[fe.path_index];
			if (!p.empty())
			{
				process_string_lowercase(crc, p);
				TORRENT_ASSERT(p.size() > 0);
				TORRENT_ASSERT(p[p.size() - 1] != TORRENT_SEPARATOR);
				crc.process_byte(TORRENT_SEPARATOR);
			}
			process_string_lowercase(crc, fe.filename());
		}

		return crc.checksum();
	}

	std::string file_storage::file_path(file_index_t const index, std::string const& save_path) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		aux::file_entry const& fe = m_files[index];

		std::string ret;

		if (fe.path_index == aux::file_entry::path_is_absolute)
		{
			ret = fe.filename().to_string();
		}
		else if (fe.path_index == aux::file_entry::no_path)
		{
			ret.reserve(save_path.size() + fe.filename().size() + 1);
			ret.assign(save_path);
			append_path(ret, fe.filename());
		}
		else if (fe.no_root_dir)
		{
			std::string const& p = m_paths[fe.path_index];

			ret.reserve(save_path.size() + p.size() + fe.filename().size() + 2);
			ret.assign(save_path);
			append_path(ret, p);
			append_path(ret, fe.filename());
		}
		else
		{
			std::string const& p = m_paths[fe.path_index];

			ret.reserve(save_path.size() + m_name.size() + p.size() + fe.filename().size() + 3);
			ret.assign(save_path);
			append_path(ret, m_name);
			append_path(ret, p);
			append_path(ret, fe.filename());
		}

		// a single return statement, just to make NRVO more likely to kick in
		return ret;
	}

	/**
	 * 根据 index ，从 m_files 中取出对应的 torrent 内部文件路径（含文件名，不含 root_dir）
	 */
	std::string file_storage::internal_file_path(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		aux::file_entry const& fe = m_files[index];

		if (fe.path_index != aux::file_entry::path_is_absolute
			&& fe.path_index != aux::file_entry::no_path)
		{
			std::string ret;
			std::string const& p = m_paths[fe.path_index];
			ret.reserve(p.size() + fe.filename().size() + 2);
			append_path(ret, p);
			append_path(ret, fe.filename());
			return ret;
		}
		else
		{
			return fe.filename().to_string();
		}
	}

	string_view file_storage::file_name(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		aux::file_entry const& fe = m_files[index];
		return fe.filename();
	}

	std::int64_t file_storage::file_size(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		return m_files[index].size;
	}

	bool file_storage::pad_file_at(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		return m_files[index].pad_file;
	}

	std::int64_t file_storage::file_offset(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		return m_files[index].offset;
	}

	int file_storage::file_num_pieces(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		TORRENT_ASSERT_PRECOND(m_piece_length > 0);
		auto const& f = m_files[index];

		if (f.size == 0) return 0;

		// this function only works for v2 torrents, where files are guaranteed to
		// be aligned to pieces
		TORRENT_ASSERT(f.pad_file == false);
		TORRENT_ASSERT((static_cast<std::int64_t>(f.offset) % m_piece_length) == 0);
		return aux::numeric_cast<int>(
			(static_cast<std::int64_t>(f.size) + m_piece_length - 1) / m_piece_length);
	}

	index_range<piece_index_t::diff_type> file_storage::file_piece_range(file_index_t const file) const
	{
		return {piece_index_t::diff_type{0}, piece_index_t::diff_type{file_num_pieces(file)}};
	}

	int file_storage::file_num_blocks(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		TORRENT_ASSERT_PRECOND(m_piece_length > 0);
		auto const& f = m_files[index];

		if (f.size == 0) return 0;

		// this function only works for v2 torrents, where files are guaranteed to
		// be aligned to pieces
		TORRENT_ASSERT(f.pad_file == false);
		TORRENT_ASSERT((static_cast<std::int64_t>(f.offset) % m_piece_length) == 0);
		return int((f.size + default_block_size - 1) / default_block_size);
	}

	int file_storage::file_first_piece_node(file_index_t index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		TORRENT_ASSERT_PRECOND(m_piece_length > 0);
		int const piece_layer_size = merkle_num_leafs(file_num_pieces(index));
		return merkle_num_nodes(piece_layer_size) - piece_layer_size;
	}

	int file_storage::file_first_block_node(file_index_t index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		TORRENT_ASSERT_PRECOND(m_piece_length > 0);
		int const leaf_layer_size = merkle_num_leafs(file_num_blocks(index));
		return merkle_num_nodes(leaf_layer_size) - leaf_layer_size;
	}

	file_flags_t file_storage::file_flags(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		aux::file_entry const& fe = m_files[index];
		return (fe.pad_file ? file_storage::flag_pad_file : file_flags_t{})
			| (fe.hidden_attribute ? file_storage::flag_hidden : file_flags_t{})
			| (fe.executable_attribute ? file_storage::flag_executable : file_flags_t{})
			| (fe.symlink_attribute ? file_storage::flag_symlink : file_flags_t{});
	}

	bool file_storage::file_absolute_path(file_index_t const index) const
	{
		TORRENT_ASSERT_PRECOND(index >= file_index_t(0) && index < end_file());
		aux::file_entry const& fe = m_files[index];
		return fe.path_index == aux::file_entry::path_is_absolute;
	}

#if TORRENT_ABI_VERSION == 1
	sha1_hash file_storage::hash(aux::file_entry const& fe) const
	{
		int const index = int(&fe - &m_files.front());
		TORRENT_ASSERT_PRECOND(index >= file_index_t{} && index < end_file());
		if (index >= int(m_file_hashes.size())) return sha1_hash(nullptr);
		return sha1_hash(m_file_hashes[index]);
	}

	std::string file_storage::symlink(aux::file_entry const& fe) const
	{
		TORRENT_ASSERT_PRECOND(fe.symlink_index < int(m_symlinks.size()));
		return m_symlinks[fe.symlink_index];
	}

	std::time_t file_storage::mtime(aux::file_entry const& fe) const
	{
		int const index = int(&fe - &m_files.front());
		TORRENT_ASSERT_PRECOND(index >= file_index_t{} && index < end_file());
		if (index >= int(m_mtime.size())) return 0;
		return m_mtime[index];
	}

	int file_storage::file_index(aux::file_entry const& fe) const
	{
		int const index = int(&fe - &m_files.front());
		TORRENT_ASSERT_PRECOND(index >= 0 && index < int(m_files.size()));
		return index;
	}

	std::string file_storage::file_path(aux::file_entry const& fe
		, std::string const& save_path) const
	{
		int const index = int(&fe - &m_files.front());
		TORRENT_ASSERT_PRECOND(index >= file_index_t{} && index < end_file());
		return file_path(index, save_path);
	}

	std::string file_storage::file_name(aux::file_entry const& fe) const
	{
		return fe.filename().to_string();
	}

	std::int64_t file_storage::file_size(aux::file_entry const& fe) const
	{
		return fe.size;
	}

	bool file_storage::pad_file_at(aux::file_entry const& fe) const
	{
		return fe.pad_file;
	}

	std::int64_t file_storage::file_offset(aux::file_entry const& fe) const
	{
		return fe.offset;
	}

	file_entry file_storage::at(file_storage::iterator i) const
	{ return at_deprecated(int(i - m_files.begin())); }
#endif // TORRENT_ABI_VERSION

	void file_storage::swap(file_storage& ti) noexcept
	{
		using std::swap;
		swap(ti.m_files, m_files);
		swap(ti.m_file_hashes, m_file_hashes);
		swap(ti.m_symlinks, m_symlinks);
		swap(ti.m_mtime, m_mtime);
		swap(ti.m_paths, m_paths);
		swap(ti.m_name, m_name);
		swap(ti.m_total_size, m_total_size);
		swap(ti.m_num_pieces, m_num_pieces);
		swap(ti.m_piece_length, m_piece_length);
		swap(ti.m_v2, m_v2);
	}

	void file_storage::canonicalize()
	{
		canonicalize_impl(false);
	}

	void file_storage::canonicalize_impl(bool const backwards_compatible)
	{
		TORRENT_ASSERT(piece_length() >= 16 * 1024);

		// use this vector to track the new ordering of files
		// this allows the use of STL algorithms despite them
		// not supporting a custom swap functor
		aux::vector<file_index_t, file_index_t> new_order(end_file());
		for (auto i : file_range())
			new_order[i] = i;

		// remove any existing pad files
		{
			auto pad_begin = std::partition(new_order.begin(), new_order.end()
				, [this](file_index_t i) { return !m_files[i].pad_file; });
			new_order.erase(pad_begin, new_order.end());
		}

		// TODO: this would be more efficient if m_paths was sorted first, such
		// that a lower path index always meant sorted-before

		// sort files by path/name
		std::sort(new_order.begin(), new_order.end()
			, [this](file_index_t l, file_index_t r)
		{
			// assuming m_paths are unique!
			auto const& lf = m_files[l];
			auto const& rf = m_files[r];
			if (lf.path_index != rf.path_index)
			{
				int const ret = path_compare(m_paths[lf.path_index], lf.filename()
					, m_paths[rf.path_index], rf.filename());
				if (ret != 0) return ret < 0;
			}
			return lf.filename() < rf.filename();
		});

		aux::vector<aux::file_entry, file_index_t> new_files;
		aux::vector<char const*, file_index_t> new_file_hashes;
		aux::vector<std::time_t, file_index_t> new_mtime;

		// reserve enough space for the worst case after padding
		new_files.reserve(new_order.size() * 2 - 1);
		if (!m_file_hashes.empty())
			new_file_hashes.reserve(new_order.size() * 2 - 1);
		if (!m_mtime.empty())
			new_mtime.reserve(new_order.size() * 2 - 1);

		// re-compute offsets and insert pad files as necessary
		std::int64_t off = 0;

		auto add_pad_file = [&](file_index_t const i) {
			if ((off % piece_length()) != 0 && m_files[i].size > 0)
			{
				auto const pad_size = piece_length() - (off % piece_length());
				TORRENT_ASSERT(pad_size < piece_length());
				TORRENT_ASSERT(pad_size > 0);
				new_files.emplace_back();
				auto& pad = new_files.back();
				pad.size = static_cast<std::uint64_t>(pad_size);
				pad.offset = static_cast<std::uint64_t>(off);
				off += pad_size;
				pad.path_index = get_or_add_path(".pad");
				char name[30];
				std::snprintf(name, sizeof(name), "%" PRIu64, pad.size);
				pad.set_name(name);
				pad.pad_file = true;

				if (!m_file_hashes.empty())
					new_file_hashes.push_back(nullptr);
				if (!m_mtime.empty())
					new_mtime.push_back(0);
			}
		};

		for (file_index_t i : new_order)
		{
			if (backwards_compatible)
				add_pad_file(i);

			TORRENT_ASSERT(!m_files[i].pad_file);
			new_files.emplace_back(std::move(m_files[i]));

			if (i < m_file_hashes.end_index())
				new_file_hashes.push_back(m_file_hashes[i]);
			else if (!m_file_hashes.empty())
				new_file_hashes.push_back(nullptr);

			if (i < m_mtime.end_index())
				new_mtime.push_back(m_mtime[i]);
			else if (!m_mtime.empty())
				new_mtime.push_back(0);

			auto& file = new_files.back();
			TORRENT_ASSERT(off < max_file_offset - static_cast<std::int64_t>(file.size));
			file.offset = static_cast<std::uint64_t>(off);
			off += file.size;

			// we don't pad single-file torrents. That would make it impossible
			// to have single-file hybrid torrents.
			if (!backwards_compatible && num_files() > 1)
				add_pad_file(i);
		}

		m_files = std::move(new_files);
		m_file_hashes = std::move(new_file_hashes);
		m_mtime = std::move(new_mtime);

		m_total_size = off;
	}

	/**
	 * 清理和验证符号链接（symlink）
	 * 
	 * 注意：符号链接本身有个 file_path，它的值指向的是 torrent 中存在的另一个文件的 file_path。
	 * 
	 * 作用：
	 * - 确保符号链接的目标路径是合法的
	 * - 不会指向 torrent 文件之外的位置
	 * - 不会导致无限循环或其他问题
	 * 
	 * 如果符号链接的目标路径不合法，则将其修改为指向自身
	 */
	void file_storage::sanitize_symlinks()
	{
		// symlinks are unusual, this function is optimized assuming there are no
		// symbolic links in the torrent. If we find one symbolic link, we'll
		// build the hash table of files it's allowed to refer to, but don't pay
		// that price up-front.
		// 用于存储 torrent 文件中所有文件的路径和索引，只有在发现符号链接时才会初始化。
		std::unordered_map<std::string, file_index_t> file_map;
		bool file_map_initialized = false;

		// lazily instantiated set of all valid directories a symlink may point to
		// TODO: in C++17 this could be string_view
		// 用于存储 torrent 文件中所有目录的路径，只有在需要时才会初始化。
		std::unordered_set<std::string> dir_map;
		bool dir_map_initialized = false;

		// symbolic links that points to directories
		// 用于存储符号链接的目标路径（如果目标是目录）。
		std::unordered_map<std::string, std::string> dir_links;

		// we validate symlinks in (potentially) 2 passes over the files.
		// remaining symlinks to validate after the first pass
		// 用于存储需要进一步验证的符号链接。
		std::vector<file_index_t> symlinks_to_validate;

		// 迭代处理在 file_storage 中的所有文件
		for (auto const i : file_range())
		{
			// 过滤掉非符号链接文件
			if (!(file_flags(i) & file_storage::flag_symlink)) continue;

			if (!file_map_initialized)
			{
				// 发现符号链接，初始化 file_map，将所有文件的路径和索引，添加到 file_map 中
				for (auto const j : file_range())
					file_map.insert({internal_file_path(j), j});
				file_map_initialized = true;
			}

			aux::file_entry const& fe = m_files[i];
			TORRENT_ASSERT(fe.symlink_index < int(m_symlinks.size()));

			// 检查符号链接的目标路径是否合法----------------------

			// symlink targets are only allowed to point to files or directories in
			// this torrent.
			// 符号链接的目标仅允许指向此种子文件中的文件或目录。
			{
				// target：符号链接的目标路径
				std::string target = m_symlinks[fe.symlink_index];

				// 如果目标是绝对路径，则在 m_symlinks 中将其修改为指向自身
				if (is_complete(target))
				{
					// a symlink target is not allowed to be an absolute path, ever
					// this symlink is invalid, make it point to itself
					// 符号链接的目标不允许是绝对路径，即使这个符号链接是无效的，也应该让它指向自己。
					m_symlinks[fe.symlink_index] = internal_file_path(i);
					continue;
				}

				// 如果 target 指向 torrent 文件中的某个文件，则在 m_symlinks 中保留该 target 路径
				// iter 是连接文件指向的内部文件路径
				auto const iter = file_map.find(target);
				if (iter != file_map.end())
				{
					m_symlinks[fe.symlink_index] = target;
					if (file_flags(iter->second) & file_storage::flag_symlink)
					{
						// 如果被链接文件指向的torrent 内部文件还是一个链接文件，就先存放在 dir_links 中。

						// we don't know whether this symlink is a file or a
						// directory, so make the conservative assumption that it's a
						// directory
						// 我们不知道这个符号链接是指向文件还是目录，所以做出保守假设，认为它是指向目录的
						dir_links[internal_file_path(i)] = target;
					}
					continue;
				}

				// it may point to a directory that doesn't have any files (but only
				// other directories), in which case it won't show up in m_paths
				// 它可能指向一个没有任何文件（但只包含其他目录）的目录，在这种情况下，它不会出现在 m_paths 中。
				if (!dir_map_initialized)
				{
					// 初始化 dir_map，即存储所有有效目录路径的集合。
					// dir_map 的作用是帮助验证符号链接的目标路径是否指向 torrent 文件中的有效目录。

					for (auto const& p : m_paths)	 // 遍历 m_paths 中的每个路径
						for (string_view pv = p; !pv.empty();  // 将当前路径 p 转换为 string_view pv，并开始循环
								pv = rsplit_path(pv).first)	   // 每次循环将 pv 更新为其父目录
							dir_map.insert(pv.to_string());	   // 将当前 pv 插入到 dir_map 中
					/*
					* example:
					* m_paths = ["/home/user/subdir/", "/home/tmp/"]
                    * 
                    * 运行 for 后：
					* dir_map = {
                    *    "/home/tmp",
                    *    "/",
                    *    "/home",
                    *    "/home/user",         //第 2 次 insert
                    *    "/home/user/subdir",  //第 1 次 insert
					*  }
					*/
							
					dir_map_initialized = true;
				}

				// 如果链接文件的目标指向 torrent 文件中的某个目录，则在 m_symlinks 和 dir_links 中保留该目标路径。
				if (dir_map.count(target))
				{
					// it points to a sub directory within the torrent, that's OK
					m_symlinks[fe.symlink_index] = target;
					// internal_file_path(i) 就是链接文件的内部路径，target 是链接文件指向的目标路径。
					// 在 dir_links 中映射 key = 链接文件, 指向的 val=目录路径。
					dir_links[internal_file_path(i)] = target;
					continue;
				}

			}

			// for backwards compatibility, allow paths relative to the link as
			// well
			// 如果符号链接的目标路径是相对路径，则将其解析为绝对路径并验证
			if (fe.path_index < aux::file_entry::path_is_absolute)
			{
				std::string target = m_paths[fe.path_index];
				append_path(target, m_symlinks[fe.symlink_index]); // 为何这两者可以进行拼接?

				// 验证目标路径是否合法 ----------

				// if it points to a directory, that's OK
				auto const it = std::find(m_paths.begin(), m_paths.end(), target);
				if (it != m_paths.end())
				{
					m_symlinks[fe.symlink_index] = *it;
					dir_links[internal_file_path(i)] = *it;
					continue;
				}

				if (dir_map.count(target))
				{
					// it points to a sub directory within the torrent, that's OK
					m_symlinks[fe.symlink_index] = target;
					dir_links[internal_file_path(i)] = target;
					continue;
				}

				auto const iter = file_map.find(target);
				if (iter != file_map.end())
				{
					m_symlinks[fe.symlink_index] = target;
					if (file_flags(iter->second) & file_storage::flag_symlink)
					{
						// we don't know whether this symlink is a file or a
						// directory, so make the conservative assumption that it's a
						// directory
						dir_links[internal_file_path(i)] = target;
					}
					continue;
				}
			}

			// we don't know whether this symlink is a file or a
			// directory, so make the conservative assumption that it's a
			// directory
			dir_links[internal_file_path(i)] = m_symlinks[fe.symlink_index];
			// 留后观察
			symlinks_to_validate.push_back(i);
		} // end for


		// 处理复杂符号链接 ---------

		// in case there were some "complex" symlinks, we nee a second pass to
		// validate those. For example, symlinks whose target rely on other
		// symlinks
		// 
		// 以防存在一些“复杂”的符号链接，我们需要再次遍历以验证它们。例如，那些目标依赖于其他符号链接的符号链接。
		// 通过多次遍历和验证，确保符号链接的目标路径指向一个有效的文件或目录，并避免无限循环。
		for (auto const i : symlinks_to_validate)
		{
			aux::file_entry const& fe = m_files[i];
			TORRENT_ASSERT(fe.symlink_index < int(m_symlinks.size()));

			std::string target = m_symlinks[fe.symlink_index];

			// to avoid getting stuck in an infinite loop, we only allow traversing
			// a symlink once
			// traversed 用来记录已经遍历过的路径（为了避免陷入无限循环，我们只允许遍历一次符号链接。）
			std::set<std::string> traversed;

			// this is where we check every path element for existence. If it's not
			// among the concrete paths, it may be a symlink, which is also OK
			// note that we won't iterate through this for the last step, where the
			// filename is included. The filename is validated after the loop
			//
			// 在这里，我们检查每个路径元素是否存在。如果它不在具体路径之中，那么它可能是一个符号链接，这也没问题。
			// 注意，我们在最后一步（包含文件名的那一步）不会对此进行迭代。文件名的验证是在循环之后进行的。
			//
			// 通过 lsplit_path 函数逐步解析目标路径 target 的每一部分（即路径的每个分支）
			for (string_view branch = lsplit_path(target).first;  // 获取第一部分的分支目录(不包含最开始的目录分隔符)
				branch.size() < target.size();
				branch = lsplit_path(target, branch.size() + 1).first) // 从头取下一个分支目录
			{
				auto branch_temp = branch.to_string();
				// this is a concrete directory
				// 对于每个路径分支，检查它是否是一个具体的目录（即是否存在于 dir_map 中）。如果是，则继续检查下一个分支。
				if (dir_map.count(branch_temp)) continue;

				 // 如果当前路径分支不是一个具体目录，检查它是否是一个符号链接
				auto const iter = dir_links.find(branch_temp);
				if (iter == dir_links.end()) goto failed; 		// 如果不是符号链接，跳转到 failed 标签
				if (traversed.count(branch_temp)) goto failed;	// 如果路径已经遍历过，跳转到 failed 标签
				
                // branch 是符号链接，且未遍历过 ------------

				// 将当前路径分支标记为已遍历
				traversed.insert(std::move(branch_temp));

				// this path element is a symlink. substitute the branch so far by
				// the link target
				// 如果当前路径分支是一个符号链接，将其目标路径替换到当前路径中
                //（也就是说，将 target 中当前分支中，指向目录的符号链接，用具体的目录替换掉）
				target = combine_path(iter->second, target.substr(branch.size() + 1));

				// start over with the new (concrete) path
				// 重置路径分支，重新开始解析新的路径
				branch = {};
			}

            /*
                对上面的 for 循环的示例：

                # 初始路径：target = "/home/user/file.txt"。
                # 符号链接：dir_links = { {"home", "/var"} }（home 是一个符号链接，指向 /var）。

                # 执行过程：
                - 第一次循环：
                    branch = "home"。
                    branch 不是具体目录，是一个符号链接。
                    替换目标路径为 /var/user/file.txt。
                    重置 branch，重新开始解析。
                - 第二次循环：
                    branch = "var"。
                    branch 是具体目录（假设 dir_map 包含 "/var"），跳过。
                - 第三次循环：
                    branch = "user"。
                    branch 是具体目录（假设 dir_map 包含 "/var/user"），跳过。
                - 最终目标路径为 /var/user/file.txt。
            */

			// the final (resolved) target must be a valid file
			// or directory
			// 验证最终的目标路径是否是一个有效的文件或目录
			if (file_map.count(target) == 0
				&& dir_map.count(target) == 0) goto failed;

			// this is OK
			// 如果符号链接有效，继续验证下一个符号链接
			continue;

// 处理无效符号链接
failed:

			// this symlink is invalid, make it point to itself
			// 如果符号链接无效，将其目标路径设置为指向自身，以避免进一步的问题
			m_symlinks[fe.symlink_index] = internal_file_path(i);
		}
	}

namespace aux {

	/**
	 * 检测 lhs 和 rhs 的 file_storage 是否兼容。
	 */
	bool files_compatible(file_storage const& lhs, file_storage const& rhs)
	{
		if (lhs.num_files() != rhs.num_files())
			return false;

		if (lhs.total_size() != rhs.total_size())
			return false;

		if (lhs.piece_length() != rhs.piece_length())
			return false;

		// for compatibility, only non-empty and non-pad files matter.
		// those files all need to match in index, name, size and offset
		//
		// 遍历所有文件，检查非空文件和非填充文件是否匹配
		for (file_index_t i : lhs.file_range())
		{
			// 获取文件相关性，即当前文件是否为非填充文件且非空文件
			bool const lhs_relevant = !lhs.pad_file_at(i) && lhs.file_size(i) > 0;
			bool const rhs_relevant = !rhs.pad_file_at(i) && rhs.file_size(i) > 0;

			 // 如果两个文件的相关性不一致（一个相关，一个不相关），返回 false
			if (lhs_relevant != rhs_relevant)
				return false;

			 // 如果当前文件不相关（是填充文件或空文件），跳过检查
			if (!lhs_relevant) continue;

			// we deliberately ignore file attributes like "hidden",
			// "executable" and mtime here. It's not critical they match
			//
			// 检查文件的以下属性是否匹配：
			// 1. 是否为填充文件
			// 2. 文件大小
			// 3. 文件路径
			// 4. 文件偏移量(在 torrent 中，文件的顺序排列后，根据每个文件的大小计算出来的 offset)
			if (lhs.pad_file_at(i) != rhs.pad_file_at(i)
				|| lhs.file_size(i) != rhs.file_size(i)
				|| lhs.file_path(i) != rhs.file_path(i)
				|| lhs.file_offset(i) != rhs.file_offset(i))
			{
				return false;
			}

			// 如果文件是符号链接，检查符号链接的目标路径是否匹配
			if ((lhs.file_flags(i) & file_storage::flag_symlink)
				&& lhs.symlink(i) != rhs.symlink(i))
			{
				return false;
			}
		}
		return true;
	}

	std::tuple<piece_index_t, piece_index_t>
	file_piece_range_exclusive(file_storage const& fs, file_index_t const file)
	{
		peer_request const range = fs.map_file(file, 0, 1);
		std::int64_t const file_size = fs.file_size(file);
		std::int64_t const piece_size = fs.piece_length();
		piece_index_t const begin_piece = range.start == 0 ? range.piece : piece_index_t(static_cast<int>(range.piece) + 1);
		// the last piece is potentially smaller than the other pieces, so the
		// generic logic doesn't really work. If this file is the last file, the
		// last piece doesn't overlap with any other file and it's entirely
		// contained within the last file.
		piece_index_t const end_piece = (file == file_index_t(fs.num_files() - 1))
			? piece_index_t(fs.num_pieces())
			: piece_index_t(int((static_cast<int>(range.piece) * piece_size + range.start + file_size + 1) / piece_size));
		return std::make_tuple(begin_piece, end_piece);
	}

	std::tuple<piece_index_t, piece_index_t>
	file_piece_range_inclusive(file_storage const& fs, file_index_t const file)
	{
		peer_request const range = fs.map_file(file, 0, 1);
		std::int64_t const file_size = fs.file_size(file);
		std::int64_t const piece_size = fs.piece_length();
		piece_index_t const end_piece = piece_index_t(int((static_cast<int>(range.piece)
			* piece_size + range.start + file_size - 1) / piece_size + 1));
		return std::make_tuple(range.piece, end_piece);
	}

	int calc_num_pieces(file_storage const& fs)
	{
		return aux::numeric_cast<int>(
			(fs.total_size() + fs.piece_length() - 1) / fs.piece_length());
	}

	std::int64_t size_on_disk(file_storage const& fs)
	{
		std::int64_t ret = 0;
		for (file_index_t i : fs.file_range())
		{
			if (fs.pad_file_at(i)) continue;
			ret += fs.file_size(i);
		}
		return ret;
	}

	} // namespace aux
}
