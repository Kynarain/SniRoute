// 联网探针：用真实 DNS 线格式（浏览器就是这么发的）查询已部署的 DoH，
// 检查目标站点的 HTTPS 记录里是否带上了 ECH 配置。
// 用法：node worker/probe-doh.mjs

const DOH = 'https://doh.kynarain.cn/dns-query';
const TARGETS = ['www.pixiv.net', 'discord.com', 'x.com'];

function buildQuery(name, qtype, id) {
  const labels = name.split('.').flatMap((l) => [l.length, ...Buffer.from(l, 'ascii')]);
  const q = new Uint8Array(12 + labels.length + 1 + 4);
  q[0] = (id >> 8) & 0xff;
  q[1] = id & 0xff;
  q[2] = 0x01;
  q[3] = 0x00;
  q[4] = 0; q[5] = 1;
  q.set(labels, 12);
  let p = 12 + labels.length;
  q[p++] = 0;
  q[p++] = (qtype >> 8) & 0xff; q[p++] = qtype & 0xff;
  q[p++] = 0; q[p++] = 1;
  return q;
}

function decode(resp) {
  const an = (resp[6] << 8) | resp[7];
  let p = 12;
  while (resp[p] !== 0) p += 1 + resp[p];
  p += 1 + 4;
  const answers = [];
  for (let i = 0; i < an; i++) {
    p += 2;
    const type = (resp[p] << 8) | resp[p + 1]; p += 2;
    const cls = (resp[p] << 8) | resp[p + 1]; p += 2;
    const ttl = ((resp[p] << 24) | (resp[p + 1] << 16) | (resp[p + 2] << 8) | resp[p + 3]) >>> 0;
    p += 4;
    const rdlen = (resp[p] << 8) | resp[p + 1]; p += 2;
    answers.push({ type, cls, ttl, rdata: resp.subarray(p, p + rdlen) });
    p += rdlen;
  }
  return answers;
}

function parseSvcB(rdata) {
  const priority = (rdata[0] << 8) | rdata[1];
  let i = 2;
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

async function query(name, qtype, id = 0x4242) {
  const body = Buffer.from(buildQuery(name, qtype, id));
  const resp = await fetch(DOH, {
    method: 'POST',
    headers: { 'content-type': 'application/dns-message', accept: 'application/dns-message' },
    body,
  });
  if (resp.status !== 200) throw new Error('HTTP ' + resp.status);
  return new Uint8Array(await resp.arrayBuffer());
}

const echOk = [];
const echBad = [];

for (const name of TARGETS) {
  try {
    const resp = await query(name, 65);
    const answers = decode(resp);
    const https = answers.find((a) => a.type === 65);
    if (!https) {
      console.log(`${name.padEnd(16)} HTTPS 记录: 无（ans=${answers.length}）`);
      echBad.push(name);
      continue;
    }
    const svcb = parseSvcB(https.rdata);
    const ech = svcb.params[5];
    const v4 = svcb.params[4];
    const ips = v4
      ? Array.from({ length: v4.length / 4 }, (_, k) => Array.from(v4.subarray(k * 4, k * 4 + 4)).join('.'))
      : [];
    if (ech) {
      echOk.push(name);
      console.log(`${name.padEnd(16)} ech=有 (${Buffer.from(ech).toString('base64').slice(0, 24)}...) ipv4=${ips.join(',')}`);
    } else {
      echBad.push(name);
      console.log(`${name.padEnd(16)} ech=无  ipv4=${ips.join(',')}`);
    }
  } catch (e) {
    console.log(`${name.padEnd(16)} 查询失败: ${e.message}`);
    echBad.push(name);
  }
}

console.log('');
console.log(`带 ECH: ${echOk.length}/${TARGETS.length}  ->  ${echOk.join(', ') || '无'}`);
if (echBad.length) console.log(`缺 ECH: ${echBad.join(', ')}`);
