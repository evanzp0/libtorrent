/*

Copyright (c) 2003-2013, Daniel Wallin
Copyright (c) 2013-2020, Arvid Norberg
Copyright (c) 2015, Steven Siloti
Copyright (c) 2016, 2020, Alden Torres
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

#include "libtorrent/config.hpp"
#include "libtorrent/aux_/alert_manager.hpp"
#include "libtorrent/alert_types.hpp"

#ifndef TORRENT_DISABLE_EXTENSIONS
#include "libtorrent/extensions.hpp"
#include <memory> // for shared_ptr
#endif

namespace libtorrent {
namespace aux {

	alert_manager::alert_manager(int const queue_limit, alert_category_t const alert_mask)
		: m_alert_mask(alert_mask)
		, m_queue_size_limit(queue_limit)
	{}

	alert_manager::~alert_manager() = default;

	alert* alert_manager::wait_for_alert(time_duration max_wait)
	{
		std::unique_lock<std::recursive_mutex> lock(m_mutex);

		if (!m_alerts[m_generation].empty())
			return m_alerts[m_generation].front();

		// this call can be interrupted prematurely by other signals
		m_condition.wait_for(lock, max_wait);
		if (!m_alerts[m_generation].empty())
			return m_alerts[m_generation].front();

		return nullptr;
	}

	void alert_manager::maybe_notify(alert* a)
	{
		if (m_alerts[m_generation].size() == 1)
		{
			// we just posted to an empty queue. If anyone is waiting for
			// alerts, we need to notify them. Also (potentially) call the
			// user supplied m_notify callback to let the client wake up its
			// message loop to poll for alerts.
			if (m_notify) m_notify();

			// TODO: 2 keep a count of the number of threads waiting. Only if it's
			// > 0 notify them
			m_condition.notify_all();
		}

#ifndef TORRENT_DISABLE_EXTENSIONS
		for (auto& e : m_ses_extensions)
			e->on_alert(a);
#else
		TORRENT_UNUSED(a);
#endif
	}

	void alert_manager::set_notify_function(std::function<void()> const& fun)
	{
		std::unique_lock<std::recursive_mutex> lock(m_mutex);
		m_notify = fun;
		if (!m_alerts[m_generation].empty())
		{
			if (m_notify) m_notify();
		}
	}

#ifndef TORRENT_DISABLE_EXTENSIONS
	void alert_manager::add_extension(std::shared_ptr<plugin> ext)
	{
		m_ses_extensions.push_back(std::move(ext));
	}
#endif

	/**
	 * 获取所有警告信息
	 * 
	 * 此函数将当前管理的所有警告信息的指针添加到提供的向量中，
	 * 它还处理警告信息下溢的情况，并在必要时重置相关数据结构
	 * 
	 * @param alerts 一个警告信息指针的向量，通过它返回当前所有的警告信息
	 */
	void alert_manager::get_all(std::vector<alert*>& alerts)
	{
	    // 锁定互斥量以保护共享资源
	    std::lock_guard<std::recursive_mutex> lock(m_mutex);
	
	    // 如果当前的警告信息为空，则清空输出向量并返回
	    if (m_alerts[m_generation].empty())
	    {
	        alerts.clear();
	        return;
	    }
	
	    // 检查是否有警告信息被丢弃，如果有则将 m_dropped 的值生成一个 alerts_dropped_alert 并 push 到列表中，
		// 然后重置 m_dropped
	    if (m_dropped.any()) {
	        emplace_alert<alerts_dropped_alert>(m_dropped);
	        m_dropped.reset();
	    }
	
	    // 获取当前所有警告信息的指针，并将它们添加到输出向量中
	    m_alerts[m_generation].get_pointers(alerts);
	
	    // 交换缓冲区，为下一轮收集做准备
	    m_generation = (m_generation + 1) & 1;

	    // 清空我们现在将开始写入的缓冲区
	    m_alerts[m_generation].clear();

		// 清空 m_allocations 对应 stack_allocator 中的字符串。
		// stack_allocator 是存放 m_alerts[m_generation] 队列中所有 alerts 需要的字符串。
	    m_allocations[m_generation].reset();
	}

	bool alert_manager::pending() const
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);
		return !m_alerts[m_generation].empty();
	}

	int alert_manager::set_alert_queue_size_limit(int queue_size_limit_)
	{
		std::lock_guard<std::recursive_mutex> lock(m_mutex);

		std::swap(m_queue_size_limit, queue_size_limit_);
		return queue_size_limit_;
	}
}
}
