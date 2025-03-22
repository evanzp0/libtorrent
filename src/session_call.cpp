/*

Copyright (c) 2014-2019, Steven Siloti
Copyright (c) 2014, 2016, 2019, Arvid Norberg
Copyright (c) 2016, Alden Torres
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

#include "libtorrent/aux_/session_call.hpp"

namespace libtorrent { namespace aux {

#ifdef TORRENT_PROFILE_CALLS
static std::mutex g_calls_mutex;
static std::unordered_map<std::string, int> g_blocking_calls;
#endif

void blocking_call()
{
#ifdef TORRENT_PROFILE_CALLS
	char stack[2048];
	print_backtrace(stack, sizeof(stack), 20);
	std::unique_lock<std::mutex> l(g_calls_mutex);
	g_blocking_calls[stack] += 1;
#endif
}

void dump_call_profile()
{
#ifdef TORRENT_PROFILE_CALLS
	FILE* out = fopen("blocking_calls.txt", "w+");

	std::map<int, std::string> profile;

	std::unique_lock<std::mutex> l(g_calls_mutex);
	for (auto const& c : g_blocking_calls)
	{
		profile[c.second] = c.first;
	}
	for (std::map<int, std::string>::const_reverse_iterator i = profile.rbegin()
		, end(profile.rend()); i != end; ++i)
	{
		std::fprintf(out, "\n\n%d\n%s\n", i->first, i->second.c_str());
	}
	fclose(out);
#endif
}

/**
 * 该函数用于等待一个特定的条件（这里是done变量的值变为true）发生
 */
void torrent_wait(bool& done, aux::session_impl& ses)
{
	// blocking_call() 是不必要的或者用于特定的初始化或同步目的
	blocking_call(); 
	
	// 建一个 std::unique_lock 对象，并锁定 ses.mut 互斥锁。
	// 互斥锁用于保护共享数据（这里是 done），确保线程安全。
	std::unique_lock<std::mutex> l(ses.mut);

	while (!done) {

		// 如果条件不满足(done 为 false), 调用 ses.cond.wait(l),
		// wait 会释放互斥锁 l，并将当前线程挂起，直到条件变量 ses.cond 被通知。
		// 当 ses.cond 被通知时，线程会被唤醒（即另一个线程调用notify_one或notify_all），并重新获取互斥锁 l。
		ses.cond.wait(l);
	}

	// 当 torrent_wait 函数结束时，l(ses.mut) 这个锁会被释放。
}

} } // namespace aux namespace libtorrent
