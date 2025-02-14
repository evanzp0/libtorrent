# 编译 libtorrent 的方法

## 下载 libtorrent 源码

```
git clone --recurse-submodules https://github.com/arvidn/libtorrent.git
```

## 下载并编译 openssl

1. 检查是否安装过 perl

```
perl -v
```

如果没有安装则下载并安装 perl 5.0

https://strawberryperl.com/download/5.32.1.1/strawberry-perl-5.32.1.1-64bit.zip

~~2. 下载并安装 nasm 2.0~~

https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip

~~3. 下载 openssl 3.0 源码，并解压~~

https://github.com/openssl/openssl/releases/download/openssl-3.4.0/openssl-3.4.0.tar.gz

~~4. 编译 openssl 3.0~~

  1. 打开 `x64 Native Tools Command Prompt for VS 2022` 命令行工具
  2. 进入 openssl 源码目录
  3. 执行以下命令
    
  ```
  perl Configure VC-WIN64A  no-asm --prefix=D:\develop\openssl-3.4.0 --openssldir=D:\develop\openssl-3.4.0\src
  ```

  如果编译静态链接版本的二进制程序，加一个 no-shared 选项即可。

  ```
  perl Configure VC-WIN64A  no-asm no-shared --prefix=D:\develop\openssl-3.4.0 --openssldir=D:\develop\openssl-3.4.0\src
  ```

  默认编译后安装在以下路径：

  | 版本 | 默认安装路径 |
  | ---- | ----------  |
  | 64 位 | C:\Program Files\OpenSSL |
  | 32 位 | C:\Program Files (x86)\OpenSSL |

  若想要自定义安装路径，添加 --prefix 选项即可

  > 注意：
  >  使用 VS2022 的开发人员提示工具切换到源码目录，注意选择要编译的位数所对应的版本，不要选择 cross 版本。所有的选项都要统一，即：编译 32 位的就启动 x86，配置 Makefile时为命令行为 VC-WIN32; 编译 64 位的就启动 x64，配置 Makefile时为命令行为 VC-WIN64A，本文以编译 64 位为例。


  ~~4. 编译速度~~

  ```
  nmake
  ```

  速度很慢，想加速编译，使用 Qt 的 jom 工具即可使用 -j 选项来指定构建的线程数， jom 可以在 [这里](https://download.qt.io/official_releases/jom/) 下载，将它解压然后添加到环境变量即可，下面的命令使用 8 线程编译

  ```
  jom -j 8
  ```

  > 注意：
  > 如果因为环境原因编译失败，需要将源文件目录删除，重新解压源文件再进行编译。

  ~~5. 安装二进制和库~~

  ```
  nmake install_sw
  ```

  使用 install_sw 代替 install 是因为默认 install 会生成 40M 左右的 HTML 文档，若不需要就使用 install_sw 仅安装二进制文件和库

  ~~6. 添加环境变量~~
  
  将编译后的安装目录下的 bin 文件夹添加到系统 Path 目录下 打开 cmd 使用 openssl version -a 测试

  ```
  C:\>openssl version -a
  OpenSSL 3.4.0 22 Oct 2024 (Library: OpenSSL 3.4.0 22 Oct 2024)
  built on: Mon Dec 16 03:02:36 2024 UTC
  platform: VC-WIN64A
  options:  bn(64,64)
  compiler: cl  /Zi /Fdossl_static.pdb /Gs0 /GF /Gy /MD /W3 /wd4090 /nologo /O2 -DL_ENDIAN -DOPENSSL_PIC -D"OPENSSL_BUILDING_OPENSSL" -D"OPENSSL_SYS_WIN32" -D"WIN32_LEAN_AND_MEAN" -D"UNICODE" -D"_UNICODE" -D"_CRT_SECURE_NO_DEPRECATE" -D"_WINSOCK_DEPRECATED_NO_WARNINGS" -D"NDEBUG"
  OPENSSLDIR: "C:\Program Files\Common Files\SSL"
  ENGINESDIR: "D:\develop\openssl-3.4.0\lib\engines-3"
  MODULESDIR: "D:\develop\openssl-3.4.0\lib\ossl-modules"
  Seeding source: os-specific
  CPUINFO: N/A
  ```

  ~~7. 下载~~

  已编译的动态链接版及静态链接版，已经移除了 pdb 调试符号文件，确保文件大小

  [动态链接版](https://cdn.taurusxin.com/softwares/openssl/openssl-3.1.3-windows-amd64-dynamic.zip)

  [静态链接版](https://cdn.taurusxin.com/softwares/openssl/openssl-3.1.3-windows-amd64-static.zip)

2. 下载 openssl 源码
https://github.com/openssl/openssl/releases/download/OpenSSL_1_1_1w/openssl-1.1.1w.tar.gz

并解压文件到 `D:\tmp\openssl-1.1.1` 目录下

3. 编译 openssl 源码

```
cd D:\tmp\openssl-1.1.1
perl D:\tmp\openssl-1.1.1\Configure VC-WIN64A no-asm no-shared
```

4. 检查编辑结果

```
nmake test
```

显示结果 PASS 为编译通过

```
All tests successful.
Files=159, Tests=2360, 407 wallclock secs ( 0.09 usr +  0.01 sys =  0.11 CPU)
Result: PASS
```

5. 安装

```
nmake install d:\develop\OpenSSL-Win64
```

## 下载并编译 boost

1. https://www.boost.org/ 下载 boost 源码 `boost_1_82_0.zip` 解压到 `d:\develop\boost\bin` 。

2. 使用  `x64 Native Tools Command Prompt for VS2022` 切换操作目录至 boost\bin 下 （避免PC中多个VS版本的影响）
3. 基于 VS2022 的 CMD 运行 `d:\develop\boost\bin\bootstrap.bat` 会自动使用 `VC14.3` 在该目录下生成生成 `b2.exe` （新版本不会生成 `bjam.exe`）

> 个人建议：首先去配置一下 cl 命令的环境变量，避免提示以下问题：
> ```
> 'cl' 不是内部或外部命令，也不是可运行的程序。
> ```
> 解决这个问题的方法是将 cl 命令的位置添加到系统的环境变量中，我的安装路径在 C 盘中，则需要将以下内容添加到 Path 环境变量中：
> ```
> C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC\14.35.32215\bin\Hostx64\x64
> ```
> 环境变量修改之后必须重启才能生效。
> 
> 添加完以上环境变量之后，再次运行 `.\bootstrap.bat` ，如果出现以下错误：
> ```
> fatal error C1034: ctype.h: 不包括路径集。
> ```
> 则说明是 Windows 环境变量没有配置或没配置好。此时我们需要在系统的环境变量里面新建两个环境变量，它们的变量名和变量值分别为：
> ```toml
> # 仅供参考
> # 第一个环境变量
> 变量名：INCLUDE
> 变量值：C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC\14.35.32215\include
>        C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\shared
>        C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\ucrt
>        C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\um
>        C:\Program Files (x86)\Windows Kits\10\Include\10.0.22621.0\winrt
> 
> # 第二个环境变量
> 变量名：LIB
> 变量值：C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC\14.35.32215\lib\x64
>        C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\ucrt\x64
>        C:\Program Files (x86)\Windows Kits\10\Lib\10.0.22621.0\um\x64
> ```

4. 该目录下运行命令

方式一：

将 boost 编译的库和 include 头文件都安装到 D:\develop\boost

```
cd d:\develop\boost\bin

b2 install --prefix=D:\develop\boost --build-type=complete --toolset=msvc-14.3 link=static runtime-link=static threading=multi address-model=64 debug release
```
执行完毕后 `D:\develop\boost` 下会生成 `include` 、 `lib` 、 `share` 三个目录。

方式二：

head 文件夹路径：D:\develop\boost\bin\boost
lib 库文件夹路径：D:\develop\boost\bin\stage\lib

```
b2 --toolset=msvc-14.3 link=static runtime-link=static --build-type=complete stage threading=multi address-model=64 debug release
```

* --toolset=msvc-14.3 定义了 MSVC++ 版本号, `C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\MSVC` 下的子目录名就是对应的 MSVC++ 版本号
* link=static 定义了链接库为静态，就是将链接库本身编译为静态的，动态设置为 shared
* runtime-link=static 定义了运行链接库为静态，就是程序使用此库打包时为静态的，动态设置为 shared
* --build-type=complete 定义了编译所有支持的变种库
* stage 定义了库文件路径：.\stage\lib。想要更换其他路径使用 --stagedir=directory 参数来定义

## 编译 libtorrent

1. 设置系统变量 `BOOST_ROOT = d:\develop\boost\bin`, 并将其加入到 `path` 系统变量中。
2. 打开 `x64 Native Tools Command Prompt for VS2022` 切换操作目录至 `d:\workspace\libtorrent` 目录下，执行命令：

```
b2 --toolset=msvc-14.3 link=static runtime-link=static openssl-lib=d:\develop\OpenSSL-Win64\lib openssl-include=d:\develop\OpenSSL-Win64\include
```

3. 安装 libtorrent

```
b2 install --toolset=msvc-14.3 --prefix=d:\develop\libtorrent link=static runtime-link=static
```

安装完成后，`d:\develop\libtorrent` 目录下会生成 `lib` 、 `include` 、 `share` 三个目录。

## 创建一个新的 C++ 项目
1. 在 `d:\workspace` 目录下新建 `libtorrent_learning` 目录

```
cd d:\workspace
mkdir libtorrent_learning
cd libtorrent_learning
```

2. 新建一个 src 目录，用于存放源代码

```
mkdir src
```

3. 在 src 目录下新建一个 `lt_client.cpp` 文件，写入以下内容:

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

4. 在 `d:\workspace\libtorrent_learning` 目录下新建一个 `CMakeLists.txt` 文件，写入以下内容:

```cmake 
cmake_minimum_required(VERSION 3.31)

# 设置策略CMP0091为NEW，新策略
if (POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif (POLICY CMP0091)

# 指定C++标准
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_definitions( -D_WIN32_WINNT=0x0A00 )

# 声明一个工程
project(learn_cmake)

set(Boost_COMPILER vc143)
set(Boost_USE_STATIC_RUNTIME not ON)

include_directories(
    "D:/develop/libtorrent/include"
    "D:/develop/boost/include/boost-1_87")

find_library(LIBTORRENT_LIB libtorrent-rasterbar "D:/develop/libtorrent/lib")
find_library(LIBTRY_SIGNAL_LIB libtry_signal "D:/develop/libtorrent/lib")
find_library(OPENSSL_CRYPTO_LIB libcrypto "D:/develop/OpenSSL-Win64/lib")
find_library(OPENSSL_LIB libssl "D:/develop/OpenSSL-Win64/lib")
find_library(BOOST_SYSTEM_LIB libboost_system-vc143-mt-sgd-x64-1_87 "D:/develop/libtorrent/lib")

# 添加一个exe
add_executable(lt_client ./src/lt_client.cpp)

target_link_libraries(lt_client ${LIBTORRENT_LIB} ${OPENSSL_CRYPTO_LIB} ${OPENSSL_LIB} ${LIBTRY_SIGNAL_LIB} ${BOOST_SYSTEM_LIB} iphlpapi.lib crypt32.lib)

# 设置MT/MTd
set_property(TARGET lt_client PROPERTY
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
```

5. 编译

```
cd d:\workspace\libtorrent_learning
mkdir build
cd build
cmake .. -G "NMake Makefiles"
nmake
```

编译成功后，在 `build` 目录下，能看到生成的可执行文件 `lt_client.exe` 。

6. VSCode 中的源码浏览跳设置

打开 setting 面板，搜索 browe ，找到 `C_Cpp: Browse.path` ，将其设置为 `D:/workspace/libtorrent/src` ( `libtorrent` 源码目录)。
这样，按住 `alt + 鼠标右键`，就可以跳转代码了。

7. VSCode 中编辑时的代码引用提示设置

在项目根目录新建 `.vscode` 文件夹，并创建 `c_cpp_properties.json` ，输入以下内容，编辑时的 include 就不会报错了。

```json
{
    "configurations": [
        {
            "name": "Win32",
            "includePath": [
                "${workspaceFolder}/**",
                "D:/develop/libtorrent/include",
                "D:/develop/boost/include/boost-1_87"
            ],
            "defines": [
                "_DEBUG",
                "UNICODE",
                "_UNICODE",
                "_WIN32_WINNT=0x0A00"
            ],
            "windowsSdkVersion": "10.0.22621.0",
            "compilerPath": "cl.exe",
            "cppStandard": "c++17"
        }
    ],
    "version": 4
}
```