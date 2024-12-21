# Libtorrent tutorial

## session 及其他相关的概念

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

## alert masks (警报掩码)

这个程序的输出将会非常详细，这可能是一个很好的起点，有助于你了解正在发生什么。Alerts 被分为不同的 [alert](https://www.libtorrent.org/reference-Alerts.html#alert) 类别。每个类别都可以通过 *`alert mask` (警报掩码)* 独立地启用和禁用。

*`alert mask`* 是 libtorrent 提供的一个配置选项。有很多配置选项，具体可以参见 [settings_pack](https://www.libtorrent.org/reference-Settings.html#settings_pack) 。*`alert mask`* 设置的值是一个整数，由多个类别标志通过 OR 位运算合成。

例如，为了只查看最相关的警报，可以这样构建会话（ `session` ）：

```cpp
lt::settings_pack pack;
pack.set_int(lt::settings_pack::alert_mask
        , lt::alert_category::error
        | lt::alert_category::storage
        | lt::alert_category::status);

lt::session ses(pack);
```

虽然可以在 `session` 开始后通过调用 [apply_settings()](https://www.libtorrent.org/reference-Session.html#apply_settings()) 来更新配置选项，但最好还是在启动 `session` 之前设置一些配置，比如监听接口（ [listen_interfaces](https://www.libtorrent.org/reference-Settings.html#listen_interfaces) ），以避免竞态条件。如果你使用默认设置启动 `session` 然后立即更改它们，那么在更改生效之前，仍然会有一段空窗期程序使用的是默认设置。

更改设置可能会触发监听套接字的关闭和重新打开，以及发送 NAT-PMP 和 UPnP 更新。因此，通常建议将设置更新批量处理为单次调用 (译者注：就是将多个更新参数放在一起集中更新)。

## session destruction (会话销毁)

`session` 析构函数（ session destructor ）默认是阻塞的。在关闭时，需要与 tracker 联系以停止 torrent 和其他未完成的操作。关闭过程有时可能需要几秒钟的时间，这主要是因为有些 tracker 没有响应（导致超时），以及 DNS 服务器没有响应。当停滞时， DNS Lookup 特别难以中止。

> **译者注：** 在 BitTorrent 协议中，当你想要停止一个 torrent （即一个正在通过 BitTorrent 协议下载或上传的文件）时，你的 BitTorrent 客户端（比如 libtorrent ）需要与 tracker 服务器进行联系。
>
> Tracker 服务器是一个特殊的服务器， 它负责记录哪些计算机（ 或称为“peers” ）正在下载或上传同一个 torrent。当你想要停止下载或上传时 ，你的客户端会向 tracker 服务器发送一个消息，告诉它你不再参与这个 torrent 的下载或上传活动。这样， tracker 服务器就可以更新它的记录，并且不再将你的计算机作为这个 torrent 的一个有效下载或上传来源。此外， tracker 还会停止以下操作：
> 
> - 更新上传和下载信息‌： tracker 服务器会定期更新每个用户的下载和上传信息。当用户决定停止 torrent 时， tracker 会停止对这些信息的更新，确保其他用户不再从这个即将停止的 torrent 源获取数据。
> - 维护 peer 列表‌： tracker 服务器维护着一个当前参与共享的 peer （即正在下载或上传该 torrent 的用户）的列表。当用户停止 torrent 时， tracker 会从列表中移除该用户，确保其他用户不会尝试与这个已经停止的用户建立连接。
> - 协调 peer 之间的连接‌：在 BitTorrent 协议中， tracker 服务器帮助用户找到其他用户并建立连接。当用户停止 torrent 时， racker 会停止为这个用户协调与其他用户的连接，避免不必要的网络活动。
> - 处理未完成的请求‌：如果用户在停止 torrent 前向 tracker 发送了某些请求（比如获取更多 peer 的信息）， tracker 会处理这些请求，但在用户停止 torrent 后，会停止处理与该 torrent 相关的任何新请求。
>
> 与 tracker 联系以停止 torrent 的过程通常是在你关闭 BitTorrent 客户端或手动停止 torrent 时自动完成的。这是 BitTorrent 协议的一部分，它有助于确保 torrent 的下载和上传活动能够高效、有序地进行。

为了能够异步地开始销毁（ `session` ），可以调用 [session::abort()](https://www.libtorrent.org/reference-Session.html#abort()) 方法。

> **译者注：** 在 libtorrent 中， `session::abort()` 方法允许异步地停止和销毁 `session` 对象。调用 `abort()` 后， `session` 会立即开始关闭过程，但不会等待所有操作完成。这意味着在 `abort()` 被调用后， `session` 可能会继续处理一些未完成的操作（如与 tracker 的通信），但这些操作会在后台进行，不会阻塞您的程序。
>
> 当决定不再需要 `session` 对象，并且希望尽快释放资源时，可以使用 `abort()` 方法。然而，请注意，由于 `abort()` 是异步的，所以需要采取额外的措施来确保所有与 `session` 相关的资源都被正确释放。

这个调用返回一个 [session_proxy](https://www.libtorrent.org/reference-Session.html#session_proxy) 对象，它是一个句柄，用于在关闭 `session` 时保持 session 状态的活跃。它故意不提供任何 `session` 操作，因为它正在关闭。

> **译者注：** 当您决定关闭一个 `session` 时，可能需要一个机制来确保在关闭过程中 `session` 的状态仍然可以被访问或查询，但又不允许进行任何会修改 `session` 状态的操作。这就是 `session_proxy` 对象的作用。它是一个轻量级的句柄，允许您在关闭 session 的过程中只能以只读方式访问其状态。

拥有 `session_proxy` 对象之后， `session` 的析构函数将不会阻塞。但是， `session_proxy` 的析构函数会阻塞(以确保所有与 `session` 相关的资源都得到正确释放)。

这可以用于并行关闭多个 session 或应用程序的其他部分。

## asynchronous operations (异步操作)

基本上，在 `session` 或 `torrent_handle` 中任何返回值的成员函数，都是一个阻塞式同步函数。这意味着调用它们时，会向 libtorrent 主线程发送一条消息，并等待响应。这样的调用可能会很耗时，在需要避免卡顿的应用中（如用户界面线程），最好不要使用阻塞式调用。

在上面的例子中， `session::add_torrent()` 函数返回一个 `torrent_handle` ，因此它是一个阻塞式调用。为了提高效率， [async_add_torrent()](https://www.libtorrent.org/reference-Session.html#async_add_torrent()) 函数会向主线程发送一条消息来添加 `torrent` ，然后可以通过消息机制在一个 alert ( [add_torrent_alert](https://www.libtorrent.org/reference-Alerts.html#add_torrent_alert) ) 中获得返回的结果 `torrent_handle` 。当需要快速连续添加大量 `torrent` 时，这种方法特别有用，因为调用之间不会有停滞。

在上面的例子中，我们实际上并没有使用 `torrent_handle` 做任何事情，所以我们只需要将 `add_torrent()` 简单的替换成替换成对 `async_add_torrent()` 的调用。

## torrent_status_updates

要获取 `torrent` 的最新的状态，需要在 `session` 对象上调用 [post_torrent_updates()](https://www.libtorrent.org/reference-Session.html#post_torrent_updates()) 函数。这将使 libtorrent 发布一个 [state_update_alert](https://www.libtorrent.org/reference-Alerts.html#state_update_alert) 消息，它包含自上次调用 `post_torrent_updates()` 以来状态已更改的所有 `torrent` 的 [torrent_status](https://www.libtorrent.org/reference-Torrent_Status.html#torrent_status) 对象。

`state_update_alert` 的结构大致如下:

```cpp
struct state_update_alert : alert
{
  virtual std::string message() const;
  std::vector<torrent_status> status;
};
```

`status` 字段只包含自上次调用以来状态有更新的 `torrent` 的 `torrent_status` 。如果没有 `torrent` 更新其状态， `status` 字段可能为空。这个特性对于**提高可扩展性至关重要**。

> **译者注：** “提高可扩展性至关重要”的意思应该指的是，如果每个 torrent 的状态变化都立即触发处理，那么可能会给系统带来很大的负担。而 state_update_alert 的这个特性允许系统批量处理只在一段时间内状态发生变化的 torrent，从而减轻了系统的负担，提高了整体的可扩展性和性能。

可以通过查看 [torrent_status](https://www.libtorrent.org/reference-Torrent_Status.html#torrent_status) 对象中包含的更多信息，深入了解它的各个字段。其中，可能令人最感兴趣的字段包括 `total_payload_download` （总下载量）、 `total_payload_upload` （总上传量）、 `num_peers` （连接的 peers 对等体的数量）和 state（状态）。

## resuming torrents (恢复 torrent 下载)

由于 BitTorrent 是以随机顺序下载文件的部分内容，因此恢复部分下载并不简单。在恢复下载时，BitTorrent 引擎必须恢复正在下载的 `torrent` 的状态，特别是已经下载了文件的哪些部分。这主要有两种方法：

1. 从磁盘上读取下载文件的每一块 `piece` ，并将其与预期的哈希值进行比较 (**译者注：** 每一块 `piece` 的校验码都是在 `torrent` 的 `info` 字段中存放的, 下载文件中的数据转换 `hash` 和 `info` 字段中对应的 `piece hash` 进行对比，如果不一致，则说明没下载或下载的内容出错了)。
2. 将已下载文件部分（以及部分下载的文件块）的状态信息保存到磁盘上，并在恢复下载时重新加载这些信息。

如果添加的 `torrent` 没有提供恢复数据， libtorrent 将默认采用第 1 种方式。

要保存恢复的数据，请在 `torrent_handle` 对象上调用 [save_resume_data()](https://www.libtorrent.org/reference-Torrent_Handle.html#save_resume_data()) 函数。这将请求 libtorrent 生成恢复数据，并在一个 [save_resume_data_alert](https://www.libtorrent.org/reference-Alerts.html#save_resume_data_alert) 中将其返回。如果出于任何原因生成恢复数据失败，则会发布一个 [save_resume_data_failed_alert](https://www.libtorrent.org/reference-Alerts.html#save_resume_data_failed_alert) 。


每当调用 [save_resume_data()](https://www.libtorrent.org/reference-Torrent_Handle.html#save_resume_data()) 函数时，都会准确地发布一个 `alert` 消息。这在关闭包含多个 `torrent` 的 `session` 时是一个重要的特性，因为在恢复关闭着的 `session` 之前，必须处理每一个 `resume alert` (**译者注：** 意思是没有在关闭 `session` 时保存好每一个 `resume alert` 的数据 ，那么之后就无法恢复整个 `session` 的 `torrent` ) 。任何 torrent 都可能无法保存恢复数据，因此客户端需要记录尚未保存的 resume 文件的数量，这个数量在收到 [save_resume_data_alert](https://www.libtorrent.org/reference-Alerts.html#save_resume_data_alert) 或 [save_resume_data_failed_alert](https://www.libtorrent.org/reference-Alerts.html#save_resume_data_failed_alert) 时都会相应减少。

[save_resume_data_alert](https://www.libtorrent.org/reference-Alerts.html#save_resume_data_alert) 消息，看起来像这样：

```cpp
struct save_resume_data_alert : torrent_alert
{
        virtual std::string message() const;

        // the resume data
        add_torrent_params params;
};
```

`params` 字段是一个 [add_torrent_params](https://www.libtorrent.org/reference-Add_Torrent.html#add_torrent_params) 对象，它包含了将 `torrent` 加回 session 中时所需的所有状态。这个对象可以使用 [write_resume_data()](https://www.libtorrent.org/reference-Resume_Data.html#write_resume_data()) 或 [write_resume_data_buf()](https://www.libtorrent.org/reference-Resume_Data.html#write_resume_data_buf()) 方法进行序列化 (**译者注：**序列化就是对数据进行 bencoded 编码)，使**译者注：**对数据进行 bencoded 编码用 [read_resume_data()](https://www.libtorrent.org/reference-Resume_Data.html#read_resume_data()) 方法可以进行反序列化。

在例子基础上，更新了以下内容：

1. 不使用阻塞调用
2. 打印洪流状态更新，而不是原始日志
3. 保存和加载恢复文件

```cpp
#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>

#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>

namespace {

using clk = std::chrono::steady_clock;

// return the name of a torrent status enum
char const* state(lt::torrent_status::state_t s)
{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#endif
  switch(s) {
    case lt::torrent_status::checking_files: return "checking";
    case lt::torrent_status::downloading_metadata: return "dl metadata";
    case lt::torrent_status::downloading: return "downloading";
    case lt::torrent_status::finished: return "finished";
    case lt::torrent_status::seeding: return "seeding";
    case lt::torrent_status::checking_resume_data: return "checking resume";
    default: return "<>";
  }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

} // anonymous namespace

int main(int argc, char const* argv[]) try
{
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <magnet-url>" << std::endl;
    return 1;
  }

  lt::settings_pack pack;
  pack.set_int(lt::settings_pack::alert_mask
    , lt::alert_category::error
    | lt::alert_category::storage
    | lt::alert_category::status);

  lt::session ses(pack);
  clk::time_point last_save_resume = clk::now();

  // load resume data from disk and pass it in as we add the magnet link
  std::ifstream ifs(".resume_file", std::ios_base::binary);
  ifs.unsetf(std::ios_base::skipws);
  std::vector<char> buf{std::istream_iterator<char>(ifs)
    , std::istream_iterator<char>()};

  lt::add_torrent_params magnet = lt::parse_magnet_uri(argv[1]);
  if (buf.size()) {
    lt::add_torrent_params atp = lt::read_resume_data(buf);
    if (atp.info_hashes == magnet.info_hashes) magnet = std::move(atp);
  }
  magnet.save_path = "."; // save in current dir
  ses.async_add_torrent(std::move(magnet));

  // this is the handle we'll set once we get the notification of it being
  // added
  lt::torrent_handle h;

  // set when we're exiting
  bool done = false;
  for (;;) {
    std::vector<lt::alert*> alerts;
    ses.pop_alerts(&alerts);

    for (lt::alert const* a : alerts) {
      if (auto at = lt::alert_cast<lt::add_torrent_alert>(a)) {
        h = at->handle;
      }
      // if we receive the finished alert or an error, we're done
      if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
        h.save_resume_data(lt::torrent_handle::only_if_modified
          | lt::torrent_handle::save_info_dict);
        done = true;
      }
      if (lt::alert_cast<lt::torrent_error_alert>(a)) {
        std::cout << a->message() << std::endl;
        done = true;
        h.save_resume_data(lt::torrent_handle::only_if_modified
          | lt::torrent_handle::save_info_dict);
      }

      // when resume data is ready, save it
      if (auto rd = lt::alert_cast<lt::save_resume_data_alert>(a)) {
        std::ofstream of(".resume_file", std::ios_base::binary);
        of.unsetf(std::ios_base::skipws);
        auto const b = write_resume_data_buf(rd->params);
        of.write(b.data(), int(b.size()));
        if (done) goto done;
      }

      if (lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
        if (done) goto done;
      }

      if (auto st = lt::alert_cast<lt::state_update_alert>(a)) {
        if (st->status.empty()) continue;

        // we only have a single torrent, so we know which one
        // the status is for
        lt::torrent_status const& s = st->status[0];
        std::cout << '\r' << state(s.state) << ' '
          << (s.download_payload_rate / 1000) << " kB/s "
          << (s.total_done / 1000) << " kB ("
          << (s.progress_ppm / 10000) << "%) downloaded ("
          << s.num_peers << " peers)\x1b[K";
        std::cout.flush();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ask the session to post a state_update_alert, to update our
    // state output for the torrent
    ses.post_torrent_updates();

    // save resume data once every 30 seconds
    if (clk::now() - last_save_resume > std::chrono::seconds(30)) {
      h.save_resume_data(lt::torrent_handle::only_if_modified
        | lt::torrent_handle::save_info_dict);
      last_save_resume = clk::now();
    }
  }

done:
  std::cout << "\ndone, shutting down" << std::endl;
}
catch (std::exception& e)
{
  std::cerr << "Error: " << e.what() << std::endl;
}
```

## session state ( session 状态 )

在构建时， `session` 对象由 [session_params](https://www.libtorrent.org/reference-Session.html#session_params) 对象进行配置。 `session_params` 对象主要包含 session_settings 、 DHT 节点的状态（例如路由表）、 `session` 的 IP 过滤器以及要使用的磁盘 I/O 后端和 DHT 存储。

可以对 [session_params](https://www.libtorrent.org/reference-Session.html#session_params) 对象进行序列化和反序列化操作，以帮助其恢复上次运行时的状态。这样做对于启动 DHT 特别有帮助，可以使用上次运行时的 DHT 节点。

在销毁 `session` 对象之前，可以调用 [session::session_state()](https://www.libtorrent.org/reference-Session.html#session_state()) 方法来获取当前的状态，这个状态会以 [session_params](https://www.libtorrent.org/reference-Session.html#session_params) 对象的形式返回。

你可以调用 [write_session_params()](https://www.libtorrent.org/reference-Session.html#write_session_params()) 函数或者 [write_session_params_buf()](https://www.libtorrent.org/reference-Session.html#write_session_params_buf()) 函数，来将 `session` 状态分别序列化为一个 `bencoded` 的 [entry](https://www.libtorrent.org/reference-Bencoding.html#entry) 或者一个 flat buffer（std::vector<char>）中。

> **译者注：**
> --
> - `entry`: 是 libtorrent 内部用来描述 `bencode` 的值的数据结构，它有一个名为 `data` 的 `union` 类型的字段，可以存放 代表 `bencoded` 中不同类型的 `raw value`，比如 `std::string` 类型用来存放 `string` 值, `std::int64_t` 类型用来存放 `int` 值, `std::map` 类型用来存放 `dict` 值, `std::vector` 类型用来存放 `list` 值。
> - `flat buffer`: 可以翻译为扁平缓冲区，通常指的是一种平坦的、连续的内存区域，用于临时存储数据，它的数据是按照线性的、连续的字节序列来存储的，而不考虑数据的逻辑结构。在 libtorrent 的 `write_session_params_buf()` 中，会将 `entry` 转化为 `bencode` 的字符串，然后写入 `flat buffer` 中。

在启动时，构建会话对象之前，你可以从磁盘上加载缓冲区，并调用 [read_session_params()](https://www.libtorrent.org/reference-Session.html#read_session_params()) 函数来将其反序列化为 `session_params` 对象。在将这个对象传递给 session 构造函数之前，你有机会更新 settings_params 中的 settings 成员( [settings_pack](https://www.libtorrent.org/reference-Settings.html#settings_pack) 类型)，或者配置 disk_io_constructor (调用 disk_io_constructor(...) 方法 会生成一个 unique_ptr<disk_interface> 对象，来指定磁盘 I/O 的后端实现) 。

> **译者注：** `session_params` 中的 `disk_io_constructor` 成员属性，其实是一个函数对象，调用它会生成一个 **用来指定磁盘 I/O 的后端实现** 的 `unique_ptr<disk_interface>` 对象，。

再次更新下之前的例子，加入以下内容：

1. 将 [session_params](https://www.libtorrent.org/reference-Session.html#session_params) 加载并保存到文件 “`.session`” 中
2. 允许在接收到 SIGINT 信号时关闭程序

```cpp
#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <csignal>

#include <libtorrent/session.hpp>
#include <libtorrent/session_params.hpp>
#include <libtorrent/add_torrent_params.hpp>
#include <libtorrent/torrent_handle.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/error_code.hpp>
#include <libtorrent/magnet_uri.hpp>

namespace {

using clk = std::chrono::steady_clock;

// return the name of a torrent status enum
char const* state(lt::torrent_status::state_t s)
{
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcovered-switch-default"
#endif
  switch(s) {
    case lt::torrent_status::checking_files: return "checking";
    case lt::torrent_status::downloading_metadata: return "dl metadata";
    case lt::torrent_status::downloading: return "downloading";
    case lt::torrent_status::finished: return "finished";
    case lt::torrent_status::seeding: return "seeding";
    case lt::torrent_status::checking_resume_data: return "checking resume";
    default: return "<>";
  }
#ifdef __clang__
#pragma clang diagnostic pop
#endif
}

std::vector<char> load_file(char const* filename)
{
  std::ifstream ifs(filename, std::ios_base::binary);
  ifs.unsetf(std::ios_base::skipws);
  return {std::istream_iterator<char>(ifs), std::istream_iterator<char>()};
}

// set when we're exiting
std::atomic<bool> shut_down{false};

void sighandler(int) { shut_down = true; }

} // anonymous namespace

int main(int argc, char const* argv[]) try
{
  if (argc != 2) {
    std::cerr << "usage: " << argv[0] << " <magnet-url>" << std::endl;
    return 1;
  }

  // load session parameters
  auto session_params = load_file(".session");
  lt::session_params params = session_params.empty()
    ? lt::session_params() : lt::read_session_params(session_params);
  params.settings.set_int(lt::settings_pack::alert_mask
    , lt::alert_category::error
    | lt::alert_category::storage
    | lt::alert_category::status);

  lt::session ses(params);
  clk::time_point last_save_resume = clk::now();

  // load resume data from disk and pass it in as we add the magnet link
  auto buf = load_file(".resume_file");

  lt::add_torrent_params magnet = lt::parse_magnet_uri(argv[1]);
  if (buf.size()) {
    lt::add_torrent_params atp = lt::read_resume_data(buf);
    if (atp.info_hashes == magnet.info_hashes) magnet = std::move(atp);
  }
  magnet.save_path = "."; // save in current dir
  ses.async_add_torrent(std::move(magnet));

  // this is the handle we'll set once we get the notification of it being
  // added
  lt::torrent_handle h;

  std::signal(SIGINT, &sighandler);

  // set when we're exiting
  bool done = false;
  for (;;) {
    std::vector<lt::alert*> alerts;
    ses.pop_alerts(&alerts);

    if (shut_down) {
      shut_down = false;
      auto const handles = ses.get_torrents();
      if (handles.size() == 1) {
        handles[0].save_resume_data(lt::torrent_handle::only_if_modified
          | lt::torrent_handle::save_info_dict);
        done = true;
      }
    }

    for (lt::alert const* a : alerts) {
      if (auto at = lt::alert_cast<lt::add_torrent_alert>(a)) {
        h = at->handle;
      }
      // if we receive the finished alert or an error, we're done
      if (lt::alert_cast<lt::torrent_finished_alert>(a)) {
        h.save_resume_data(lt::torrent_handle::only_if_modified
          | lt::torrent_handle::save_info_dict);
        done = true;
      }
      if (lt::alert_cast<lt::torrent_error_alert>(a)) {
        std::cout << a->message() << std::endl;
        done = true;
        h.save_resume_data(lt::torrent_handle::only_if_modified
          | lt::torrent_handle::save_info_dict);
      }

      // when resume data is ready, save it
      if (auto rd = lt::alert_cast<lt::save_resume_data_alert>(a)) {
        std::ofstream of(".resume_file", std::ios_base::binary);
        of.unsetf(std::ios_base::skipws);
        auto const b = write_resume_data_buf(rd->params);
        of.write(b.data(), int(b.size()));
        if (done) goto done;
      }

      if (lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
        if (done) goto done;
      }

      if (auto st = lt::alert_cast<lt::state_update_alert>(a)) {
        if (st->status.empty()) continue;

        // we only have a single torrent, so we know which one
        // the status is for
        lt::torrent_status const& s = st->status[0];
        std::cout << '\r' << state(s.state) << ' '
          << (s.download_payload_rate / 1000) << " kB/s "
          << (s.total_done / 1000) << " kB ("
          << (s.progress_ppm / 10000) << "%) downloaded ("
          << s.num_peers << " peers)\x1b[K";
        std::cout.flush();
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ask the session to post a state_update_alert, to update our
    // state output for the torrent
    ses.post_torrent_updates();

    // save resume data once every 30 seconds
    if (clk::now() - last_save_resume > std::chrono::seconds(30)) {
      h.save_resume_data(lt::torrent_handle::only_if_modified
        | lt::torrent_handle::save_info_dict);
      last_save_resume = clk::now();
    }
  }

done:
  std::cout << "\nsaving session state" << std::endl;
  {
    std::ofstream of(".session", std::ios_base::binary);
    of.unsetf(std::ios_base::skipws);
    auto const b = write_session_params_buf(ses.session_state()
      , lt::save_state_flags_t::all());
    of.write(b.data(), int(b.size()));
  }

  std::cout << "\ndone, shutting down" << std::endl;
}
catch (std::exception& e)
{
  std::cerr << "Error: " << e.what() << std::endl;
}
```

## torrent files (种子文件)

要将种子文件添加到 `session` 中（与磁力链接相对），必须先将其加载到 [torrent_info](https://www.libtorrent.org/reference-Torrent_Info.html#torrent_info) 对象中。 `torrent_info` 对象可以通过文件名、缓冲区或 `bencoded` 结构来创建。当通过文件名添加时，会对文件大小进行合理性检查和限制，如果要添加任意大小的种子文件，请在构造函数外部加载该文件。 `torrent_info` 对象提供了在将 `.torrent` 文件添加到会话之前，查询该文件信息以及对其进行修改的机会。

## bencoding (bencoding 编码)

`Bencoding` （或称为 `bencoded` 结构）是 `BitTorrent` 默认使用的数据存储格式，应用于如 `.torrent` 文件、 `Tracker` 的 `announce` 和 `scrape` 响应以及一些线路协议扩展中。 libtorrent 提供了一个高效的框架，通过 [bdecode()](https://www.libtorrent.org/reference-Bdecoding.html#bdecode()) 函数来解码 bencoded 数据。

编码和解码分别有两种独立的机制。解码时，使用 `bdecode()` 函数，该函数返回一个 [bdecode_node](https://www.libtorrent.org/reference-Bdecoding.html#bdecode_node) 。编码时，则使用接受一个 [entry](https://www.libtorrent.org/reference-Bencoding.html#entry) 对象的 `bencode()` 函数。 

`bdecode()` 的一个关键特性是，它不会从解析的缓冲区中复制任何数据。相反，它构建了一个指向缓冲区内部的引用树结构。只要 `bdecode_node` 在使用中，缓冲区就必须保持活跃和有效。有关 `bdecode()` 的性能详细信息，请参阅相关的 blog 文章 ([bdecode parsers](https://blog.libtorrent.org/2015/03/bdecode-parsers/))。