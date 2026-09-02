'use strict';
'require dom';
'require fs';
'require form';
'require poll';
'require ui';
'require uci';
'require view';

var statusPath = '/tmp/edgenode/modem.status';

function parseStatus(content) {
	var status = {};

	(content || '').split(/\r?\n/).forEach(function(line) {
		var separator = line.indexOf('=');
		if (separator > 0)
			status[line.slice(0, separator)] = line.slice(separator + 1);
	});

	return status;
}

function text(value) {
	return value !== undefined && value !== null && value !== '' ? String(value) : '-';
}

function boolText(value, yes, no) {
	return value === '1' ? yes : value === '0' ? no : '-';
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

function formatSignal(value, unit) {
	return value !== undefined && value !== null && value !== '' && value !== '-1' ?
		String(value) + unit : '-';
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
		row('网络连接', boolText(status.connected, '已连接', '未连接')),
		row('网络注册', boolText(status.registered, '已注册', '未注册')),
		row('最后更新', formatTimestamp(status.updated_at))
	];
}

function identityRows(status) {
	return [
		row('模块 IMEI', text(status.imei)),
		row('SIM ICCID', text(status.iccid)),
		row('运营商', text(status.mobile_operator)),
		row('SIM 状态', enumText(status.sim_state, {
			'0': '未指定',
			'1': '未知',
			'2': '就绪',
			'3': '未插入',
			'4': '需要 PIN',
			'5': '需要 PUK',
			'6': '已锁定'
		}))
	];
}

function signalRows(status) {
	var csq = status.csq === undefined || status.csq === '' ? '-' :
		status.csq === '99' ? '未知' : text(status.csq) + ' / 31';

	return [
		row('信号质量（CSQ）', csq),
		row('接收信号（RSSI）', formatSignal(status.rssi_dbm, ' dBm')),
		row('信号百分比', status.signal_percent !== undefined && status.signal_percent !== '' ?
			text(status.signal_percent) + '%' : '-'),
		row('注册状态码（CEREG）', status.registration_status === '-1' ?
			'未知' : text(status.registration_status)),
		row('APN', text(status.apn)),
		row('移动 IPv4', text(status.mobile_ipv4))
	];
}

function configRows(config) {
	return [
		row('USB 厂商 ID', text(config.usbVendor)),
		row('USB 产品 ID', text(config.usbProduct)),
		row('AT 通信端口', text(config.atPort)),
		row('4G 网络接口', text(config.wanInterface)),
		row('状态文件', text(config.statusPath)),
		row('监测间隔', config.monitorInterval ? text(config.monitorInterval) + ' 秒' : '-'),
		row('UCI 保存的 IMEI', text(config.configuredImei)),
		row('UCI 保存的 ICCID', text(config.configuredIccid))
	];
}

function commandMessage(result, fallback) {
	return (result && (result.stdout || result.stderr) || fallback).trim();
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
		'配置当前 EdgeNode 4G 连接参数。保存配置后，可应用到模块或执行重拨。账号密码保存在本机 UCI 配置中。');
	var s = m.section(form.NamedSection, 'modem', 'modem', '移动网络参数');
	var o;

	s.addremove = false;

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
		return /^[\x20-\x7e]*$/.test(value) && !value.includes('"') ||
			'用户名不能包含控制字符或双引号';
	};

	o = s.option(form.Value, 'password', '密码');
	o.password = true;
	o.maxlength = 128;
	o.depends('auth_type', 'pap');
	o.depends('auth_type', 'chap');
	o.depends('auth_type', 'pap_or_chap');
	o.validate = function(section_id, value) {
		return /^[\x20-\x7e]*$/.test(value) && !value.includes('"') ||
			'密码不能包含控制字符或双引号';
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
	o.description = '应用 APN 和认证参数后重启模块连接，可能会短暂中断网络。';

	o = s.option(form.Button, '_probe', '测试 AT 通信');
	o.inputtitle = '测试';
	o.inputstyle = 'button';
	o.description = '读取模块信息，不修改 APN、认证或网络状态。';
	o.onclick = function() {
		return fs.exec('/usr/sbin/4ginfo-modemctl', [ 'probe' ]).then(function(result) {
			notifyCommand(result, 'AT 通信测试成功', 'info');
		}).catch(function(error) {
			notifyCommand(error, error.message || 'AT 通信测试失败', 'error');
		});
	};

	o = s.option(form.Button, '_apply', '应用连接配置');
	o.inputtitle = '应用';
	o.inputstyle = 'apply';
	o.description = '先保存当前表单，再向 4G 模块写入 SIM PIN、APN、PDP 和认证参数。';
	o.onclick = function() {
		return runCommand(m, '/usr/sbin/4ginfo-modemctl', [ 'apply' ], '4G 连接配置已应用');
	};

	o = s.option(form.Button, '_redial', '重拨模块');
	o.inputtitle = '重拨';
	o.inputstyle = 'warning';
	o.description = '执行模块重启连接，网络会短暂中断。';
	o.onclick = function() {
		return fs.exec('/usr/sbin/4ginfo-modemctl', [ 'redial' ]).then(function(result) {
			notifyCommand(result, '模块正在重拨', 'info');
		}).catch(function(error) {
			notifyCommand(error, error.message || '模块重拨失败', 'error');
		});
	};

	return m;
}

function addDeviceMap() {
	var m = new form.Map('edgenode', '4G设备配置',
		'配置 EdgeNode 使用的 4G USB 设备、AT 串口、状态文件和监测参数。保存后请点击重启 EdgeNode 使设备配置生效。');
	var s = m.section(form.NamedSection, 'modem', 'modem', '4G 模块');
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

	o = s.option(form.Value, 'status_path', '状态文件');
	o.readonly = true;
	o.description = '4G信息页面读取此文件；固定为 /tmp/edgenode/modem.status。';

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
	o.description = '保存本页设备配置并重启 EdgeNode，模块监测会重新打开 AT 串口。';
	o.onclick = function() {
		return runCommand(m, '/etc/init.d/edgenode', [ 'restart' ], 'EdgeNode 已重启');
	};

	return m;
}

return view.extend({
	load: function() {
		return Promise.all([ uci.load('edgenode'), uci.load('4ginfo') ]).then(function() {
			var config = {
				usbVendor: uci.get('edgenode', 'modem', 'usb_vendor'),
				usbProduct: uci.get('edgenode', 'modem', 'usb_product'),
				atPort: uci.get('edgenode', 'modem', 'at_port'),
				statusPath: uci.get('edgenode', 'modem', 'status_path') || statusPath,
				monitorInterval: uci.get('edgenode', 'modem', 'monitor_interval'),
				wanInterface: uci.get('edgenode', 'hardware', 'wan_interface'),
				configuredImei: uci.get('edgenode', 'modem', 'imei'),
				configuredIccid: uci.get('edgenode', 'modem', 'iccid')
			};

			return L.resolveDefault(fs.read(statusPath), '').then(function(content) {
				return {
					config: config,
					status: parseStatus(content),
					hasStatus: content !== '',
					profile: {
						automaticApn: uci.get('4ginfo', 'modem', 'automatic_apn'),
						apn: uci.get('4ginfo', 'modem', 'apn'),
						pdpType: uci.get('4ginfo', 'modem', 'pdp_type'),
						authType: uci.get('4ginfo', 'modem', 'auth_type')
					}
				};
			});
		});
	},

	renderStatus: function(data) {
		var content = [];

		if (!data.hasStatus)
			content.push(E('div', { 'class': 'alert-message warning' },
				'暂时无法读取 4G 状态文件，请检查 EdgeNode 服务、模块端口和状态文件权限。'));

		content.push(section('运行状态', statusRows(data.status)));
		content.push(section('模块与 SIM', identityRows(data.status)));
		content.push(section('信号与移动网络', signalRows(data.status)));
		content.push(section('本地配置', configRows(data.config)));

		return content;
	},

	render: function(data) {
		var statusContainer = E('div');
		var self = this;
		var connectionMap = addConnectionMap();
		var deviceMap = addDeviceMap();

		dom.content(statusContainer, this.renderStatus(data));
		poll.add(function() {
			return self.load().then(function(next) {
				dom.content(statusContainer, self.renderStatus(next));
			});
		}, 10);

		return Promise.all([ connectionMap.render(), deviceMap.render() ]).then(function(maps) {
			return E('div', { 'class': 'cbi-map' }, [
			E('h2', { 'class': 'cbi-map-title' }, '4G信息'),
			E('div', { 'class': 'cbi-map-descr' },
				'显示 EdgeNode 最近一次从 4G 模块读取的完整状态，并提供 4G 连接和设备参数配置；状态每 10 秒自动刷新。'),
			statusContainer,
			maps[0],
			maps[1]
			]);
		});
	}
});
