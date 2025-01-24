## 示例

除了概述中的示例程序外，还有一个更完整的客户端示例（稍微复杂一些），名为 `client_test`。如果你想尝试使用它，这里有单独的使用说明。

### 简单客户端

这是一个简单的客户端。为了保持简洁，它的输出不多：

```cpp
#include <cstdlib>
#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/torrent_info.hpp"

#include <iostream>

int main(int argc, char *argv[])
try
{
    if (argc != 2)
    {
        std::cerr << "usage: ./simple_client torrent-file\n"
                     "to stop the client, press return.\n";
        return 1;
    }

    lt::session s;
    lt::add_torrent_params p;
    p.save_path = ".";
    p.ti = std::make_shared<lt::torrent_info>(argv[1]);
    s.add_torrent(p);

    // wait for the user to end
    char a;
    int ret = std::scanf("%c\n", &a);
    (void)ret; // ignore
    return 0;
}
catch (std::exception const &e)
{
    std::cerr << "ERROR: " << e.what() << "\n";
}
```

## make_torrent

展示如何从目录树创建一个 torrent 文件：

```cpp
#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/create_torrent.hpp"

#include <functional>
#include <cstdio>
#include <sstream>
#include <fstream>
#include <iostream>

#ifdef TORRENT_WINDOWS
#include <direct.h> // for _getcwd
#endif

namespace
{

    using namespace std::placeholders;

    std::vector<char> load_file(std::string const &filename)
    {
        std::fstream in;
        in.exceptions(std::ifstream::failbit);
        in.open(filename.c_str(), std::ios_base::in | std::ios_base::binary);
        in.seekg(0, std::ios_base::end);
        size_t const size = size_t(in.tellg());
        in.seekg(0, std::ios_base::beg);
        std::vector<char> ret(size);
        in.read(ret.data(), int(ret.size()));
        return ret;
    }

    /**
     * 获取文件路径中 parent 路径的部分，不支持绝对路径
     */
    std::string branch_path(std::string const &f)
    {
        if (f.empty())
            return f;

#ifdef TORRENT_WINDOWS
        if (f == "\\\\")
            return "";
#endif
        if (f == "/")
            return "";

        auto len = f.size();
        // if the last character is / or \ ignore it
        if (f[len - 1] == '/' || f[len - 1] == '\\')
            --len;
        while (len > 0)
        {
            --len;
            if (f[len] == '/' || f[len] == '\\')
                break;
        }

        if (f[len] == '/' || f[len] == '\\')
            ++len;
        return std::string(f.c_str(), len);
    }

    // do not include files and folders whose
    // name starts with a .
    //
    // 过滤 "." 开头的文件和文件夹
    bool file_filter(std::string const &f)
    {
        if (f.empty())
            return false;

        char const *first = f.c_str();
        char const *sep = strrchr(first, '/');
#if defined(TORRENT_WINDOWS) || defined(TORRENT_OS2)
        char const *altsep = strrchr(first, '\\');
        if (sep == nullptr || altsep > sep)
            sep = altsep;
#endif
        // if there is no parent path, just set 'sep'
        // to point to the filename.
        // if there is a parent path, skip the '/' character
        if (sep == nullptr)
            sep = first;
        else
            ++sep;

        // return false if the first character of the filename is a .
        if (sep[0] == '.')
            return false;

        std::cerr << f << "\n";
        return true;
    }

    [[noreturn]] void print_usage()
    {
        std::cerr << R"(usage: make_torrent FILE [OPTIONS]

Generates a torrent file from the specified file
or directory and writes it to standard out


OPTIONS:
-w url        adds a web seed to the torrent with
              the specified url
-t url        adds the specified tracker to the
              torrent. For multiple trackers, specify more
              -t options. Specify a dash character "-" as a tracker to indicate
              the following trackers should be in a higher tier.
-c comment    sets the comment to the specified string
-C creator    sets the created-by field to the specified string
-s bytes      specifies a piece size for the torrent
              This has to be a power of 2, minimum 16kiB
-l            Don't follow symlinks, instead encode them as
              links in the torrent file
-o file       specifies the output filename of the torrent file
              If this is not specified, the torrent file is
              printed to the standard out, except on windows
              where the filename defaults to a.torrent
-r file       add root certificate to the torrent, to verify
              the HTTPS tracker
-S info-hash  add a similar torrent by info-hash. The similar
              torrent is expected to share some files with this one
-L collection add a collection name to this torrent. Other torrents
              in the same collection is expected to share files
              with this one.
-2            Only generate V2 metadata
-T            Include file timestamps in the .torrent file.
)";
        std::exit(1);
    }

} // anonymous namespace

int main(int argc_, char const *argv_[])
try
{
    lt::span<char const *> args(argv_, argc_);
    std::string creator_str = "libtorrent";
    std::string comment_str;

    if (args.size() < 2)
        print_usage();

    std::vector<std::string> web_seeds;
    std::vector<std::string> trackers;
    std::vector<std::string> collections;
    std::vector<lt::sha1_hash> similar;
    int piece_size = 0;
    lt::create_flags_t flags = {};
    std::string root_cert;

    std::string outfile;
#ifdef TORRENT_WINDOWS
    // don't ever write binary data to the console on windows
    // it will just be interpreted as text and corrupted
    outfile = "a.torrent";
#endif

    std::string full_path = args[1];
    args = args.subspan(2);

    for (; !args.empty(); args = args.subspan(1))
    {
        if (args[0][0] != '-')
            print_usage();

        char const flag = args[0][1];

        switch (flag)
        {
        case 'l':
            // 在 .torrent 使用符号链接，而不是符号链接指向的实际文件
            flags |= lt::create_torrent::symlinks; 
            continue;
        case '2':
            flags |= lt::create_torrent::v2_only;
            continue;
        case 'T':
            flags |= lt::create_torrent::modification_time;
            continue;
        }

        if (args.size() < 2)
            print_usage();

        switch (flag)
        {
        case 'w':
            web_seeds.push_back(args[1]);
            break;
        case 't':
            trackers.push_back(args[1]);
            break;
        case 's':
            piece_size = atoi(args[1]);
            break;
        case 'o':
            outfile = args[1];
            break;
        case 'C':
            creator_str = args[1];
            break;
        case 'c':
            comment_str = args[1];
            break;
        case 'r':
            root_cert = args[1];
            break;
        case 'L':
            collections.push_back(args[1]);
            break;
        case 'S': 
        // similar torrent by info-hash
        {
            if (strlen(args[1]) != 40)
            {
                std::cerr << "invalid info-hash for -S. "
                             "Expected 40 hex characters\n";
                print_usage();
            }
            std::stringstream hash(args[1]);
            lt::sha1_hash ih;
            hash >> ih;
            if (hash.fail())
            {
                std::cerr << "invalid info-hash for -S\n";
                print_usage();
            }
            similar.push_back(ih);
            break;
        }
        default:
            print_usage();
        }
        args = args.subspan(1);
    }

    lt::file_storage fs;

    // 确保 full_path 是一个绝对路径。如果它不是绝对路径，则将其转换为当前工作目录下的绝对路径
#ifdef TORRENT_WINDOWS
    if (full_path[1] != ':')
#else
    if (full_path[0] != '/')
#endif
    {
        char cwd[2048];
#ifdef TORRENT_WINDOWS
#define getcwd_ _getcwd
#else
#define getcwd_ getcwd
#endif
        // 调用 getcwd_ 函数获取当前工作目录，并将其存储在 cwd 中
        char const *ret = getcwd_(cwd, sizeof(cwd));
        if (ret == nullptr)
        {
            std::cerr << "failed to get current working directory: "
                      << strerror(errno) << "\n";
            return 1;
        }

        // 将 full_path 转换为绝对路径。
#undef getcwd_
#ifdef TORRENT_WINDOWS
        full_path = cwd + ("\\" + full_path);
#else
        full_path = cwd + ("/" + full_path);
#endif
    }

    // 添加文件到 fs 中
    lt::add_files(fs, full_path, file_filter, flags);
    if (fs.num_files() == 0)
    {
        std::cerr << "no files specified.\n";
        return 1;
    }

    // 生成一个 create_torrent 对象（torrent builder）
    lt::create_torrent t(fs, piece_size, flags);
    int tier = 0;
    for (std::string const &tr : trackers)
    {
        if (tr == "-")
            ++tier;
        else
            t.add_tracker(tr, tier);
    }

    for (std::string const &ws : web_seeds)
        t.add_url_seed(ws);

    for (std::string const &c : collections)
        t.add_collection(c);

    for (lt::sha1_hash const &s : similar)
        t.add_similar_torrent(s);

    auto const num = t.num_pieces();

    // 计算并设置 torrent 文件中每个分片（piece）的哈希值
    lt::set_piece_hashes(t, branch_path(full_path), [num](lt::piece_index_t const p)
                         { std::cerr << "\r" << p << "/" << num; });

    std::cerr << "\n";
    t.set_creator(creator_str.c_str());
    if (!comment_str.empty())
    {
        t.set_comment(comment_str.c_str());
    }

    if (!root_cert.empty())
    {
        std::vector<char> const pem = load_file(root_cert);
        t.set_root_cert(std::string(&pem[0], pem.size()));
    }

    // create the torrent and print it to stdout
    // 生成 .torrent 文件, 将 t.generate() 产生的 bencoding 编码的字节流保存到该文件中
    std::vector<char> torrent;
    lt::bencode(back_inserter(torrent), t.generate());
    if (!outfile.empty())
    {
        std::fstream out;
        out.exceptions(std::ifstream::failbit);
        out.open(outfile.c_str(), std::ios_base::out | std::ios_base::binary);
        out.write(torrent.data(), int(torrent.size()));
    }
    else
    {
        std::cout.write(torrent.data(), int(torrent.size()));
    }

    return 0;
}
catch (std::exception &e)
{
    std::cerr << "ERROR: " << e.what() << "\n";
    return 1;
}
```

## dump_torrent

这是一个示例程序，它将 torrent 文件作为参数，并将其信息打印到标准输出：

```cpp
#include <cstdio> // for snprintf
#include <cinttypes> // for PRId64 et.al.
#include <fstream>
#include <iostream>

#include "libtorrent/entry.hpp"
#include "libtorrent/bencode.hpp"
#include "libtorrent/load_torrent.hpp"
#include "libtorrent/bdecode.hpp"
#include "libtorrent/magnet_uri.hpp"
#include "libtorrent/span.hpp"

namespace {

[[noreturn]] void print_usage()
{
  std::cerr << R"(usage: dump_torrent torrent-file [options]
    OPTIONS:
    --items-limit <count>    set the upper limit of the number of bencode items
                             in the torrent file.
    --depth-limit <count>    set the recursion limit in the bdecoder
    --show-padfiles          show pad files in file list
    --max-pieces <count>     set the upper limit on the number of pieces to
                             load in the torrent.
    --max-size <size in MiB> reject files larger than this size limit
)";
  std::exit(1);
}

}

int main(int argc, char const* argv[]) try
{
  lt::span<char const*> args(argv, argc);

  // strip executable name
  args = args.subspan(1);

  lt::load_torrent_limits cfg;
  bool show_pad = false;

  if (args.empty()) print_usage();

  char const* filename = args[0];
  args = args.subspan(1);

  using namespace lt::literals;

  while (!args.empty())
  {
    // _sv 是一个用户定义的字面量（user-defined literal），用于将字符串字面量转换为 std::string_view 类型的对象。
    // std::string_view 是 C++17 引入的一个轻量级、非拥有（non-owning）的字符串视图类，
    // 它可以高效地表示一个字符串或字符串的一部分，而不需要分配额外的内存或复制字符串内容。
    if (args[0] == "--items-limit"_sv && args.size() > 1) 
    {
      // 设置解码的最大 token 数量
      cfg.max_decode_tokens = atoi(args[1]);
      args = args.subspan(2);
    }
    else if (args[0] == "--depth-limit"_sv && args.size() > 1)
    {
      // 设置解码的最大深度
      cfg.max_decode_depth = atoi(args[1]);
      args = args.subspan(2);
    }
    else if (args[0] == "--max-pieces"_sv && args.size() > 1)
    {
      // 设置最大片段数量
      cfg.max_pieces = atoi(args[1]);
      args = args.subspan(2);
    }
    else if (args[0] == "--max-size"_sv && args.size() > 1)
    {
      // 设置最大缓冲区大小（以 MB 为单位）
      cfg.max_buffer_size = atoi(args[1]) * 1024 * 1024;
      args = args.subspan(2);
    }
    else if (args[0] == "--show-padfiles"_sv)
    {
      // 启用显示 pad 文件的功能
      show_pad = true;
      args = args.subspan(1);
    }
    else
    {
      std::cerr << "unknown option: " << args[0] << "\n";
      print_usage();
    }
  }

  // 载入并解析 torrent 文件
  lt::add_torrent_params const atp = lt::load_torrent_file(filename, cfg);

  // print info about torrent
  // 打印 dht nodes
  if (!atp.dht_nodes.empty())
  {
    std::printf("nodes:\n");
    for (auto const& i : atp.dht_nodes)
      std::printf("%s: %d\n", i.first.c_str(), i.second);
  }

  // 打印 trackers
  if (!atp.trackers.empty())
  {
    puts("trackers:\n");
    auto tier_it = atp.tracker_tiers.begin();
    int tier = 0;
    for (auto const& i : atp.trackers)
    {
      if (tier_it != atp.tracker_tiers.end())
      {
        tier = *tier_it;
        ++tier_it;
      }
      std::printf("%2d: %s\n", tier, i.c_str());
    }
  }

  // 打印 v1 和 v2 的 infohash，magnet_uri 以及 info 字段里的内容
  std::stringstream ih;
  ih << atp.info_hashes.v1;
  if (atp.info_hashes.has_v2())
    ih << ", " << atp.info_hashes.v2;
  
  std::printf("number of pieces: %d\n"
    "piece length: %d\n"
    "info hash: %s\n"
    "comment: %s\n"
    "created by: %s\n"
    "magnet link: %s\n"
    "name: %s\n"
    "number of files: %d\n"
    "files:\n"
    , atp.ti->num_pieces()
    , atp.ti->piece_length()
    , ih.str().c_str()
    , atp.ti->comment().c_str()
    , atp.ti->creator().c_str()
    , make_magnet_uri(atp).c_str()
    , atp.name.c_str()
    , atp.ti->num_files());

  // 打印 "info" 中的文件列表信息
  lt::file_storage const& st = atp.ti->files();
  for (auto const i : st.file_range())
  {
    auto const first = st.map_file(i, 0, 0).piece;
    auto const last = st.map_file(i, std::max(std::int64_t(st.file_size(i)) - 1, std::int64_t(0)), 0).piece;
    auto const flags = st.file_flags(i);

    if ((flags & lt::file_storage::flag_pad_file) && !show_pad) continue;

    std::stringstream file_root;
    // 获取第 i 个文件的 root hash
    if (!st.root(i).is_all_zeros())
      file_root << st.root(i);
    std::printf(" %8" PRIx64 " %11" PRId64 " %c%c%c%c [ %5d, %5d ] %7u %s %s %s%s\n"
      , st.file_offset(i)
      , st.file_size(i)
      , ((flags & lt::file_storage::flag_pad_file)?'p':'-')
      , ((flags & lt::file_storage::flag_executable)?'x':'-')
      , ((flags & lt::file_storage::flag_hidden)?'h':'-')
      , ((flags & lt::file_storage::flag_symlink)?'l':'-')
      , static_cast<int>(first)
      , static_cast<int>(last)
      , std::uint32_t(st.mtime(i))
      , file_root.str().c_str()
      , st.file_path(i).c_str()
      , (flags & lt::file_storage::flag_symlink) ? "-> " : ""
      , (flags & lt::file_storage::flag_symlink) ? st.symlink(i).c_str() : "");
  }

  // 打印 web seeds
  std::printf("web seeds:\n");
  for (auto const& ws : atp.url_seeds)
    std::printf("%s\n", ws.c_str());

  return 0;
}
catch (std::exception const& e)
{
  std::cerr << "ERROR: " << e.what() << "\n";
}
```