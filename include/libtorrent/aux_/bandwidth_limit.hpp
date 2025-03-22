/*

Copyright (c) 2007, 2009-2010, 2012, 2014, 2016-2017, 2019-2020, Arvid Norberg
Copyright (c) 2017, Andrei Kurushin
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

#ifndef TORRENT_BANDWIDTH_CHANNEL_HPP_INCLUDED
#define TORRENT_BANDWIDTH_CHANNEL_HPP_INCLUDED

#include <cstdint>
#include <limits>

#include "libtorrent/assert.hpp"

namespace libtorrent {
namespace aux {

/**
 * peer_connection 的成员，用于管理带宽限制和配额。
 * 它与 bandwidth_manager 和 bw_request 一起工作，共同实现带宽分配的逻辑。
 */
// member of peer_connection
struct TORRENT_EXTRA_EXPORT bandwidth_channel
{
	//  表示无限带宽
	static constexpr int inf = (std::numeric_limits<std::int32_t>::max)();

	bandwidth_channel();

	// 设置带宽限制（字节/秒）。0 表示无限带宽。
	// 0 means infinite
	void throttle(int limit);
	int throttle() const
	{
		TORRENT_ASSERT_VAL(m_limit >= 0, m_limit);
		TORRENT_ASSERT_VAL(m_limit < inf, m_limit);
		return m_limit;
	}

	// 返回剩余的配额（字节）。
	int quota_left() const;

	// 根据时间间隔 dt_milliseconds 更新配额。
	//例如，如果带宽限制为 100 KB/s，且 dt_milliseconds 为 1000 毫秒（1 秒），则配额增加 100 KB。
	void update_quota(int dt_milliseconds);

	// this is used when connections disconnect with
	// some quota left. It's returned to its bandwidth
	// channels.
	//
	// 将未使用的配额返还给带宽通道。例如，如果一个连接断开时还有未使用的配额，可以将其返还。
	void return_quota(int amount);

	// 使用配额（减少剩余的配额）。
	void use_quota(int amount);

	// this is an optimization. If there is more than one second
	// of quota built up in this channel, just apply it right away
	// instead of introducing a delay to split it up evenly. This
	// should especially help in situations where a single peer
	// has a capacity under the rate limit, but would otherwise be
	// held back by the latency of getting bandwidth from the limiter
	//
	// 注意：这里的剩余带宽（m_quota_left）是一段时间的累计值。
	// 按时间累积配额的设计可以减少延迟，特别是在带宽需求低于带宽限制的情况下。
	// 例如，如果一个对等方的带宽需求是 50 KB/s，而带宽限制是 100 KB/s，那么它可以立即使用积累的配额，而不需要等待时间周期。
	//
	// 优化的背景：
	// 在带宽限制的场景中，带宽配额是按时间分配的。例如，如果带宽限制是 100 KB/s，那么每秒钟会分配 100 KB 的配额。
	// 如果没分配，则会和下一秒的配额累累加到 m_quota_left 中。
	// 通常情况下，带宽配额是按时间均匀分配的，以避免突发流量导致网络拥塞。
	//
	// 问题描述：
	// - 如果按时间均匀分配配额，对等方需要等待一个完整的时间周期（1 秒）才能获得 100 KB 的配额。 
	//   但实际上，对等方只需要 10 KB 的配额，因此它需要等待 1 秒才能获得 100 KB 的配额，即使它只需要 10 KB。
	//   这种等待会导致延迟，特别是在对等方的带宽需求远低于带宽限制的情况下。
	// 例如：
	//   假设带宽限制为 100 KB/s，对等方的带宽需求为 10 KB/s。
	//   第 1 秒：分配 100 KB 配额，对等方使用 10 KB，剩余 90 KB。
	//   第 2 秒：分配 100 KB 配额，对等方使用 10 KB，剩余 180 KB。
	//   第 3 秒：分配 100 KB 配额，对等方使用 10 KB，剩余 270 KB。
	//   对等方每秒钟只能使用 10 KB 的配额，但它需要等待 1 秒才能获得 100 KB 的配额，导致延迟。
	//
	// 优化的逻辑：
	// - 如果带宽通道中积累了超过一秒的配额（即 m_quota_left - amount >= m_limit），则可以直接分配配额，而不需要按时间均匀分配。
	// - 如果 m_quota_left - amount < m_limit, 就需要等待一个完整的时间周期才能获得固定的配额。
	// - 这样可以避免延迟，特别是在对等方的带宽需求低于带宽限制的情况下。
	//
	// 检查是否需要将请求加入队列。
	// - 如果剩余配额不足以满足请求，则返回 true，表示需要排队。
	// - 否则，减少剩余配额并返回 false，表示可以立即分配带宽。
	bool need_queueing(int amount)
	{
		// 如果 m_quota_left - amount < m_limit, 意味着当前剩余配额不足以满足请求，且剩余的配额不足以维持带宽限制。
		// 这就要需要将请求加入队列，等待下一个时间周期分配固定的带宽配额（按时间均匀分配）。
		if (m_quota_left - amount < m_limit) return true;

		// 如果带宽通道中积累了超过一秒的配额（即 m_quota_left - amount >= m_limit），
		// 这意味着当前剩余配额足够满足请求，且剩余的配额仍然可以维持带宽限制。
		// 则可以直接分配配额，而不需要按时间均匀分配，所以不需要将请求加入队列。
		m_quota_left -= amount;
		return false;
	}

	// used as temporary storage while distributing
	// bandwidth
	int tmp;

	// this is the number of bytes to distribute this round
	// 当前轮次分配的配额。
	int distribute_quota;

private:

	// this is the amount of bandwidth we have
	// been assigned without using yet.
	// 剩余配额。这是我们已被分配后，剩余尚未使用的带宽量（也就是可用的累计带宽）。
	std::int64_t m_quota_left;

	// the limit is the number of bytes
	// per second we are allowed to use.
	// 带宽限制（字节/秒）。
	std::int32_t m_limit;
};

}
}

#endif
