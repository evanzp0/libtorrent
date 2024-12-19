# session对象
在libtorrent中，session类代表了一个BitTorrent客户端的会话。它是管理网络连接、磁盘I/O、DHT（分布式哈希表）、PEX（Peer Exchange，对等节点交换）等所有核心功能的核心对象。

# add_torrent_params对象
add_torrent_params是一个结构体（或类），用于封装添加新torrent（即要下载或共享的文件）到session时所需的所有参数。这些参数可能包括torrent的文件信息、tracker的URL、保存路径、初始对等节点列表等。

# session::add_torrent()方法
session类提供了add_torrent()方法，用于将一个新的torrent添加到会话中。由于不同的用户可能有不同的需求，add_torrent()方法通常有多个重载版本，允许用户以不同的方式提供add_torrent_params对象或相关信息。

实例：

```cpp
#include <libtorrent/session.hpp>
#include <libtorrent/add_torrent_params.hpp>

int main() {
    lt::session sess;

    // 假设已经填充了torrent的信息
    lt::add_torrent_params params;
    params.ti = std::make_shared<lt::torrent_info>("path/to/torrent/file.torrent");
    params.save_path = "./downloads";

    // 将params对象传递给session的add_torrent()方法
    sess.add_torrent(params);

    // 运行session（通常在一个事件循环中）
    // ...

    return 0;
}
```