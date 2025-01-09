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

