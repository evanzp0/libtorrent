/*

Copyright (c) 2007, 2009, 2012, 2014-2015, 2020, Arvid Norberg
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

#ifndef TORRENT_BANDWIDTH_QUEUE_ENTRY_HPP_INCLUDED
#define TORRENT_BANDWIDTH_QUEUE_ENTRY_HPP_INCLUDED

#include <memory>

#include "libtorrent/aux_/bandwidth_limit.hpp"
#include "libtorrent/aux_/bandwidth_socket.hpp"
#include "libtorrent/aux_/array.hpp"

namespace libtorrent {
namespace aux {

/**
 * bw_request 表示一个带宽请求。
 * 它包含了对等方（peer）的信息、请求的优先级、已分配的带宽、请求的大小等信息，并提供了分配带宽的功能。
 */
struct TORRENT_EXTRA_EXPORT bw_request
{
	bw_request(std::shared_ptr<bandwidth_socket> pe
		, int blk, int prio);

	std::shared_ptr<bandwidth_socket> peer;
	// 1 is normal prio
	int priority;

	// the number of bytes assigned to this request so far
	// 表示已经分配给该请求的带宽（字节数）
	int assigned;

	// 表示请求的总带宽大小（字节数）。
	//
	// once assigned reaches this, we dispatch the request function
	// 当 assigned 达到 request_size 时，请求完成，我们就会调用 request 函数。
	int request_size;

	// the max number of rounds for this request to survive
	// this ensures that requests gets responses at very low
	// rate limits, when the requested size would take a long
	// time to satisfy
	// 表示该请求的最大生存轮数，
	// 这是为了确保在带宽限制非常低的情况下，请求仍然能够得到响应，避免请求长时间得不到满足。
	int ttl;

	// loops over the bandwidth channels and assigns bandwidth
	// from the most limiting one

	/**
	 * @brief 遍历各个带宽通道，并从限制最严格的通道开始分配带宽。
	 * 
	 * @return int 返回分配的带宽量
	 */
	int assign_bandwidth();

	// 一个常量，表示每个对等方最多可以关联的带宽通道数量。
	static constexpr int max_bandwidth_channels = 10;

	// channel 是一个数组(上限为 10)，存储了该请求关联的带宽通道指针。
	// 每个带宽通道（bandwidth_channel）表示一个带宽限制的通道（例如，上传或下载通道）。
	// we don't actually support more than 10 channels per peer
	aux::array<bandwidth_channel*, max_bandwidth_channels> channel{};
};

}
}

#endif
