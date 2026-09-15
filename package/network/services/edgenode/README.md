# edgenode for OpenWrt

This directory contains a small C daemon and an OpenWrt package recipe. It has no C++
runtime, full protobuf runtime, or database dependency. This package repository is the
sole source location for the OpenWrt node implementation and its node-side tests.

Implemented foundations:

工业协议（0.3.46）：新增 MC/SLMP 二进制 3E/4E、FINS/TCP 和 DL/T645 1997/2007。
MC、FINS 使用以太网；DL/T645 支持串口及 TCP 透传。协议共用原有物理 I/O 调度、
一秒采集周期、按设备类型配置的上报周期、单点写入与回读比较、命令结果及报文追踪。
FINS 在建立连接时协商节点地址；超时后重建连接，迟到报文不用于确认下一条命令。

点位数据类型与平台保持一致。DL/T645 支持有符号/无符号 BCD、HEX、后续帧读取，
BCD 使用精确十进制字段上报，避免浮点舍入；写入认证字节从配置下发，发送日志隐藏
密码及操作者代码。一次点位读取累计报文最多 4096 字节，一轮原始报文最多 512 帧，
超限拒绝上报，不截断数值。DL/T645 地址为 12 位十进制表号，数据标识为 4/8 位十六进制。

`CapabilityReport.supported_protocols` 声明实际支持的六种协议；新增字段及枚举保留旧编号，
协议版本仍为 6，保留 0.3.44 升级兼容路径。平台应依据能力声明下发新协议。
主机测试覆盖 MC 3E/4E、FINS 节点协商、DL/T645 两个版本的真实 TCP 读取与写后回读，
以及畸形报文、校验和、BCD 精度及越界拒绝。实际 PLC、电表和串口硬件互通仍需现场验收。

SL651（0.3.45）：串口及 TCP Client/Server 接入支持 HEX/BCD M1–M4；M2 的
ETB/ETX 分别返回 ACK/EOT，M3 按 SYN 序号重组、逐个 NAK 请求缺包，M4 支持
ENQ 查询与连续应答。确认须晚于本平台 tmpfs outbox 写入成功，最终 EOT 还须
等待命令结果入队。tmpfs 不提供断电持久性，空间不足时仍遵循既有 outbox 淘汰策略。

要素支持引导符和固定位置。`fixed_position=true` 时 `byte_offset` 从重组后正文
的流水号首字节算起（0 基），偏移 8 跳过流水号和发报时间；固定位置必须有正长度，
不要求引导符。混合配置先放固定字段区，再放引导符区。下行固定字段不得覆盖偏移
0–7，字段不能重叠；空隙填 0。普通及 FF 扩展引导符校验长度和小数位，名称及业务
功能码不设白名单。`response_element` 是查询应答的解析配置，不决定 ACK/EOT。

同一物理测站只发一份确认，各平台分别解析、入队，全部成功后确认。共享测站必须
使用一致模式；SL651 被动串口不能与轮询协议或不同串口参数混用。查询超时重试两次，
随后隔离该连接的同一测站，重连恢复；已确认响应不完成后续新查询。

运行上限：每帧正文 4095 字节，4095 个分包，重组正文 64 KiB；单个二进制 HEX/JPEG
要素 8 KiB，单次解析二进制总量 64 KiB，最多 128 个出站记录。大要素按二进制字段
上传，平台还原 HEX 或 JPEG data URL；超限不截断、不发送成功确认。ASCII 及非纯 BCD
站址未实现，完整标准的所有业务语义不等同于此传输实现。协议版本仍为 6；新增字段均为
可选扩展，不修改 0.3.44 原字段编号，也不移除旧固件下载和升级路径。

本地主机验证包含 Linux 19 项、Windows Release 14 项；真实 TCP 测试覆盖入队失败不
确认、命令结果提交边界、8 KiB 图片三包重组；协议测试覆盖 CRC、缺包 NAK、重复包、
超时隔离以及配置默认值和越界拒绝。实际测站互通仍需用目标设备验收。

- the node registers independently with up to four platforms using its 15-digit IMEI;
- an HTTP or HTTPS platform base address is upgraded internally to a binary WS or WSS
  session carrying one nanopb `Envelope` per message; HTTP/WS is unencrypted and intended
  only for trusted or temporary networks;
- WSS validates both the certificate chain and hostname against OpenWrt's `ca-bundle`;
  TLS initialization and verification failures fail closed;
- every platform has isolated registration, config, reconnect, heartbeat, and outbox state;
  failed connections retry forever; a 30-second application handshake deadline, an
  enrolled-session watchdog, and a 60-second outbox ACK deadline break half-open sessions;
- config and outbox files are raw nanopb messages under
  `/tmp/edgenode/<platform_id>/`; process restarts recover them, device reboots do not;
- before every tmpfs write, the daemon preserves 15% free space by rolling the oldest
  outbox message across all platforms; active and staging config are never rolled;
- one supervised application-level acquisition worker reads every second, processes
  queued writes in that same cadence, and reports each platform independently at its
  configured interval; the first successful read after a configuration becomes active
  is reported immediately before that interval begins. IPC is consumed by `ev_io`
  readiness events so device I/O never blocks the WebSocket event loop;
- platforms may share a physical serial channel and use different baud/parity settings;
  the worker drains the prior request, applies the next task's serial settings, clears
  stale input, observes the RTU quiet interval, and then performs that task. TCP Server
  endpoints sharing a listen port use one listener. Lower numeric platform priority is
  scheduled first, while every platform task remains active;
- Modbus TCP/RTU and S7 request/response codecs implement reads and writes. Every completed
  write readback is reported immediately; a verified command then reports at the configured
  fast-read interval for the configured window before returning to the regular interval.
  A successful command requires readback equality;
- an unresponsive S7 PLC closes the TCP socket and repeats TCP, COTP, and S7 Setup
  Communication on the next one-second cycle.
- network and serial capabilities are reported automatically; the built-in PTY bridge
  exposes an authenticated remote terminal without a separate terminal daemon;
- commands from any enrolled platform can create, update, or delete UCI logical interfaces
  backed by one physical device or a bridge, using DHCP or static IPv4. The configured
  4G/WAN interface and its descendants are excluded, and an unconfirmed network change
  restores the previous UCI network configuration automatically. Only the initiating
  platform can confirm its transaction, while apply, confirmation, and rollback results
  are broadcast to every enrolled platform;
- any enrolled platform can stream verified firmware through its authenticated WS/WSS
  session and invoke `sysupgrade`; network and firmware mutations are serialized.
  Bootstrap-only modem and terminal privileges remain unchanged.
- the node advertises kernel WireGuard capability and keeps its private key in
  `/etc/edgenode/vpn.key`; an enrolled platform can apply a versioned Edge VPN
  configuration over the existing authenticated WS/WSS control channel. The data
  plane stays in the kernel, while nftables provides the declared LAN DNAT and
  MASQUERADE rules.

The active-config-to-physical-endpoint binding is kept separate from the wire/session
layer. The current code provides the tested protocol codecs and scheduler that binding
uses; actual target hardware is still required before declaring a target deployable.

## Firmware transfer trust

The platform sends firmware in bounded chunks over the node's existing authenticated
WS/WSS session. The node pulls one offset at a time, rejects gaps and overlaps, retries
the current offset after reconnects, and never receives or opens a direct firmware URL.
The requested size, SHA-256 digest, and `sysupgrade -T` image validation remain mandatory.
The platform keeps its tokenized direct-download route for deployed legacy firmware;
only new builds advertising WS firmware streaming use this transfer path.

## Edge VPN

The VPN control messages are additive protobuf fields, so older platforms and nodes
continue to use the existing payloads. A capable node creates the `wg` logical
interface and its `iot_server` peer through OpenWrt UCI/netifd, assigns its `/32`
overlay address, adds the `wg` network to the existing `lan` firewall zone, and
loads the subnet mapping rules through firewall4 nftables includes. Only enabled, equal-prefix
private-LAN mappings are accepted; no `0.0.0.0/0` route or inner packet is sent over
the WebSocket. The device private key is generated from `/dev/urandom`, stored with
mode `0600`, and never included in protobuf or logs.

The package depends on `kmod-wireguard` and `wireguard-tools`. It does not install
`wireguard-go`, `wg-quick`, a second VPN daemon, or a separate LuCI VPN page; VPN controls
remain on the Edge Node page in the platform UI.

## Runtime observability

The node separates operational events from continuously changing state:

- the runtime event log contains lifecycle changes, configuration outcomes, transport
  changes, and failures; it is not a per-cycle activity journal;
- successful polling and telemetry enqueue operations do not create log entries;
- current delivery pressure is reported by the heartbeat `outbox_records` and
  `outbox_bytes` fields instead of repeated success messages;
- command, configuration, network, and firmware outcomes remain in their typed protocol
  results and platform task history;
- raw protocol packets are available only at `debug` level.

## Configure

Platform addresses are not compiled into the daemon. Every connection comes from a
`config platform` UCI section. A fresh install creates the default platform
`https://i.a-z.xin`; it can be edited in LuCI or with UCI, and up to four platforms can
be enabled at the same time. The daemon maps `http://` to `ws://`, maps `https://` to
`wss://`, and derives the internal upgrade path `/edge/v1/connect` for each base address.
Do not append that path to the configured URL. HTTP/WS sends credentials and telemetry
without transport encryption.

The default IMEI and model are empty: the init service reads IMEI from the modem and
model from `/tmp/sysinfo/model` before starting the platform client. To override either
value explicitly, use UCI and restart the service:

```sh
uci set edgenode.node.imei='your-15-digit-imei'
uci set edgenode.node.model='your-model'
uci commit edgenode
/etc/init.d/edgenode restart
```

The default platform is ordinary persistent UCI configuration:

```sh
uci set edgenode.bootstrap.url='https://i.a-z.xin'
uci commit edgenode
/etc/init.d/edgenode reload
```

Install `luci-app-edgenode` from the companion LuCI feed to add, edit, enable, disable,
order, or remove platform sections in **Services → Edge Node**. LuCI generates the
internal connection ID and never asks the user to enter it. The default ID
`00000000-0000-7000-8000-000000000001` remains the bootstrap platform for modem and
terminal privileges, but network management and firmware upgrade are available to every
enrolled platform.

New nodes enter the platform's manual approval flow by IMEI. Approval upgrades that
connection to an enrolled session. A connection that never receives approval is
closed at the application handshake deadline and retried, so a half-open or stale
pending session cannot stop reconnection.
Additional platform sections may be managed locally through LuCI/UCI or by authenticated
commands from the default platform; the node applies remote changes through
`uci set/delete` and `uci commit`.

The LuCI page also provides a read-only runtime dashboard. It reports both procd
instances, each platform's WS enrollment and heartbeat age, outbox pressure, active and
staging revisions, recent platform tasks, and the complete active acquisition snapshot
(endpoints, devices, read/report intervals, fast-read windows, and Modbus/S7 points).
Runtime status is written atomically to tmpfs and contains no platform credentials or
command payload values.

## Build an IPK

`net/edgenode` is a complete OpenWrt package directory. Copy or link this whole
directory into the selected SDK as `package/edgenode`; do not copy only its `Makefile`:

```sh
ln -s /path/to/openwrt-dtu-packages/net/edgenode /path/to/openwrt/package/edgenode
make menuconfig
make package/edgenode/compile V=s
```

The package is self-contained apart from dependencies fetched by the OpenWrt build
system: `Makefile`, `files/`, `proto/`, and `src/` stay together.
`proto/edge.proto` is the node-side wire-contract source and must stay byte-identical
to the platform copy in `iot-engine/service/features/edge/edge.proto`.
The OpenWrt SDK uses the committed nanopb C sources when compiling.
The recipe downloads nanopb `0.4.9.1`, compiles only its three C runtime files, enables
`-Os`, LTO, function sections, and linker garbage collection, and dynamically uses
OpenWrt's mbedTLS-backed libuwsc.

The package, daemon, init service, UCI configuration, and runtime paths are all named
`edgenode`.

The nanopb C files generated from `proto/edge.proto` are committed under `generated/`.
This keeps the OpenWrt 18.06 build independent of host Python and protobuf packages.
Regenerate both files with nanopb 0.4.9.1 whenever the protocol changes.

Node-side protocol, runtime, and configuration tests are kept under `tests/` beside
the implementation instead of in the platform repository. They can be run on a host
with the exact nanopb source used for generation:

```sh
cmake -S tests -B build/tests -DNANOPB_ROOT=/path/to/nanopb-0.4.9.1
cmake --build build/tests
ctest --test-dir build/tests --output-on-failure
```

Hardware paths, interface names, bridge mode, modem USB ID, AT port, status path, and
monitor interval are UCI settings rather than compiled constants. The TAS-682 package
defaults describe its single field serial port and Ethernet port plus the LierdaComm
modem. The service initializes the model from `/tmp/sysinfo/model`, initializes IMEI and
ICCID from the modem, and keeps registration and signal status in tmpfs without
repeatedly writing flash.

The resulting package must be cross-compiled and installed on the actual target. A host
binary is not an OpenWrt deliverable.
