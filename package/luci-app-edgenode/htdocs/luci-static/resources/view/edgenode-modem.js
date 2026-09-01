'use strict';
'require dom';
'require fs';
'require poll';
'require uci';
'require view';

var defaultStatusPath = '/tmp/edgenode/modem.status';

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

return view.extend({
	load: function() {
		return uci.load('edgenode').then(function() {
			var statusPath = uci.get('edgenode', 'modem', 'status_path') || defaultStatusPath;
			var config = {
				usbVendor: uci.get('edgenode', 'modem', 'usb_vendor'),
				usbProduct: uci.get('edgenode', 'modem', 'usb_product'),
				atPort: uci.get('edgenode', 'modem', 'at_port'),
				statusPath: statusPath,
				monitorInterval: uci.get('edgenode', 'modem', 'monitor_interval'),
				wanInterface: uci.get('edgenode', 'hardware', 'wan_interface'),
				configuredImei: uci.get('edgenode', 'modem', 'imei'),
				configuredIccid: uci.get('edgenode', 'modem', 'iccid')
			};

			return L.resolveDefault(fs.read(statusPath), '').then(function(content) {
				return {
					config: config,
					status: parseStatus(content),
					hasStatus: content !== ''
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

		dom.content(statusContainer, this.renderStatus(data));
		poll.add(function() {
			return self.load().then(function(next) {
				dom.content(statusContainer, self.renderStatus(next));
			});
		}, 10);

		return E('div', { 'class': 'cbi-map' }, [
			E('h2', { 'class': 'cbi-map-title' }, '4G 模块信息'),
			E('div', { 'class': 'cbi-map-descr' },
				'显示 EdgeNode 最近一次从 4G 模块读取的完整状态和本地硬件配置；页面每 10 秒自动刷新。'),
			statusContainer
		]);
	}
});
