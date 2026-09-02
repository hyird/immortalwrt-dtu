'use strict';
'require dom';
'require fs';
'require form';
'require poll';
'require ui';
'require uci';
'require view';

var detailStatusPath = '/tmp/4ginfo/modem.status';
var detailRawPath = '/tmp/4ginfo/modem.raw';
var legacyStatusPath = '/tmp/edgenode/modem.status';
var initialProbe = true;

function parseStatus(content) {
	var status = {};

	(content || '').split(/\r?\n/).forEach(function(line) {
		var separator = line.indexOf('=');

		if (separator > 0)
			status[line.slice(0, separator)] = line.slice(separator + 1);
	});

	return status;
}

function mergeStatus(primary, fallback) {
	var status = {};

	Object.keys(fallback || {}).forEach(function(key) {
		status[key] = fallback[key];
	});
	Object.keys(primary || {}).forEach(function(key) {
		status[key] = primary[key];
	});

	return status;
}

function text(value) {
	if (value === undefined || value === null || value === '' || value === '-1')
		return '-';
	if (value === 'unsupported')
		return '设备不支持或无响应';

	return String(value);
}

function boolText(value, yes, no) {
	return value === '1' ? yes : value === '0' ? no : text(value);
}

function enumText(value, values) {
	return Object.prototype.hasOwnProperty.call(values, value) ? values[value] : text(value);
}

function formatTimestamp(value) {
	var seconds = Number(value);

	if (!Number.isFinite(seconds) || seconds <= 0)
		return '-';

	return new Date(seconds * 1000).toLocaleString();
}

function withUnit(value, unit) {
	var display = text(value);

	return display === '-' || display === '设备不支持或无响应' ? display : display + unit;
}

function row(label, value) {
	return E('tr', { 'class': 'tr' }, [
		E('th', { 'class': 'th' }, label),
		E('td', { 'class': 'td' }, value)
	]);
}

function section(title, rows) {
	return E('div', { 'class': 'cbi-section' }, [
		E('h3', { 'class': 'cbi-section-title' }, title),
		E('table', { 'class': 'table cbi-section-table' }, [
			E('tbody', {}, rows)
		])
	]);
}

function statusRows(status) {
	return [
		row('模块可用', boolText(status.available, '已发现', '未发现')),
		row('完整 AT 探测', boolText(status.probe_ok, '已完成', '基础命令失败')),
		row('网络连接', boolText(status.connected, '已连接', '未连接')),
		row('网络注册', boolText(status.registered, '已注册', '未注册')),
		row('AT 端口', text(status.at_port)),
		row('最后更新', formatTimestamp(status.updated_at))
	];
}

function identityRows(status) {
	return [
		row('模块厂商', text(status.manufacturer)),
		row('模块型号', text(status.model)),
		row('固件版本', text(status.firmware)),
		row('模块 IMEI', text(status.imei)),
		row('SIM IMSI', text(status.imsi)),
		row('SIM ICCID', text(status.iccid)),
		row('本机号码（MSISDN）', text(status.msisdn)),
		row('SIM 状态', enumText(status.sim_state, {
			'0': '未指定', '1': '未知', '2': '就绪', '3': '未插入',
			'4': '需要 PIN', '5': '需要 PUK', '6': '已锁定'
		})),
		row('SIM 状态文本', text(status.sim_state_name)),
		row('SIM 卡检测', text(status.sim_presence))
	];
}

function registrationRows(status) {
	return [
		row('CEREG 注册状态', enumText(status.cereg_status, {
			'0': '未注册', '1': '已注册（本地）', '2': '搜索中',
			'3': '注册被拒绝', '4': '未知', '5': '已注册（漫游）'
		})),
		row('CREG 电路域注册', text(status.creg_status)),
		row('CGREG 分组域注册', text(status.cgreg_status)),
		row('注册模式', text(status.registration_mode)),
		row('运营商 PLMN', text(status.plmn)),
		row('运营商名称', text(status.operator_name || status.mobile_operator)),
		row('网络制式', text(status.network_mode || status.network_type)),
		row('接入技术码', text(status.access_technology || status.registration_act)),
		row('网络信息原始值', text(status.network_info_raw)),
		row('服务小区状态', text(status.serving_state)),
		row('服务小区 RAT', text(status.serving_rat)),
		row('双工模式', text(status.serving_duplex)),
		row('小区 MCC', text(status.mcc)),
		row('小区 MNC', text(status.mnc)),
		row('位置区/跟踪区', text(status.location_area_code)),
		row('跟踪区代码（TAC）', text(status.tracking_area_code || status.location_area_code)),
		row('小区 ID', text(status.cell_id)),
		row('物理小区 ID（PCI）', text(status.pci))
	];
}

function signalRows(status) {
	return [
		row('信号质量（CSQ）', status.csq === '99' ? '未知' : withUnit(status.csq, ' / 31')),
		row('接收信号（RSSI）', withUnit(status.rssi_dbm, ' dBm')),
		row('信号百分比', withUnit(status.signal_percent, '%')),
		row('误码率（BER）', text(status.ber)),
		row('扩展 RSRP', withUnit(status.rsrp_dbm, ' dBm')),
		row('扩展 RSRQ', withUnit(status.rsrq_db, ' dB')),
		row('扩展 SINR', withUnit(status.sinr_db, ' dB')),
		row('QCSQ 原始指标', text(status.qcsq_rssi) + ' / ' + text(status.rsrp_dbm) + ' / ' + text(status.rsrq_db) + ' / ' + text(status.sinr_db)),
		row('CESQ 原始指标', text(status.cesq)),
		row('当前频段', text(status.band)),
		row('服务小区频段码', text(status.serving_band)),
		row('信道/EARFCN', text(status.channel || status.earfcn)),
		row('上行带宽码', text(status.uplink_bandwidth)),
		row('下行带宽码', text(status.downlink_bandwidth))
	];
}

function dataRows(status) {
	return [
		row('分组域附着', text(status.packet_attached)),
		row('PDP 激活', text(status.pdp_active)),
		row('PDP 类型', text(status.pdp_type)),
		row('APN', text(status.apn)),
		row('PDP 地址', text(status.pdp_address || status.mobile_ipv4)),
		row('移动 IPv4', text(status.mobile_ipv4)),
		row('移动 IPv6', text(status.mobile_ipv6)),
		row('PDP 配置原始值', text(status.pdp_config)),
		row('网关/DNS 原始信息', text(status.cgcontrdp)),
		row('Quectel 数据会话', text(status.qiact)),
		row('自动 APN 查询', text(status.cgnapn)),
		row('载波聚合信息', text(status.qca_info))
	];
}

function ancillaryRows(status) {
	return [
		row('射频功能状态（CFUN）', enumText(status.radio_function, {
			'0': '最小功能', '1': '全功能', '4': '飞行/离线模式'
		})),
		row('电池状态', text(status.battery_status)),
		row('电池电量', withUnit(status.battery_percent, '%')),
		row('电池电压', withUnit(status.battery_voltage_mv, ' mV')),
		row('短信存储（CPMS）', text(status.cpms)),
		row('字符集（CSCS）', text(status.cscs)),
		row('短信格式（CMGF）', text(status.cmgf)),
		row('短信通知（CNMI）', text(status.cnmi)),
		row('GPS 状态', text(status.gps_status)),
		row('GPS 定位结果', text(status.gps_location)),
		row('首选网络模式', text(status.preferred_mode)),
		row('首选频段', text(status.preferred_band))
	];
}

function configRows(config) {
	return [
		row('USB 厂商 ID', text(config.usbVendor)),
		row('USB 产品 ID', text(config.usbProduct)),
		row('AT 通信端口', text(config.atPort)),
		row('4G 网络接口', text(config.wanInterface)),
		row('EdgeNode 状态文件', text(config.statusPath)),
		row('完整信息文件', detailStatusPath),
		row('监测间隔', config.monitorInterval ? withUnit(config.monitorInterval, ' 秒') : '-'),
		row('UCI 保存的 IMEI', text(config.configuredImei)),
		row('UCI 保存的 ICCID', text(config.configuredIccid))
	];
}

function commandMessage(result, fallback) {
	return ((result && (result.stdout || result.stderr)) || fallback || '').trim();
}

function notifyCommand(result, fallback, level) {
	ui.addNotification(null, E('p', {}, commandMessage(result, fallback)), level || 'info');
}

function runCommand(map, command, args, fallback) {
	return map.save(null, true).then(function() {
		return fs.exec(command, args);
	}).then(function(result) {
		notifyCommand(result, fallback, 'info');
		return map.reset();
	}).catch(function(error) {
		notifyCommand(error, error.message || '操作失败', 'error');
	});
}

function addConnectionMap() {
	var m = new form.Map('4ginfo', '4G连接配置',
		'这里可以真正修改 4G 模块的连接和选网参数；保存只是写入 UCI，点击应用后才逐条发送 AT 命令。');
	var s = m.section(form.NamedSection, 'modem', 'modem', '连接与选网');
	var o;

	s.addremove = false;

	o = s.option(form.ListValue, 'radio_function', '射频功能');
	o.value('full', '全功能/在线');
	o.value('minimum', '最小功能');
	o.value('offline', '离线/飞行模式');
	o.default = 'full';
	o.rmempty = false;
	o.description = '对应标准 AT+CFUN；切换为最小或离线功能可能断开 4G。';

	o = s.option(form.ListValue, 'operator_mode', '运营商选择');
	o.value('auto', '自动选网');
	o.value('manual', '手动指定 PLMN');
	o.default = 'auto';
	o.rmempty = false;

	o = s.option(form.Value, 'operator_mccmnc', '手动 PLMN（MCCMNC）');
	o.maxlength = 6;
	o.depends('operator_mode', 'manual');
	o.validate = function(section_id, value) {
		return /^\d{5,6}$/.test(value) || 'PLMN 必须是 5 至 6 位数字';
	};

	o = s.option(form.Flag, 'automatic_apn', '自动 APN');
	o.default = '1';
	o.rmempty = false;
	o.description = '启用后由运营商或模块自动选择 APN；关闭后必须填写 APN。';

	o = s.option(form.Value, 'apn', 'APN');
	o.maxlength = 100;
	o.depends('automatic_apn', '0');
	o.validate = function(section_id, value) {
		return value === '' || /^[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*$/.test(value) ||
			'APN 只能包含字母、数字、短横线和点，长度不超过 100 个字符';
	};

	o = s.option(form.ListValue, 'pdp_type', 'PDP 类型');
	o.value('ipv4', 'IPv4');
	o.value('ipv6', 'IPv6');
	o.value('ipv4v6', 'IPv4 + IPv6');
	o.default = 'ipv4';
	o.rmempty = false;

	o = s.option(form.ListValue, 'auth_type', '认证方式');
	o.value('none', '无认证');
	o.value('pap', 'PAP');
	o.value('chap', 'CHAP');
	o.value('pap_or_chap', 'PAP 或 CHAP');
	o.default = 'none';
	o.rmempty = false;

	o = s.option(form.Value, 'username', '用户名');
	o.maxlength = 128;
	o.depends('auth_type', 'pap');
	o.depends('auth_type', 'chap');
	o.depends('auth_type', 'pap_or_chap');
	o.validate = function(section_id, value) {
		return /^[\x20-\x7e]*$/.test(value) && !value.includes('"') || '用户名不能包含控制字符或双引号';
	};

	o = s.option(form.Value, 'password', '密码');
	o.password = true;
	o.maxlength = 128;
	o.depends('auth_type', 'pap');
	o.depends('auth_type', 'chap');
	o.depends('auth_type', 'pap_or_chap');
	o.validate = function(section_id, value) {
		return /^[\x20-\x7e]*$/.test(value) && !value.includes('"') || '密码不能包含控制字符或双引号';
	};

	o = s.option(form.Value, 'pin_code', 'SIM PIN');
	o.password = true;
	o.maxlength = 8;
	o.description = '如 SIM 未启用 PIN 保护请留空；启用时填写 4 至 8 位数字。';
	o.validate = function(section_id, value) {
		return value === '' || /^\d{4,8}$/.test(value) || 'SIM PIN 必须为空或 4 至 8 位数字';
	};

	o = s.option(form.Flag, 'redial_after_apply', '应用后重拨');
	o.default = '0';
	o.description = '应用连接参数后执行模块重启，可能短暂中断网络。';

	o = s.option(form.Button, '_apply', '应用连接配置');
	o.inputtitle = '应用';
	o.inputstyle = 'apply';
	o.description = '按 SIM PIN、PDP/APN、认证、选网、射频功能的顺序逐条等待 AT 最终响应。';
	o.onclick = function() {
		return runCommand(m, '/usr/sbin/4ginfo-modemctl', [ 'apply' ], '4G 连接配置已应用');
	};

	o = s.option(form.Button, '_redial', '重拨模块');
	o.inputtitle = '重拨';
	o.inputstyle = 'warning';
	o.description = '执行 AT+CFUN=1,1；网络会短暂中断。';
	o.onclick = function() {
		return fs.exec('/usr/sbin/4ginfo-modemctl', [ 'redial' ]).then(function(result) {
			notifyCommand(result, '模块正在重拨', 'info');
		}).catch(function(error) {
			notifyCommand(error, error.message || '模块重拨失败', 'error');
		});
	};

	s = m.section(form.NamedSection, 'modem', 'modem', '单条 AT 事务');
	s.addremove = false;
	o = s.option(form.Value, 'custom_at', 'AT 命令');
	o.maxlength = 256;
	o.placeholder = '例如：AT+QENG="servingcell"';
	o.description = '用于执行模块厂商特有功能。每次只发送一条命令，并等待 OK/ERROR 最终响应；不要输入回车。';
	o.validate = function(section_id, value) {
		return /^AT[^\r\n\x00-\x1f\x7f]*$/.test(value) || '命令必须以 AT 开头且不能包含控制字符';
	};
	o = s.option(form.Button, '_at', '执行单条 AT');
	o.inputtitle = '发送并显示响应';
	o.inputstyle = 'button';
	o.onclick = function() {
		return m.save(null, true).then(function() {
			var command = uci.get('4ginfo', 'modem', 'custom_at') || '';

			return fs.exec('/usr/sbin/4ginfo-modemctl', [ 'at', command ]);
		}).then(function(result) {
			notifyCommand(result, 'AT 事务完成', 'info');
			return m.reset();
		}).catch(function(error) {
			notifyCommand(error, error.message || 'AT 事务失败', 'error');
		});
	};

	return m;
}

function addDeviceMap() {
	var m = new form.Map('edgenode', '4G设备接入配置',
		'配置 EdgeNode 使用的 4G USB 设备、AT 串口、状态文件和监测参数。保存后点击重启 EdgeNode。');
	var s = m.section(form.NamedSection, 'modem', 'modem', 'USB/串口');
	var o;

	s.addremove = false;
	o = s.option(form.Value, 'usb_vendor', 'USB 厂商 ID');
	o.datatype = 'hex16';
	o.maxlength = 4;
	o.rmempty = false;
	o = s.option(form.Value, 'usb_product', 'USB 产品 ID');
	o.datatype = 'hex16';
	o.maxlength = 4;
	o.rmempty = false;
	o = s.option(form.Value, 'at_port', 'AT 通信端口');
	o.maxlength = 127;
	o.rmempty = false;
	o.validate = function(section_id, value) {
		return /^\/dev\/[A-Za-z0-9._/-]+$/.test(value) || '请输入有效的 /dev 设备路径';
	};
	o = s.option(form.Value, 'status_path', 'EdgeNode 状态文件');
	o.readonly = true;
	o.description = '完整 4G信息另存于 /tmp/4ginfo/modem.status。';
	o = s.option(form.Value, 'monitor_interval', '监测间隔（秒）');
	o.datatype = 'range(1,3600)';
	o.default = '30';
	o.rmempty = false;
	o = s.option(form.Value, 'imei', '模块 IMEI');
	o.maxlength = 15;
	o.validate = function(section_id, value) {
		return value === '' || /^\d{15}$/.test(value) || 'IMEI 必须为空或 15 位数字';
	};
	o = s.option(form.Value, 'iccid', 'SIM ICCID');
	o.maxlength = 22;
	o.validate = function(section_id, value) {
		return value === '' || /^\d{18,22}$/.test(value) || 'ICCID 必须为空或 18 至 22 位数字';
	};

	s = m.section(form.NamedSection, 'hardware', 'hardware', '网络接口');
	s.addremove = false;
	o = s.option(form.Value, 'wan_interface', '4G 网络接口');
	o.maxlength = 15;
	o.rmempty = false;
	o.validate = function(section_id, value) {
		return /^[A-Za-z0-9_.-]+$/.test(value) || '请输入有效的网络接口名称';
	};
	o = s.option(form.Button, '_restart', '重启 EdgeNode');
	o.inputtitle = '保存并重启';
	o.inputstyle = 'apply';
	o.description = '保存设备接入参数并重启监测进程。';
	o.onclick = function() {
		return runCommand(m, '/etc/init.d/edgenode', [ 'restart' ], 'EdgeNode 已重启');
	};

	return m;
}

return view.extend({
	load: function() {
		var probe = initialProbe ? L.resolveDefault(fs.exec('/usr/sbin/4ginfo-modemctl', [ 'probe' ]), null) : Promise.resolve(null);

		initialProbe = false;
		return Promise.all([
			uci.load('edgenode'),
			uci.load('4ginfo'),
			probe,
			L.resolveDefault(fs.read(detailStatusPath), ''),
			L.resolveDefault(fs.read(legacyStatusPath), ''),
			L.resolveDefault(fs.read(detailRawPath), '')
		]).then(function(values) {
			var config = {
				usbVendor: uci.get('edgenode', 'modem', 'usb_vendor'),
				usbProduct: uci.get('edgenode', 'modem', 'usb_product'),
				atPort: uci.get('edgenode', 'modem', 'at_port'),
				statusPath: uci.get('edgenode', 'modem', 'status_path') || legacyStatusPath,
				monitorInterval: uci.get('edgenode', 'modem', 'monitor_interval'),
				wanInterface: uci.get('edgenode', 'hardware', 'wan_interface'),
				configuredImei: uci.get('edgenode', 'modem', 'imei'),
				configuredIccid: uci.get('edgenode', 'modem', 'iccid')
			};

			return {
				config: config,
				status: mergeStatus(parseStatus(values[3]), parseStatus(values[4])),
				raw: values[5] || '',
				hasStatus: values[3] !== '' || values[4] !== ''
			};
		});
	},

	refreshStatus: function(container) {
		var self = this;

		return fs.exec('/usr/sbin/4ginfo-modemctl', [ 'probe' ]).then(function(result) {
			notifyCommand(result, '完整 4G信息已刷新', 'info');
			return self.load();
		}).then(function(data) {
			dom.content(container, self.renderStatus(data));
		}).catch(function(error) {
			notifyCommand(error, error.message || '完整 4G信息刷新失败', 'error');
		});
	},

	renderStatus: function(data) {
		var refresh = E('button', {
			'class': 'cbi-button cbi-button-action',
			'type': 'button'
		}, '刷新全部 4G信息');
		var content = [
			E('div', { 'class': 'cbi-section-descr' },
				'本页会显示模块支持的所有已知 AT 项；设备不支持的厂商扩展会明确显示。点击刷新会逐条执行探测，每条命令都等待最终响应。')
		];
		var self = this;

		refresh.addEventListener('click', function() {
			refresh.disabled = true;
			self.refreshStatus(data.statusContainer).finally(function() {
				refresh.disabled = false;
			});
		});
		content.push(refresh);

		if (!data.hasStatus)
			content.push(E('div', { 'class': 'alert-message warning' },
				'暂时无法读取 4G 状态，请检查 EdgeNode 服务、AT 端口和权限。'));

		content.push(section('运行状态', statusRows(data.status)));
		content.push(section('模块身份与 SIM', identityRows(data.status)));
		content.push(section('注册与运营商', registrationRows(data.status)));
		content.push(section('无线信号与小区', signalRows(data.status)));
		content.push(section('移动数据与 PDP', dataRows(data.status)));
		content.push(section('射频、电池、短信与定位', ancillaryRows(data.status)));
		content.push(section('本地设备配置', configRows(data.config)));
		content.push(E('div', { 'class': 'cbi-section' }, [
			E('h3', { 'class': 'cbi-section-title' }, '逐条 AT 原始响应'),
			E('pre', { 'style': 'max-height: 32rem; overflow: auto; white-space: pre-wrap;' },
				data.raw || '尚未生成原始响应；请点击“刷新全部 4G信息”。')
		]));

		return content;
	},

	render: function(data) {
		var statusContainer = E('div');
		var self = this;
		var connectionMap = addConnectionMap();
		var deviceMap = addDeviceMap();

		data.statusContainer = statusContainer;
		dom.content(statusContainer, this.renderStatus(data));
		poll.add(function() {
			return self.load().then(function(next) {
				next.statusContainer = statusContainer;
				dom.content(statusContainer, self.renderStatus(next));
			});
		}, 10);

		return Promise.all([ connectionMap.render(), deviceMap.render() ]).then(function(maps) {
			return E('div', { 'class': 'cbi-map' }, [
				E('h2', { 'class': 'cbi-map-title' }, '4G信息'),
				E('div', { 'class': 'cbi-map-descr' },
					'完整显示 4G 模块信息，并提供连接、选网、射频、设备接入和逐条 AT 配置能力。'),
				statusContainer,
				maps[0],
				maps[1]
			]);
		});
	}
});
