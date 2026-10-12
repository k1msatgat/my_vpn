#!/usr/bin/node
/* my_vpn_ctl — MCP 서버가 sudo 로 부르는 유일한 프로그램.
 *
 * MCP 서버(server.js)는 일반 계정으로 돌고 권한이 필요한 일은 전부 여기로 넘긴다.
 * sudoers 는 이 파일 하나만, 인자 없이 부르는 것만 허용한다. 그래서 MCP 쪽이 뚫려도
 * 할 수 있는 일은 아래 COMMANDS 에 있는 것이 전부다.
 *
 * 요청은 argv 가 아니라 stdin 의 JSON 한 덩어리로 받는다. 기기 키가 들어오는데
 * argv 는 ps 와 sudo 로그에 남는다.
 *
 * root 로 도는 코드라 npm 패키지를 쓰지 않는다. 호출한 쪽의 검증은 믿지 않고 여기서
 * 다시 한다. */
'use strict';

const fs = require('node:fs');
const crypto = require('node:crypto');
const { spawnSync } = require('node:child_process');

const UNIT = 'my_vpn.service';
const ETC = '/etc/my_vpn';
const DEVICE_FILE = `${ETC}/devices.conf`;
const ACL_FILE = `${ETC}/acl.conf`;
const ENV_FILE = `${ETC}/server.env`;
const ACL_APPLIED = '/run/my_vpn/acl.applied';
const SERVER_BIN = '/opt/my_vpn/server/bin/vpn_server';
const AUTH_CHECK = '/opt/my_vpn/test/auth_check.py';

/* server/include 의 값과 같아야 한다. */
const MAX_DEVICES = 16;
const MAX_RULES = 32;
const LINE_MAX = 255;	/* device.c / alc.c 가 한 줄을 char[256] 으로 읽는다 */

const ENV = { PATH: '/usr/sbin:/usr/bin', LC_ALL: 'C' };

class CtlError extends Error {}

function fail(msg) {
	throw new CtlError(msg);
}

function run(cmd, args, opts = {}) {
	const r = spawnSync(cmd, args, {
		encoding: 'utf8',
		env: ENV,
		maxBuffer: 64 * 1024 * 1024,
		...opts,
	});

	if (r.error && r.error.code !== 'ETIMEDOUT') {
		fail(`${cmd}: ${r.error.message}`);
	}

	return r;
}

/* ---- 입력 검증 ------------------------------------------------------------ */

function isIPv4(s) {
	return typeof s === 'string' &&
		/^(0|[1-9]\d{0,2})(\.(0|[1-9]\d{0,2})){3}$/.test(s) &&
		s.split('.').every((o) => Number(o) <= 255);
}

function isCidr(s) {
	if (s === 'any') {
		return true;
	}
	if (typeof s !== 'string') {
		return false;
	}

	const [ip, bits, extra] = s.split('/');
	if (extra !== undefined || !isIPv4(ip)) {
		return false;
	}

	return bits === undefined || (/^\d{1,2}$/.test(bits) && Number(bits) <= 32);
}

function isProto(s) {
	return ['any', 'icmp', 'tcp', 'udp'].includes(s) ||
		(typeof s === 'string' && /^\d{1,3}$/.test(s) && Number(s) <= 255);
}

function isPort(s) {
	if (s === 'any' || s === '-') {
		return true;
	}
	if (typeof s !== 'string' || !/^\d{1,5}(-\d{1,5})?$/.test(s)) {
		return false;
	}

	const [lo, hi = lo] = s.split('-').map(Number);

	return lo <= 65535 && hi <= 65535 && lo <= hi;
}

function wantInt(v, name, lo, hi) {
	if (!Number.isInteger(v) || v < lo || v > hi) {
		fail(`${name} must be an integer in ${lo}..${hi}`);
	}

	return v;
}

function wantId(v) {
	if (typeof v !== 'string' || !/^[0-9a-fA-F]{32}$/.test(v)) {
		fail('id must be 32 hex digits');
	}

	return v.toLowerCase();
}

/* 조건은 통째로 받는다. 빠진 항목은 "검사하지 않음"이다. */
function wantRequirements(v) {
	const r = { firewall: false, antivirus: false, min_build: 0 };

	if (v === undefined || v === null) {
		return r;
	}
	if (typeof v !== 'object' || Array.isArray(v)) {
		fail('requirements must be an object');
	}

	for (const k of Object.keys(v)) {
		if (!(k in r)) {
			fail(`unknown requirement '${k}' (firewall, antivirus, min_build)`);
		}
	}
	if (v.firewall !== undefined) {
		if (typeof v.firewall !== 'boolean') fail('requirements.firewall must be a boolean');
		r.firewall = v.firewall;
	}
	if (v.antivirus !== undefined) {
		if (typeof v.antivirus !== 'boolean') fail('requirements.antivirus must be a boolean');
		r.antivirus = v.antivirus;
	}
	if (v.min_build !== undefined) {
		r.min_build = wantInt(v.min_build, 'requirements.min_build', 0, 0xFFFFFFFF);
	}

	return r;
}

/* 이름은 줄 끝의 주석으로 들어간다. '#' 나 줄바꿈이 섞이면 파일 형식이 깨진다. */
function wantLabel(v) {
	if (v === undefined || v === null || v === '') {
		return '';
	}
	if (typeof v !== 'string' || !/^[A-Za-z0-9_.-]([A-Za-z0-9 _.-]{0,30}[A-Za-z0-9_.-])?$/.test(v)) {
		fail('label must be 1-32 characters of letters, digits, space, _ . -');
	}

	return v;
}

/* ---- 설정 파일 ------------------------------------------------------------ */

function readEnv() {
	const env = { PORT: '9000', TUN_IP: '10.0.0.1', PREFIX: '24', MTU: '1400' };
	let text;

	try {
		text = fs.readFileSync(ENV_FILE, 'utf8');
	} catch (e) {
		return env;
	}

	for (const line of text.split('\n')) {
		const m = /^\s*([A-Z_]+)=(.*)$/.exec(line);
		if (m) {
			env[m[1]] = m[2].trim();
		}
	}

	return env;
}

/* 같은 디렉터리에 쓰고 rename 한다. 쓰다가 죽어도 반쯤 쓰인 기기 목록이 남지 않고,
 * 그 사이에 SIGHUP 을 받은 서버가 잘린 파일을 읽을 일도 없다. */
function writeAtomic(path, text) {
	const tmp = `${path}.tmp.${process.pid}`;

	fs.writeFileSync(tmp, text, { mode: 0o600 });
	fs.renameSync(tmp, path);
}

function readDevices() {
	const devices = [];
	let text;

	try {
		text = fs.readFileSync(DEVICE_FILE, 'utf8');
	} catch (e) {
		if (e.code === 'ENOENT') {
			return devices;
		}
		throw e;
	}

	text.split('\n').forEach((raw, i) => {
		const hash = raw.indexOf('#');
		const body = hash < 0 ? raw : raw.slice(0, hash);
		const label = hash < 0 ? '' : raw.slice(hash + 1).trim();
		const tok = body.trim().split(/\s+/).filter(Boolean);
		const d = { requirements: { firewall: false, antivirus: false, min_build: 0 } };

		if (tok.length === 0) {
			return;
		}
		if (tok.length < 3 || !/^[0-9a-fA-F]{32}$/.test(tok[0]) ||
				!/^[0-9a-fA-F]{64}$/.test(tok[1]) || !isIPv4(tok[2])) {
			fail(`${DEVICE_FILE}: line ${i + 1} is malformed; fix it by hand first`);
		}

		d.id = tok[0].toLowerCase();
		d.key = tok[1].toLowerCase();
		d.tunnel_ip = tok[2];
		d.label = /^[A-Za-z0-9 _.-]{1,32}$/.test(label) ? label : '';

		for (const t of tok.slice(3)) {
			const m = /^min-build=(\d+)$/.exec(t);

			if (t === 'firewall=on') {
				d.requirements.firewall = true;
			} else if (t === 'antivirus=on') {
				d.requirements.antivirus = true;
			} else if (m) {
				d.requirements.min_build = Number(m[1]);
			} else {
				fail(`${DEVICE_FILE}: line ${i + 1}: unknown requirement '${t}'; fix it by hand first`);
			}
		}

		devices.push(d);
	});

	return devices;
}

function writeDevices(devices) {
	const lines = [
		'# my_vpn 기기 목록. MCP(my_vpn_ctl)가 관리한다 — 손으로 고쳐도 되지만 주석은 줄 끝의',
		'# 이름표만 남는다. 형식은 docs/linux.md 의 "기기 등록과 포스처 정책".',
		'#',
		'# id (32 hex)  key (64 hex)  tunnel-ip  requirements  # label',
	];

	for (const d of devices) {
		const req = [];
		let line;

		if (d.requirements.firewall) req.push('firewall=on');
		if (d.requirements.antivirus) req.push('antivirus=on');
		if (d.requirements.min_build > 0) req.push(`min-build=${d.requirements.min_build}`);

		line = [d.id, d.key, d.tunnel_ip, ...req].join(' ');
		if (d.label) {
			line += `  # ${d.label}`;
		}
		if (Buffer.byteLength(line) > LINE_MAX - 1) {
			fail('device line too long');
		}
		lines.push(line);
	}

	writeAtomic(DEVICE_FILE, lines.join('\n') + '\n');
}

/* 키는 어떤 응답에도 넣지 않는다. */
function publicDevice(d) {
	return { id: d.id, label: d.label, tunnel_ip: d.tunnel_ip, requirements: d.requirements };
}

function readAcl() {
	let text;

	try {
		text = fs.readFileSync(ACL_FILE, 'utf8');
	} catch (e) {
		if (e.code === 'ENOENT') {
			return null;
		}
		throw e;
	}

	const rules = [];

	for (const raw of text.split('\n')) {
		const tok = raw.replace(/#.*/, '').trim().split(/\s+/).filter(Boolean);

		if (tok.length >= 5) {
			rules.push({ src: tok[0], dst: tok[1], proto: tok[2], dport: tok[3], action: tok[4] });
		}
	}

	return rules;
}

function aclHash() {
	try {
		return crypto.createHash('sha256').update(fs.readFileSync(ACL_FILE)).digest('hex');
	} catch (e) {
		return 'none';
	}
}

/* 서버가 지금 쓰고 있는 ACL 의 해시. service.sh 가 시작할 때 적고, 다시 읽기에
 * 성공하면 reload() 가 고쳐 적는다. ACL 없이 뜬 서버는 'none' 이다. */
function aclApplied() {
	try {
		return fs.readFileSync(ACL_APPLIED, 'utf8').trim();
	} catch (e) {
		return null;
	}
}

/* 파일은 바뀌었는데 서버는 아직 예전 규칙으로 도는 상태. 규칙을 고친 것은 SIGHUP 으로
 * 적용되지만, ACL 을 켜고 끄는 것은 서버를 다시 띄워야 한다 — 서버는 시작할 때 규칙
 * 파일을 받았는지로 ACL 을 쓸지 정하고, 도는 중에는 그걸 바꾸지 않는다. */
function aclPending(running) {
	const applied = aclApplied();

	return running && applied !== null && applied !== aclHash();
}

/* ---- systemd / journal ---------------------------------------------------- */

function unit() {
	const r = run('/usr/bin/systemctl', ['show', UNIT, '--timestamp=unix',
		'-p', 'LoadState,ActiveState,SubState,MainPID,InvocationID,Result,ExecMainStatus,UnitFileState,ExecMainStartTimestamp']);
	const p = {};

	for (const line of r.stdout.split('\n')) {
		const eq = line.indexOf('=');
		if (eq > 0) {
			p[line.slice(0, eq)] = line.slice(eq + 1);
		}
	}
	if (p.LoadState !== 'loaded') {
		fail(`${UNIT} is not installed (run mcp/install.sh)`);
	}

	p.running = p.ActiveState === 'active';

	return p;
}

function journal(args) {
	const r = run('/usr/bin/journalctl', ['--no-pager', '-q', '-o', 'json', ...args]);
	const out = [];

	for (const line of r.stdout.split('\n')) {
		let e;

		if (!line) {
			continue;
		}
		try {
			e = JSON.parse(line);
		} catch (err) {
			continue;
		}
		if (typeof e.MESSAGE === 'string') {
			out.push({
				time: new Date(Number(e.__REALTIME_TIMESTAMP) / 1000).toISOString(),
				msg: e.MESSAGE,
			});
		}
	}

	return out;
}

function tailLog(n) {
	return journal(['-u', UNIT, '-n', String(n)]).map((e) => `${e.time} ${e.msg}`);
}

const POSTURE_STATE = ['unknown', 'off', 'on'];

/* 서버에는 상태를 물어볼 창구가 없다. 이번 실행의 로그에서 세션이 생기고 사라진
 * 줄만 뽑아 차례로 적용하면 서버의 피어 테이블과 같은 것이 나온다. */
function sessions(invocation) {
	const live = new Map();
	const rejected = [];
	let m;

	if (!invocation) {
		return { sessions: [], recent_rejections: [] };
	}

	for (const e of journal([`_SYSTEMD_INVOCATION_ID=${invocation}`, '--grep', '^\\[(peer|HS|ka)\\] '])) {
		const s = e.msg;

		if ((m = /^\[peer\] register (\S+) <- (\S+) idx=([0-9a-f]{8})/.exec(s))) {
			live.set(m[1], { tunnel_ip: m[1], endpoint: m[2], session_idx: m[3], since: e.time });
		} else if ((m = /^\[HS\] device ([0-9a-f]{32}) authenticated as (\S+) \(build=(\d+) firewall=(\d+) antivirus=(\d+)\)/.exec(s))) {
			const p = live.get(m[2]);
			if (p) {
				p.device_id = m[1];
				p.posture_at_handshake = {
					os_build: Number(m[3]),
					firewall: POSTURE_STATE[m[4]] || m[4],
					antivirus: POSTURE_STATE[m[5]] || m[5],
				};
			}
		} else if ((m = /^\[peer\] rebind (\S+): \S+ -> (\S+)/.exec(s))) {
			if (live.has(m[1])) {
				live.get(m[1]).endpoint = m[2];
			}
		} else if ((m = /^\[peer\] drop (\S+) idx=/.exec(s)) ||
				(m = /^\[peer\] expire inner ip\[([^\]]+)\]/.exec(s)) ||
				(m = /^\[peer\] table full evicting (\S+)/.exec(s))) {
			live.delete(m[1]);
		} else if (/rejected|bad auth|unknown device|no longer registered|not its tunnel ip/.test(s)) {
			rejected.push(`${e.time} ${s}`);
		}
	}

	return { sessions: [...live.values()], recent_rejections: rejected.slice(-5) };
}

function tunnel(pid) {
	let dev = null;

	try {
		for (const fd of fs.readdirSync(`/proc/${pid}/fdinfo`)) {
			const m = /^iff:\s*(\S+)/m.exec(fs.readFileSync(`/proc/${pid}/fdinfo/${fd}`, 'utf8'));
			if (m) {
				dev = m[1];
				break;
			}
		}
	} catch (e) {
		return null;
	}
	if (dev === null) {
		return null;
	}

	try {
		const link = JSON.parse(run('/usr/sbin/ip', ['-j', 'addr', 'show', 'dev', dev]).stdout)[0];
		const a = (link.addr_info || []).find((x) => x.family === 'inet');

		return {
			device: dev,
			address: a ? `${a.local}/${a.prefixlen}` : null,
			mtu: link.mtu,
			up: (link.flags || []).includes('UP'),
		};
	} catch (e) {
		return { device: dev };
	}
}

function status() {
	const u = unit();
	const env = readEnv();
	const acl = readAcl();
	const st = {
		running: u.running,
		state: `${u.ActiveState}/${u.SubState}`,
		start_at_boot: u.UnitFileState === 'enabled',
		listen: `udp/${env.PORT}`,
		tunnel_address: `${env.TUN_IP}/${env.PREFIX}`,
		registered_devices: readDevices().length,
		acl: {
			enabled: acl !== null,
			rules: acl === null ? 0 : acl.length,
			restart_required: aclPending(u.running),
		},
	};

	if (u.running) {
		st.pid = Number(u.MainPID);
		st.started_at = new Date(Number(u.ExecMainStartTimestamp.slice(1)) * 1000).toISOString();
		st.tunnel = tunnel(u.MainPID);
		Object.assign(st, sessions(u.InvocationID));
	} else if (u.ActiveState === 'failed') {
		st.last_result = `${u.Result} (exit status ${u.ExecMainStatus})`;
		st.last_log = tailLog(15);
	}

	return st;
}

function systemctl(verb) {
	const r = run('/usr/bin/systemctl', [verb, UNIT], { timeout: 30000 });

	if (r.status !== 0) {
		fail(`systemctl ${verb} failed: ${(r.stderr || '').trim()}\n--- last log\n${tailLog(15).join('\n')}`);
	}
}

/* SIGHUP 을 보내고 서버가 실제로 새 목록을 받아들였는지 로그로 확인한다. 서버는
 * 파일이 마음에 들지 않으면 이전 목록을 그대로 쓰는데, 그걸 모르고 "적용됨"이라고
 * 답하면 폐기했다고 믿은 기기가 계속 붙어 있게 된다. */
function reload() {
	const u = unit();
	const filter = [`_SYSTEMD_INVOCATION_ID=${u.InvocationID}`, '--grep', '^(device|acl): reload'];
	let before, now, i, acl, dev, aclLive;

	if (!u.running) {
		return { reloaded: false, note: 'server is not running; the new files are read at the next start' };
	}

	/* ACL 을 켠 채 뜬 서버만 ACL 을 다시 읽고, 그때는 결과를 두 줄 찍는다. */
	aclLive = aclApplied() !== 'none';
	before = journal(filter).length;
	systemctl('reload');

	/* 서버는 epoll 타임아웃(1초)마다 요청을 확인한다. */
	for (i = 0; i < 30; i++) {
		now = journal(filter).slice(before);
		dev = now.find((e) => e.msg.startsWith('device:'));
		acl = now.find((e) => e.msg.startsWith('acl:'));
		if (dev && (acl || !aclLive)) {
			break;
		}
		spawnSync('/usr/bin/sleep', ['0.1']);
	}
	if (i === 30) {
		fail('sent SIGHUP but the server did not report a reload within 3s');
	}

	if (dev.msg.includes('failed')) {
		fail('server rejected the device file and kept the previous list');
	}
	if (acl && !acl.msg.includes('failed')) {
		fs.writeFileSync(ACL_APPLIED, aclHash() + '\n');
	} else if (acl && fs.existsSync(ACL_FILE)) {
		fail('server rejected the ACL file and kept the previous rules');
	}

	return {
		reloaded: true,
		note: 'ACL rules apply from the next packet; open sessions are re-evaluated against the device list at their next keepalive (within 10s)',
	};
}

/* ---- 명령 ------------------------------------------------------------------ */

const COMMANDS = {
	status() {
		return status();
	},

	start() {
		if (unit().running) {
			return { changed: false, ...status() };
		}
		systemctl('start');
		return { changed: true, ...status() };
	},

	stop() {
		const was = unit().running;

		systemctl('stop');
		/* 시작에 실패한 유닛은 stop 뒤에도 failed 로 남는다. 내리라고 했으면 지운다. */
		run('/usr/bin/systemctl', ['reset-failed', UNIT]);
		return { changed: was, ...status() };
	},

	restart() {
		systemctl('restart');
		return { changed: true, ...status() };
	},

	reload() {
		return reload();
	},

	logs(a) {
		const lines = a.lines === undefined ? 50 : wantInt(a.lines, 'lines', 1, 500);
		const args = ['-u', UNIT];
		let out;

		if (a.contains !== undefined && typeof a.contains !== 'string') {
			fail('contains must be a string');
		}
		if (a.since_seconds !== undefined) {
			args.push('--since', `-${wantInt(a.since_seconds, 'since_seconds', 1, 7 * 86400)}s`);
		}
		/* 걸러 낼 때는 넉넉히 읽고 뒤에서 자른다. 패킷마다 한 줄씩 찍히는 로그라
		 * 마지막 N 줄만 읽어서는 찾는 줄이 거의 남지 않는다. */
		args.push('-n', a.contains ? '20000' : String(lines));

		out = journal(args);
		if (a.contains) {
			out = out.filter((e) => e.msg.includes(a.contains));
		}

		return { lines: out.slice(-lines).map((e) => `${e.time} ${e.msg}`) };
	},

	device_list() {
		return { devices: readDevices().map(publicDevice) };
	},

	device_add(a) {
		const devices = readDevices();
		const d = {
			id: wantId(a.id),
			key: a.key,
			tunnel_ip: a.tunnel_ip,
			requirements: wantRequirements(a.requirements),
			label: wantLabel(a.label),
		};

		if (typeof d.key !== 'string' || !/^[0-9a-fA-F]{64}$/.test(d.key)) {
			fail('key must be 64 hex digits');
		}
		d.key = d.key.toLowerCase();

		if (!isIPv4(d.tunnel_ip)) {
			fail('tunnel_ip must be an IPv4 address');
		}
		if (d.tunnel_ip === readEnv().TUN_IP) {
			fail(`tunnel_ip ${d.tunnel_ip} is the server's own tunnel address`);
		}
		if (devices.length >= MAX_DEVICES) {
			fail(`device list is full (max ${MAX_DEVICES})`);
		}
		if (devices.some((x) => x.id === d.id)) {
			fail('a device with this id is already registered');
		}
		if (devices.some((x) => x.tunnel_ip === d.tunnel_ip)) {
			fail(`tunnel_ip ${d.tunnel_ip} is already assigned`);
		}

		devices.push(d);
		writeDevices(devices);

		return { device: publicDevice(d), ...reload() };
	},

	device_update(a) {
		const devices = readDevices();
		const id = wantId(a.id);
		const d = devices.find((x) => x.id === id);

		if (!d) {
			fail('no device with this id');
		}
		if (a.requirements !== undefined) {
			d.requirements = wantRequirements(a.requirements);
		}
		if (a.label !== undefined) {
			d.label = wantLabel(a.label);
		}
		if (a.tunnel_ip !== undefined) {
			if (!isIPv4(a.tunnel_ip)) {
				fail('tunnel_ip must be an IPv4 address');
			}
			if (a.tunnel_ip === readEnv().TUN_IP) {
				fail(`tunnel_ip ${a.tunnel_ip} is the server's own tunnel address`);
			}
			if (devices.some((x) => x !== d && x.tunnel_ip === a.tunnel_ip)) {
				fail(`tunnel_ip ${a.tunnel_ip} is already assigned`);
			}
			d.tunnel_ip = a.tunnel_ip;
		}

		writeDevices(devices);

		return { device: publicDevice(d), ...reload() };
	},

	device_remove(a) {
		const devices = readDevices();
		const id = wantId(a.id);
		const left = devices.filter((x) => x.id !== id);

		if (left.length === devices.length) {
			fail('no device with this id');
		}

		writeDevices(left);

		return { removed: id, ...reload() };
	},

	acl_get() {
		const rules = readAcl();
		const running = unit().running;

		return {
			enabled: rules !== null,
			rules: rules || [],
			restart_required: aclPending(running),
			note: 'first matching rule wins; no match means deny. Applies to client -> server packets only. Rule changes apply without a restart; turning the ACL on or off needs server_restart.',
		};
	},

	acl_set(a) {
		const lines = ['# my_vpn ACL. MCP(my_vpn_ctl)가 관리한다. 형식은 docs/linux.md 의 "ACL".',
			'# src  dst  proto  dport  action'];

		if (!Array.isArray(a.rules) || a.rules.length === 0) {
			fail('rules must be a non-empty array (use acl_clear to turn the ACL off)');
		}
		if (a.rules.length > MAX_RULES) {
			fail(`too many rules (max ${MAX_RULES})`);
		}

		a.rules.forEach((r, i) => {
			const dport = r && r.dport !== undefined ? r.dport : 'any';

			if (!r || typeof r !== 'object') fail(`rule ${i}: must be an object`);
			if (!isCidr(r.src)) fail(`rule ${i}: bad src '${r.src}' (a.b.c.d, a.b.c.d/n, any)`);
			if (!isCidr(r.dst)) fail(`rule ${i}: bad dst '${r.dst}' (a.b.c.d, a.b.c.d/n, any)`);
			if (!isProto(r.proto)) fail(`rule ${i}: bad proto '${r.proto}' (icmp, tcp, udp, any, 0-255)`);
			if (!isPort(dport)) fail(`rule ${i}: bad dport '${dport}' (n, lo-hi, any)`);
			if (r.action !== 'allow' && r.action !== 'deny') fail(`rule ${i}: action must be allow or deny`);

			lines.push([r.src, r.dst, r.proto, dport, r.action].join(' '));
		});

		writeAtomic(ACL_FILE, lines.join('\n') + '\n');

		/* ACL 을 켠 채 돌고 있는 서버에는 바로 적용한다. ACL 없이 뜬 서버는 다시
		 * 읽어도 켜지지 않으므로 restart_required 로 남는다. */
		if (unit().running && aclApplied() !== 'none') {
			reload();
		}

		return COMMANDS.acl_get();
	},

	acl_clear() {
		fs.rmSync(ACL_FILE, { force: true });

		return COMMANDS.acl_get();
	},

	/* auth_check.py 는 자기 서버를 따로 띄워 tun 에 10.0.0.1/24 를 붙인다. 운영 서버와
	 * 같은 대역이라 둘이 함께 돌 수 없다. */
	auth_check() {
		let r;

		if (unit().running || run('/usr/bin/pgrep', ['-x', 'vpn_server']).status === 0) {
			fail('a vpn_server is running; stop it first (the check starts its own server on the same tunnel subnet)');
		}

		r = run('/usr/bin/python3', ['-I', AUTH_CHECK], { timeout: 180000, cwd: '/' });

		return {
			passed: r.status === 0,
			output: (r.stdout || '') + (r.stderr || ''),
		};
	},
};

function main() {
	let req, out;

	if (process.getuid() !== 0) {
		process.stderr.write('my_vpn_ctl: must run as root (sudo -n my_vpn_ctl)\n');
		return 2;
	}
	if (process.argv.length > 2) {
		process.stderr.write('my_vpn_ctl: takes no arguments; send {"cmd": ...} on stdin\n');
		return 2;
	}

	try {
		req = JSON.parse(fs.readFileSync(0, 'utf8'));
		if (req === null || typeof req !== 'object' || !Object.hasOwn(COMMANDS, req.cmd)) {
			fail(`unknown command (${Object.keys(COMMANDS).join(', ')})`);
		}
		out = { ok: true, ...COMMANDS[req.cmd](req) };
	} catch (e) {
		/* 예상한 실패만 그대로 전한다. 그 밖의 예외는 경로 같은 내부 사정을 담고
		 * 있을 수 있으니 종류만 알린다. */
		out = { ok: false, error: e instanceof CtlError ? e.message : `internal error: ${e.code || e.name}` };
		if (!(e instanceof CtlError)) {
			process.stderr.write(`${e.stack}\n`);
		}
	}

	process.stdout.write(JSON.stringify(out) + '\n');

	return out.ok ? 0 : 1;
}

process.exitCode = main();
