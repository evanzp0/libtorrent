/*

Copyright (c) 2005-2007, 2016-2017, 2019-2020, Arvid Norberg
Copyright (c) 2017-2018, 2020, Alden Torres
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

#include <iterator> // for next

#include "libtorrent/ip_filter.hpp"
#include "libtorrent/assert.hpp"

namespace libtorrent {

	ip_filter::ip_filter() = default;
	ip_filter::ip_filter(ip_filter const&) = default;
	ip_filter::ip_filter(ip_filter&&) = default;
	ip_filter& ip_filter::operator=(ip_filter const&) = default;
	ip_filter& ip_filter::operator=(ip_filter&&) = default;
	ip_filter::~ip_filter() = default;

	bool ip_filter::empty() const { return m_filter4.empty() && m_filter6.empty(); }

	void ip_filter::add_rule(address const& first, address const& last, std::uint32_t flags)
	{
		if (first.is_v4())
		{
			TORRENT_ASSERT(last.is_v4());
			m_filter4.add_rule(first.to_v4().to_bytes(), last.to_v4().to_bytes(), flags);
		}
		else if (first.is_v6())
		{
			TORRENT_ASSERT(last.is_v6());
			m_filter6.add_rule(first.to_v6().to_bytes(), last.to_v6().to_bytes(), flags);
		}
		else
			TORRENT_ASSERT_FAIL();
	}

	std::uint32_t ip_filter::access(address const& addr) const
	{
		if (addr.is_v4())
			return m_filter4.access(addr.to_v4().to_bytes());
		TORRENT_ASSERT(addr.is_v6());
		return m_filter6.access(addr.to_v6().to_bytes());
	}

	ip_filter::filter_tuple_t ip_filter::export_filter() const
	{
		return std::make_tuple(m_filter4.export_filter<address_v4>()
			, m_filter6.export_filter<address_v6>());
	}

	port_filter::port_filter() = default;
	port_filter::port_filter(port_filter const&) = default;
	port_filter::port_filter(port_filter&&) = default;
	port_filter& port_filter::operator=(port_filter const&) = default;
	port_filter& port_filter::operator=(port_filter&&) = default;
	port_filter::~port_filter() = default;

	void port_filter::add_rule(std::uint16_t first, std::uint16_t last, std::uint32_t flags)
	{
		m_filter.add_rule(first, last, flags);
	}

	std::uint32_t port_filter::access(std::uint16_t port) const
	{
		return m_filter.access(port);
	}

namespace aux {

	template <typename Addr>
	Addr zero()
	{
		Addr zero;
		std::fill(zero.begin(), zero.end(), static_cast<typename Addr::value_type>(0));
		return zero;
	}

	template <typename Addr>
	Addr plus_one(Addr const& a)
	{
		Addr tmp(a);
		for (int i = int(tmp.size()) - 1; i >= 0; --i)
		{
			auto& t = tmp[std::size_t(i)];
			if (t < (std::numeric_limits<typename Addr::value_type>::max)())
			{
				t += 1;
				break;
			}
			t = 0;
		}
		return tmp;
	}


	/**
	 * 函数minus_one实现了一个自定义类型Addr的减一操作。
	 * 这个函数通过复制输入参数a，并在复制的临时对象上进行操作，最终返回修改后的临时对象。
	 * 
	 * 例如：
	 * 192.168.1.0，被转换成数组：a = [192, 168, 1, 0]，minus_one(a) = 192.168.0.255
	 * 192.168.1.5，被转换成数组：a = [192, 168, 1, 5]，minus_one(a) = 192.168.1.4
	 * 
	 * @param a 一个Addr类型的常量引用，表示要进行减一操作的 IP 地址。
	 * @return 返回一个新的 Addr 对象，其值为输入参数 a 减一后的结果。
	 */
	template <typename Addr>
	Addr minus_one(Addr const& a)
	{
	    // 复制输入参数a到临时变量 tmp，以便进行后续操作。
	    Addr tmp(a);
	
	    // 从 tmp（内部是个数组，tmp.size()是该数组的大小）的最后一个元素开始向前遍历，尝试进行减一操作。
	    for (int i = int(tmp.size()) - 1; i >= 0; --i)
	    {
	        // 获取 tmp[i] 的引用，以便进行修改。
	        auto& t = tmp[std::size_t(i)];
	
	        // 如果当前 tmp[i] 元素的值大于 0，则对其进行减一操作，并结束循环。
	        if (t > 0)
	        {
	            t -= 1;
	            break;
	        }
	        // 如果当前元素的值为 0，则将其设置为类型的最大值，以准备下一轮的减一操作。
	        t = (std::numeric_limits<typename Addr::value_type>::max)();
	    }
	
	    // 返回经过减一操作后的临时对象 tmp。
	    return tmp;
	}

	template <typename Addr>
	Addr max_addr()
	{
		Addr tmp;
		std::fill(tmp.begin(), tmp.end()
			, (std::numeric_limits<typename Addr::value_type>::max)());
		return tmp;
	}

#ifdef _MSC_VER
#define EXPORT_INST TORRENT_EXTRA_EXPORT
#else
#define EXPORT_INST
#endif

	template EXPORT_INST address_v4::bytes_type minus_one<address_v4::bytes_type>(address_v4::bytes_type const&);
	template EXPORT_INST address_v6::bytes_type minus_one<address_v6::bytes_type>(address_v6::bytes_type const&);
	template EXPORT_INST address_v4::bytes_type plus_one<address_v4::bytes_type>(address_v4::bytes_type const&);
	template EXPORT_INST address_v6::bytes_type plus_one<address_v6::bytes_type>(address_v6::bytes_type const&);
	template EXPORT_INST address_v4::bytes_type zero<address_v4::bytes_type>();
	template EXPORT_INST address_v6::bytes_type zero<address_v6::bytes_type>();
	template EXPORT_INST address_v4::bytes_type max_addr<address_v4::bytes_type>();
	template EXPORT_INST address_v6::bytes_type max_addr<address_v6::bytes_type>();

	template <typename Addr>
	filter_impl<Addr>::filter_impl()
	{
		// make the entire ip-range non-blocked
		m_access_list.insert(range(zero<Addr>(), 0));
	}

	template <typename Addr>
	bool filter_impl<Addr>::empty() const
	{
		return m_access_list.empty()
			|| (m_access_list.size() == 1 && *m_access_list.begin() == range(zero<Addr>(), 0));
	}


	/**
	 * @brief 向访问列表中添加规则，指定地址范围的访问权限。
	 * 
	 * 该函数实现添加规则的逻辑，包括与现有规则合并以及必要时拆分规则。
	 * 
	 * @param first 起始 IP 地址
	 * @param last 结束 IP 地址
	 * @param flags 访问标志（权限）
	 * 
	 * 
	 * eg 1: 
	 * |===================================================| [(0.0.0.0, 0)]
	 * inert    !----------------!                           [(192.168.1.50), (192.168.1.80), 1]
     * result                                                [(0.0.0.0, 0), (192.168.1.50, 1), (192.168.1.81, 0)]
	 * 
	 * eg 2: 
	 * |====================================|--------------| [(0.0.0.0, 0), (192.168.1.100, 1)]
	 * inert    !----------------!							 [(192.168.1.50), (192.168.1.80), 1]
	 * result                                                [(0.0.0.0, 0), (192.168.1.50,1), (192.168.1.81, 0), (192.168.1.100, 1)]
	 * 
	 * eg 3: 
	 * |====================================|--------------| [(0.0.0.0, 0), (192.168.1.100, 1)]
	 * inert                      !-----------------!        [(192.168.1.80), (192.168.1.120), 1]
	 * result                                                [(0.0.0.0, 0), (192.168.1.80, 1)]
	 */
	template <typename Addr>
	void filter_impl<Addr>::add_rule(Addr first, Addr last, std::uint32_t const flags)
	{
		// 断言访问列表不为空，并且起始地址不大于结束地址
		// 初始化 filter_impl<Addr> 会插入一条 range(zero<Addr>(), 0)
		TORRENT_ASSERT(!m_access_list.empty());
		TORRENT_ASSERT(first < last || first == last);

		// 查找起始地址和结束地址应插入的位置
		//
		// 查找 m_access_list 中第一个大于 first 的元素，并返回指向该元素的迭代器。
		// 如果所有元素都小于或等于 first，则返回指向列表末尾的迭代器（即 end()）
		auto i = m_access_list.upper_bound(first);
		// （同上）
		auto j = m_access_list.upper_bound(last);

		// 如果起始地址的位置不是列表开头（zero_addr），则移动到前一个元素
		if (i != m_access_list.begin()) --i; // add_rule 的 first 的插入位置是在 std::prev(i) 元素后。

		// 断言结束地址的位置不是列表开头，并且不等于起始位置
		TORRENT_ASSERT(j != m_access_list.begin());
		TORRENT_ASSERT(j != i);

		// 获取起始位置和结束位置之前的访问权限
		std::uint32_t first_access = i->access;
		std::uint32_t last_access = std::prev(j)->access; // add_rule 的 last 的插入位置是在 std::prev(j) 元素后。

		// 如果插入规则的 IP 地址和 i 规则的 IP 地址不匹配，并且访问权限不同，则插入新规则
		// 比如，[ (0.0.0.0, 0) ] 插入 (192.168.1.1, 192.168.1.5, 1) ，进入该分支结果是 [ (0.0.0.0, 0), (192.168.1.1, 1) ]，
		if (i->start != first && first_access != flags)
		{
			// i 指向新插入后的元素的位置，也就是 (192.168.1.1, 1)
			i = m_access_list.insert(i, range(first, flags));
		}
		// 当新规则的权限与前一个规则相同时，合并规则，避免重复插入？
		else if (i != m_access_list.begin() && std::prev(i)->access == flags) 
		{
			// 将 i 移动到前一个规则的位置（--i）
			--i;
			// 更新 first_access 为前一个规则的权限
			first_access = i->access;
		}

		// 断言访问列表不为空，并且 i 指向了一条规则
		TORRENT_ASSERT(!m_access_list.empty());
		TORRENT_ASSERT(i != m_access_list.end());

		// 删除重叠的规则（删除 item[i + 1] 到 item[j -1] 的所有 item）
		if (i != j) m_access_list.erase(std::next(i), j);

		// 如果 i 规则的起始 IP 地址和插入规则的起始 IP 匹配，则优化更新规则（可以忽略？）
		if (i->start == first)
		{
			// This is an optimization over erasing and inserting a new element
			// here.
			// this const-cast is OK because we know that the new
			// start address will keep the set correctly ordered
			//
			// 这是一个优化操作，避免删除和重新插入元素。
			// 使用 const_cast 是安全的，因为我们确保新的起始地址保持有序
			const_cast<Addr&>(i->start) = first;
			const_cast<std::uint32_t&>(i->access) = flags;
		}
		// 如果访问权限不同，则插入新规则
		else if (first_access != flags)
		{
			m_access_list.insert(i, range(first, flags));
		}

		// 如果 j 存在且和 last 的 ip 地址之间有空隙，或 j 不存在且 last 未到达最大地址（255.255.255.255），则插入新规则
		// 比如，[ (0.0.0.0, 0) ] 插入 (192.168.1.1, 192.168.1.5, 1) ，结果是 [ (0.0.0.0, 0), (192.168.1.1, 1), (192.168.1.6, 0) ]
		if ((j != m_access_list.end() && minus_one(j->start) != last)
			|| (j == m_access_list.end() && last != max_addr<Addr>()))
		{
			// last < minus_one(j->start)) 意味着， last < j 且 last 和 j 之间有间隙
			TORRENT_ASSERT(j == m_access_list.end() || last < minus_one(j->start)); 

			// 被 m_access_list.erase(std::next(i), j) 删除的最后一个元素的权限(last_access) 和 要插入的权限(flags)不同，
			// 则拆分出一个新规则 start = plus_one(last)，来延续被删除的最后一个元素的权限。
			// 例如 (192.168.1.6, 0) 这条就是该分支生成的
			if (last_access != flags)
				j = m_access_list.insert(j, range(plus_one(last), last_access));
		}

		// 合并规则：如果 j 存在，且 j 的权限和 插入的规则权限相同，则删除规则 j。
		//（也就是说，如果 j 的权限和 插入的规则权限相同，就被合并插入的规则中了）
		if (j != m_access_list.end() && j->access == flags) m_access_list.erase(j);

		// 断言访问列表不为空
		TORRENT_ASSERT(!m_access_list.empty());
	}

	template <typename Addr>
	std::uint32_t filter_impl<Addr>::access(Addr const& addr) const
	{
		TORRENT_ASSERT(!m_access_list.empty());
		auto i = m_access_list.upper_bound(addr);
		if (i != m_access_list.begin()) --i;
		TORRENT_ASSERT(i != m_access_list.end());
		TORRENT_ASSERT(i->start <= addr && (std::next(i) == m_access_list.end()
			|| addr < std::next(i)->start));
		return i->access;
	}

	template <typename Addr>
	template <typename ExternalAddressType>
	std::vector<ip_range<ExternalAddressType>> filter_impl<Addr>::export_filter() const
	{
		std::vector<ip_range<ExternalAddressType>> ret;
		ret.reserve(m_access_list.size());

		for (auto i = m_access_list.begin()
			, end(m_access_list.end()); i != end;)
		{
			ip_range<ExternalAddressType> r;
			r.first = ExternalAddressType(i->start);
			r.flags = i->access;

			++i;
			if (i == end)
				r.last = ExternalAddressType(max_addr<Addr>());
			else
				r.last = ExternalAddressType(minus_one(i->start));

			ret.push_back(r);
		}
		return ret;
	}

	template class EXPORT_INST filter_impl<address_v4::bytes_type>;
	template class EXPORT_INST filter_impl<address_v6::bytes_type>;
	template class EXPORT_INST filter_impl<std::uint16_t>;

	template EXPORT_INST std::vector<ip_range<address_v4>> filter_impl<address_v4::bytes_type>::export_filter() const;
	template EXPORT_INST std::vector<ip_range<address_v6>> filter_impl<address_v6::bytes_type>::export_filter() const;
	template EXPORT_INST std::vector<ip_range<std::uint16_t>> filter_impl<std::uint16_t>::export_filter() const;

#undef EXPORT_INST

} // namespace aux
}
