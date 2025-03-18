/*

Copyright (c) 2003-2013, Daniel Wallin
Copyright (c) 2013, 2015-2020, Arvid Norberg
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

#ifndef TORRENT_ALERT_MANAGER_HPP_INCLUDED
#define TORRENT_ALERT_MANAGER_HPP_INCLUDED

#include "libtorrent/config.hpp"
#include "libtorrent/alert.hpp"
#include "libtorrent/aux_/heterogeneous_queue.hpp"
#include "libtorrent/stack_allocator.hpp"
#include "libtorrent/alert_types.hpp" // for abi_alert_count
#include "libtorrent/aux_/array.hpp"

#include <functional>
#include <utility> // for std::forward
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <bitset>

#ifndef TORRENT_DISABLE_EXTENSIONS
#include "libtorrent/extensions.hpp"
#include <memory> // for shared_ptr
#include <list>
#endif

namespace libtorrent {
namespace aux {

	struct TORRENT_EXTRA_EXPORT alert_manager
	{
		explicit alert_manager(int queue_limit
			, alert_category_t alert_mask = alert_category::error);

		alert_manager(alert_manager const&) = delete;
		alert_manager& operator=(alert_manager const&) = delete;

		~alert_manager();

		template <class T, typename... Args>
		void emplace_alert(Args&&... args) try
		{
			std::unique_lock<std::recursive_mutex> lock(m_mutex);

			// 获取当前生成的警报队列的引用
			heterogeneous_queue<alert>& queue = m_alerts[m_generation];

			// don't add more than this number of alerts, unless it's a
			// high priority alert, in which case we try harder to deliver it
			// for high priority alerts, double the upper limit
			//
			// 不要添加超过这个数量的警报，除非是
			// 高优先级警报，在这种情况下，我们会更努力地传递它
			// 对于高优先级警报，上限加倍
			//
			// 优先级越高，分母越大，最后值越小。
			// 这意味着，对于高优先级的警报，队列可以容纳更多的警报而不触发上限检查。
			if (queue.size() / (1 + static_cast<int>(T::priority)) >= m_queue_size_limit)
			{
				// record that we dropped an alert of this type
				m_dropped.set(T::alert_type);
				return;
			}
			
			// 使用提供的参数将新的警报对象放置在队列中
			// m_allocations[m_generation] => heterogeneous_queue , 就是说 m_allocations 这个 array 对象里有 2 个 heterogeneous_queue。
			T& alert = queue.emplace_back<T>(
				m_allocations[m_generation], std::forward<Args>(args)...);

			// 通知任何等待的线程有新警报已添加
			maybe_notify(&alert);
		}
		catch (std::bad_alloc const&)
		{
			// record that we dropped an alert of this type
			std::unique_lock<std::recursive_mutex> lock(m_mutex);
			m_dropped.set(T::alert_type);
		}

		bool pending() const;
		void get_all(std::vector<alert*>& alerts);

		template <class T>
		bool should_post() const
		{
			return bool(m_alert_mask.load(std::memory_order_relaxed) & T::static_category);
		}

		alert* wait_for_alert(time_duration max_wait);

		void set_alert_mask(alert_category_t const m) noexcept
		{
			m_alert_mask = m;
		}

		alert_category_t alert_mask() const noexcept
		{
			return m_alert_mask;
		}

		int alert_queue_size_limit() const noexcept { return m_queue_size_limit; }
		int set_alert_queue_size_limit(int queue_size_limit_);

		void set_notify_function(std::function<void()> const& fun);

#ifndef TORRENT_DISABLE_EXTENSIONS
		void add_extension(std::shared_ptr<plugin> ext);
#endif

	private:

		void maybe_notify(alert* a);

		// this mutex protects everything. Since it's held while executing user
		// callbacks (the notify function and extension on_alert()) it must be
		// recursive to support recursively post new alerts.
		//
		// 这个互斥锁保护着所有内容。
		// 由于在执行用户回调函数（notify 函数和 extension 的 on_alert() 函数）时会持有该互斥锁，
		// 所以它必须是可递归的，以支持递归地发布新警报。
		//
		// std::recursive_mutex 允许同一个线程多次锁定同一个互斥锁，而不会导致死锁。这在递归函数或嵌套调用中非常有用。
		mutable std::recursive_mutex m_mutex;

		// 条件变量
		std::condition_variable_any m_condition;

		std::atomic<alert_category_t> m_alert_mask;

		int m_queue_size_limit;

		// a bitfield where each bit represents an alert type. Every time we drop
		// an alert (because the queue is full or of some other error) we set the
		// corresponding bit in this mask, to communicate to the client that it
		// may have missed an update.
		//
		// 这是一个 bitfield (位域)，其中每一位代表一种 alert type (警报类型)。
		// 每当我们 drop 一个 alert 时（可能是因为队列已满或出现其他错误），
		// 我们会在这个掩码中设置相应的 bit 位，以此向客户端传达它可能错过了一次更新的信息。
		std::bitset<abi_alert_count> m_dropped;

		// this function (if set) is called whenever the number of alerts in
		// the alert queue goes from 0 to 1. The client is expected to wake up
		// its main message loop for it to poll for alerts (using get_alerts()).
		// That call will drain every alert in one atomic operation and this
		// notification function will be called again the next time an alert is
		// posted to the queue
		//
		// 每当警报队列中的警报数量从 0 变为 1 时，就会调用此函数（如果已设置）。
		// 这是期望客户端唤醒其主消息循环，以便对警报进行轮询（使用 get_alerts() 函数）。
		// get_alerts() 会清空当前 m_alerts 队列中的所有警报，并且当下一次有警报被添加到这个队列时，m_notify 函数会再次被调用。
		//
		// 为什么需要 m_notify？
		// libtorrent 是一个异步库，它会在后台生成警报（例如下载进度更新、错误通知等）。
		// 客户端通常有一个主消息循环（main message loop），用于处理事件和更新 UI。
		// 如果没有 m_notify，客户端需要不断地轮询 alert_manager 来检查是否有新的警报，这会浪费 CPU 资源。
		// 通过 m_notify，客户端可以在有新的警报时被立即通知，从而高效地处理警报。
		std::function<void()> m_notify;

		// this is either 0 or 1, it indicates which m_alerts and m_allocations
		// the alert_manager is allowed to use right now. This is swapped when
		// the client calls get_all(), at which point all of the alert objects
		// passed to the client will be owned by libtorrent again, and reset.
		//
		// 该值为 0 或 1，分别表示 alert_manager 允许使用的 m_alerts[2] 和 m_allocations[2] 数组的当前 idx。
		// 当客户端调用 get_all() 时:
		// 1. m_generation 会在 0 和 1 间进行切换。
		// 2. 当前缓冲区中的 alerts 会传递给客户端。
		// 3. 重置之前的缓冲区，以便 libtorrent 可以重新使用它生成新的警报。
		// 
		// 所以，
		// m_alerts[m_generation] 是 libbtorrent 内部正在 push alert 缓冲区。
		// m_alerts[1 - m_generation] 是 客户端 正在读的 alert 缓冲区。
		int m_generation = 0;

		// this is where all alerts are queued up. There are two heterogeneous
		// queues to double buffer the thread access. The std::mutex in the alert
		// manager gives exclusive access to m_alerts[m_generation] and
		// m_allocations[m_generation] whereas the other copy is exclusively
		// used by the client thread.
		//
		// 所有警报都在此处排队。为了实现线程访问的双重缓冲，使用了两个异构队列。
		// 警报管理器中的 std::mutex 为 m_alerts[m_generation] 和 m_allocations[m_generation] 提供独占访问权限，
		// 而另一个副本则由客户端线程独占使用。
		// 展开后是 container_wrapper<heterogeneous_queue<alert>, IndexType, [heterogeneous_queue<alert>; 2]>
		// 也就是说 container_wrapper 继承了 std::array<heterogeneous_queue<alert>, 2>
		//
		// 相当于声明了 m_alerts[heterogeneous_queue<alert>; 2]
		aux::array<heterogeneous_queue<alert>, 2> m_alerts;

		// this is a stack where alerts can allocate variable length content,
		// such as strings, to go with the alerts.
		// 这是一个栈，警报可以利用它来为可变长度的内容（比如字符串）分配空间，这些内容会与 alerts 关联在一起。
		aux::array<stack_allocator, 2> m_allocations;

#ifndef TORRENT_DISABLE_EXTENSIONS
		std::list<std::shared_ptr<plugin>> m_ses_extensions;
#endif
	};
}
}

#endif
