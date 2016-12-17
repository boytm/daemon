daemon
======

A utility, make a command line tool run as a service on windows.

# Installation #

### Install required development components
Require _Visual Studio 2003 and later_ or _Windows SDK_, or even  [Microsoft Visual C++ Compiler for Python 2.7](https://www.microsoft.com/en-us/download/details.aspx?id=44266).

### Compile 
* Visual Studio just open the solution file

* Without Visual Studio, you need CMake to generate Makefile
```bash
daemon>mkdir build
daemon>cd build
daemon\build>cmake.exe -G "NMake Makefiles"  ..
daemon\build>nmake
```



# Usage #

    Usage: daemon.exe [cmdline]
                -c              configuration file, default .\daemon.ini
                -f              run foreground
                -d              run as a background service
                -i              install service
                -u              uninstall service
                -r              start service
                -k              kill service
                -h              show this help


中文帮助
=======

这个小工具作为 Windows 服务启动——在 vista 及以上系统是自动延迟启动，可以守护多个小程序——最多63个。如果子进程挂掉了，会再启动一个。

在 daemon.exe 目录下放置同名 daemon.ini 配置文件，样例如下：
```ini
[Settings]
ServiceName=ASpecialDeamon
DisplayName=A Special Deamon
Description=long description
; 本地系统（LocalSystem 账号是最高权限） ServiceStartName=
; 本地服务 ServiceStartName=NT AUTHORITY\LocalService
; virtual accounts （win7支持，更好的与其它用户隔离） ServiceStartName=NT SERVICE\your service name
; 网络服务 ServiceStartName=NT AUTHORITY\NetworkService
ServiceStartName=
CheckProcessSeconds=30 

; 服务启动时候，会将自己切换到 daemon.exe 所在目录，所以 CommandLine 最好使用绝对路径
[Process0]
; 定期执行 ping （因为子进程退出后会被自动启动）
CommandLine=ping qq.com 
...
[Process61]
; 守护 mproxy，当前目录
CommandLine=mproxy  "-l8888" "-b127.0.0.1" "-s9.9.9.9" "-p60000" -k "your_key"  "-maes-256-cfb" "--pac" "proxy.pac"
[Process62]
; 守护 kcptun，绝对路径
CommandLine="D:\Program Files\kcptun\kcptun.exe"  -r 9.9.9.9:9999 --key your_key --crypt aes --datashard 0 --parityshard 0 -l 0.0.0.0:8081
```

* 安装服务
```bash
daemon.exe -i
```

* 启动服务
```bash
daemon.exe -r
```

* 停止服务
```bash
daemon.exe -k
```

* 卸载服务
```bash
daemon.exe -u
```

