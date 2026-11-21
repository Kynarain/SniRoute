/**
 * sniroute DoH Worker  v3
 *
 * 给「挂在 Cloudflare 上、但自己没发布 ECH 配置」的站点补上 Cloudflare 全网共享的
 * ECH 配置（greeting name = cloudflare-ech.com）。浏览器拿到后会把真实 SNI 加密，
 * 外层只露 cloudflare-ech.com，按域名做的阻断因此失效。
 *
 * v3 修改：不再用 JSON 接口取 ECH 配置（Workers 里访问 cloudflare-dns.com 的 JSON
 * 接口实测取不到），改成走同一条线格式 POST 通道 —— 这条通道已经被证明可用。
 * 另外加了 /debug 端点，方便直接看它到底取到了什么。
 */

const MANAGED = new Set([
  'pixiv.net', 'www.pixiv.net',
  'discord.com', 'www.discord.com', 'gateway.discord.gg', 'discord.gg',
  'cdn.discordapp.com', 'media.discordapp.net',
  'x.com', 'www.x.com', 'api.x.com', 't.co',
  // X 的样式/JS 和 API 也走 Cloudflare —— 不补这些，页面会是裸 HTML
  'abs.twimg.com', 'twitter.com', 'www.twitter.com', 'api.twitter.com',
  'upload.twitter.com',
  'grok.com', 'www.grok.com', 'x.ai', 'www.x.ai',
]);

const CF_DOH = 'https://cloudflare-dns.com/dns-query';
const ECH_TTL_MS = 30 * 60 * 1000;

// 取不到时用的兜底值（2026-11-21 实测）。它会轮换，只作为最后手段。
const ECH_FALLBACK = 'AEX+DQBBegAgACBhBxtyWmq+iphSc8XUVP5t7SkbbLuvOPW13YyC0rIIWAAEAAEAAQASY2xvdWRmbGFyZS1lY2guY29tAAA=';

let echCache = { value: null, at: 0, source: 'none' };

// ---------------- DNS 线格式 ----------------

function buildQuery(name, qtype, id) {
  const labels = [];
  for (const part of name.split('.')) {
    const bytes = [...part].map((c) => c.charCodeAt(0) & 0xff);
    labels.push(bytes.length, ...bytes);
  }
  const out = new Uint8Array(12 + labels.length + 1 + 4);
  out[0] = (id >> 8) & 0xff;
  out[1] = id & 0xff;
  out[2] = 0x01;
  out[3] = 0x00;
  out[4] = 0; out[5] = 1;
  out.set(labels, 12);
  let p = 12 + labels.length;
  out[p++] = 0;
  out[p++] = (qtype >> 8) & 0xff; out[p++] = qtype & 0xff;
  out[p++] = 0; out[p++] = 1;
  return out;
}

function parseQuestion(u8) {
  if (u8.length < 12) return null;
  const qdcount = (u8[4] << 8) | u8[5];
  if (qdcount !== 1) return null;
  let off = 12;
  const labels = [];
  while (off < u8.length && u8[off] !== 0) {
    const len = u8[off];
    if (off + 1 + len > u8.length) return null;
    let s = '';
    for (let i = 0; i < len; i++) s += String.fromCharCode(u8[off + 1 + i]);
    labels.push(s);
    off += 1 + len;
  }
  if (off >= u8.length) return null;
  off += 1;
  if (off + 4 > u8.length) return null;
  return {
    id: (u8[0] << 8) | u8[1],
    name: labels.join('.').toLowerCase(),
    qtype: (u8[off] << 8) | u8[off + 1],
    qclass: (u8[off + 2] << 8) | u8[off + 3],
    qend: off + 4,
    raw: u8,
  };
}

/** 从 DNS 响应里取出第一条 HTTPS(65) 记录的 SVCB 参数。 */
function parseHttpsAnswer(resp) {
  if (resp.length < 12) return null;
  const ancount = (resp[6] << 8) | resp[7];
  let p = 12;
  while (p < resp.length && resp[p] !== 0) p += 1 + resp[p];
  p += 1 + 4;
  for (let i = 0; i < ancount; i++) {
    if (p + 10 > resp.length) return null;
    p += 2;
    const type = (resp[p] << 8) | resp[p + 1]; p += 2;
    p += 2; // class
    p += 4; // ttl
    const rdlen = (resp[p] << 8) | resp[p + 1]; p += 2;
    if (p + rdlen > resp.length) return null;
    if (type === 65) {
      const rdata = resp.subarray(p, p + rdlen);
      const priority = (rdata[0] << 8) | rdata[1];
      let q = 2;
      while (q < rdata.length && rdata[q] !== 0) q += 1 + rdata[q];
      q += 1;
      const params = {};
      while (q + 4 <= rdata.length) {
        const key = (rdata[q] << 8) | rdata[q + 1];
        const len = (rdata[q + 2] << 8) | rdata[q + 3];
        q += 4;
        params[key] = rdata.subarray(q, q + len);
        q += len;
      }
      return { priority, params };
    }
    p += rdlen;
  }
  return null;
}

function b64(bytes) {
  let s = '';
  for (let i = 0; i < bytes.length; i++) s += String.fromCharCode(bytes[i]);
  return btoa(s);
}

/** 用线格式通道向 Cloudflare 的 DoH 取 ECH 配置（这条通道已验证可用）。 */
async function fetchEchViaWire() {
  const query = buildQuery('cloudflare-ech.com', 65, 0x5a5a);
  const resp = await fetch(CF_DOH, {
    method: 'POST',
    headers: { 'content-type': 'application/dns-message', accept: 'application/dns-message' },
    body: query,
  });
  if (resp.status !== 200) return null;
  const bytes = new Uint8Array(await resp.arrayBuffer());
  const svcb = parseHttpsAnswer(bytes);
  if (!svcb || !svcb.params[5]) return null;
  return b64(svcb.params[5]);
}

async function getEch() {
  const now = Date.now();
  if (echCache.value && now - echCache.at < ECH_TTL_MS) return echCache;
  try {
    const ech = await fetchEchViaWire();
    if (ech) {
      echCache = { value: ech, at: now, source: 'wire' };
      return echCache;
    }
  } catch (e) {
    // 落到兜底
  }
  echCache = { value: ECH_FALLBACK, at: now, source: 'fallback' };
  return echCache;
}

// ---------------- 合成 HTTPS 记录 ----------------

function svcParam(key, value) {
  const out = new Uint8Array(4 + value.length);
  out[0] = (key >> 8) & 0xff;
  out[1] = key & 0xff;
  out[2] = (value.length >> 8) & 0xff;
  out[3] = value.length & 0xff;
  out.set(value, 4);
  return out;
}

function buildSvcB(ips, echB64) {
  const chunks = [new Uint8Array([0, 1]), new Uint8Array([0])];
  // alpn = h3,h2：把 HTTP/3 也声明出来，让浏览器优先尝试 QUIC。
  // QUIC 的 ClientHello 在加密载荷里，SNI 天生不可见 —— 实测即使关掉 ECH，
  // 强制 QUIC 访问 pixiv 也能 3/3 成功，而同一 IP 走 TCP 立刻被重置。
  chunks.push(svcParam(1, new Uint8Array([2, 0x68, 0x33, 2, 0x68, 0x32])));
  const v4 = [];
  for (const ip of ips) for (const part of ip.split('.')) v4.push(Number(part) & 0xff);
  if (v4.length) chunks.push(svcParam(4, new Uint8Array(v4)));
  if (echB64) {
    const bin = atob(echB64);
    const arr = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; i++) arr[i] = bin.charCodeAt(i);
    chunks.push(svcParam(5, arr));
  }
  const total = chunks.reduce((n, c) => n + c.length, 0);
  const out = new Uint8Array(total);
  let p = 0;
  for (const c of chunks) {
    out.set(c, p);
    p += c.length;
  }
  return out;
}

function buildHttpsResponse(q, rdata) {
  const out = new Uint8Array(q.qend + 12 + rdata.length);
  out[0] = (q.id >> 8) & 0xff;
  out[1] = q.id & 0xff;
  out[2] = 0x81;
  out[3] = 0x80;
  out[4] = 0; out[5] = 1;
  out[6] = 0; out[7] = 1;
  out.set(q.raw.subarray(12, q.qend), 12);
  let p = q.qend;
  out[p++] = 0xc0; out[p++] = 0x0c;
  out[p++] = 0x00; out[p++] = 65;
  out[p++] = 0x00; out[p++] = 0x01;
  out[p++] = 0; out[p++] = 0; out[p++] = 0x01; out[p++] = 0x2c;
  out[p++] = (rdata.length >> 8) & 0xff;
  out[p++] = rdata.length & 0xff;
  out.set(rdata, p);
  return out;
}

/** 目标站点的真实 IP：走同一条线格式通道查 A。 */
async function fetchA(name) {
  const query = buildQuery(name, 1, 0xa1a1);
  const resp = await fetch(CF_DOH, {
    method: 'POST',
    headers: { 'content-type': 'application/dns-message', accept: 'application/dns-message' },
    body: query,
  });
  if (resp.status !== 200) return [];
  const bytes = new Uint8Array(await resp.arrayBuffer());
  const ancount = (bytes[6] << 8) | bytes[7];
  let p = 12;
  while (p < bytes.length && bytes[p] !== 0) p += 1 + bytes[p];
  p += 1 + 4;
  const ips = [];
  for (let i = 0; i < ancount && p + 10 <= bytes.length; i++) {
    p += 2;
    const type = (bytes[p] << 8) | bytes[p + 1]; p += 2;
    p += 2;
    p += 4;
    const rdlen = (bytes[p] << 8) | bytes[p + 1]; p += 2;
    if (type === 1 && rdlen === 4) {
      ips.push(bytes[p] + '.' + bytes[p + 1] + '.' + bytes[p + 2] + '.' + bytes[p + 3]);
    }
    p += rdlen;
  }
  return ips.slice(0, 4);
}

// ---------------- 入口 ----------------

async function forwardRaw(url, method, headers, body) {
  const init = { method, headers };
  if (body) init.body = body;
  const resp = await fetch(CF_DOH + url.search, init);
  return new Response(resp.body, {
    status: resp.status,
    headers: {
      'content-type': resp.headers.get('content-type') || 'application/dns-message',
      'cache-control': 'max-age=60',
    },
  });
}

export default {
  async fetch(request) {
    const url = new URL(request.url);

    if (url.pathname === '/') {
      return new Response('sniroute doh ok\n', { headers: { 'content-type': 'text/plain' } });
    }

    // 自诊断：直接看它取到的 ECH 配置来源
    if (url.pathname === '/debug') {
      const ech = await getEch();
      return new Response(
        JSON.stringify(
          {
            ech_source: ech.source,
            ech_prefix: ech.value ? ech.value.slice(0, 32) : null,
            ech_length: ech.value ? ech.value.length : 0,
            managed_count: MANAGED.size,
          },
          null,
          2,
        ),
        { headers: { 'content-type': 'application/json' } },
      );
    }

    if (url.pathname !== '/dns-query') {
      return new Response('not found', { status: 404 });
    }

    if (request.method === 'POST') {
      let body;
      try {
        body = new Uint8Array(await request.arrayBuffer());
      } catch (e) {
        return new Response('bad body', { status: 400 });
      }

      const q = parseQuestion(body);
      if (q && q.qtype === 65 && MANAGED.has(q.name)) {
        const ech = await getEch();
        if (ech.value) {
          const ips = await fetchA(q.name);
          const rdata = buildSvcB(ips, ech.value);
          return new Response(buildHttpsResponse(q, rdata), {
            headers: { 'content-type': 'application/dns-message', 'cache-control': 'max-age=300' },
          });
        }
      }

      const accept = request.headers.get('accept') || 'application/dns-message';
      return forwardRaw(url, 'POST',
        { accept, 'content-type': 'application/dns-message' }, body);
    }

    const accept = request.headers.get('accept') || 'application/dns-message';
    return forwardRaw(url, 'GET', { accept });
  },
};
