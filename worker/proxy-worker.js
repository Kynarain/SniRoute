/**
 * Sniroute 云通道：Cloudflare Worker 版
 *
 * 用法：把本文件贴进自己的 Cloudflare Worker（或通过 wrangler 部署），
 * 然后在 Sniroute 里注册这个地址：
 *
 *   sniroute.cmd cloud add --name cf --template "https://<你的域名>/proxy?url={url}" --token <可选密钥>
 *   sniroute.cmd cloud fetch <目标URL> --provider cf -o out.bin
 *
 * 密钥：在 Worker 设置里加环境变量 TOKEN=xxx；不设则任何人可调（不建议公开）。
 *
 * 额度算你自己的（免费 10 万次/天）。这个文件只做 HTTP(S) 转发，
 * 不是代理/隧道——SSH、游戏 UDP 都不经过它。
 */
export default {
  async fetch(request, env) {
    const u = new URL(request.url);
    if (u.pathname !== "/proxy") {
      return new Response("sniroute proxy ok\n", { status: 200 });
    }

    if (env.TOKEN) {
      const got = request.headers.get("x-relay-token") || "";
      if (got !== env.TOKEN) {
        return new Response("forbidden\n", { status: 403 });
      }
    }

    const target = u.searchParams.get("url");
    if (!target) {
      return new Response("missing ?url=\n", { status: 400 });
    }
    if (!/^https?:\/\//i.test(target)) {
      return new Response("only http(s) targets\n", { status: 400 });
    }

    const headers = {};
    const range = request.headers.get("range");
    if (range) headers["range"] = range;

    let resp;
    try {
      resp = await fetch(target, {
        headers,
        redirect: "follow",
        cf: { cacheTtl: 0 },
      });
    } catch (e) {
      return new Response("upstream error: " + e.message + "\n", { status: 502 });
    }

    const out = new Headers(resp.headers);
    out.set("access-control-allow-origin", "*");
    out.delete("set-cookie");
    return new Response(resp.body, { status: resp.status, headers: out });
  },
};
