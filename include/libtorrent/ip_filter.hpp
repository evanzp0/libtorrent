/*

Copyright (c) 2017, Andrei Kurushin
Copyright (c) 2005-2007, 2009-2010, 2013, 2015-2017, 2019-2020, Arvid Norberg
Copyright (c) 2016-2018, 2020, Alden Torres
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

#ifndef TORRENT_IP_FILTER_HPP
#define TORRENT_IP_FILTER_HPP

#include "libtorrent/config.hpp"

#include <set>
#include <vector>
#include <cstdint>
#include <tuple>
#include <limits>

#include "libtorrent/address.hpp"

namespace libtorrent {

template <typename Addr>
struct ip_range
{
	Addr first;
	Addr last;
	std::uint32_t flags;
	friend bool operator==(ip_range const& lhs, ip_range const& rhs)
	{
		return lhs.first == rhs.first
			&& lhs.last == rhs.last
			&& lhs.flags == rhs.flags;
	}
};

namespace aux {

	template <typename Addr>
	TORRENT_EXTRA_EXPORT Addr zero();
	template <typename Addr>
	TORRENT_EXTRA_EXPORT Addr plus_one(Addr const& a);
	template <typename Addr>
	TORRENT_EXTRA_EXPORT Addr minus_one(Addr const& a);
	template <typename Addr>
	TORRENT_EXTRA_EXPORT Addr max_addr();

	extern template address_v4::bytes_type minus_one<address_v4::bytes_type>(address_v4::bytes_type const&);
	extern template address_v6::bytes_type minus_one<address_v6::bytes_type>(address_v6::bytes_type const&);
	extern template address_v4::bytes_type plus_one<address_v4::bytes_type>(address_v4::bytes_type const&);
	extern template address_v6::bytes_type plus_one<address_v6::bytes_type>(address_v6::bytes_type const&);
	extern template address_v4::bytes_type zero<address_v4::bytes_type>();
	extern template address_v6::bytes_type zero<address_v6::bytes_type>();
	extern template address_v4::bytes_type max_addr<address_v4::bytes_type>();
	extern template address_v6::bytes_type max_addr<address_v6::bytes_type>();

	inline std::uint16_t plus_one(std::uint16_t val) { return val + 1; }
	inline std::uint16_t minus_one(std::uint16_t val) { return val - 1; }
	template<>
	inline std::uint16_t zero<std::uint16_t>() { return 0; }
	template<>
	inline std::uint16_t max_addr<std::uint16_t>()
	{ return (std::numeric_limits<std::uint16_t>::max)(); }

	// this is the generic implementation of
	// a filter for a specific address type.
	// it works with IPv4 and IPv6
	template <typename Addr>
	class filter_impl
	{
	public:

		filter_impl();
		bool empty() const;
		 /**
		 * @brief 添加一个地址范围规则到过滤器中。
		 *
		 * @param first 范围的起始地址。
		 * @param last  范围的结束地址（包含）。
		 * @param flags 规则的标志位，用于定义访问权限等附加信息。
		 */
		void add_rule(Addr first, Addr last, std::uint32_t flags);

		/**
		 * @brief 查询指定地址的访问权限。
		 *
		 * @param addr 需要查询的地址。
		 * @return 返回该地址对应的访问权限值。
		 */
		std::uint32_t access(Addr const& addr) const;

		/**
		 * @brief 导出过滤器中的所有规则为指定类型的IP范围列表。
		 *
		 * @tparam ExternalAddressType 外部地址类型，通常为IPv4或IPv6。
		 * @return 返回一个包含所有规则的IP范围列表。
		 */
		template <typename ExternalAddressType>
		std::vector<ip_range<ExternalAddressType>> export_filter() const;

	private:
		/**
		 * @brief 表示一个地址范围及其访问权限。
		 */
		struct range
		{
			/**
			 * @brief 构造函数，初始化地址范围。
			 *
			 * @param addr 范围的起始地址。
			 * @param a    访问权限，默认为0。
			 */
			range(Addr addr, std::uint32_t a = 0) : start(addr), access(a) {} // NOLINT
			/**
			 * @brief 比较两个范围对象，按起始地址比较。
			 *
			 * @param r 另一个范围对象。
			 * @return 如果当前范围的起始地址小于另一个范围，则返回 true。
			 */
			bool operator<(range const& r) const { return start < r.start; }
			/**
			 * @brief 比较范围对象与单个地址，按起始地址比较。
			 *
			 * @param a 单个地址。
			 * @return 如果当前范围的起始地址小于给定地址，则返回 true。
			 */
			bool operator<(Addr const& a) const { return start < a; }

			 /**
			 * @brief 范围的起始地址。
			 */
			Addr start;

			// the end of the range is implicit
			// and given by the next entry in the set
			//
			// 该范围的结束是隐含的，
			// 并由集合中的下一个条目来确定。

			/**
			 * @brief 访问权限，表示该范围内的地址可以如何访问。
			 */
			std::uint32_t access;
			friend bool operator==(range const& lhs, range const& rhs)
			{ return lhs.start == rhs.start && lhs.access == rhs.access; }
		};

		std::set<range> m_access_list;
	};

	extern template class filter_impl<address_v4::bytes_type>;
	extern template class filter_impl<address_v6::bytes_type>;
	extern template class filter_impl<std::uint16_t>;

	extern template std::vector<ip_range<address_v4>> filter_impl<address_v4::bytes_type>::export_filter() const;
	extern template std::vector<ip_range<address_v6>> filter_impl<address_v6::bytes_type>::export_filter() const;
	extern template std::vector<ip_range<std::uint16_t>> filter_impl<std::uint16_t>::export_filter() const;
}

// The ``ip_filter`` class is a set of rules that uniquely categorizes all
// ip addresses as allowed or disallowed. The default constructor creates
// a single rule that allows all addresses (0.0.0.0 - 255.255.255.255 for
// the IPv4 range, and the equivalent range covering all addresses for the
// IPv6 range).
//
// A default constructed ip_filter does not filter any address.
//
// “ip_filter” 类是一组规则，这些规则能唯一地将所有 IP 地址归类为允许或不允许的。
// 默认构造函数会创建一条单一规则，
// 该规则允许所有地址（对于 IPv4 地址范围是 0.0.0.0 到 255.255.255.255，对于 IPv6 地址范围则是涵盖所有地址的等效范围）。
//
// 一个通过默认方式构造的 “ip_filter” 不会过滤任何地址。
struct TORRENT_EXPORT ip_filter
{
	ip_filter();
	ip_filter(ip_filter const&);
	ip_filter(ip_filter&&);
	ip_filter& operator=(ip_filter const&);
	ip_filter& operator=(ip_filter&&);
	~ip_filter();

	// the flags defined for an IP range
	enum access_flags
	{
		// indicates that IPs in this range should not be connected
		// to nor accepted as incoming connections
		blocked = 1
	};

	// returns true if the filter does not contain any rules
	bool empty() const;

	// Adds a rule to the filter. ``first`` and ``last`` defines a range of
	// ip addresses that will be marked with the given flags. The ``flags``
	// can currently be 0, which means allowed, or ``ip_filter::blocked``, which
	// means disallowed.
	//
	// precondition:
	// ``first.is_v4() == last.is_v4() && first.is_v6() == last.is_v6()``
	//
	// postcondition:
	// ``access(x) == flags`` for every ``x`` in the range [``first``, ``last``]
	//
	// This means that in a case of overlapping ranges, the last one applied takes
	// precedence.
	//
	// 向过滤器添加一条规则。“first” 和 “last” 定义了一个 IP 地址范围，该范围内的 IP 地址将被标记上给定的标志。
	// 当前，“flags” 可以为 0，这表示允许；或者为 “ip_filter::blocked”，这表示不允许。
	//
	// 前置条件：
	// “first.is_v4() == last.is_v4() && first.is_v6() == last.is_v6()”
	//
	// 后置条件：
	// 对于范围 [“first”，“last”] 内的每个 “x”，“access(x) == flags”
	//
	// 这意味着，在存在范围重叠的情况下，最后应用的那条规则具有优先权。 
	void add_rule(address const& first, address const& last, std::uint32_t flags);

	// Returns the access permissions for the given address (``addr``). The permission
	// can currently be 0 or ``ip_filter::blocked``. The complexity of this operation
	// is O(``log`` n), where n is the minimum number of non-overlapping ranges to describe
	// the current filter.
	std::uint32_t access(address const& addr) const;

	using filter_tuple_t = std::tuple<std::vector<ip_range<address_v4>>
		, std::vector<ip_range<address_v6>>>;

	// This function will return the current state of the filter in the minimum number of
	// ranges possible. They are sorted from ranges in low addresses to high addresses. Each
	// entry in the returned vector is a range with the access control specified in its
	// ``flags`` field.
	//
	// The return value is a tuple containing two range-lists. One for IPv4 addresses
	// and one for IPv6 addresses.
	//
	// 此函数将以尽可能少的范围数量返回过滤器的当前状态。这些范围按照从低地址范围到高地址范围进行排序。
	// 返回的向量中的每个条目都是一个范围，其 “flags” 字段中指定了访问控制权限。
	//
	// 返回值是一个包含两个范围列表的元组。一个用于 IPv4 地址，另一个用于 IPv6 地址。
	filter_tuple_t export_filter() const;

private:

	aux::filter_impl<address_v4::bytes_type> m_filter4;
	aux::filter_impl<address_v6::bytes_type> m_filter6;
};

// the port filter maps non-overlapping port ranges to flags. This
// is primarily used to indicate whether a range of ports should
// be connected to or not. The default is to have the full port
// range (0-65535) set to flag 0.
class TORRENT_EXPORT port_filter
{
public:

	port_filter();
	port_filter(port_filter const&);
	port_filter(port_filter&&);
	port_filter& operator=(port_filter const&);
	port_filter& operator=(port_filter&&);
	~port_filter();

	// the defined flags for a port range
	enum access_flags
	{
		// this flag indicates that destination ports in the
		// range should not be connected to
		blocked = 1
	};

	// set the flags for the specified port range (``first``, ``last``) to
	// ``flags`` overwriting any existing rule for those ports. The range
	// is inclusive, i.e. the port ``last`` also has the flag set on it.
	void add_rule(std::uint16_t first, std::uint16_t last, std::uint32_t flags);

	// test the specified port (``port``) for whether it is blocked
	// or not. The returned value is the flags set for this port.
	// see access_flags.
	std::uint32_t access(std::uint16_t port) const;

private:

	aux::filter_impl<std::uint16_t> m_filter;

};

}

#endif
