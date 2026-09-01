'use strict';
'require view';
'require uci';
'require form';

function newUuid() {
	if (window.crypto && typeof window.crypto.randomUUID === 'function')
		return window.crypto.randomUUID();

	let bytes = new Uint8Array(16);
	window.crypto.getRandomValues(bytes);
	bytes[6] = (bytes[6] & 0x0f) | 0x40;
	bytes[8] = (bytes[8] & 0x3f) | 0x80;

	return Array.from(bytes, function(byte, index) {
		let separator = index === 4 || index === 6 || index === 8 || index === 10 ? '-' : '';
		return separator + byte.toString(16).padStart(2, '0');
	}).join('');
}

return view.extend({
	render: function() {
		let m, s, o, enabled;

		m = new form.Map('edgenode', '边缘节点',
			'平台连接完全由 UCI 管理。默认地址为 https://i.a-z.xin，最多可同时启用 4 个平台；保存应用后服务会重新加载。');

		s = m.section(form.TypedSection, 'platform', '平台列表',
			'只需填写平台名称和地址；连接标识由节点自动维护。平台连接仅在当前设备上配置。');
		s.anonymous = true;
		s.addremove = true;
		s.sortable = true;
		s.sectiontitle = function(section_id) {
			return uci.get('edgenode', section_id, 'name') ||
				uci.get('edgenode', section_id, 'url') || '未命名平台';
		};
		s.handleAdd = function(ev) {
			let section_id = uci.add('edgenode', 'platform');

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
			let count = 0;

			uci.sections('edgenode', 'platform', function(section) {
				let current = section['.name'] === section_id ? value : enabled.formvalue(section['.name']);
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

		return m.render();
	}
});
