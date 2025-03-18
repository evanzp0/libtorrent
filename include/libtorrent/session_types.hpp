/*

Copyright (c) 2017, Alden Torres
Copyright (c) 2017-2020, Arvid Norberg
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

#ifndef TORRENT_SESSION_TYPES_HPP_INCLUDED
#define TORRENT_SESSION_TYPES_HPP_INCLUDED

#include <cstdint>
#include "libtorrent/flags.hpp"

namespace libtorrent {

	// save_state_flags_t 是一个位标志字段类型，它的底层类型是 std::uint32_t，可以存储 32 个二进制标志。
  // 它使用 save_state_flags_tag 作为类型标签，确保类型安全。
  // 它可以用于表示一组二进制标志，例如 save_settings、save_dht_state 等。
  // 
  // struct save_state_flags_tag 是一个空的标签结构体。它的唯一作用是作为一个唯一的类型标识符，用于区分不同的位标志字段类型。
  // 如果定义了 using other_flags_t = flags::bitfield_flag<std::uint32_t, struct other_flags_tag>;
  // 那么 save_state_flags_t 和 other_flags_t 就是完全不同的类型，即使它们的底层类型都是 std::uint32_t。
  // 这种设计可以避免不同类型的位标志字段之间的误用。
  //
  // Example:
  // ```c++
  // static constexpr save_state_flags_t save_settings = 0_bit;  // 第 0 位，值为 1
  // static constexpr save_state_flags_t save_dht_state = 1_bit; // 第 1 位，值为 2
  // ```
	using save_state_flags_t = flags::bitfield_flag<std::uint32_t, struct save_state_flags_tag>;

	// hidden
	using session_flags_t = flags::bitfield_flag<std::uint8_t, struct session_flags_tag>;

	// The flags type used to specify options to removing files of torrents
	using remove_flags_t = flags::bitfield_flag<std::uint8_t, struct remove_flags_tag>;

	// hidden
	using reopen_network_flags_t = flags::bitfield_flag<std::uint8_t, struct reopen_network_flags_tag>;
}

#endif

