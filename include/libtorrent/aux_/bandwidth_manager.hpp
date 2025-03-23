/*

Copyright (c) 2007, 2009, 2011-2016, 2020, Arvid Norberg
Copyright (c) 2016, 2018, 2020, Alden Torres
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

#ifndef TORRENT_BANDWIDTH_MANAGER_HPP_INCLUDED
#define TORRENT_BANDWIDTH_MANAGER_HPP_INCLUDED

#include <memory>
#include <vector>

#include "libtorrent/aux_/invariant_check.hpp"
#include "libtorrent/assert.hpp"
#include "libtorrent/aux_/bandwidth_limit.hpp"
#include "libtorrent/aux_/bandwidth_queue_entry.hpp"
#include "libtorrent/aux_/bandwidth_socket.hpp"
#include "libtorrent/time.hpp"

namespace libtorrent {
namespace aux {

/**
 * bandwidth_manager 是一个用于管理带宽分配的结构体，主要用于控制上传和下载带宽的分配。
 * 它通过维护一个请求队列，并根据优先级和配额来分配带宽。
 */
struct TORRENT_EXTRA_EXPORT bandwidth_manager
{
	/**
	 * @brief bandwidth_manager 构造函数
	 * @param channel 参数表示带宽通道的类型（例如，上传或下载）
	 */
	explicit bandwidth_manager(int channel);

	/**
	 * @brief 关闭带宽管理器，停止所有带宽分配操作
	 */
	void close();

#if TORRENT_USE_ASSERTS
	bool is_queued(bandwidth_socket const* peer) const;
#endif

	/**
	 * @brief 返回当前请求队列的大小，即等待带宽分配的请求数量。
	 */
	int queue_size() const;

	/**
	 * @brief 返回当前请求队列中所有请求的总字节数。
	 */
	std::int64_t queued_bytes() const;

	// non prioritized means that, if there's a line for bandwidth,
	// others will cut in front of the non-prioritized peers.
	// this is used by web seeds
	// returns the number of bytes to assign to the peer, or 0
	// if the peer's 'assign_bandwidth' callback will be called later
    // 非优先级对等节点意味着，如果存在带宽分配的情况，其他节点会插队到非优先级对等节点之前。
	// 这在网络种子（web seeds）的场景中会用到。
	/**
	 * @brief 用于请求带宽分配。
	 * 当一个对等方需要上传或下载数据时，会调用 request_bandwidth 请求带宽。
	 * 根据带宽通道的状态，请求可能立即得到满足，或者需要排队等待。
	 * 
	 * @param peer 请求带宽的 bandwidth_socket 对象，表示需要带宽的对等方（peer）。
	 * @param blk 请求的块大小（字节数）。
	 * @param priority 请求的优先级，优先级越高，越优先分配带宽。
	 * @param chan 带宽通道数组，表示该请求涉及的带宽通道。
	 * 		比如 peer_class_info 中就有属性 bandwidth_channel channel[2]，
	 * 		用来跟踪当前的上传和下载通道的配额情况（0: 上传；1：下载）。
	 * @param num_channels 请带宽通道的数量。如果 num_channels 为 0，表示该请求不受任何带宽通道的限制。
	 * 
	 * @return 如果请求可以立即满足，返回分配的字节数（通常是 blk）。
	 * 如果请求需要排队，返回 0，表示稍后会通过回调分配带宽。
	 * 
	 * @example:
	 * ```cpp
	 * bandwidth_manager download_manager;
	 * 
	 * // 创建一个带宽请求
	 * std::shared_ptr<bandwidth_socket> peer = ...;
	 * bandwidth_channel* channels[] = { &upload_channel, &download_channel };
	 * int bytes_assigned = download_manager.request_bandwidth(peer, 1024, 1, channels, 2);
	 *
	 * if (bytes_assigned > 0) {
	 *    // 请求立即得到满足
	 *    peer->assign_bandwidth(bytes_assigned);
	 * } else {
	 *    // 请求需要排队，稍后会通过回调分配带宽
	 * }
	 * ```
	 */
	int request_bandwidth(std::shared_ptr<bandwidth_socket> peer
		, int blk, int priority, bandwidth_channel** chan, int num_channels);

#if TORRENT_USE_INVARIANT_CHECKS
	void check_invariant() const;
#endif

	/**
	 * @brief 更新带宽配额，并根据配额分配带宽给队列中的请求。
	 * 
	 * @param dt 时间间隔，表示从上一次更新配额到现在的时间差。
	 */
	void update_quotas(time_duration const& dt);

private:

	// these are the consumers that want bandwidth
	// 存储带宽请求的队列。
	std::vector<bw_request> m_queue;
	
	// the number of bytes all the requests in queue are for
	// 队列中所有请求的总字节数。
	std::int64_t m_queued_bytes;

	// this is the channel within the consumers
	// that bandwidth is assigned to (upload or download)
	// 带宽通道的类型（例如，上传或下载）。
	// 这是消费者内部的通道，带宽会分配到该通道。
	int m_channel;

	// 标志位，表示是否中止带宽分配
	bool m_abort;
};

}
}

#endif
