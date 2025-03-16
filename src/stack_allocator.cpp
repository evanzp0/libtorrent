/*

Copyright (c) 2017-2019, Arvid Norberg
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

#include "libtorrent/stack_allocator.hpp"
#include <cstdarg> // for va_list, va_copy, va_end

namespace libtorrent {
namespace aux {

	/**
	 * 在栈分配器中复制字符串
	 * 
	 * 此函数的目的是将给定的字符串复制到分配器的内存池中它首先确定字符串在内存池中的起始位置，
	 * 然后调整内存池的大小以容纳新字符串，接着复制字符串内容，并在末尾添加空字符终止最后，返回字符串起始位置的索引
	 * 
	 * @param str 要复制到内存池中的字符串视图
	 * @return 返回一个复制好的字符串在 m_storage 中起始位置 idx 的分配槽
	 */
	allocation_slot stack_allocator::copy_string(string_view str)
	{
	    // 确定字符串在内存池中的起始位置
	    int const ret = int(m_storage.size());
	    // 调整内存池大小以容纳字符串内容和末尾的空字符
	    m_storage.resize(ret + numeric_cast<int>(str.size()) + 1);
	    // 复制字符串内容到内存池中
	    std::memcpy(&m_storage[ret], str.data(), str.size());
	    // 在字符串末尾添加空字符终止
	    m_storage[ret + int(str.length())] = '\0';
	    // 返回字符串起始位置的索引
	    return allocation_slot(ret);
	}

	/**
	 * 在栈分配器中复制字符串
	 * 
	 * 此函数的目的是将一个C风格字符串复制到分配器的内部存储中
	 * 它首先计算字符串的长度，然后扩展内部存储以容纳新字符串
	 * 接着，它使用memcpy函数将字符串复制到内部存储中，并在末尾添加空字符
	 * 最后，它返回一个表示字符串在内部存储中起始位置的分配槽
	 * 
	 * @param str 要复制到内部存储的C风格字符串
	 * @return 返回一个复制好的字符串在 m_storage 中起始位置 idx 的分配槽
	 */
	allocation_slot stack_allocator::copy_string(char const* str)
	{
	    // 获取当前存储的大小，用于计算新字符串的起始位置
	    int const ret = int(m_storage.size());
	    // 计算输入字符串的长度
	    int const len = int(std::strlen(str));
	    // 扩展存储以容纳新字符串和末尾的空字符
	    m_storage.resize(ret + len + 1);
	    // 将字符串复制到内部存储中
	    std::memcpy(&m_storage[ret], str, numeric_cast<std::size_t>(len));
	    // 在字符串末尾添加空字符
	    m_storage[ret + len] = '\0';
	    // 返回表示字符串起始位置的分配槽
	    return allocation_slot(ret);
	}

	allocation_slot stack_allocator::format_string(char const* fmt, va_list v)
	{
		int const pos = int(m_storage.size());
		int len = 512;

		for(;;)
		{
			m_storage.resize(pos + len + 1);

			va_list args;
			va_copy(args, v);

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
			int const ret = std::vsnprintf(m_storage.data() + pos, static_cast<std::size_t>(len) + 1, fmt, args);
#ifdef __clang__
#pragma clang diagnostic pop
#endif

			va_end(args);

			if (ret < 0)
			{
				m_storage.resize(pos);
				return copy_string("(format error)");
			}
			if (ret > len)
			{
				// try again
				len = ret;
				continue;
			}
			break;
		}

		// +1 is to include the 0-terminator
		m_storage.resize(pos + len + 1);
		return allocation_slot(pos);
	}

	allocation_slot stack_allocator::copy_buffer(span<char const> buf)
	{
		int const ret = int(m_storage.size());
		int const size = int(buf.size());
		if (size < 1) return {};
		m_storage.resize(ret + size);
		std::memcpy(&m_storage[ret], buf.data(), numeric_cast<std::size_t>(size));
		return allocation_slot(ret);
	}

	/**
	 * @brief 从栈分配器中分配指定字节数的内存
	 * 
	 * 该函数用于从栈分配器中分配一段内存，以支持数据的存储需求。它首先检查请求分配的字节数是否有效，
	 * 然后在内部存储中预留出指定大小的空间，并返回一个表示内存分配位置的槽。
	 * 
	 * @param bytes 要分配的字节数，必须大于0
	 * @return allocation_slot 返回一个表示分配位置的槽，它持有 stack_allocator.m_storage 这个 vec 的 index。
	 * 						   如果分配失败（如请求的字节数小于1），则返回一个空槽。
	 */
	allocation_slot stack_allocator::allocate(int const bytes)
	{
	    // 检查请求分配的字节数是否小于1，如果是，则返回一个空的分配槽
	    if (bytes < 1) return {};
	
	    // 获取当前存储区域的末尾索引，这将是新分配内存的起始位置
	    int const ret = m_storage.end_index();
	
	    // 调整存储区域的大小，以容纳新分配的内存
	    m_storage.resize(ret + bytes);
	
	    // 返回表示新分配内存位置的槽
	    return allocation_slot(ret);
	}

	char* stack_allocator::ptr(allocation_slot const idx)
	{
		if(idx.val() < 0) return nullptr;
		TORRENT_ASSERT(idx.val() < int(m_storage.size()));
		return &m_storage[idx.val()];
	}

	char const* stack_allocator::ptr(allocation_slot const idx) const
	{
		if(idx.val() < 0) return nullptr;
		TORRENT_ASSERT(idx.val() < int(m_storage.size()));
		return &m_storage[idx.val()];
	}

	void stack_allocator::swap(stack_allocator& rhs)
	{
		m_storage.swap(rhs.m_storage);
	}

	void stack_allocator::reset()
	{
		m_storage.clear();
	}
}
}

