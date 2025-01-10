# Manual Reference

## Overview

libtorrent 的接口由几个类组成。主要的类是 [session](https://www.libtorrent.org/reference-Session.html#session) ，它包含了为所有 torrent 提供服务的主循环。

基本用法如下：

- 构建一个 `session` ，可以选择传入之前 `session` 的状态。使用 [read_session_params()](https://www.libtorrent.org/reference-Session.html#read_session_params()) 解码，
并将得到的 [session_params](https://www.libtorrent.org/reference-Session.html#session_params) 对象传递给 `session` 构造函数。

- 启动扩展（参见 [add_extension()](https://www.libtorrent.org/reference-Torrent_Handle.html#add_extension())）。

- 启动 `DHT`、`LSD`、`UPnP`、`NAT-PMP` 等功能（参见 `start_dht()`、`start_lsd()`、`start_upnp()` 和 `start_natpmp()`）。

- 解析 `.torrent` 文件并将其添加到 `session 中`（参见 [torrent_info](https://www.libtorrent.org/reference-Torrent_Info.html#torrent_info)、
[async_add_torrent()](https://www.libtorrent.org/reference-Session.html#async_add_torrent()) 和 
[add_torrent()](https://www.libtorrent.org/reference-Session.html#add_torrent())）。

- 运行主循环（参见 [session](https://www.libtorrent.org/reference-Session.html#session) ）。
    - 轮询 alerts 消息（参见 [wait_for_alert()](https://www.libtorrent.org/reference-Session.html#wait_for_alert()) 和 [pop_alerts()](https://www.libtorrent.org/reference-Session.html#pop_alerts())）。
    - 更新 torrent 的状态（参见 [state_update_alert](https://www.libtorrent.org/reference-Alerts.html#state_update_alert)）。
    - 处理其他 alerts 消息（参见 [alert](https://www.libtorrent.org/reference-Alerts.html#alert)）。
    - 查询 [session](https://www.libtorrent.org/reference-Session.html#session) 信息（参见 `session::status()`）。
    - 添加或移除 [session](https://www.libtorrent.org/reference-Session.html#session) 中的 torrent（参见 [remove_torrent()](https://www.libtorrent.org/reference-Custom_Storage.html#remove_torrent())）。

- 为所有 torrent_handle 保存恢复数据（可选，参见 [save_resume_data()](https://www.libtorrent.org/reference-Torrent_Handle.html#save_resume_data())）。

- 保存 `session` 状态（参见 [session_state()](https://www.libtorrent.org/reference-Session.html#session_state()) 和 [write_session_params()](https://www.libtorrent.org/reference-Session.html#write_session_params())）。

- 销毁 `session` 对象。

本手册详细描述了每个类和函数，您也可以查看 [tutorial](https://www.libtorrent.org/tutorial-ref.html) 以获取更多信息。

关于如何创建 torrent 文件的说明，请参阅 [create_torrent](https://www.libtorrent.org/reference-Create_Torrents.html#create_torrent)。

## forward declarations（前置声明）

不建议对 libtorrent 命名空间中的类型进行前置声明，因为这可能会在未来的版本中导致问题。相反，应该包含 `libtorrent/fwd.hpp` 以获取 libtorrent 中所有公共类型的前置声明。

## trouble shooting（问题排除）

开发人员面临的一个常见问题是 torrents 会无故停止。以下描述了 libtorrent 在哪些条件下会停止你的 torrents、如何发现这一问题以及如何解决。

请务必跟踪 torrents 的暂停状态、错误状态和上传模式。默认情况下，torrents 是自动管理的，这意味着 libtorrent 会自动暂停、恢复、清理它们，并将它们从上传模式中移除。

每当一个 torrent 遇到致命错误时，它将被停止，`torrent_status::error` 会描述导致停止的错误。如果 torrent 是自动管理的，它会定期被清理，
并根据每个 seed (种子, 指拥有完整文件的上传者‌‌) 对应的下载者数量来决定暂停或恢复。这将有效地为最需要种子的 torrent 提供种子。

如果一个 torrent 遇到磁盘写入错误，它将被设置为上传模式。这意味着它将不会下载任何内容，而只会进行上传。
假设写入错误是由磁盘已满或写入权限错误引起的：
如果 torrent 文件是自动管理的，它会定期退出上传模式，尝试再次将数据写入磁盘，（如果成功）这意味着如果问题得到解决，torrent 文件可以从某些磁盘错误中恢复；
如果 torrent 文件不是自动管理的，你需要调用 `set_upload_mode()` 来重新开启下载。

有关如何排查性能问题的更详细指南，请参阅 [故障排除](https://www.libtorrent.org/troubleshooting.html) 部分。

## ABI considerations（ABI 注意事项）

libtorrent 在主要版本（major）和次要版本（minor）相同的情况下会保持稳定的 ABI（应用程序二进制接口）。

例如，libtorrent-1.2.0 与 libtorrent-1.2.1 是 ABI 兼容的，但与 libtorrent-1.1 不兼容。

## network primitives（网络原语，网络基本操作）

在 libtorrent 命名空间中，有一些 typedef（类型别名）从 boost::asio 命名空间中引入了网络类型。这些类型别名包括：

```cpp
using address = boost::asio::ip::address;
using address_v4 = boost::asio::ip::address_v4;
using address_v6 = boost::asio::ip::address_v6;
using boost::asio::ip::tcp;
using boost::asio::ip::udp;
```

它们是在 `<libtorrent/socket.hpp>` 头文件里定义的。

`using` 语句将提供对以下内容的便捷访问：

```cpp
tcp::endpoint
udp::endpoint
```

这些是 libtorrent 中使用的端点类型。端点（endpoint） 是一个带有关联端口号的地址。

关于这些类型的文档，请参考 [asio 文档](https://www.boost.org/doc/libs/1_66_0/doc/html/boost_asio.html)。

## exceptions (异常)

在 libtorrent 中，许多函数都有两个版本：

- 一个版本在出错时抛出异常。

- 另一个版本接受一个 `error_code` 引用，并在出错时填充错误代码。

对于抛出异常的情况，libtorrent 会抛出 `boost::system::system_error` 异常，该异常携带一个描述底层错误的 `error_code`。


### translating error codes (翻译错误代码)

对于系统错误（即属于通用类别或系统类别的错误），`error_code::message()` 函数通常会返回本地化的错误字符串。

然而，属于 libtorrent 错误类别的错误不会本地化，它们仅提供英文描述。为了翻译 libtorrent 错误，
可以将 `error_code` 对象的错误类别与 `lt::libtorrent_category()` 进行比较，如果匹配，则说明错误代码属于上述列表中的错误。
你可以提供自己的从错误代码到字符串的映射，并实现本地化。在这种情况下，你不能依赖 `error_code::message()` 来生成字符串。

错误代码的数值是 API 的一部分，并且会保持不变，尽管可能会在末尾添加新的错误代码。

以下是一个如何翻译错误代码的简单示例：

```cpp
std::string error_code_to_string(boost::system::error_code const& ec)
{
        if (ec.category() != lt::libtorrent_category())
        {
                return ec.message();
        }
        // the error is a libtorrent error

        int code = ec.value();
        static const char const* swedish[] =
        {
                "inget fel",
                "en fil i torrenten kolliderar med en fil fran en annan torrent",
                "hash check misslyckades",
                "torrentfilen ar inte en dictionary",
                "'info'-nyckeln saknas eller ar korrupt i torrentfilen",
                "'info'-faltet ar inte en dictionary",
                "'piece length' faltet saknas eller ar korrupt i torrentfilen",
                "torrentfilen saknar namnfaltet",
                "ogiltigt namn i torrentfilen (kan vara en attack)",
                // ... more strings here
        };

        // use the default error string in case we don't have it
        // in our translated list
        if (code < 0 || code >= sizeof(swedish)/sizeof(swedish[0]))
                return ec.message();

        return swedish[code];
}
```

## magnet links (磁力链)

磁力链接是包含 `info-hash`、显示名称以及可选的 `tracker url` 的 `URI`。`magnet link` 的理念是，终端用户可以在浏览器中点击链接，
然后由 bittorrent 应用程序进行下载，而无需任何.torrent文件。

`magnet URI` 的格式如下：

```
magnet:?xt=urn:btih: Base16 encoded info-hash [ &dn= name of download ] [ &tr= tracker URL ]*
```

为了通过 `magnet link` 仅下载 `.torrent` 文件中的 `metadata`（元数据），在添加之前，
应在 [add_torrent_params](https://www.libtorrent.org/reference-Add_Torrent.html#add_torrent_params) 中设置 `torrent_flags::upload_mode` 标志。

在这种情况下，当从 `swarm`（下载群）中接收到元数据时，`torrent` 任务仍将继续运行，
但它会断开与大多数 `peer`s（对等节点）的连接（因为与已经拥有 `metadata` 的 `peers` 保持连接是多余的）。它将仅保留对 `metadata` 的 seeding（播种）。

需要注意的是，如果 `torrent` 中包含文件，那就将会导致空文件的创建。
若需避免此情况，可以将 `file_priority` 设置为一系列 0（因为文件数量事先未知），或者将 `save_path` 设置为一个无效路径。

### .torrent file

要从通过 `magnet link`（或以任何方式）添加的 `torrent` 中保存一个 `.torrent` 文件，请按照以下步骤操作：

- 在 [torrent_handle](https://www.libtorrent.org/reference-Torrent_Handle.html#save_resume_data()) 上调用 [save_resume_data()](https://www.libtorrent.org/reference-Torrent_Handle.html#torrent_handle) ，并确保传入 `save_info_dict` 标志。
- 等待 `resume_data_alert` 。
- 调用 [write_torrent_file()](https://www.libtorrent.org/reference-Resume_Data.html#write_torrent_file()) ，并传入从 [alert](https://www.libtorrent.org/reference-Alerts.html#alert) 中获取的 [add_torrent_params](https://www.libtorrent.org/reference-Add_Torrent.html#add_torrent_params
续传数据格式与.torrent文件格式非常相似，当在续传数据中包含信息字典（info-dict）时，续传文件可以作为一个.torrent文件使用（仅有一些细微的例外）。) 对象。

续传数据格式(`resume data format`)与 `.torrent` 文件格式非常相似，当在续传数据中包含 `info-dict` 时，续传文件可以作为一个 `.torrent` 文件使用（仅有一些细微的例外）。
flat
## BitTorrent v2 torrents (BitTorrent v2 种子)

BitTorrent v2 引入了许多新特性，这些特性在此 [博客文章](https://blog.libtorrent.org/2020/09/bittorrent-v2/) 
以及 [BEP 52](https://www.bittorrent.org/beps/bep_0052.html) 中有所概述。
v2 协议引入了使用 `merkle hash tree`（默克尔哈希树）替代扁平的 `piece hashes list` (分片哈希列表)的可能性。
它还支持 `hybrid torrents`（混合种子），这些 `torrents` 既是有效的经典种子（v1），也是有效的 v2 种子。
混合种子同时包含扁平的 `piece hashes list` 和 `merkle hash tree`。

这引入了一些新的错误情况。混合种子的 v1 和 v2 哈希可能会不匹配。由于 v2 种子使用 `SHA-256` 而 v1 使用 `SHA-1`，而哈希不匹配的问题只有在分片下载完成后才能被检测到。
这将导致 `torrent_inconsistent_hashes` 错误。

磁力链可能仅包含 v1 的 `info-hash` 或 v2 的 `info-hash`。如果两个独立的磁力链，一个是仅 v1 的，另一个是仅 v2 的，最终解析为同一个混合种子时，
这两种 [torrent_handle](https://www.libtorrent.org/reference-Torrent_Handle.html#torrent_handle) 对象都会将被置于 `duplicate_torrent` 错误状态。
在这种状态下，必须移除其中一个，另一个可以恢复，以便重新下载 `metadata` 。

当两个 `torrent` 发生冲突时，会发布 [torrent_conflict_alert](https://www.libtorrent.org/reference-Alerts.html#torrent_conflict_alert)。
该 alert 继承自 [torrent_alert](https://www.libtorrent.org/reference-Alerts.html#torrent_alert)，因此与一个 `torrent_handle` 相关联。
它包含第二个 `torrent_handle`，指向冲突中的另一个种子，以及已下载的元数据。
解决冲突的一种方法是移除这两个种子，并使用 `torrent_conflict_alert` 中提供的 `metadata` 重新添加。

当 v1-only 或 v2-only 的磁力链解析后发现其实是 `hybird torrent` 时，与该种子关联的 [info_hash_t](https://www.libtorrent.org/reference-Core.html#info_hash_t) 对象将被更新，以同时包含 v1 和 v2 的信息哈希。
这适用于 [torrent_handle::info_hashes()](https://www.libtorrent.org/reference-Torrent_Handle.html#info_hashes()) 和 [torrent_info::info_hashes()](https://www.libtorrent.org/reference-Torrent_Info.html#info_hashes()) 。

> *译注：*
> 举个例子，你有一个 v1-only 磁力链，只包含 v1 的 info-hash（SHA-1）。
当你开始下载时，发现这个 v1 torrent 实际上是一个 hybird torrent（既支持 v1 也支持 v2）。
客户端会更新这个原 v1 torrent 的 info_hash_t 对象，使其同时包含 v1 和 v2 的 info-hash。
同样的情况也适用于 v2-only 磁力链。

## queuing（队列管理）

libtorrent 支持队列管理。队列管理是一种基于特定条件自动暂停和恢复种子的机制。这些条件取决于种子的整体状态（检查中、下载中或做种中）。

要选择退出队列管理逻辑，请确保在添加种子时，`add_torrent_params::flags` 中清除了 `torrent_flags::auto_managed` 标志位。
或者在种子句柄上调用 [torrent_handle::unset_flags()](https://www.libtorrent.org/reference-Torrent_Handle.html#unset_flags()) 并传入 `torrent_flags::auto_managed`。

队列管理逻辑的总体目的是在任意 `torrent` 下载和上传负载下提高性能。
例如，如果你想在有限的家用网络连接上下载 100 个 `torrent`，通过一次下载一个（或可能两个）而不是同时下载所有 `torrent`，可以提高性能。
其好处包括：

- `torrent` 平均完成时间：比同时下载所有 `torrent` 的情况减少一半。
- 上传容量利用率：更有可能达到对等节点（peers）的互惠速率，并可能提高你的投资回报率（下载与上传的比例）。
- 磁盘 I/O 负载：更可能集中在局部，这可能会提高 I/O 性能并减少碎片化。

从根本上说，队列管理分为三个独立的队列：

- checking torrents (检查中的种子队列)
- downloading torrents (下载中的种子队列)
- seeding torrents (做种中的种子队列)

每个未处于做种状态的 `torrent` 都有一个与之关联的队列编号，这表示它在启动队列中的位置。请参阅 [torrent_status::queue_position](https://www.libtorrent.org/reference-Torrent_Status.html#queue_position)。

除了每个队列的限制外，还有一个总体的限制，设置在 [settings_pack::active_limit](https://www.libtorrent.org/reference-Settings.html#active_limit) 中。
自动管理器永远不会启动超过此数量的 `torrent`（除了下面描述的一个例外情况）。非自动管理的 `torrent` 不受此逻辑的限制，也不会被计入。

在固定的时间间隔内，系统会检查是否需要重新排序哪些 `torrent` 处于活动状态，哪些 `torrent` 处于队列中。
此间隔可以通过 [settings_pack::auto_manage_interval](https://www.libtorrent.org/reference-Settings.html#auto_manage_interval) 进行控制。

为了使队列管理正常工作，需要保存并恢复所有 `torrent` 的续传数据。请参阅 [torrent_handle::save_resume_data()](https://www.libtorrent.org/reference-Torrent_Handle.html#save_resume_data())。

### queue position (队列位置)

队列前端的 `torrent` 会被启动，其余的 `torrent` 则按其队列位置排序。任何新添加的 `torrent` 都会被放置在队列的末尾。
一旦某个 `torrent` 被移除或转变为做种状态，其队列位置将变为 -1，而所有原本位于它之后的 `torrent` 会依次向前移动，以填补空缺。

队列位置始终是连续的，形成一个没有空缺的序列。

较低的队列位置意味着更接近队列的前端，这些 `torrent` 会比队列位置较高的 `torrent` 更早启动。

要查询某个 `torrent` 在队列中的位置或更改其位置，请参阅以下函数：

[torrent_handle::queue_position()](https://www.libtorrent.org/reference-Torrent_Handle.html#queue_position()), 
[torrent_handle::queue_position_up()](https://www.libtorrent.org/reference-Torrent_Handle.html#queue_position_up()),
[torrent_handle::queue_position_down()](https://www.libtorrent.org/reference-Torrent_Handle.html#queue_position_down()), 
[torrent_handle::queue_position_top()](https://www.libtorrent.org/reference-Torrent_Handle.html#queue_position_top()),
[torrent_handle::queue_position_bottom()](https://www.libtorrent.org/reference-Torrent_Handle.html#queue_position_bottom()).

### checking queue (检查队列)

检查队列会影响处于 `torrent_status::checking`（检查中）或 `torrent_status::allocating`（分配中）状态且为自动管理的 `torrent`。

检查队列会确保在其队列中的 `torrent` 在任何给定时间内，启动的 `torrent` 数量不超过 `settings_pack::active_checking_limit` 设置的限制。
一旦某个 `torrent` 完成检查并转移到其他状态，队列中的下一个 `torrent` 将开始检查。

任何通过 `force-started`（强制启动）或 `force-stopped`（强制停止）添加的 `torrent`（即未设置自动管理标志），将不受此限制的影响，它们会独立且并行地进行检查。

一旦 `torrent` 完成文件检查或恢复数据检查，它将被放入下载队列，并可能立即开始下载。
如果希望在添加种子并检查其文件时不启动下载，可以用 `stop_when_ready` 模式添加种子。
请参阅 `add_torrent_params::flag_stop_when_ready`。此标志会在 `torrent` 准备好开始下载时立即停止它。

从概念上讲，这与等待 `torrent_checked_alert` 消息，然后调用以下操作，是相同的：

```
h.set_flags(torrent_flags::paused, torrent_flags::paused | torrent_flags::auto_managed);
```

重要区别在于，它完全避免了 `torrent` 短暂进入下载状态的时间窗口。

### downloading queue（下载队列）

与检查队列类似，下载队列将确保在任何给定时间内，
处于下载状态的种子数量不超过 [settings_pack::active_downloads](https://www.libtorrent.org/reference-Settings.html#active_downloads) 设置的值。

这里再次使用 [torrent_status::queue_position](https://www.libtorrent.org/reference-Torrent_Status.html#queue_position) 来确定当一个下载中的 `torrent` 完成或停止/移除后，
下一个应该启动的 `torrent` 是谁。

### seeding queue（做种队列）

做种队列并不使用 [torrent_status::queue_position](https://www.libtorrent.org/reference-Torrent_Status.html#queue_position) 来决定哪个 `torrent` 应该进行做种。
相反，它会估算 `torrent` 的需求程度。一个拥有较少做种者和较多下载者的种子，被认为比那些拥有较多做种者和较少下载者的 `torrent` 更需要增加做种者。

它将启动的做种数量限制在 [settings_pack::active_seeds](https://www.libtorrent.org/reference-Settings.html#active_seeds) 设定的范围内。

除了这一基本倾向外，`torrent` 的优先级还可以通过指定 `seed ratio`（上传与下载的比率）、`seed-time ratio`（下载时间与做种时间的比率）
以及 `seed-time`（为一个 `torrent` 做种的绝对时间）来控制。在这些目标全部达成之前，该 `torrent` 将被优先考虑进行做种。

在已经达到做种目标的 `torrent` 中(已经达到了上传的数据量或达到了预设的上传比例的 `torrent`)，那些我们不知道有其他做种者的 `torrent` 将严格优先。

为了避免频繁切换，那些启动时间不足30分钟的 `torrent` 也会优先保持做种状态。

最后，对于上述条件均不适用的情况，`torrent` 将根据 `seed ratio` 来进行优先级排序。

控制这些限制的相关设置包括：
[settings_pack::share_ratio_limit](https://www.libtorrent.org/reference-Settings.html#share_ratio_limit)（分享比率限制）,
[settings_pack::seed_time_ratio_limit](https://www.libtorrent.org/reference-Settings.html#seed_time_ratio_limit)（做种时间比率限制）,
[settings_pack::seed_time_limit](https://www.libtorrent.org/reference-Settings.html#seed_time_ratio_limit)（做种时间限制）。

### queuing options（队列选项）

除了简单地启动和停止 `torrent` 外，队列机制还可以对 `torrent` 使用的资源进行更精细的控制。

#### half-started torrents（半启动种子）

> *译注:*
> 半启动种子就是指：任务已经启动，但并未连接到所有可用的网络资源（如DHT或 tracker）的状态的种子。

除了下载和做种的限制外，对 `torrent` 的执行操作也有限制。
下载和做种的限制决定了是否允许连接对等节点（peers），如果不允许连接对等节点，`torrent` 将被停止且不会执行任何操作。
如果允许连接对等节点，`torrent` 可能会执行以下操作：

- announce to trackers (向 trackers 宣告)
- announce to the DHT (向 DHT 宣告)
- announce to local peer discovery (向本地对等节点发现宣告) / (本地服务发现 local service discovery)

每个操作需要耗费一定的资源，因此可能需要单独的限制。
这些限制分别由 [settings_pack::active_tracker_limit](https://www.libtorrent.org/reference-Settings.html#active_tracker_limit)、
[settings_pack::active_dht_limit](https://www.libtorrent.org/reference-Settings.html#active_dht_limit) 和 
[settings_pack::active_lsd_limit](https://www.libtorrent.org/reference-Settings.html#active_lsd_limit) 控制。

具体来说，向 tracker 宣告通常比向 DHT 宣告更便宜。
[settings_pack::active_dht_limit](https://www.libtorrent.org/reference-Settings.html#active_dht_limit) 将限制允许向 DHT 宣告的 `torrent` 数量。
优先级较高的 `torrent` 会向 DHT 宣告，而优先级较低的则不会。不过，这些(未被允许的宣告) `torrent` 仍会被视为已启动，并会接受任何进入的对等节点。

如果你不希望施加此类限制（基本上，如果你不希望有“半启动”的种子），请确保将这些限制设置为-1（无限）。

#### prefer seeds（优先种子）

在 active_downloads（活动下载任务数） + active_seeds（活动做种任务数） > active_limit（活动任务限制数） 的情况下，系统会面临一个模糊性问题：是应该优先满足下载任务，还是优先满足做种任务。为了解决这种模糊性，settings_pack::auto_manage_prefer_seeds 参数用于确定是否优先处理做种任务。

如果 auto_manage_prefer_seeds 设置为 true，则系统会优先保留做种任务，即使这意味着需要暂停一些下载任务。

如果设置为 false，则系统可能会优先保留下载任务，暂停部分做种任务。

这个设置可以帮助用户在下载和做种之间根据需求进行权衡。