/*

Copyright (c) 2016-2018, 2020, Alden Torres
Copyright (c) 2016, 2018, Steven Siloti
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

#include "libtorrent/aux_/disk_io_thread_pool.hpp"
#include "libtorrent/assert.hpp"

#include <algorithm>

namespace {

	constexpr std::chrono::seconds reap_idle_threads_interval(60);
}

namespace libtorrent {
namespace aux {

	disk_io_thread_pool::disk_io_thread_pool(pool_thread_interface& thread_iface
		, io_context& ios)
		: m_thread_iface(thread_iface)
		, m_max_threads(0)
		, m_threads_to_exit(0)
		, m_abort(false)
		, m_num_idle_threads(0)
		, m_min_idle_threads(0)
		, m_idle_timer(ios)
		, m_ioc(ios)
	{}

	disk_io_thread_pool::~disk_io_thread_pool()
	{
		abort(true);
	}

	void disk_io_thread_pool::set_max_threads(int const i)
	{
		std::lock_guard<std::mutex> l(m_mutex);
		if (i == m_max_threads) return;
		m_max_threads = i;
		if (int(m_threads.size()) < i) return;
		stop_threads(int(m_threads.size()) - i);
	}

	void disk_io_thread_pool::abort(bool wait)
	{
		std::unique_lock<std::mutex> l(m_mutex);
		if (m_abort) return;
		m_abort = true;
		m_idle_timer.cancel();
		stop_threads(int(m_threads.size()));
		for (auto& t : m_threads)
		{
			if (wait)
			{
				// must release m_mutex to avoid a deadlock if the thread
				// tries to acquire it
				l.unlock();
				t.join();
				l.lock();
			}
			else
				t.detach();
		}
		m_threads.clear();
	}

	void disk_io_thread_pool::thread_active()
	{
		int const num_idle_threads = --m_num_idle_threads;
		TORRENT_ASSERT(num_idle_threads >= 0);

		int current_min = m_min_idle_threads;
		while (num_idle_threads < current_min
			&& !m_min_idle_threads.compare_exchange_weak(current_min, num_idle_threads));
	}

	bool disk_io_thread_pool::try_thread_exit(std::thread::id id)
	{
		int to_exit = m_threads_to_exit;
		while (to_exit > 0 &&
			!m_threads_to_exit.compare_exchange_weak(to_exit, to_exit - 1));
		if (to_exit > 0)
		{
			std::unique_lock<std::mutex> l(m_mutex);
			if (!m_abort)
			{
				auto new_end = std::remove_if(m_threads.begin(), m_threads.end()
					, [id](std::thread& t)
				{
					if (t.get_id() == id)
					{
						t.detach();
						return true;
					}
					return false;
				});
				TORRENT_ASSERT(new_end != m_threads.end());
				m_threads.erase(new_end, m_threads.end());
				if (m_threads.empty()) m_idle_timer.cancel();
			}
		}
		return to_exit > 0;
	}

	std::thread::id disk_io_thread_pool::first_thread_id()
	{
		std::lock_guard<std::mutex> l(m_mutex);
		if (m_threads.empty()) return {};
		return m_threads.front().get_id();
	}

	/**
	 * 构造时仅初始化成员变量，不立即创建线程（空池设计），当有新任务加入时触发线程创建
	 * 
	 * libtorrent 中磁盘 I/O 线程池的动态扩缩容核心逻辑，
	 * 根据队列任务数 (queue_size) 动态调整线程池规模：
	 * - 缩减待退出线程数（避免过度收缩）
	 * - 扩容线程池（直到满足需求或达上限）
	 * - 启动空闲线程回收定时器（当首次创建线程时）
	 */
	void disk_io_thread_pool::job_queued(int const queue_size)
	{
		// this check is not strictly necessary
		// but do it to avoid acquiring the mutex in the trivial case
		// 这个检查并非绝对必要，但进行此检查是为了在简单情形下避免获取互斥锁。
		//
		// 当空闲线程足够处理新任务（queue_size）时立即返回，无需调整线程数
		if (m_num_idle_threads >= queue_size) return;

		std::lock_guard<std::mutex> l(m_mutex);

		if (m_abort) return;

		// reduce the number of threads requested to stop if we're going to need
		// them for these new jobs
		// 如果我们即将为这些新任务用到某些线程，那就减少请求停止的线程数量。
		//
		// 注：在多线程编程场景中，有时候系统可能会请求停止部分线程以释放资源，
		// 但如果有新的任务到来，并且这些新任务需要使用之前打算停止的线程来执行，那么就可以减少请求停止的线程数量。
		//
		// to_exit 期望退出的线程数
		int to_exit = m_threads_to_exit;

		/**
		 * 线程收缩控制
		 * 
		 * 实际需要退出的线程数 = max(0, 当前闲置线程_m_num_idle_threads - 待处理任务数_queue_size)
		 * 
		 * 这个 while 循环是典型的 CAS (Compare-And-Swap) 模式，用于在并发环境下安全地更新共享变量 m_threads_to_exit。
		 * 逻辑上相当于：
		 * if (to_exit > (m_num_idle_threads - queue_size) {
		 *     to_exit = m_num_idle_threads - queue_size;
		 * }
		 */
		while (to_exit > std::max(0, m_num_idle_threads - queue_size) // 如果，期望退出的线程数 > 实际需要退出的线程数，
			&&  !m_threads_to_exit.compare_exchange_weak(to_exit
				, std::max(0, m_num_idle_threads - queue_size)))
			// 如果 to_exit == m_threads_to_exit，则将 m_threads_to_exit 更新为 “实际需要退出的线程数”，并返回 true；
			// 否则不修改 m_threads_to_exit，但会将 to_exit 更新为 m_threads_to_exit 的当前实际值，并返回 false。
		;

		// 此处创建线程
		//
		// now start threads until we either have enough to service
		// all queued jobs without blocking or hit the max
		// 现在开始创建新线程，直至满足以下两个条件之一：
		// 1. i >= queue_size，有足够的线程来处理所有已排队的任务且不会造成阻塞；
		// 2. m_threads.size() >= m_max_threads，达到线程数量上限。
		//
		//  线程扩容逻辑
		for (int i = m_num_idle_threads
			; i < queue_size && int(m_threads.size()) < m_max_threads
			; ++i)
		{
			// if this is the first thread started, start the reaper timer
			// 如果这是启动的第一个线程，就启动定时清理器。
			if (m_threads.empty())
			{
				m_idle_timer.expires_after(reap_idle_threads_interval); // 设置一个 60 秒后允许且过期的定时器
				// m_idle_timer 到时后，会调用 reap_idle_threads() 清理空闲线程，并自动又定时 60 秒。
				m_idle_timer.async_wait([this](error_code const& ec) { reap_idle_threads(ec); });
			}

			// 创建 disk_io 工作线程，并加入到 m_threads。
			// 它会创建一个新的线程，并将 pool_thread_interface::thread_fun 作为线程的入口函数。
			// 这个新线程会使用 m_thread_iface 作为线程接口对象，*this 作为线程池的引用，
			// 并且使用 make_work_guard(m_ioc) 来确保 m_ioc 在线程运行期间保持有效。
			// 
			// work keeps the io_context::run() call blocked from returning.
			// When shutting down, it's possible that the event queue is drained
			// before the disk_io_thread has posted its last callback. When this
			// happens, the io_context will have a pending callback from the
			// disk_io_thread, but the event loop is not running. this means
			// that the event is destructed after the disk_io_thread. If the
			// event refers to a disk buffer it will try to free it, but the
			// buffer pool won't exist anymore, and crash. This prevents that.
			//
			// work 会使 io_context::run() 调用保持阻塞状态，避免其返回。
			// 在程序关闭时，有可能在磁盘 I/O 线程发布其最后一个回调之前，事件队列就已经被清空了。
			// 当这种情况发生时，io_context 会有一个来自磁盘 I/O 线程的待处理回调，但事件循环却未在运行。
			// 这意味着该事件会在磁盘 I/O 线程之后被销毁。
			// 如果该事件引用了一个磁盘缓冲区，它会尝试释放该缓冲区，但此时缓冲区池已不存在，从而导致程序崩溃。
			// 而 work 机制能防止这种情况发生。
			//
			// make_work_guard(m_ioc) 创建了 executor_work_guard 对象，确保所有有 work_guard 的线程完成前，
			// io_context::run() 事件循环不提前退出。
			//
			// 线程生成后，会立刻运行 job_queue (也就是 m_thread_iface) 对象的 thread_fun 函数。
			m_threads.emplace_back(&pool_thread_interface::thread_fun 	// 线程入口函数，这是线程启动后执行的核心函数
				, &m_thread_iface										// 线程接口对象，实现 pool_thread_interface 的实例
				, std::ref(*this)					  					// 线程池引用
				, make_work_guard(m_ioc));								// 关键：工作守卫
		}
	}

	/**
	 * 使用定时器（m_idle_timer）定期执行 reap_idle_threads，动态调整线程数量。
	 * 
	 * 空闲线程回收：
	 * - 如果空闲线程过多（min_idle > 0），则尝试回收部分线程。
	 * 
	 * 线程数上限控制：
	 * - 如果当前线程总数超过 m_max_threads，会强制停止多余的线程（即使它们不空闲）。
	 */
	void disk_io_thread_pool::reap_idle_threads(error_code const& ec)
	{
		// take the minimum number of idle threads during the last
		// sample period and request that many threads to exit
		if (ec) return;

		std::lock_guard<std::mutex> l(m_mutex);

		if (m_abort) return;
		if (m_threads.empty()) return;

		m_idle_timer.expires_after(reap_idle_threads_interval); 
		m_idle_timer.async_wait([this](error_code const& e) { reap_idle_threads(e); });

		// 获取最近采样周期内的最小空闲线程数（min_idle = m_min_idle_threads），
		// 并更新当前空闲线程数（m_min_idle_threads = m_num_idle_threads）。
		//
		// min_idle 的值为 m_min_idle_threads 旧值；
		// m_min_idle_threads 的值被更新为 m_num_idle_threads。
		//
		// 注意：在线程池中，会在线程变为 idle 时立刻维护 m_min_idle_threads 值，确保它时间是最近采样周期内的最小的空闲线程数。
		// 此处仅仅时将“最近采样周期内的最小的空闲线程数” 提取到 min_idle。
		int const min_idle = m_min_idle_threads.exchange(m_num_idle_threads);

		if (min_idle <= 0) return;

		// stop either the minimum number of idle threads or the number of threads
		// which must be stopped to get below the max, whichever is larger
		//
		// 计算需要停止的线程数：
		// 1. 至少停止 min_idle 个空闲线程
		// 2. 如果当前线程数超过最大限制（m_max_threads），则停止足够多的线程以使总数不超过限制
		// 最终取两者中的较大值
		int const to_stop = std::max(min_idle, int(m_threads.size()) - m_max_threads);

		 // 调用 stop_threads 方法，停止指定数量的线程
		stop_threads(to_stop);
	}
   
	void disk_io_thread_pool::stop_threads(int num_to_stop)
	{
		m_threads_to_exit = num_to_stop;
		m_thread_iface.notify_all();
	}

}
} // namespace libtorrent
