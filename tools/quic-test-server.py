"""本地 QUIC 服务端：用来验证 quic-probe.py 构造的 Initial 是否合法。

它只做一件事：接受 QUIC 连接并回复。如果 quic-probe 能收到它的回包，
说明我们的包构造正确；这样"境外无回包"才能被解释为网络层面的丢包。

用法：
    python quic-test-server.py [port]
"""
import asyncio
import datetime
import ipaddress
import sys

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

from aioquic.asyncio import QuicConnectionProtocol, serve
from aioquic.quic.configuration import QuicConfiguration
from aioquic.quic.events import HandshakeCompleted, QuicEvent


def make_self_signed(tmpdir: str):
    key = ec.generate_private_key(ec.SECP256R1())
    subject = issuer = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(subject)
        .issuer_name(issuer)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=30))
        .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), critical=False)
        .sign(key, hashes.SHA256())
    )
    import os
    cert_path = os.path.join(tmpdir, "cert.pem")
    key_path = os.path.join(tmpdir, "key.pem")
    with open(cert_path, "wb") as fh:
        fh.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as fh:
        fh.write(key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        ))
    return cert_path, key_path


class EchoProtocol(QuicConnectionProtocol):
    def quic_event_received(self, event: QuicEvent) -> None:
        if isinstance(event, HandshakeCompleted):
            print(f"  [server] 握手完成 from {self._quic._network_paths[0].addr}", flush=True)

    def datagram_received(self, data, addr):
        print(f"  [server] 收到 {len(data)} 字节 from {addr[0]}:{addr[1]}", flush=True)
        super().datagram_received(data, addr)


async def main(port: int):
    import tempfile
    tmpdir = tempfile.mkdtemp()
    cert_path, key_path = make_self_signed(tmpdir)
    conf = QuicConfiguration(is_client=False, alpn_protocols=["h3"])
    conf.load_cert_chain(cert_path, key_path)
    print(f"  [server] 监听 127.0.0.1:{port}  alpn=h3", flush=True)
    await serve("127.0.0.1", port, configuration=conf, create_protocol=EchoProtocol)
    await asyncio.Future()


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 4433
    asyncio.run(main(port))
