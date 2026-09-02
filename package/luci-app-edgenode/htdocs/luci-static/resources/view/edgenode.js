'use strict';
'require view';
'require rpc';
'require poll';
'require dom';
'require ui';
'require uci';
'require form';

var callStatus = rpc.declare({
	object: 'luci.edgenode',
	method: 'status',
	expect: {}
});

function newUuid() {
	if (window.crypto && typeof window.crypto.randomUUID === 'function')
		return window.crypto.randomUUID();

	var bytes = new Uint8Array(16);
	window.crypto.getRandomValues(bytes);
	bytes[6] = (bytes[6] & 0x0f) | 0x40;
	bytes[8] = (bytes[8] & 0x3f) | 0x80;

	return Array.from(bytes, function(byte, index) {
		var separator = index === 4 || index === 6 || index === 8 || index === 10 ? '-' : '';
		return separator + byte.toString(16).padStart(2, '0');
	}).join('');
}

function badge(text, state) {
	var colors = state === 'good'
		? [ '#256029', '#edf8ed' ]
		: state === 'warn'
			? [ '#8a5700', '#fff6df' ]
			: [ '#8b1a1a', '#fff0f0' ];

	return E('span', {
		'style': 'display:inline-block;padding:.15rem .5rem;border-radius:.75rem;' +
			'font-weight:600;color:' + colors[0] + ';background:' + colors[1]
	}, [ text ]);
}

function stateLabel(platform, enabled) {
	if (!enabled)
		return badge('已禁用', 'warn');
	if (!platform)
		return badge('无运行数据', 'bad');
	if (platform.enrolled)
		return badge('已连接并注册', 'good');
	if (platform.websocketOpen)
		return badge('等待平台注册', 'warn');
	if (platform.state === 'connecting')
		return badge('连接中', 'warn');
	if (platform.state === 'waiting')
		return badge('等待重连', 'bad');
	return badge('未连接', 'bad');
}

function ageText(seen, seconds) {
	if (!seen)
		return '暂无';
	seconds = Number(seconds || 0);
	if (seconds < 60)
		return seconds + ' 秒前';
	if (seconds < 3600)
		return Math.floor(seconds / 60) + ' 分钟前';
	return Math.floor(seconds / 3600) + ' 小时前';
}

function bytesText(value) {
	value = Number(value || 0);
	if (value < 1024)
		return value + ' B';
	if (value < 1024 * 1024)
		return (value / 1024).toFixed(1) + ' KiB';
	return (value / 1024 / 1024).toFixed(1) + ' MiB';
}

function timeText(value) {
	var number = Number(value || 0);
	return number > 0 ? new Date(number).toLocaleString() : '—';
}

function renderTable(columns, rows, emptyText) {
	var children = [
		E('div', { 'class': 'tr table-titles' }, columns.map(function(column) {
			return E('div', { 'class': 'th left' }, [ column ]);
		}))
	];

	if (!rows.length) {
		children.push(E('div', { 'class': 'tr placeholder' }, [
			E('div', { 'class': 'td', 'style': 'grid-column:1/-1' }, [
				E('em', [ emptyText ])
			])
		]));
	} else {
		rows.forEach(function(row) {
			children.push(E('div', { 'class': 'tr' }, row.map(function(cell) {
				return E('div', { 'class': 'td' }, Array.isArray(cell) ? cell : [ cell ]);
			})));
		});
	}

	return E('div', { 'class': 'table' }, children);
}

function serviceInstance(instance, label) {
	if (instance && instance.running) {
		return E('div', [
			badge(label + '运行中', 'good'),
			E('span', { 'style': 'margin-left:.5rem;color:#666' }, [
				'PID ' + (instance.pid || '—') + (instance.respawn ? ' · procd 守护' : '')
			])
		]);
	}
	return E('div', [ badge(label + '未运行', 'bad') ]);
}

function pointLocation(point) {
	if (point.kind === 'modbus')
		return (point.registerType || '') + ' · 地址 ' + Number(point.address || 0) +
			' · 数量 ' + Number(point.quantity || 0);
	if (point.kind === 's7')
		return (point.area || '') + (point.dbNumber ? ' DB' + point.dbNumber : '') +
			' · ' + Number(point.start || 0) +
			(Number(point.startBit || 0) ? '.' + Number(point.startBit) : '');
	return point.kind || '—';
}

function showPlatformDetails(platform) {
	var config = platform.config || {};
	var endpoints = Array.isArray(config.endpoints) ? config.endpoints : [];
	var devices = Array.isArray(config.devices) ? config.devices : [];
	var points = Array.isArray(config.points) ? config.points : [];

	ui.showModal('采集任务与配置 · ' + (platform.name || platform.id || ''), [
		ui.itemlist(E([]), [
			'连接状态', platform.enrolled ? '已连接并注册' : (platform.state || '未知'),
			'生效 revision', Number(config.revision || 0),
			'暂存 revision', Number(platform.stagingRevision || 0),
			'暂存进度', Number(platform.stagingReceived || 0) + ' / ' +
				Number(platform.stagingItems || 0),
			'端点 / 设备 / 点位', endpoints.length + ' / ' + devices.length + ' / ' + points.length,
			'最近上报', ageText(platform.hasHeartbeat, platform.lastHeartbeatAgeSec),
			'最近平台应答', ageText(platform.hasInbound, platform.lastInboundAgeSec)
		]),
		E('h4', [ '通讯端点' ]),
		renderTable(
			[ '名称 / ID', '协议', '连接方式', '串口或网络参数', '状态' ],
			endpoints.map(function(endpoint) {
				var connection = endpoint.transport === 'serial'
					? endpoint.channel || '—'
					: (endpoint.ip || '—') + ':' + Number(endpoint.port || 0);
				var parameters = endpoint.transport === 'serial'
					? Number(endpoint.baudRate || 0) + ' / ' +
						Number(endpoint.dataBits || 0) + endpoint.parity + ' / ' +
						Number(endpoint.stopBits || 0) + ' · ' +
						(endpoint.rs485 ? 'RS485' : 'RS232')
					: (endpoint.mode || '—') +
						(endpoint.interface ? ' · ' + endpoint.interface : '');
				return [
					[ E('strong', [ endpoint.name || '未命名端点' ]), E('br'), E('code', [ endpoint.id || '—' ]) ],
					endpoint.protocol || '—',
					connection,
					parameters,
					endpoint.enabled ? badge('启用', 'good') : badge('停用', 'warn')
				];
			}),
			'当前平台没有生效通讯端点'
		),
		E('h4', [ '采集设备' ]),
		renderTable(
			[ '设备', '协议', '底层读取', '上报间隔', '快读窗口', '协议参数' ],
			devices.map(function(device) {
				var protocol = device.protocol === 'Modbus'
					? 'Slave ' + Number(device.slaveId || 0) + ' · ' + (device.modbusMode || 'RTU')
					: 'Rack ' + Number(device.rack || 0) + ' / Slot ' + Number(device.slot || 0);
				return [
					[ E('strong', [ device.name || device.code || '未命名设备' ]),
						E('br'), E('code', [ device.code || device.id || '—' ]) ],
					device.protocol || '—',
					Number(device.ioIntervalMs || 1000) + ' ms',
					Number(device.reportIntervalSec || 0) + ' 秒',
					Number(device.fastReadDurationSec || 0) + ' 秒 / ' +
						Number(device.fastReadIntervalSec || 0) + ' 秒',
					protocol
				];
			}),
			'当前平台没有生效采集设备'
		),
		E('h4', [ '采集点位' ]),
		renderTable(
			[ '点位', '位置', '数据类型', '换算', '权限' ],
			points.map(function(point) {
				return [
					[ E('strong', [ point.name || point.elementId || '未命名点位' ]),
						E('br'), E('code', [ point.elementId || '—' ]) ],
					pointLocation(point),
					point.dataType || '—',
					'× ' + Number(point.scale == null ? 1 : point.scale) +
						' · ' + Number(point.decimals || 0) + ' 位小数' +
						(point.unit ? ' · ' + point.unit : ''),
					point.writable ? badge('可读写', 'warn') : badge('只读', 'good')
				];
			}),
			'当前平台没有生效采集点位'
		),
		E('div', { 'class': 'right', 'style': 'margin-top:1rem' }, [
			E('button', {
				'class': 'btn',
				'click': ui.hideModal
			}, [ '关闭' ])
		])
	]);
}

function renderStatus(data) {
	data = data || {};
	var service = data.service || {};
	var instances = service.instances || {};
	var runtime = data.runtime || {};
	var runtimePlatforms = Array.isArray(runtime.platforms) ? runtime.platforms : [];
	var byId = {};
	runtimePlatforms.forEach(function(platform) {
		byId[platform.id] = platform;
	});

	var configured = [];
	uci.sections('edgenode', 'platform', function(section) {
		configured.push(section);
	});

	var platformRows = configured.map(function(section) {
		var enabled = (section.enabled || '1') === '1';
		var current = byId[section.id];
		var config = current && current.config || {};
		var outbox = current && current.outbox || {};
		var report = current
			? [
				E('div', [ '上报：' + ageText(current.hasHeartbeat, current.lastHeartbeatAgeSec) ]),
				E('div', { 'style': 'color:#666' }, [
					'应答：' + ageText(current.hasInbound, current.lastInboundAgeSec) +
					' · 间隔 ' + Number(current.heartbeatIntervalSec || 0) + ' 秒'
				])
			]
			: [ '—' ];
		var details = current
			? E('button', {
				'class': 'btn cbi-button',
				'click': ui.createHandlerFn(null, showPlatformDetails, current)
			}, [ '查看采集配置' ])
			: E('span', [ '—' ]);

		return [
			[ E('strong', [ section.name || '未命名平台' ]), E('br'),
				E('small', [ section.url || '' ]) ],
			stateLabel(current, enabled),
			report,
			current
				? 'rev ' + Number(config.revision || 0) + ' · ' +
					Number(config.endpointCount || 0) + ' 端点 / ' +
					Number(config.deviceCount || 0) + ' 设备 / ' +
					(Array.isArray(config.points) ? config.points.length : 0) + ' 点位'
				: '—',
			current
				? Number(outbox.records || 0) + ' 条 / ' + bytesText(outbox.bytes) +
					'（发送中 ' + Number(outbox.inFlight || 0) + '）'
				: '—',
			details
		];
	});

	var tasks = Array.isArray(data.tasks) ? data.tasks : [];
	var taskNames = {
		'acquisition-config': '采集配置',
		'device-command': '设备写入',
		'network-config': '网络配置',
		'firmware-update': '固件升级',
		'modem-control': '移动网络'
	};

	return E('div', { 'class': 'cbi-map' }, [
		E('h2', [ '运行状态' ]),
		E('div', {
			'style': 'display:grid;grid-template-columns:repeat(auto-fit,minmax(230px,1fr));gap:.75rem;margin-bottom:1rem'
		}, [
			E('div', { 'class': 'cbi-section', 'style': 'padding:.75rem' }, [
				E('strong', [ '主服务' ]), E('div', { 'style': 'margin-top:.5rem' }, [
					serviceInstance(instances.main, 'EdgeNode ')
				])
			]),
			E('div', { 'class': 'cbi-section', 'style': 'padding:.75rem' }, [
				E('strong', [ '移动网络监控' ]), E('div', { 'style': 'margin-top:.5rem' }, [
					serviceInstance(instances.modem, 'Modem ')
				])
			]),
			E('div', { 'class': 'cbi-section', 'style': 'padding:.75rem' }, [
				E('strong', [ '软件版本' ]),
				E('div', { 'style': 'margin-top:.5rem' }, [
					runtime.softwareVersion || '暂无运行数据',
					service.enabled ? ' · 已设为开机启动' : ' · 未设为开机启动'
				])
			])
		]),
		E('h3', [ '平台连接与上报' ]),
		renderTable(
			[ '平台', '状态', '上报 / 应答', '生效采集配置', '待上报队列', '详情' ],
			platformRows,
			'尚未配置平台'
		),
		E('h3', { 'style': 'margin-top:1.25rem' }, [ '最近下发任务' ]),
		renderTable(
			[ '时间', '任务', '级别', '详情' ],
			tasks.map(function(task) {
				return [
					timeText(task.timeMs),
					taskNames[task.type] || task.type || '未知任务',
					task.level === 'warn' || task.level === 'error'
						? badge(task.level, 'bad')
						: badge(task.level || 'info', 'good'),
					E('code', { 'style': 'white-space:pre-wrap;word-break:break-all' }, [
						task.detail || '—'
					])
				];
			}),
			'暂无下发任务记录'
		)
	]);
}

function configurationMap() {
	var m, s, o, enabled;

	m = new form.Map('edgenode', '平台配置',
		'平台连接完全由 UCI 管理。最多可同时启用 4 个平台；保存应用后服务会重新加载。');

	s = m.section(form.TypedSection, 'platform', '平台列表',
		'只需填写平台名称和地址；连接标识由节点自动维护。平台连接仅在当前设备上配置。');
	s.anonymous = true;
	s.addremove = true;
	s.sortable = true;
	s.sectiontitle = function(section_id) {
		return uci.get('edgenode', section_id, 'name') ||
			uci.get('edgenode', section_id, 'url') || '未命名平台';
	};
	s.handleAdd = function() {
		var section_id = uci.add('edgenode', 'platform');

		uci.set('edgenode', section_id, 'id', newUuid());
		uci.set('edgenode', section_id, 'name', '新平台');
		uci.set('edgenode', section_id, 'enabled', '1');
		uci.set('edgenode', section_id, 'priority', '100');
		uci.set('edgenode', section_id, 'reconnect_interval_sec', '5');
		uci.set('edgenode', section_id, 'outbox_max_bytes', '262144');

		m.addedSection = section_id;
		return m.save(null, true);
	};

	enabled = s.option(form.Flag, 'enabled', '启用');
	enabled.default = enabled.enabled;
	enabled.rmempty = false;
	enabled.validate = function(section_id, value) {
		var count = 0;

		uci.sections('edgenode', 'platform', function(section) {
			var current = section['.name'] === section_id
				? value
				: enabled.formvalue(section['.name']);
			if ((current ?? section.enabled ?? '1') === '1')
				count++;
		});

		return count <= 4 || '最多只能同时启用 4 个平台';
	};

	o = s.option(form.Value, 'name', '平台名称');
	o.placeholder = '生产平台';
	o.maxlength = 48;
	o.rmempty = false;

	o = s.option(form.Value, 'url', '平台 HTTP(S) 地址');
	o.placeholder = 'https://i.a-z.xin';
	o.rmempty = false;
	o.validate = function(section_id, value) {
		return /^https?:\/\/[^\/\s\x00-\x1f\x7f][^\s\x00-\x1f\x7f]{0,246}$/.test(value) ||
			'请输入不超过 255 个字符的 HTTP(S) 地址';
	};
	o.write = function(section_id, value) {
		return uci.set('edgenode', section_id, 'url', value.replace(/\/+$/, ''));
	};

	o = s.option(form.Value, 'priority', '优先级');
	o.description = '数值越小优先级越高；共享资源按此顺序调度。';
	o.datatype = 'range(0,65535)';
	o.default = '100';
	o.rmempty = false;

	o = s.option(form.Value, 'reconnect_interval_sec', '重连间隔（秒）');
	o.datatype = 'range(1,3600)';
	o.default = '5';
	o.rmempty = false;

	o = s.option(form.Value, 'outbox_max_bytes', '离线缓存上限（字节）');
	o.datatype = 'range(16384,8388608)';
	o.default = '262144';
	o.rmempty = false;

	return m;
}

return view.extend({
	load: function() {
		return Promise.all([
			uci.load('edgenode'),
			L.resolveDefault(callStatus(), {})
		]);
	},

	render: function(data) {
		var statusNode = E('div', [ renderStatus(data[1]) ]);

		poll.add(function() {
			return L.resolveDefault(callStatus(), {}).then(function(status) {
				dom.content(statusNode, renderStatus(status));
			});
		}, 5);

		return Promise.resolve(configurationMap().render()).then(function(configuration) {
			return E([], [ statusNode, configuration ]);
		});
	}
});
