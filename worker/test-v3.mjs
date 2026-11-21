import { readFileSync } from 'node:fs';
const src = readFileSync('worker/doh-worker.js', 'utf8');
const grab = (name) => {
  const i = src.indexOf('function ' + name);
  if (i < 0) throw new Error('missing ' + name);
  let depth = 0, started = false, j = i;
  for (; j < src.length; j++) {
    if (src[j] === '{') { depth++; started = true; }
    else if (src[j] === '}') { depth--; if (started && depth === 0) { j++; break; } }
  }
  return src.slice(i, j);
};
const code = ['buildQuery','parseQuestion','parseHttpsAnswer','svcParam','buildSvcB','buildHttpsResponse']
  .map(grab).join('\n');
const M = new Function('atob','btoa', code + '\nreturn {buildQuery,parseQuestion,parseHttpsAnswer,buildSvcB,buildHttpsResponse};')(
  (s) => Buffer.from(s, 'base64').toString('binary'),
  (s) => Buffer.from(s, 'binary').toString('base64')
);
const ECH = 'AEX+DQBBegAgACBhBxtyWmq+iphSc8XUVP5t7SkbbLuvOPW13YyC0rIIWAAEAAEAAQASY2xvdWRmbGFyZS1lY2guY29tAAA=';
const q = M.parseQuestion(M.buildQuery('www.pixiv.net', 65, 0x1234));
console.log('  buildQuery/parseQuestion :', q.name, 'qtype=' + q.qtype);
const resp = M.buildHttpsResponse(q, M.buildSvcB(['104.18.42.239'], ECH));
const got = M.parseHttpsAnswer(resp);
console.log('  parseHttpsAnswer priority:', got.priority);
console.log('  alpn                    :', got.params[1] ? Buffer.from(got.params[1]).toString('ascii').replace(/\x00/g,'') : '无');
console.log('  ech 往返一致            :', Buffer.from(got.params[5]).toString('base64') === ECH ? '✓' : '✗');
const v4 = got.params[4];
console.log('  ipv4hint                :', Array.from({length: v4.length/4}, (_,k)=>Array.from(v4.subarray(k*4,k*4+4)).join('.')).join(','));