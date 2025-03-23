/*

Copyright (c) 2009, 2011, 2013-2014, 2016-2017, 2019-2020, Arvid Norberg
Copyright (c) 2015-2016, 2018, 2020, Alden Torres
Copyright (c) 2016, Andrei Kurushin
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

#include "libtorrent/aux_/bandwidth_manager.hpp"

#if TORRENT_USE_ASSERTS
#include <climits>
#endif

namespace libtorrent {
namespace aux {

	bandwidth_manager::bandwidth_manager(int channel)
		: m_queued_bytes(0)
		, m_channel(channel)
		, m_abort(false)
	{
	}

	void bandwidth_manager::close()
	{
		m_abort = true;

		std::vector<bw_request> queue;
		queue.swap(m_queue);
		m_queued_bytes = 0;

		while (!queue.empty())
		{
			bw_request& bwr = queue.back();
			bwr.peer->assign_bandwidth(m_channel, bwr.assigned);
			queue.pop_back();
		}
	}

#if TORRENT_USE_ASSERTS
	bool bandwidth_manager::is_queued(bandwidth_socket const* peer) const
	{
		for (auto const& r : m_queue)
		{
			if (r.peer.get() == peer) return true;
		}
		return false;
	}
#endif

	int bandwidth_manager::queue_size() const
	{
		return int(m_queue.size());
	}

	std::int64_t bandwidth_manager::queued_bytes() const
	{
		return m_queued_bytes;
	}

	// non prioritized means that, if there's a line for bandwidth,
	// others will cut in front of the non-prioritized peers.
	// this is used by web seeds

	int bandwidth_manager::request_bandwidth(std::shared_ptr<bandwidth_socket> peer
		, int const blk, int const priority, bandwidth_channel** chan, int const num_channels)
	{
		INVARIANT_CHECK;
		if (m_abort) return 0;

		TORRENT_ASSERT(blk > 0);
		TORRENT_ASSERT(priority > 0);

		// if this assert is hit, the peer is requesting more bandwidth before
		// being assigned bandwidth for an already outstanding request
		TORRENT_ASSERT(!is_queued(peer.get()));

		if (num_channels == 0)
		{
			// the connection is not rate limited by any of its
			// bandwidth channels, or it doesn't belong to any
			// channels. There's no point in adding it to
			// the queue, just satisfy the request immediately
			// 如果 num_channels 为 0，表示该请求不受任何带宽通道的限制，或者对等方不属于任何带宽通道。
			// 在这种情况下，直接返回 blk，表示请求可以立即满足。
			return blk;
		}

		// 一个计数器，用于记录需要排队的带宽通道数量。
		int k = 0;

		// 初始化带宽请求
		bw_request bwr(std::move(peer), blk, priority);

		// 遍历所有带宽通道，检查每个通道是否需要排队。
		for (int i = 0; i < num_channels; ++i)
		{
			// 如果 chan[i]->need_queueing(blk) 返回 true，表示该通道需要排队。
			// 将其加入 bwr.channel 数组，并递增 k。
			if (chan[i]->need_queueing(blk))
				bwr.channel[k++] = chan[i];
		}

		// 如果 k 为 0，表示所有带宽通道都不需要排队。
		// 直接返回 blk，表示请求可以立即满足。
		if (k == 0) return blk;

		m_queued_bytes += blk;
		m_queue.push_back(std::move(bwr));
		return 0;
	}

#if TORRENT_USE_INVARIANT_CHECKS
	void bandwidth_manager::check_invariant() const
	{
		std::int64_t queued = 0;
		for (auto const& r : m_queue)
		{
			queued += r.request_size - r.assigned;
		}
		TORRENT_ASSERT(queued == m_queued_bytes);
	}
#endif

	void bandwidth_manager::update_quotas(time_duration const& dt)
	{
		
		// 如果带宽管理器已经中止，直接返回
		if (m_abort) return;

		// 如果没有待处理的带宽请求，直接返回
		if (m_queue.empty()) return;

		INVARIANT_CHECK;

		std::int64_t dt_milliseconds = total_milliseconds(dt);

		// 如果时间间隔超过 3 秒，则将其限制为 3 秒，避免一次性分配过多配额
		if (dt_milliseconds > 3000) dt_milliseconds = 3000;

		// for each bandwidth channel, call update_quota(dt)

		std::vector<bandwidth_channel*> channels;

		std::vector<bw_request> queue;

		// 处理请求队列中，正在断开连接的 peer 的配额
		for (auto i = m_queue.begin(); i != m_queue.end();)
		{
			// 如果 peer 正在断开连接
			if (i->peer->is_disconnecting())
			{
				// 减少 m_queued_bytes，表示队列中总字节数减少。
				m_queued_bytes -= i->request_size - i->assigned;

				// return all assigned quota to all the
				// bandwidth channels this peer belongs to
				// 将已分配的配额返还给所有该 bw_request 关联的带宽通道。
				for (int j = 0; j < bw_request::max_bandwidth_channels && i->channel[j]; ++j)
				{
					bandwidth_channel* bwc = i->channel[j];
					bwc->return_quota(i->assigned);
				}

				// 将 bw_request 请求移动到 queue 中，并从 m_queue 中移除。
				i->assigned = 0;
				queue.push_back(std::move(*i));
				i = m_queue.erase(i);
				continue;
			}

			// 如果 peer 没有断开连接，则初始化带宽通道的临时变量 tmp
			for (int j = 0; j < bw_request::max_bandwidth_channels && i->channel[j]; ++j)
			{
				bandwidth_channel* bwc = i->channel[j];
				bwc->tmp = 0;
			}
			++i;
		}

		// 计算每个带宽通道的总优先级
		for (auto const& r : m_queue)
		{
			for (int j = 0; j < bw_request::max_bandwidth_channels && r.channel[j]; ++j)
			{
				bandwidth_channel* bwc = r.channel[j];
				if (bwc->tmp == 0) channels.push_back(bwc);
				TORRENT_ASSERT(INT_MAX - bwc->tmp > r.priority);

				// 累加每个带宽通道的总优先级
				bwc->tmp += r.priority;
			}
		}

		// 遍历 channels 中的每个带宽通道，更新带宽通道的配额
		for (auto const& ch : channels)
		{
			// 根据时间间隔 dt_milliseconds 更新配额
			ch->update_quota(int(dt_milliseconds));
		}

		// 遍历 m_queue 中的每个带宽请求，分配带宽给请求
		for (auto i = m_queue.begin(); i != m_queue.end();)
		{
			// 分配带宽
			int a = i->assign_bandwidth();

			// 如果请求已经完成（i->assigned == i->request_size）或生存时间耗尽（i->ttl <= 0）
			if (i->assigned == i->request_size
				|| (i->ttl <= 0 && i->assigned > 0))
			{
				a += i->request_size - i->assigned;
				TORRENT_ASSERT(i->assigned <= i->request_size);
				queue.push_back(std::move(*i));
				i = m_queue.erase(i);
			}
			else
			{
				++i;
			}

			// 减少 m_queued_bytes，表示队列中总字节数减少
			m_queued_bytes -= a;
		}

		// 通知 peer 分配带宽
		while (!queue.empty())
		{
			bw_request& bwr = queue.back();

			// 调用对等方的 assign_bandwidth 函数，通知分配的带宽
			bwr.peer->assign_bandwidth(m_channel, bwr.assigned);
			queue.pop_back();
		}
	}
}
}
