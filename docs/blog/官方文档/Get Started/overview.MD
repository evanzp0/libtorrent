# Overview

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

