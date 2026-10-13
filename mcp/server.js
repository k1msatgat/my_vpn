#!/usr/bin/env node
/* my_vpn MCP 서버 (stdio).
 *
 * AI 클라이언트가 ssh 로 이 프로그램을 띄우고 stdin/stdout 으로 MCP 를 말한다.
 * 여기에는 권한도 로직도 없다. 도구의 입력 형식을 선언하고, 요청을 그대로
 * my_vpn_ctl(root 헬퍼)에 넘기고, 답을 돌려줄 뿐이다. 검증과 실제 동작은 ctl.cjs 에 있다.
 *
 * stdout 은 프로토콜 전용이다. 사람이 볼 메시지는 stderr 로만 보낸다. */

import { spawn } from 'node:child_process';
import { McpServer } from '@modelcontextprotocol/sdk/server/mcp.js';
import { StdioServerTransport } from '@modelcontextprotocol/sdk/server/stdio.js';
import { z } from 'zod';

const CTL = '/usr/local/sbin/my_vpn_ctl';
const CTL_TIMEOUT_MS = 200000;	/* auth_check 가 가장 길다 */

function ctl(request) {
	const root = process.getuid() === 0;
	const child = spawn(root ? CTL : 'sudo', root ? [] : ['-n', CTL], {
		stdio: ['pipe', 'pipe', 'pipe'],
		timeout: CTL_TIMEOUT_MS,
	});

	return new Promise((resolve) => {
		let out = '';
		let err = '';

		child.stdout.on('data', (d) => { out += d; });
		child.stderr.on('data', (d) => { err += d; });
		child.on('error', (e) => resolve({ ok: false, error: `cannot run my_vpn_ctl: ${e.message}` }));
		child.on('close', () => {
			try {
				resolve(JSON.parse(out));
			} catch (e) {
				resolve({ ok: false, error: `my_vpn_ctl gave no answer: ${err.trim() || 'no output'}` });
			}
		});
		child.stdin.end(JSON.stringify(request));
	});
}

async function call(cmd, args = {}) {
	const { ok, error, ...result } = await ctl({ cmd, ...args });

	if (!ok) {
		return { isError: true, content: [{ type: 'text', text: error }] };
	}

	return { content: [{ type: 'text', text: JSON.stringify(result, null, 2) }] };
}

const requirements = z.object({
	firewall: z.boolean().optional().describe('Windows Firewall must be on'),
	antivirus: z.boolean().optional().describe('Windows Security Center must report a healthy antivirus'),
	min_build: z.number().int().min(0).optional().describe('minimum OS build (Windows 11 starts at 22000); 0 = no check'),
}).strict().describe('Posture the device must report at handshake and at every keepalive. Omitted items are not checked. A value the client reports as "unknown" never passes.');

const deviceId = z.string().regex(/^[0-9a-fA-F]{32}$/).describe('device id, 32 hex digits (from device_list)');
const label = z.string().max(32).describe('human-readable name, e.g. "woo-laptop"');
const tunnelIp = z.string().describe('tunnel IPv4 address assigned to this device, e.g. 10.0.0.3. The server pushes it to the device; nothing is typed on the client');

const READ = { readOnlyHint: true, openWorldHint: false };
const WRITE = { readOnlyHint: false, destructiveHint: false, openWorldHint: false };
const DISRUPT = { readOnlyHint: false, destructiveHint: true, openWorldHint: false };

const server = new McpServer({ name: 'my_vpn', version: '0.1.0' }, {
	instructions: [
		'Controls the my_vpn server (a UDP/TUN VPN with device authentication and posture checks) on this Linux host.',
		'Clients are separate Windows machines and are not controlled from here; a client is cut off or admitted by changing its entry in the device list.',
		'Device changes (device_add / device_update / device_remove) and ACL rule changes apply to the running server immediately without dropping sessions. Only turning the ACL on for the first time or off (acl_clear) needs server_restart; restart_required tells you when.',
		'Call server_status first: it shows whether the server is up, who is connected and why a device was recently rejected.',
	].join(' '),
});

/* ---- 서버 ------------------------------------------------------------------ */

server.registerTool('server_status', {
	description: 'Show whether the VPN server is running, its listen port and tunnel interface, the current sessions as the server itself reports them (device, endpoint, last reported posture, seconds since last packet, packet/byte counts, ACL denials), counters of handshakes and rejections by reason, the last few rejection log lines, and whether an ACL change is waiting for a restart.',
	inputSchema: {},
	annotations: READ,
}, () => call('status'));

server.registerTool('server_start', {
	description: 'Start the VPN server and bring its tunnel interface up. No-op if it is already running.',
	inputSchema: {},
	annotations: WRITE,
}, () => call('start'));

server.registerTool('server_stop', {
	description: 'Stop the VPN server. Every client loses its tunnel until the server is started again.',
	inputSchema: {},
	annotations: DISRUPT,
}, () => call('stop'));

server.registerTool('server_restart', {
	description: 'Restart the VPN server. All sessions are dropped; clients re-handshake on their own within about 30 seconds. Needed only to turn the ACL on or off.',
	inputSchema: {},
	annotations: DISRUPT,
}, () => call('restart'));

server.registerTool('server_reload', {
	description: 'Make the running server re-read the device list and the ACL (SIGHUP) without dropping sessions. The device_* and acl_set tools already do this; use it only after a file was edited by hand.',
	inputSchema: {},
	annotations: WRITE,
}, () => call('reload'));

server.registerTool('server_logs', {
	description: 'Read the server log, oldest first. Lines look like "[HS] device <id> authenticated as <ip>", "[ka] device <id> posture rejected", "[peer] register/drop/expire", "[deny] <src> -> <dst>" (ACL), "[udp->tun]" / "[tun->udp]" (one per data packet).',
	inputSchema: {
		lines: z.number().int().min(1).max(500).optional().describe('how many lines to return (default 50)'),
		contains: z.string().max(100).optional().describe('only lines containing this exact text, e.g. "[HS]" or a device id'),
		since_seconds: z.number().int().min(1).max(604800).optional().describe('only lines from the last N seconds'),
	},
	annotations: READ,
}, (a) => call('logs', a));

/* ---- 기기 ------------------------------------------------------------------ */

server.registerTool('device_list', {
	description: 'List registered devices: id, label, tunnel IP and posture requirements. Keys are never returned.',
	inputSchema: {},
	annotations: READ,
}, () => call('device_list'));

server.registerTool('device_add', {
	description: 'Register a device so it may connect. The id and key come from the device itself ("vpn_client.exe enroll", or the GUI posture tab -> enroll). Applied to the running server immediately.',
	inputSchema: {
		id: deviceId.describe('device id, 32 hex digits, as printed by enroll'),
		key: z.string().regex(/^[0-9a-fA-F]{64}$/).describe('device key, 64 hex digits, as printed by enroll'),
		tunnel_ip: tunnelIp,
		requirements: requirements.optional(),
		label: label.optional(),
	},
	annotations: WRITE,
}, (a) => call('device_add', a));

server.registerTool('device_update', {
	description: 'Change a device\'s posture requirements, tunnel IP or label. "requirements" replaces the whole set. Applied immediately: a connected device that no longer meets the policy is dropped at its next keepalive (within 10s).',
	inputSchema: {
		id: deviceId,
		requirements: requirements.optional(),
		tunnel_ip: tunnelIp.optional(),
		label: label.optional(),
	},
	annotations: DISRUPT,
}, (a) => call('device_update', a));

server.registerTool('device_remove', {
	description: 'Revoke a device. Its key is deleted and cannot be read back; a connected session is dropped at its next keepalive (within 10s). Re-admitting it requires enrolling again.',
	inputSchema: { id: deviceId },
	annotations: DISRUPT,
}, (a) => call('device_remove', a));

/* ---- ACL ------------------------------------------------------------------- */

server.registerTool('acl_get', {
	description: 'Show the ACL rules for packets coming out of the tunnel (client -> server) and whether the running server still uses an older version (restart_required).',
	inputSchema: {},
	annotations: READ,
}, () => call('acl_get'));

server.registerTool('acl_set', {
	description: 'Replace the whole ACL: which destinations an admitted device may reach, optionally with a stricter posture per destination than the device needs to connect. The allowed destination ranges are also what each device is told to route into the tunnel (ranges inside the tunnel subnet and "any" are not pushed), so a rule for 10.20.0.0/16 both permits and routes it; connected devices pick up route changes within 10s. Rules are checked top to bottom, the first match wins, and a packet matching no rule is denied. Applied from the next packet if the server is already running with an ACL; if it was started without one, restart_required comes back true and server_restart turns it on.',
	inputSchema: {
		rules: z.array(z.object({
			src: z.string().describe('a.b.c.d, a.b.c.d/n or "any"'),
			dst: z.string().describe('a.b.c.d, a.b.c.d/n or "any"'),
			proto: z.string().describe('"icmp", "tcp", "udp", "any" or a protocol number'),
			dport: z.string().optional().describe('"n", "lo-hi" or "any" (tcp/udp only; default "any")'),
			action: z.enum(['allow', 'deny']),
			require: requirements.optional().describe('allow rules only: posture a session must currently report to be allowed by this rule, e.g. SSH only from devices with the firewall on. If the rule matches but the posture falls short the packet is denied; later rules are not tried.'),
		}).strict()).min(1).max(32),
	},
	annotations: DISRUPT,
}, (a) => call('acl_set', a));

server.registerTool('acl_clear', {
	description: 'Turn the ACL off (every packet from an authenticated session is accepted). Takes effect only after server_restart.',
	inputSchema: {},
	annotations: DISRUPT,
}, () => call('acl_clear'));

/* ---- 검사 ------------------------------------------------------------------ */

server.registerTool('auth_check', {
	description: 'Run the protocol-level self test of device authentication, posture checks and continuous verification against a temporary server instance (about a minute). The VPN server must be stopped first.',
	inputSchema: {},
	annotations: WRITE,
}, () => call('auth_check'));

await server.connect(new StdioServerTransport());
