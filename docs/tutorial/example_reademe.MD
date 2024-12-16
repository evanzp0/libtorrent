libtorrent 是一个开源bt库，它将 meta 文件生成，hash 生成，下载文件, 网络连接等操作封装成接口，供别人调用，并且都有很好的性能与稳定性。它大量使用了 boost 库的东西，并且还使用了 openssl .可以说，在 windows 下开发 bt 客户端，是很好的选择。利用它，你可以使用不到 30 的代码就可以轻松做一个非常稳定的bt客户端。

对于它的使用，在例程中有 5 个例子，可以很好的展示其使用方式。

1. `client_test.cpp`: 这个例程用来测试 `libtorrent` 的很多方面。
2. `dump_torrent.cpp`: 用来查看下载文件的信息，如分多少块，块大小，校验等。
    
    涉及 `lazy_entry`, `torrent_info` 等结构

3. `enum_if`: 这个例程用来显示与其它对等点的网络信息，如 `ip` , `port` 资源情况。 

    涉及 `io_service` , `address`.

4. `make_torrent.cpp`:用来生成一个种子文件，它需要种子名，文件名，服务器端地址。
    
    涉及 `file_storage` , `file_pool` 。

5. `simple_client.cpp`: 用来生成一个简单的客户端。没有速度，与进度，但能下完。

    涉及 `session` , `add_torrent_params` 。