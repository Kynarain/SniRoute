// 离线验证 worker 里的 DNS 线格式组装是否正确。
// 用法：node worker/test-wire.mjs

const ECH = 'AEX+DQBBegAgACBhBxtyWmq+iphSc8XUVP5t7SkbbLuvOPW13YyC0rIIWAAEAAEAAQASY2xvdWRmbGFyZS1lY2guY29tAAA=';
const IPS = ['104.18.42.239', '172.64.145.17'];

// ---- 从 worker 里复制过来的函数（保持一致） ----
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
  const chunks = [];
  chunks.push(new Uint8Array([0, 1]));
  chunks.push(new Uint8Array([0]));
  const alpn = new Uint8Array([2, 0x68, 0x32]);
  chunks.push(svcParam(1, alpn));
  const v4 = [];
  for (const ip of ips) for (const part of ip.split('.')) v4.push(Number(part) & 0xff);
  if (v4.length) chunks.push(svcParam(4, new Uint8Array(v4)));
  if (echB64) {
    const bin = Buffer.from(echB64, 'base64');
    chunks.push(svcParam(5, new Uint8Array(bin)));
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

function buildResponse(q, rdata) {
  const out = new Uint8Array(q.qend + 12 + rdata.length);
  out[0] = (q.id >> 8) & 0xff;
  out[1] = q.id & 0xff;
  out[2] = 0x81;
  out[3] = 0x80;
  out[4] = 0; out[5] = 1;
  out[6] = 0; out[7] = 1;
  out[8] = 0; out[9] = 0;
  out[10] = 0; out[11] = 0;
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

// ---- 构造一个真实形状的查询（www.pixiv.net type 65） ----
function buildQuery(name, qtype, id = 0x1234) {
  const labels = name.split('.').flatMap((l) => [l.length, ...Buffer.from(l, 'ascii')]);
  const q = new Uint8Array(12 + labels.length + 1 + 4);
  q[0] = (id >> 8) & 0xff; q[1] = id & 0xff;
  q[2] = 0x01; q[3] = 0x00;       // RD
  q[4] = 0; q[5] = 1;             // qdcount
  q.set(labels, 12);
  let p = 12 + labels.length;
  q[p++] = 0;                     // root label
  q[p++] = (qtype >> 8) & 0xff; q[p++] = qtype & 0xff;
  q[p++] = 0; q[p++] = 1;         // IN
  return q;
}

// ---- 解码响应，逐字段核对 ----
function decode(resp) {
  const qd = (resp[4] << 8) | resp[5];
  const an = (resp[6] << 8) | resp[7];
  const flags = resp[2];
  // 跳过问题
  let p = 12;
  while (resp[p] !== 0) p += 1 + resp[p];
  p += 1 + 4;
  const out = { qd, an, flags, answers: [] };
  for (let i = 0; i < an; i++) {
    const namePtr = (resp[p] << 8) | resp[p + 1];
    p += 2;
    const type = (resp[p] << 8) | resp[p + 1]; p += 2;
    const cls = (resp[p] << 8) | resp[p + 1]; p += 2;
    const ttl = ((resp[p] << 24) | (resp[p + 1] << 16) | (resp[p + 2] << 8) | resp[p + 3]) >>> 0;
    p += 4;
    const rdlen = (resp[p] << 8) | resp[p + 1]; p += 2;
    const rdata = resp.subarray(p, p + rdlen); p += rdlen;
    out.answers.push({ namePtr, type, cls, ttl, rdata });
  }
  return out;
}

function parseSvcB(rdata) {
  const priority = (rdata[0] << 8) | rdata[1];
  let i = 2;
  // target
  while (rdata[i] !== 0) i += 1 + rdata[i];
  i += 1;
  const params = {};
  while (i + 4 <= rdata.length) {
    const key = (rdata[i] << 8) | rdata[i + 1];
    const len = (rdata[i + 2] << 8) | rdata[i + 3];
    i += 4;
    params[key] = rdata.subarray(i, i + len);
    i += len;
  }
  return { priority, params };
}

// ---- 跑 ----
const query = buildQuery('www.pixiv.net', 65);
const q = parseQuestion(query);
console.log('解析查询 :', { id: q.id.toString(16), name: q.name, qtype: q.qtype, qclass: q.qclass });

const rdata = buildSvcB(IPS, ECH);
const resp = buildResponse(q, rdata);
const dec = decode(resp);
console.log('响应头   : qd=%d an=%d flags=0x%s', dec.qd, dec.an, dec.flags.toString(16));

const a = dec.answers[0];
console.log('答案记录 : type=%d class=%d ttl=%d rdlen=%d', a.type, a.cls, a.ttl, a.rdata.length);

const svcb = parseSvcB(a.rdata);
console.log('SVCB     : priority=%d 参数键=%s', svcb.priority, Object.keys(svcb.params).join(','));

const alpn = svcb.params[1];
console.log('alpn     :', alpn ? Buffer.from(alpn).toString('ascii').replace(/\x00/g, '') : '无');

const v4 = svcb.params[4];
console.log('ipv4hint :', v4 ? Array.from({ length: v4.length / 4 }, (_, k) => Array.from(v4.subarray(k * 4, k * 4 + 4)).join('.')) : '无');

const echParam = svcb.params[5];
const echB64 = echParam ? Buffer.from(echParam).toString('base64') : '';
console.log('ech 参数 :', echB64 === ECH ? '与原始配置完全一致 ✓' : '不一致 ✗');

// 再验一条：非 65 类型不应进入合成分支
const qA = parseQuestion(buildQuery('www.pixiv.net', 1));
console.log('A 查询   : qtype=%d（应为 1，不触发合成）', qA.qtype);
