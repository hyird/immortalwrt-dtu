# edgenode for OpenWrt

This directory contains a small C daemon and an OpenWrt package recipe. It has no C++
runtime, full protobuf runtime, or database dependency. This package repository is the
sole source location for the OpenWrt node implementation and its node-side tests.

## 本地故障诊断（0.3.68）

- `/tmp/edgenode/logs/acquisition.log` 默认记录全部六种采集协议：SL651、Modbus RTU/TCP、S7、MC、FINS、DLT645。记录配置应用、连接和握手、帧解析、测点不完整、读错误、报告构造和 Worker IPC、父进程 outbox 入队／发送及 ACK 超时；写命令失败仅记录结果及“已开始／已确认”标志，不把超时记成确定未执行。被动 SL651 收帧损坏、缓冲区溢出、拼包与发送失败也记脱敏原因码。DTU、VPN、固件升级、蜂窝模块及网络配置的可观测操作失败另有固定事件码。不会为了记日志增加 PLC 请求、自动重发写或更改协议行为。
- TSV 字段依次为毫秒时间戳、平台 UUID、设备 UUID、协议码（0=非设备事件，1=SL651，2=Modbus，3=S7，4=MC，5=FINS，6=DLT645）、固定事件、原因码、点数、帧数。`io-*` 原因码为 0 正常、1 无响应、2 离线、3 协议错误、4 写命令开始超时；`report-*` 原因码为 0 Worker 已入队、1 帧数超限、2 内存分配失败、3 空记录、4 入队失败、5 无法解码；`config-applied` 的后三列为扫描间隔（毫秒）、上报间隔（秒）、传输码（1=以太网，2=串口）；Modbus 可据此区分 TCP 与 RTU。`parent-outbox` 原因码 0 表示节点本地 outbox 已接收，1/2/3 分别为无效遥测、封装失败、落盘入队失败，4/5/6 分别为命令结果无效、封装失败、落盘入队失败；它不代表平台已持久化或已 ACK。`sample-incomplete` 的原因码是缺失点数；`protocol-decode` 对 Modbus/S7 读响应保留 `(解析结果 << 8) | 异常码`，工业协议 1/2 表示无效响应／长度超限，3 为 Modbus 点提取失败，4/5 为 S7 COTP／握手响应无效，7 为写点不存在。`capture-failed` 的 1/2 为帧长度超限／内存不足。
- `sl651-frame`、`sl651-buffer`、`sl651-report` 的原因码为 1 帧无效、2 缓冲区溢出、3 分配失败、4 报文内容无效、5 发送失败、6 等待超时。`network-failed` 的 1/2/3 为平台连接／应用握手／ACK 超时，4 为 SL651 链路断开，5/6 为采集 Worker 异常退出／看门狗重启，8 为串口配置失败；WebSocket 关闭则记录标准关闭码。`outbox-send` 的 1 为发送失败、2 为 ACK 超时，`outbox-corrupt` 的 1 为本地消息无法解码，`outbox-evicted` 的 1 为容量上限下丢弃旧消息。`command-failed` 的原因码高位为内部写结果编号、低两位分别表示是否已发起南向写和是否收到写 ACK，**不能**由超时推断 PLC 未执行。
- 仅写节点本地日志，不自动上传，也不由节点日志 API 读取。记录不含 PLC 原始报文、测点值、地址、设备名、网络端点或凭据。同一设备和原因的持续失败及连续的成功快报最多每分钟记一次（故障恢复立即记录，合并重复事件，不保证逐次计数）；单文件不超过 256 KiB、最多四份。`/tmp` 余量不足时停止写入，重启会清空，因此无法承诺记录内核故障、异常断电或设备尚未升级前的历史错误。节点已入队、平台收到和数据库确认须分别以本地 outbox 和平台证据验证。

## 到期报告持续尝试（0.3.66）

- 主动采集到达普通上报期限后，只有本轮读到完整新点集并成功写入节点上报队列，才上报一次并从该成功时刻重新计算 `report_interval_sec`；本轮读取失败、写任务占用或入队失败时保留到期状态，下一采集轮继续尝试，不发送残缺或缓存的旧点集，不累积补报次数。
- 快读窗口内的到期轮同样只在完整新点集成功入队后从成功时刻计下一次快读间隔；窗口结束后不继续补发过期快读。持续无法读取或入队时无法承诺定时成功上报，需结合节点故障与队列状态排查。
- 节点上报队列的成功入队不等于平台已经确认：断线后的持久化、重发与确认仍按原 outbox 协议执行；写指令优先、不重放写及已部署固件的兼容协议不变。旧 0.3.65 仍可能在到期读失败后跳过当轮上报，必须使用本版本固件才能获得新调度行为。

## 命令启动截止与 S7 采集修复（0.3.64）

- S7 TCP 读取超时后立即重连并完成握手，在同一采集周期最多重读一次；重读失败或点集不完整时不发布该轮点集，留待下一主动读取周期。
- `command_attempt.deadline` 原值作为启动截止，不因节点接收或排队而续期。只采用 nonce 关联且 RTT 不超过 2 秒的数据库时钟样本；样本缺失、映射满 330 秒、关联不符或观测到样本跳变即失效关闭。换算仅在样本间数据库时钟稳定（相对单调时钟速率误差不超过 1000 ppm）时成立，不能保证未观测到的时钟跳变或设备实际执行时刻。
- 未发送且已过期或无法验证的命令拒绝（`REJECTED`）；可能已发送但无 ACK、或已获 ACK 而回读失败均记为 `UNKNOWN`，不自动重放。
- 启用该能力前须先升级平台。保留 `0.3.44` 兼容路径，但该旧固件不提供新截止保证。

## 主动轮询与遥测调度（0.3.63）

- 主动 Modbus RTU/TCP、S7、FINS、MC、DLT645 使用 `EDGE_ACQUISITION_TICK_MS=1000ms` 为目标读取间隔；平台下发更慢的 `io_interval_ms` 不再降低读取频率。该值是调度目标，不是严格实时承诺；超时、共享物理链路和平台优先级会造成延迟。迟到调度跳过过期轮次，不补跑堆积读取。
- `report_interval_sec` 独立控制普通遥测。配置生效后的首次成功读取不立即上报；首次也要等普通到期轮，并且只上报该轮新读取成功的完整样本。读失败、无连接或写命令占用到期轮时不拿旧样本补报。
- Worker 在每次开始下一项读取前先有限量消费控制队列，并按既有平台优先级顺序优先执行已入队写任务；每轮最多插队 4 项写任务，随后仍处理当前扫描索引（同一设备刚执行过写时跳过该设备的重复普通读），不会因写任务跳过其他设备或被动 SL651 收帧。预算耗尽时，仍有待写命令的设备不执行普通读。单 Worker 的同步协议交换不可抢占：已发出的请求必须先完成或超时，之后才能写入；写请求只发送一次，不自动重放。共享物理资源仍由同一 Worker 串行操作，SL651 被动确认、DTU 透传及平台优先级规则不变。
- 经验证的写命令可以启动快读窗口；快窗口仅在 fast due 轮成功取得新样本后上报。写入仍即时返回命令结果，不因写后回读额外发送自动遥测，也不会把写入占用到期轮的旧遥测补报。
- 静默扫描不发自动 RawPacket、解析值或报文 debug；必要故障状态和命令结果即时上报，到期轮和显式人工调试按原路径处理。S7 TCP Client 持连及断线重连、共享串口资源和平台优先级保持原机制。
- 被动 SL651 的确认、分包和主动上送，DTU 透传及串口监听不属于主动轮询；其既有行为不变、不受此扫描节奏约束，不丢帧。协议字段、固件身份及 0.3.44 升级兼容路径保持不变。
- 行为变化：首次遥测延迟至 `report_interval_sec` 的首个到期轮，间隔较长时平台不再获得配置后立即样本；需要首轮遥测时应配置较短报告周期。此版本保持协议字段、固件身份和 0.3.44 兼容升级路径。

## 临时日志级别

- 节点启动、进程重启默认 `silent`；不产生新的 EdgeNode 应用日志，既有日志仍可按需查询。
- 设置 `debug`、`info`、`warn`、`error` 后，使用单调时钟授权 300 秒；重复设置相同级别也重新计时。
- 到期后的每次写入检查均拒绝新日志，级别查询返回 `silent`，不依赖平台、网络、心跳或浏览器存活。
- 设置按节点生效，多个平台最后一次成功设置为准。主进程原子发布 `/tmp/edgenode/log-level`，采集子进程通过本地 tmpfs 获取同一期限；级别变更传播最多约 1 秒，读取设置不会延长截止时间。
- 级别通过已有 Hello/Heartbeat 上报，平台不会在浏览器中猜测已恢复；界面可能要等下一次心跳才显示 `silent`。不为日志到期另建网络轮询。
- 仅升级后的固件支持自动静默；旧固件的四种日志级别、请求及响应继续保留，设置其不支持的 `silent` 会返回失败，不伪装成功。
- 静默不删除遥测、原始采集报文、命令结果、终端及串口调试数据，也不修改系统其他进程的日志策略。

## 平台连接节流

- 0.3.65 通过 Hello 的 `supports_status_reporting` 声明只定时上报状态和流量，新平台协商周期为 900 秒。
  没有应用 Ping 或连接后的应用心跳超时；现有 Heartbeat/HeartbeatAck 字段保留为报告与确认，兼容旧平台。
- libwebsockets 原生 WS Ping/Pong 在空闲 300 秒后探活，360 秒内没有有效流量则断开。
  首次 Hello 握手仍有截止时间；未声明新能力的旧固件保留服务端原有保活时序。
- 连续失败指数退避至 300 秒（若配置初始间隔更长则保留），稳定通信 300 秒后复位。
  待审批响应结束初始握手等待，后续按五分钟心跳等待原连接获批。
- WG `persistent_keepalive` 为 120 秒，重新应用 VPN 配置后生效；移动 NAT 的空闲入站
  可达性需要现场验证。采样、原始报文、单记录 ACK 和必要失败重传保持不变。

## VPN 按需启用及平台隔离（0.3.65）

- 平台 Web 开启的是 VPN 能力：提前分配节点地址和虚拟网段。只有已授权的 Windows 客户端开启 VPN 并选中节点，平台才下发启用任务；最后一个客户端停止使用后下发关闭任务，分配结果保留。
- 每个平台拥有独立 network namespace、WireGuard 密钥、配置版本、路由、nftables/conntrack 及流量确认状态，允许不同平台使用相同的 Overlay 和虚拟网段。平台 UUID 使用完整 128 位标识，最多四个平台。
- WireGuard UDP socket 在物理网络 namespace 创建后把接口移入所属平台，以共享物理 WAN；通过独立 veth /30 和平台专属源地址连接 LAN。命名空间内先做虚拟网段映射，再做中转 SNAT，物理网络最终使用 LAN 接口地址访问设备，避免多个平台的相同客户端地址混淆返回路径。
- 关闭或失去平台连接只回收所属平台的隧道和防火墙配置，保留密钥。进程启动清理自身遗留 namespace/veth，并迁移旧的受管共享 wg 配置；旧密钥保留用于回退。需要内核 NET_NS、kmod-veth 和 ip-full，必须使用完整新固件镜像。
- 中转 SNAT 对 NAT 和 routed 两种映射均生效；routed 不做网段前缀转换。LAN 设备看到的是节点 LAN 地址，无法直接区分平台客户端。

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

## 采集上报按编码大小分片

- 普通采集报告不超过 14,000 字节时仍整条发送；超过后按实际 Protobuf 编码大小装入分片，不再固定每 8 个点或每 2 条原始帧拆包。每片不超过 14,000 字节，保留 Envelope 的传输空间，最多 256 片。
- 先预检整个报告，再开始入队；单项无法装入或分片数量超限时失败，不截断。所有点值、名称、单位、原始帧、帧身份及各自顺序保留；不改变采集周期、存储策略、ACK、outbox 和重传语义。
- 使用原有 `report_id`、`part_index`、`part_count` 重组契约，不新增协议字段，不改旧固件路径。分片只借用原报告内存，编码入队后释放的职责不变。
- 合成测试：96 个字符串点值与 32 条原始帧，共 30,079 字节，从旧算法 28 片减少到 3 片；新分片记录共 30,283 字节（不含 Envelope、ACK、WS/TCP 等开销）。这不是线上总流量节省比例，也不代表当前小报告会减少流量。
- Windows Release 主机测试 17/17 通过，覆盖分片编解码、逐项无损比较、边界、旧记录无帧 ID、超大单项拒绝。MIPS 对象检查：协议文件通过 `-Wall -Wextra -Werror`；采集文件在未改动的 `NLMSG_OK` 处触发符号比较警告，禁用该项警告后对象编译通过。完整 OpenWrt 镜像、平台实际重组、断线回放及真实网络节流效果仍待验证；尚未发布。

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
- 采集 Worker 每秒检查调度；主动 Modbus RTU/TCP、S7、FINS、MC、DLT645 均以 1000ms 为目标读取周期，忽略更慢的 `io_interval_ms`，迟到轮次跳过且不追赶。普通遥测独立遵循 `report_interval_sec`，首次也只在首次普通到期且当轮读取成功时上报；命令结果即时返回。IPC 由 `ev_io` 就绪事件消费，设备 I/O 不阻塞 WebSocket 事件循环；
- platforms may share a physical serial channel and use different baud/parity settings;
  the worker drains the prior request, applies the next task's serial settings, clears
  stale input, observes the RTU quiet interval, and then performs that task. TCP Server
  endpoints sharing a listen port use one listener. Lower numeric platform priority is
  scheduled first, while every platform task remains active;
- Modbus TCP/RTU and S7 request/response codecs implement reads and writes. Command results
  (including write readback) are reported immediately; a verified command then reports only
  newly sampled data at the configured fast-read interval for the configured window before
  returning to the regular interval. A successful command requires readback equality;
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

## 原始业务报文上报

新平台通过 HelloAck.raw_telemetry 启用原文格式后，采集遥测只包含时间、设备 ID 与成对的业务请求和响应，不上传测点解析值和派生值。raw_requests 与 raw_payloads 按下标对应；SL651 主动上报的请求为空，查询响应保留实际业务请求。握手、注册、心跳和异常包仅在调试开启后使用 RawPacket 上报。Modbus、S7、MC、FINS、DLT645 的正常轮询跳过数值转换，协议匹配和校验继续在节点执行。旧平台未启用新格式时，保持原有上报；不同平台独立协商。

推送前已合并最新 `origin/main`，保留命令时限、状态上报、libwebsockets 与平台 VPN 隔离实现。Windows Release 主机测试 17/17、Linux 六项协议与采集相关测试 6/6 通过；`edge_protocol.c`、`edge_acquisition.c`、`edge_ws.c` 使用指定 MIPS 工具链完成对象编译。对象检查沿用包的 `-Wno-strict-aliasing`，另抑制既有 `NLMSG_OK` 符号比较与升级状态日志格式截断警告；完整镜像、刷机及真实硬件联调仍未覆盖。
