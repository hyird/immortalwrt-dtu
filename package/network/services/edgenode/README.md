# edgenode for OpenWrt

This directory contains a small C daemon and an OpenWrt package recipe. It has no C++
runtime, full protobuf runtime, or database dependency. This package repository is the
sole source location for the OpenWrt node implementation and its node-side tests.

## S7 TCP 采集（0.3.60）

- 仅 S7 TCP Client 在一轮全部点位读取及必要的写后回读结束后关闭 TCP；下一轮采集或新的命令重新建立 TCP、COTP 和 S7 会话。串口、TCP Server 和其他协议不改连接生命周期。
- 首次读取超时或响应无效时，关闭原连接并在同轮最多重新握手重读一次；写命令不自动重放。失败后保持原定采集间隔，不做周期探活或额外业务轮询。
- 主动关闭后的空闲状态保留最近一次成功采集的逻辑连接状态，但客户端连接数为零；后续 I/O 失败仍报告重连状态和原因。

## 临时日志级别

- 节点启动、进程重启默认 `silent`；不产生新的 EdgeNode 应用日志，既有日志仍可按需查询。
- 设置 `debug`、`info`、`warn`、`error` 后，使用单调时钟授权 300 秒；重复设置相同级别也重新计时。
- 到期后的每次写入检查均拒绝新日志，级别查询返回 `silent`，不依赖平台、网络、心跳或浏览器存活。
- 设置按节点生效，多个平台最后一次成功设置为准。主进程原子发布 `/tmp/edgenode/log-level`，采集子进程通过本地 tmpfs 获取同一期限；级别变更传播最多约 1 秒，读取设置不会延长截止时间。
- 级别通过已有 Hello/Heartbeat 上报，平台不会在浏览器中猜测已恢复；界面可能要等下一次心跳才显示 `silent`。不为日志到期另建网络轮询。
- 仅升级后的固件支持自动静默；旧固件的四种日志级别、请求及响应继续保留，设置其不支持的 `silent` 会返回失败，不伪装成功。
- 静默不删除遥测、原始采集报文、命令结果、终端及串口调试数据，也不修改系统其他进程的日志策略。

## 平台连接节流

- libwebsockets 使用显式零空闲策略关闭周期 WS Ping，应用心跳遵守平台协商值，本平台协商为 300 秒。
  保留握手超时及对端 Ping 的 Pong，不使用库默认的 40 秒 Ping。
  应用看门狗上限为 900 秒，正常心跳计时器运行时不另发应用 Ping。平台仍保留空闲时才触发的 WS 探活。
- Hello 的可选 `supports_sparse_heartbeat` 声明新时序；服务端对未声明能力的旧固件
  保留原看门狗兼容时序。协议版本及既有字段编号不变，不能仅升级服务端便停掉旧节点保活。
- 连续失败指数退避至 300 秒（若配置初始间隔更长则保留），稳定通信 300 秒后复位。
  待审批响应结束初始握手等待，后续按五分钟心跳等待原连接获批。
- WG `persistent_keepalive` 为 120 秒，重新应用 VPN 配置后生效；移动 NAT 的空闲入站
  可达性需要现场验证。采样、原始报文、单记录 ACK 和必要失败重传保持不变。

## WebSocket 压缩

- `0.3.59` 的种子配置开启 `CONFIG_MBEDTLS_ECP_DP_SECP521R1_ENABLED`，用于解析系统
  CA 包中的 P-521 根证书；不关闭证书或主机名校验。`0.3.58` 存在 CA 包加载失败问题，
  不应继续用于升级。主机验证中，原配置解析 120 张、失败 1 张；开启后 121 张全部成功。

- 使用官方 feeds 的 `libwebsockets-mbedtls`，通过 `scripts/libwebsockets-edgenode.patch`
  开启 zlib、permessage-deflate 和内置 libev；不继续扩展 libuwsc。
- `scripts/libwebsockets-pmd-final.patch` 回移上游 `f41c8e66b0989c60c22af68d294cea9509b7f9fa`
  的 11 行库修复，解决 [#3660](https://github.com/warmcat/libwebsockets/issues/3660)
  中解压未完成却提前报告消息结束的问题；由 feeds 安装脚本接入标准 OpenWrt 补丁阶段。
  libwebsockets 包版本为 `4.5.8-r2`。未经修复的 `0.3.57-r2` 固件不应升级使用。
- 通过 `LWS_SERVER_OPTION_LIBEV` 与 `foreign_loops` 复用 EdgeNode 已有事件循环；
  不另建线程或事件循环，不维护 external poll 桥接，也不增加定时调用 `lws_service` 的驱动。
  库负责自身 socket、TLS、压缩及超时调度，应用保持采集、ACK、重连等原有职责。
- 握手提供标准 `permessage-deflate`，不要求 `no_context_takeover`；发送压缩级别为 9。
  字典仅在各平台各连接内部复用，断线销毁，无固定字典、定期轮换或业务消息白名单。
- 压缩遵守服务端协商结果：服务端禁用上下文复用时不能擅自复用；未协商扩展则使用原协议。
  要实现双向 level 9 和跨消息复用，平台也必须支持对应协商及编码策略。
- 所有业务消息经同一压缩路径；Ping/Pong/Close 控制帧不压缩。接收分片先有界重组再解码。
- 每个平台拥有独立 LWS context、256 KiB 有界待发队列及接收缓冲；网络写入仅发生在 writable
  回调内。outbox 仍以应用 ACK 确认，入队不代表服务端确认，断线按原规则重放。
- 不改变采集间隔、原始数据、协议字段、升级流、平台配置及 WS/WSS 证书校验策略。

### 传输回归测试

主机测试开启 `EDGENODE_WS_TRANSPORT_TESTS=ON`，需要带压缩和原生 libev 的 libwebsockets、
libev 开发文件及 Python 3；交叉编译固件不能替代运行该测试。
`ctest --test-dir <主机测试构建目录> -R '^edge_ws_transport$' --output-on-failure`
验证真实 TCP 线帧：未协商压缩、禁止字典复用、允许字典复用，逐字节对比 level 9 编码，
压缩分片重组、穿插 Ping/Pong、Close 不压缩和外部事件循环销毁边界。
测试不访问生产平台，不刷机；WS 线帧通过不代表 WSS、应用 ACK/outbox 或实机验收完成。

## 全链路冗余抑制

- `edge_report.c` 只缓存能力、设备状态及不含 trace 的 DTU 状态，比较实际 Protobuf 内容，不因 Envelope 的 UUID、时间、序号变化重复发送；不使用结构体 `memcmp` 或有碰撞风险的摘要替代内容比较。
- 缓存按平台会话隔离，传输接受后才更新；重连、明确补报强制发送。保留每会话首次配置后的能力兼容补报，后续配置及网络确认仅补发改变的能力。
- 设备活动时间、DTU 字节计数变化仍立即发送；含 trace 的状态、原始报文、遥测、心跳、ACK、命令及升级结果不参与快照去重。遥测可能更新设备状态，所以发送遥测后清除独立设备状态缓存，避免状态回退被错误抑制。
- 单条 outbox ACK 超时先只补发该记录，原记录 ID 和载荷不变；最多在原连接重试两次，每次仍等待 60 秒。耗尽后恢复连接，不删除未确认记录，也不重发未超时的其他记录。
- WS 写缓冲达到 64 KiB 时暂停继续填充 outbox；已有本地采集计时器在缓冲疏通后继续排空，没有待发记录时不产生网络包。
- 相同活动配置版本与摘要的重放不重新应用、不重启采集、不重复关闭调试；收到 commit 补回 `ConfigApplied`。同版本不同摘要拒绝，新版本仍完整校验、落盘并应用。
- 升级重复分块或跳号不再强制立即重复拉块，按既有重试期限恢复；收到真正推进 offset 的块才立即请求下一块。分块大小、校验、回滚、旧固件下载路径不变。
- 日志按需查询，串口与终端保留活动会话、续租、顺序和背压。采集报文调试仍由显式 `debug_enabled` 控制，不把配置开启的持续抓取误删为重复数据；第三方 DTU/S7/Modbus 等协议的注册、心跳及重传保持原契约。

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
- 采集 Worker 每秒检查调度，但实际读取遵守设备配置间隔。`io_interval_ms=0`
  时使用 `report_interval_sec`，非零时接受 1000–3600000 ms；控制命令无需等待
  下一次周期读取。各平台独立上报，配置生效后的首次成功读取立即上报，随后按配置周期
  执行。IPC 由 `ev_io` 就绪事件消费，设备 I/O 不阻塞 WebSocket 事件循环；
- platforms may share a physical serial channel and use different baud/parity settings;
  the worker drains the prior request, applies the next task's serial settings, clears
  stale input, observes the RTU quiet interval, and then performs that task. TCP Server
  endpoints sharing a listen port use one listener. Lower numeric platform priority is
  scheduled first, while every platform task remains active;
- Modbus TCP/RTU and S7 request/response codecs implement reads and writes. Every completed
  write readback is reported immediately; a verified command then reports at the configured
  fast-read interval for the configured window before returning to the regular interval.
  A successful command requires readback equality;
- S7 读取超时后立即关闭 TCP，重新连接并完成 COTP、S7 Setup Communication，
  在同一采集周期重读一次；重连或握手失败、重读再次超时或断线时清理连接状态，等待下一配置采集周期。
  每周期最多立即重读一次，不改变原采集周期，不自动重发写入命令；其他协议行为不变。
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
OpenWrt's mbedTLS-backed libwebsockets.

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
