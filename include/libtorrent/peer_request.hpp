/*

Copyright (c) 2004, 2009, 2013-2014, 2016-2017, 2019-2020, Arvid Norberg
Copyright (c) 2004, Magnus Jonsson
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

#ifndef TORRENT_PEER_REQUEST_HPP_INCLUDED
#define TORRENT_PEER_REQUEST_HPP_INCLUDED

#include "libtorrent/units.hpp"

namespace libtorrent {

	// represents a byte range within a piece. Internally this is is used for
	// incoming piece requests.
	/**
	 * @brief 描述一个数据块请求的范围信息
	 * 
	 * @note 内部用于表示收到的piece请求数据范围
	 * @note 所有字段使用网络字节序(大端序)
	 */
	struct TORRENT_EXPORT peer_request
	{
		// The index of the piece in which the range starts.
		// 数据块索引(piece索引，从0开始)
		piece_index_t piece;

		// The byte offset within that piece where the range starts.
		// 块内起始字节偏移量，必须小于piece_size
		int start;

		// The size of the range, in bytes.
		// 请求的字节长度，start + length <= piece_size
		int length;

		// returns true if the right hand side peer_request refers to the same
		// range as this does.
		/**
		 * @brief 判断两个请求是否指向相同数据范围
		 * 
		 * @param r 要比较的另一个peer_request
		 * 
		 * @return true  所有字段完全相同
		 *         false 任一字段不同
		 */
		bool operator==(peer_request const& r) const
		{ return piece == r.piece && start == r.start && length == r.length; }
	};
}

#endif // TORRENT_PEER_REQUEST_HPP_INCLUDED
