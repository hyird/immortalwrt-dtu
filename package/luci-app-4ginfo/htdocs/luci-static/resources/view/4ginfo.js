'use strict';
'require dom';
'require fs';
'require form';
'require poll';
'require ui';
'require uci';
'require view';

var detailStatusPath = '/tmp/4ginfo/modem.status';
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

function tabbedPanels(items, active, onSelect) {
	var root = E('div', { 'class': 'cbi-4g-tabs' });
	var menu = E('div', { 'class': 'cbi-4g-tabmenu', 'role': 'tablist' });
	var list = E('ul', { 'class': 'cbi-4g-tablist' });
	var tabs = [];
	var panels = [];
	var selected = active || items[0].id;

	items.forEach(function(item) {
		var panelId = '4ginfo-tab-' + item.id;
		var link = E('a', {
			'href': '#' + panelId,
			'role': 'tab',
			'aria-controls': panelId
		}, item.label);
		var tab = E('li', { 'class': 'cbi-4g-tab' }, [ link ]);
		var panel = E('div', {
			'id': panelId,
			'class': 'cbi-4g-tabpanel',
			'role': 'tabpanel',
			'aria-labelledby': panelId + '-tab'
		}, item.content);

		link.id = panelId + '-tab';
		link.addEventListener('click', function(event) {
			event.preventDefault();
			selected = item.id;
			update();
			if (onSelect)
				onSelect(item.id);
		});

		list.appendChild(tab);
		tabs.push({ id: item.id, node: tab, link: link });
		panels.push({ id: item.id, node: panel });
	});

	function update() {
		tabs.forEach(function(tab) {
			var isActive = tab.id === selected;

			tab.node.classList.toggle('cbi-4g-tab-active', isActive);
			tab.link.setAttribute('aria-selected', isActive ? 'true' : 'false');
		});
		panels.forEach(function(panel) {
			panel.node.style.display = panel.id === selected ? '' : 'none';
		});
	}

	menu.appendChild(list);
	root.appendChild(menu);
	panels.forEach(function(panel) {
		root.appendChild(panel.node);
	});
	update();

	return root;
}

function tabStyles() {
	return E('style', { 'type': 'text/css' }, [
		'.cbi-4g-tabs { margin: .75rem 0 1.5rem; }',
		'.cbi-4g-tabmenu { margin: 0 0 1rem; border-bottom: 1px solid #c6c6c6; overflow-x: auto; }',
		'.cbi-4g-tablist { display: flex; flex-wrap: wrap; gap: .35rem; list-style: none; margin: 0; padding: 0; }',
		'.cbi-4g-tab { display: block; margin: 0; padding: 0; }',
		'.cbi-4g-tab a { display: block; padding: .45rem .85rem; border: 1px solid #c6c6c6; border-bottom: 0; border-radius: .35rem .35rem 0 0; background: #f3f3f3; color: inherit; text-decoration: none; white-space: nowrap; }',
		'.cbi-4g-tab a:hover { background: #e8e8e8; }',
		'.cbi-4g-tab-active a { margin-bottom: -1px; border-color: #888; background: #fff; color: #0069c0; font-weight: 600; }',
		'.cbi-4g-tabpanel { padding-top: .25rem; }',
		'.cbi-4g-tabs .cbi-4g-tabs { margin-top: 1rem; }',
		'@media screen and (max-width: 600px) { .cbi-4g-tablist { flex-wrap: nowrap; } }'
	]);
}

function removePageActions(node) {
	if (!node)
		return;

	if (node.classList && node.classList.contains('cbi-page-actions') && node.parentNode)
		node.parentNode.removeChild(node);

	if (node.querySelectorAll)
		Array.prototype.forEach.call(node.querySelectorAll('.cbi-page-actions'), function(actions) {
			if (actions.parentNode)
				actions.parentNode.removeChild(actions);
		});
}

function moduleSimRows(status) {
	return [
		row('模块可用', boolText(status.available, '已发现', '未发现')),
		row('模块厂商', text(status.manufacturer)),
		row('模块型号', text(status.model)),
		row('固件版本', text(status.firmware)),
		row('模块 IMEI', text(status.imei)),
		row('SIM 状态', text(status.sim_state_name)),
		row('SIM ICCID', text(status.iccid))
	];
}

function signalRows(status) {
	return [
		row('信号质量（CSQ）', status.csq === '99' ? '未知' : withUnit(status.csq, ' / 31')),
		row('接收信号（RSSI）', withUnit(status.rssi_dbm, ' dBm')),
		row('信号百分比', withUnit(status.signal_percent, '%'))
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
		'这里只保留 4G 连接所需的 APN、认证和 SIM 参数。保存后点击应用，模块会按对应型号下发适配的 AT 命令。');
	var s = m.section(form.NamedSection, 'modem', 'modem', 'APN 与 SIM');
	var o;

	s.addremove = false;

	o = s.option(form.Flag, 'automatic_apn', '自动 APN');
	o.default = '1';
	o.rmempty = false;
	o.description = '启用后不主动写入 APN；关闭后按当前模块型号下发 APN 设置。';

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

	o = s.option(form.Button, '_apply', '应用连接配置');
	o.inputtitle = '应用';
	o.inputstyle = 'apply';
	o.description = '按模块型号选择 APN 指令：694 使用 AT+CSTT，692 使用其旧版 PDP 配置方式。';
	o.onclick = function() {
		return runCommand(m, '/usr/sbin/4ginfo-modemctl', [ 'apply' ], '4G 连接配置已应用');
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
			L.resolveDefault(fs.read(legacyStatusPath), '')
		]).then(function(values) {
			return {
				status: mergeStatus(parseStatus(values[3]), parseStatus(values[4])),
				hasStatus: values[3] !== '' || values[4] !== ''
			};
		});
	},

	refreshStatus: function(container) {
		var self = this;

	return fs.exec('/usr/sbin/4ginfo-modemctl', [ 'probe' ]).then(function(result) {
			notifyCommand(result, '必要 4G 信息已刷新', 'info');
			return self.load();
		}).then(function(data) {
			dom.content(container, self.renderStatus(data));
		}).catch(function(error) {
			notifyCommand(error, error.message || '必要 4G 信息刷新失败', 'error');
		});
	},

	renderStatus: function(data) {
		var refresh = E('button', {
			'class': 'cbi-button cbi-button-action',
			'type': 'button'
		}, '刷新 4G 信息');
	var content = [
		E('div', { 'class': 'cbi-section-descr' },
			'这里只显示模块、SIM 和信号信息；不支持的厂商扩展不会阻塞上报。')
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
				'暂时无法读取 4G 状态，请检查 4ginfo 服务、AT 端口和权限。'));

		content.push(section('模块与 SIM', moduleSimRows(data.status)));
		content.push(section('信号', signalRows(data.status)));

		return content;
	},

	render: function(data) {
		var statusContainer = E('div');
		var connectionContainer = E('div');
		var self = this;
		var connectionMap = addConnectionMap();

		data.statusContainer = statusContainer;
		dom.content(statusContainer, this.renderStatus(data));
		poll.add(function() {
			return self.load().then(function(next) {
				next.statusContainer = statusContainer;
				dom.content(statusContainer, self.renderStatus(next));
			});
		}, 10);

		return connectionMap.render().then(function(connectionMapView) {
			removePageActions(connectionMapView);
			dom.content(connectionContainer, connectionMapView);

			var topTabs = tabbedPanels([
				{ id: 'status', label: '4G信息', content: statusContainer },
				{ id: 'connection', label: 'APN设置', content: connectionContainer }
			], 'status');

			return E('div', { 'class': 'cbi-map' }, [
				tabStyles(),
				E('h2', { 'class': 'cbi-map-title' }, '4G信息'),
				E('div', { 'class': 'cbi-map-descr' },
					'显示必要的 4G 模块状态，并提供按模块型号适配的 APN 设置。'),
				topTabs
			]);
		});
	}
});
