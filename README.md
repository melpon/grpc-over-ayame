# gRPC over Ayame

WebRTC Data Channel を使って gRPC で通信します。

シグナリングサーバとして [WebRTC Signaling Server Ayame](https://github.com/OpenAyame/ayame) を利用します。

## 実装

gRPC Core の transport を弄って、HTTP/2 の代わりに Data Channel を使うようにしています。
