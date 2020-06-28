#include <string>

// ggrpc
#include <ggrpc/ggrpc.h>

#include "ayame_signaling.h"
#include "ayame_transport.h"
#include "goa.grpc.pb.h"

class AyameServerCredentialsImpl final : public grpc::ServerCredentials {
 public:
  virtual void SetAuthMetadataProcessor(
      const std::shared_ptr<grpc::AuthMetadataProcessor>& processor) {}
  virtual int AddPortToServer(const grpc::string& addr, grpc_server* server) {
    grpc_core::ExecCtx exec_ctx;
    url_ = addr;
    grpc_channel_args args = {0, nullptr};
    transport_ =
        (AyameTransport*)goa_create_ayame_transport(addr.c_str(), &args, false);
    grpc_server_setup_transport(server, transport_, nullptr, &args, nullptr);
    grpc_server_add_listener(
        server, this, &AyameServerCredentialsImpl::StartListener,
        &AyameServerCredentialsImpl::DestroyListener, nullptr);
    return 1;
  }

  static void StartListener(grpc_server* /*server*/,
                            void* arg,
                            grpc_pollset** pollsets,
                            size_t pollset_count) {
    auto p = (AyameServerCredentialsImpl*)arg;
    p->transport_->Start(p->url_);
  }
  static void DestroyListener(grpc_server* /*server*/,
                              void* arg,
                              grpc_closure* destroy_done) {}

 private:
  std::string url_;
  AyameTransport* transport_;
};

std::shared_ptr<grpc::ServerCredentials> AyameServerCredentials() {
  return std::shared_ptr<grpc::ServerCredentials>(
      new AyameServerCredentialsImpl());
}

class SayHelloHandler
    : public ggrpc::ServerResponseWriterHandler<goa::HelloResponse,
                                                goa::HelloRequest> {
  goa::Greeter::AsyncService* service_;

 public:
  SayHelloHandler(goa::Greeter::AsyncService* service) : service_(service) {}

  // サーバ開始時にインスタンスが作られて Request() が呼ばれる。
  // Request() では接続待ち状態にする処理を書くこと。
  void Request(grpc::ServerContext* context,
               goa::HelloRequest* request,
               grpc::ServerAsyncResponseWriter<goa::HelloResponse>* writer,
               grpc::ServerCompletionQueue* cq,
               void* tag) override {
    service_->RequestSayHello(context, request, writer, cq, cq, tag);
  }
  // 接続が確立すると OnAccept() が呼ばれる。
  void OnAccept(goa::HelloRequest req) override {
    goa::HelloResponse resp;
    std::cout << "recv HelloRequest: " << req.DebugString();
    resp.set_message("Hello, " + req.name());
    std::cout << "send HelloResponse: " << resp.DebugString();
    Context()->Finish(resp, grpc::Status::OK);
  }
  void OnError(ggrpc::ServerResponseWriterError error) override {}
};

int main() {
  //rtc::LogMessage::LogToDebug(rtc::LS_INFO);
  //rtc::LogMessage::LogTimestamps();
  //rtc::LogMessage::LogThreads();

  ggrpc::Server server;
  goa::Greeter::AsyncService service;

  grpc::ServerBuilder builder;
  builder.AddListeningPort("wss://ayame-lite.shiguredo.jp/signaling",
                           AyameServerCredentials());
  builder.RegisterService(&service);

  // リクエストハンドラの登録
  server.AddResponseWriterHandler<SayHelloHandler>(&service);

  // スレッド数1でサーバを起動して待つ
  server.Start(builder, 1);
  server.Wait();
}
