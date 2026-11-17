# TC1 免拆刷机图文教程

用电脑的**有线网卡提供 TFTP、无线网卡发送触发请求**，把社区固件刷进 TC1 的非活动槽，原厂槽不动。不拆机、不焊接。

**适用设备：TC1 智能排插 A1 硬件版（EMW3031 / MK3031），原厂固件 1.0.0.26、1.0.0.24 或 1.0.0.21。** TC1 A2、其他原厂版本和 A1 空气净化器不适用。

<!-- os-picker -->

## 1. 建好 tc1-flash 文件夹

所有文件放在同一个文件夹，后面每条命令都在这个文件夹里执行（Windows 用 CMD 或 PowerShell）：

```sh
mkdir tc1-flash
cd tc1-flash
```

下载这两个文件，保持原名：

| 下载 | 用途 |
| --- | --- |
| [mico_ota.bin](assets/tools/tc1-no-disassembly/mico_ota.bin) | 649,344 字节的 0.9.6-ed4474 过渡包；放进 TFTP 服务目录，文件名不可改 |
| [tc1-trigger.py](assets/tools/tc1-no-disassembly/tc1-trigger.py) | 读原厂版本并发送触发请求；与 `mico_ota.bin` 放同一文件夹 |

下载完用[校验和](assets/SHA256SUMS)核对这两个文件，不符就重新下载。给排插稳定供电，刷机时拔掉排插上的用电设备。

完成标志：`tc1-flash` 里有两个文件，名字没变。

## 2. 建 PHICOMM_OTA_AP 热点，接上网线

TC1 收到触发请求就离开配网热点，改连 `PHICOMM_OTA_AP` 下载固件。**先把下载网络准备好，再动 TC1。**

路由器或 AP 的无线设置里选 **2.4 GHz**，四项逐个确认：

| 设置项 | 填写 |
| --- | --- |
| Wi-Fi 名称 / SSID | `PHICOMM_OTA_AP`，大小写一致，不加空格 |
| 安全方式 / 密码 | 开放，无密码 |
| 隐藏 SSID | 关闭 |
| 访客模式 / 客户端隔离 | 关闭 |

![路由器无线设置：PHICOMM_OTA_AP、无密码、关闭隐藏与访客模式](assets/images/tc1-ota-ap.png)

保存后，用网线把电脑接到这台路由器的 **LAN 口**。路由器不用联网，也不用把管理地址改成 `10.0.0.2`（这个地址留给电脑），但要确认局域网里没有别的设备占用它；有线和无线**不桥接、不开共享**。这个热点不需要 DHCP，TC1 连上后自己使用静态地址。

完成标志：手机能连上无密码的 `PHICOMM_OTA_AP`，电脑侧 LAN 口指示灯亮。

## 3. 给有线网卡加 10.0.0.2，把原配置抄下来

TFTP 服务器地址必须是 `10.0.0.2/8`，**不加网关、不加 DNS**。加地址前先把原配置抄一遍，第 7 节按这张表还原：

```text
有线网卡名称：
原地址 / 掩码：
原网关：
原 DNS：
原来是自动获取（DHCP）还是静态：
```

<!-- os:windows -->

查询网卡名称和当前配置，找到接 LAN 口的那张**有线网卡**：

```cmd
netsh interface ipv4 show interfaces
ipconfig /all
```

以 `Ethernet` 为例，名称不同时替换引号内的内容：

```cmd
netsh interface ipv4 add address name="Ethernet" address=10.0.0.2 mask=255.0.0.0
netsh interface ipv4 show addresses name="Ethernet"
```

![管理员命令行给 Ethernet 添加 10.0.0.2/8](assets/images/tc1-ota-windows-address.png)

<!-- /os -->

<!-- os:linux -->

查网卡名，下面以 `enp3s0` 为例：

```sh
ip -br address
ip route
sudo ip address add 10.0.0.2/8 dev enp3s0
ip -4 address show dev enp3s0
```

<!-- /os -->

<!-- os:mac -->

先分清 Wi-Fi 和有线网卡，USB 网卡不一定是 `en0`。下面以 `en5` 为例：

```sh
networksetup -listallhardwareports
ifconfig en5
sudo ifconfig en5 inet 10.0.0.2 netmask 255.0.0.0 alias
ifconfig en5
```

<!-- /os -->

这些命令会把原来自动获取地址的网卡改成静态配置，第 7 节必须还原。

完成标志：有线网卡出现 `10.0.0.2/8`，上面那五行已经填好。

## 4. 起 TFTP 服务，先用另一台设备下一遍

TFTP 服务目录里只放 `mico_ota.bin`；**payload 不要放进 TFTP 目录**。服务一直开着，直到传输结束。

<!-- os:windows -->

打开 Tftpd64，完成四项设置：

1. **Current Directory** → **Browse** → 选 `tc1-flash` 文件夹。
2. **Server interfaces** 选 **`10.0.0.2`**，不要选 Wi-Fi 的地址。
3. 打开 **Tftp Server** 标签，留着窗口看下载记录；不用开它自带的 DHCP。
4. Windows 防火墙询问时，允许 Tftpd64 在这条刷机网络上通信。

![Tftpd64：选固件目录，Server interfaces 选 10.0.0.2](assets/images/tc1-ota-tftpd64.png)

<!-- /os -->

<!-- os:linux -->

Debian / Ubuntu 装 `dnsmasq-base`，在 `tc1-flash` 里运行：

```sh
sudo apt install dnsmasq-base
tc1_tftp_dir=$(mktemp -d /tmp/tc1-tftp.XXXXXX)
chmod 755 "$tc1_tftp_dir"
install -m 644 mico_ota.bin "$tc1_tftp_dir/mico_ota.bin"
sudo dnsmasq --no-daemon --conf-file=/dev/null --port=0 \
  --listen-address=10.0.0.2 --bind-interfaces --enable-tftp \
  --tftp-root="$tc1_tftp_dir" --log-facility=-
```

这条命令只提供 TFTP，不提供 DNS 或 DHCP。终端保持运行，另开一个终端发请求；提示 UDP 69 已占用时，改用现有服务和它的服务目录，不要重复启动。

<!-- /os -->

<!-- os:mac -->

在 `tc1-flash` 里运行。先确认 `/private/tftpboot` 中没有要保留的同名文件：

```sh
sudo mkdir -p /private/tftpboot
sudo cp mico_ota.bin /private/tftpboot/mico_ota.bin
sudo chmod 644 /private/tftpboot/mico_ota.bin
sudo launchctl load -w /System/Library/LaunchDaemons/tftp.plist
```

系统自带服务没有进度窗口，传输记录看第 6 节的设备状态。

<!-- /os -->

TFTP 初始请求走 **UDP 69**，后续传输用服务器的会话端口；防火墙要放行整段传输，不能只放行 UDP 69。

### 自检：先用另一台设备完整下载一次

在让 TC1 进入配网模式之前，用另一台连接 `PHICOMM_OTA_AP` 的设备把文件完整下载一遍（Windows 用 `curl.exe`）：

```sh
curl --fail --max-time 30 -o tc1-download-check.bin tftp://10.0.0.2/mico_ota.bin
```

curl 不支持 `tftp://` 时，换一个带 TFTP 下载功能的客户端，操作等价。

完成标志：这次下载正常结束。没下完就先修服务，不要进入下一步。

## 5. 让 TC1 进配网模式，读出原厂版本

给 TC1 重新上电，在**上电 3 秒内双击配网键**。LED 慢闪后出现开放热点 `Phicomm_TC1_XXXX`，末四位因机而异。

看不到热点时：长按配网键到 LED 快闪再松开，随后快速短按两次；仍不出现就断电重试，不要提前发请求。

电脑用**无线网卡**连这个热点，有线连接和 TFTP 保持不动。Windows 提示「无 Internet」时保持连接，不要断开重连。

![电脑无线列表中的原厂配网热点](assets/images/tc1-stock-ap.png)

确认无线拿到 `10.10.10.x/24`，然后读原厂信息：

```sh
curl --noproxy '*' --fail --max-time 5 http://10.10.10.1:8000/read-domain
```

Windows 用 `curl.exe`。`--noproxy '*'` 不能省：系统代理会让发往 `10.10.10.1` 的请求到不了设备。

![read-domain 返回原厂 VERSION](assets/images/tc1-stock-version.png)

只看返回里的 `VERSION`。不在开头列的三个版本之内就停，按[免拆原理与自备镜像](tc1-no-disassembly)处理；第 6 节的脚本读不到或不在范围内，也会自己停下。

完成标志：返回的 JSON 里有 `VERSION`，且取值在开头那三个版本之内。

## 6. 运行脚本，然后什么都别动

发请求前确认三件事：TFTP 还在跑、有线地址还是 `10.0.0.2`、无线还连着 TC1。然后执行一次：

```sh
python3 tc1-trigger.py
```

脚本自己读 `10.10.10.1:8000` 的 `VERSION`，按版本选目标地址，发送 397 字节的触发请求。版本不在已验证范围、或读不到设备时它直接退出，不会回退到别的版本的地址。

**只发送一次。** 脚本报「设备已应答」就停手；报「未确认」先看 TFTP 日志和设备，不要重发。

### 没有 Python 时的手工路径

按第 5 节读到的版本，下载对应那一个文件：1.0.0.26 用 [tftp-trigger.bin](assets/tools/tc1-no-disassembly/tftp-trigger.bin)，1.0.0.24 用 [tftp-trigger-1.0.0.24.bin](assets/tools/tc1-no-disassembly/tftp-trigger-1.0.0.24.bin)，1.0.0.21 用 [tftp-trigger-1.0.0.21.bin](assets/tools/tc1-no-disassembly/tftp-trigger-1.0.0.21.bin)。

在 `tc1-flash` 里发送一次，Windows 用 `curl.exe`：

```sh
curl --noproxy '*' --max-time 30 --header 'Content-Type: application/json' --data-binary '@tftp-trigger.bin' http://10.10.10.1:8000/OTA
```

`--data-binary` 原样发送文件：不要把 payload 打开后复制成文本，也不要把截图里的提示符或反引号带进命令。

![curl 发送后原厂接口返回 OTA 字样](assets/images/tc1-ota-trigger.png)

### 接下来 1～2 分钟

设备先擦掉非活动槽（约 6 秒），再从 `10.0.0.2:69` 下载 `mico_ota.bin`，校验尾部 MD5 后切槽重启。这期间：

- 配网热点消失、电脑无线断开、curl 超时，都是正常现象；
- 有 TFTP 日志或进度窗口的，应看到读取请求和完整传输记录；
- 电脑无线不用改连 `PHICOMM_OTA_AP`，网线继续负责传输。

**保持 TC1、AP 和电脑供电；下载中不要关 TFTP、拔网线或给 TC1 断电。**

完成标志：TFTP 日志显示 `mico_ota.bin` 传输完成，随后设备自动重启。

## 7. 确认刷好了，再把电脑网络还原

刷好后出现新的开放热点 **`TC1_A1-AP-<MAC 后六位大写>`**，截图中的 `TC1_A1-AP-636D3D` 只是示例。

![刷入社区固件后出现 TC1_A1-AP 热点](assets/images/tc1-community-ap.png)

连上它，浏览器打开 **`http://192.168.0.1/`**，能进社区固件网页就是刷机成功。在这里填写日常 Wi-Fi；按键、配网和六路开关见 [TC1 功能与操作](tc1-guide)。

停掉临时服务，删掉临时地址：

<!-- os:windows -->

```cmd
netsh interface ipv4 delete address name="Ethernet" address=10.0.0.2
```

关闭 Tftpd64。原来自动获取地址的，再恢复 DHCP：

```cmd
netsh interface ipv4 set address name="Ethernet" source=dhcp
```

<!-- /os -->

<!-- os:linux -->

```sh
sudo ip address del 10.0.0.2/8 dev enp3s0
```

在跑 dnsmasq 的终端按 `Ctrl+C`。

<!-- /os -->

<!-- os:mac -->

```sh
sudo launchctl unload -w /System/Library/LaunchDaemons/tftp.plist
sudo ifconfig en5 inet 10.0.0.2 -alias
```

<!-- /os -->

**原来就是静态地址的，不要跑 DHCP 命令。** 按第 3 节抄下的五项逐项填回：地址与掩码、网关、DNS、是否自动获取。然后关掉临时热点，或把路由器恢复成原来的 Wi-Fi 名称和密码；撤销本次的防火墙放行。

日常升级到[下载区]({{SITE_URL}}#releases)取最新兼容稳定版，步骤见[固件更新](firmware-update)；不需要再发一次触发文件。

完成标志：电脑能正常上网，浏览器能打开路由器管理页和 `http://192.168.0.1/`。

刷机失败、或设备停在配网模式起不来，按[有线刷机与恢复](tc1-flash)处理。原厂槽没有被改动，走这条恢复路径。

## 常见问题

| 现象 | 先做这一个动作 |
| --- | --- |
| 脚本报连不上 `10.10.10.1:8000` | 回第 5 节：无线还连着 `Phicomm_TC1_XXXX` 吗，拿到 `10.10.10.x` 了吗 |
| 手工发 curl 时报找不到 payload | 在 `tc1-flash` 里执行，确认文件名不是 `tftp-trigger.bin.txt`，且文件与第 5 节读到的版本对应 |
| TFTP 没有下载请求 | 查第 2 节的四项热点设置、第 4 节的服务绑定与防火墙 |
| TFTP 传到一半停住 | 查会话端口是否被拦、电脑是否休眠、网线是否松动 |
| 传输完成后没有新热点 | 等 1～2 分钟，看 TFTP 日志里 `mico_ota.bin` 是否传完 |

网络拓扑、自备镜像与原理见[免拆原理与自备镜像](tc1-no-disassembly)；需要下载器恢复见[有线刷机与恢复](tc1-flash)。
