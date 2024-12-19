# Libtorrent tutorial

在 libtorrent 中，启动和下载 `torrent` 的基本功能是通过创建一个 `session` (会话) 来实现的， `session` 为 `torrent` 提供了上下文和一个容器。这是通过 [session](https://www.libtorrent.org/reference-Session.html#session) 类来完成的，尽管其大部分接口都在 [session_handle](https://www.libtorrent.org/reference-Session.html#session_handle) 下有文档说明 (译者注: session 继承自 session_handle ) 。

要向 session 中添加一个 torrent，你需要填充一个 [add_torrent_params](https://www.libtorrent.org/reference-Add_Torrent.html#add_torrent_params) 对象，并将其传递给 [add_torrent()](https://www.libtorrent.org/reference-Session.html#add_torrent()) 或 [async_add_torrent()](https://www.libtorrent.org/reference-Session.html#async_add_torrent()) 方法。

> **译者注：** 在 libtorrent `中，add_torrent_params` 是一个结构体或类，用于指定要添加的 `torrent` 的各种参数，比如 `torrent` 文件的路径、保存位置、下载模式等。通过填充这个对象，你可以告诉 libtorrent 你想要如何下载这个 `torrent` 。
> 
> 然后，你可以将这个填充好的 `add_torrent_params` 对象作为参数传递给 `add_torrent()` 或 `async_add_torrent()` 方法。`add_torrent()` 方法会同步地添加 `torrent` 到 `session` 中，并立即返回结果；而 `async_add_torrent()` 方法则是异步的，它会在后台添加 `torrent` ，并通过某种方式（比如回调或 `alert` ）通知调用方添加的结果。

`add_torrent()` 是一个阻塞调用，它会返回一个 [torrent_handle](https://www.libtorrent.org/reference-Torrent_Handle.html#torrent_handle) 。

> **译者注：** 当你调用 `add_torrent()` 方法时，程序会等待这个 `torrent` 被成功添加到 `session` 中，然后才会继续执行后面的代码。这个过程是阻塞的，意味着在 `torrent` 被添加完成之前，程序不会继续往下运行。
> 
> 一旦 `torrent` 被成功添加，`add_torrent()` 方法会返回一个 `torrent_handle` 对象。这个对象是对新添加的 `torrent` 的一个引用句柄，你可以通过它来访问 `torrent` 的各种属性和方法，比如获取下载进度、暂停或恢复下载等。

举个例子：

```cpp
#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/magnet_uri.hpp>

int main(int argc, char const* argv[])
{
        if (argc != 2) {
                fprintf(stderr, "usage: %s <magnet-url>\n");
                return 1;
        }
        lt::session ses;

        lt::add_torrent_params atp = lt::parse_magnet_uri(argv[1]);
        atp.save_path = "."; // save in current dir
        lt::torrent_handle h = ses.add_torrent(atp);

        // ...
}
```

一旦你有了 [torrent_handle](https://www.libtorrent.org/reference-Torrent_Handle.html#torrent_handle) ，你就可以影响这个 `torrent` （比如控制下载、上传等）以及查询它的状态。首先，让我们扩展这个示例，来打印出 bittorrent 引擎内部产生的与进度和事件相关的消息。libtorrent 有一个机制，被称为 `alerts` （警报），用于向客户端应用程序反馈信息。

客户端可以通过调用 [pop_alerts()](https://www.libtorrent.org/reference-Session.html#pop_alerts()) 来轮询 [session](https://www.libtorrent.org/reference-Session.html#session) 以获取新的 `alert` 。此函数会用一个指向 [alert](https://www.libtorrent.org/reference-Alerts.html#alert) 指针的列表 (vec )填充自上次调用此函数以来产生的所有新的 `alert` 。这些指针由 `session` 对象所拥有，并且在下一次调用 [pop_alerts()](https://www.libtorrent.org/reference-Session.html#pop_alerts()) 时将失效。

这些 alerts 形成了一个类层次结构，其中 [alert](https://www.libtorrent.org/reference-Alerts.html#alert) 是根类。每种特定的 `alert` 都可能包含针对其消息类型而特有的附加状态。所有警报都实现了一个 [message()](https://www.libtorrent.org/reference-Alerts.html#message()) 函数，该函数会打印出 [alert](https://www.libtorrent.org/reference-Alerts.html#alert) 消息的相关信息。这对于简单地用日志记录事件来说非常方便。

> **译者注：** `alert` 结构的子类包括 `torrent_alert` 、 `piece_finished_alert` 、 `peer_connect_alert` 等等，这些子类都是在 `alert_type.hpp` 文件中定义的。

为了能程序化地响应某些事件，可以使用 [alert_cast](https://www.libtorrent.org/reference-Alerts.html#alert_cast()) 尝试将警报对象向下转换为更具体的类型 (译者注：向下转型后我们可以获取特定的 `alert` 子类中特有的成员数据)。

为了能打印出 libtorrent 中的事件以及能在 `torrent` 下载完成时退出，我们可以定期轮询 `session` 以获取 alerts 并打印它们，并同时监听 [torrent_finished_alert](https://www.libtorrent.org/reference-Alerts.html#torrent_finished_alert) ，该警报会在一个 `torrent` 下载完成时发布。

```cpp
#include <iostream>
#include <thread>
#include <chrono>

#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>

int main(int argc, char const* argv[]) try
{
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <magnet-url>" << std::endl;
    return 1;
  }
  lt::settings_pack p;
  p.set_int(lt::settings_pack::alert_mask, lt::alert_category::status
    | lt::alert_category::error);
  lt::session ses(p);

  lt::add_torrent_params atp = lt::parse_magnet_uri(argv[1]);
  atp.save_path = "."; // save in current dir
  lt::torrent_handle h = ses.add_torrent(std::move(atp));

  for (;;) {
    std::vector<lt::alert*> alerts;
    ses.pop_alerts(&alerts);

    for (lt::alert const* a : alerts) {
      std::cout << a->message() << std::endl;
      // if we receive the finished alert or an error, we're done
      if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
        goto done;
      }
      if (lt::alert_cast<lt::torrent_error_alert>(a)) {
        goto done;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  done:
  std::cout << "done, shutting down" << std::endl;
}
catch (std::exception& e)
{
  std::cerr << "Error: " << e.what() << std::endl;
}
```