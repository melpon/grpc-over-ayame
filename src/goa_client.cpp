#include <iostream>
#include <string>

// ggrpc
#include <ggrpc/ggrpc.h>

#include "ayame_signaling.h"
#include "ayame_transport.h"
#include "goa.grpc.pb.h"

class AyameChannelCredentialsImpl final : public grpc::ChannelCredentials {
  std::shared_ptr<grpc::Channel> CreateChannelWithInterceptors(
      const std::string& target,
      const grpc::ChannelArguments& args,
      std::vector<std::unique_ptr<
          grpc::experimental::ClientInterceptorFactoryInterface>>
          interceptor_creators) override {
    grpc_channel_args channel_args;
    args.SetChannelArgs(&channel_args);
    return ::grpc::CreateChannelInternal(
        "", goa_create_ayame_channel(target.c_str(), &channel_args, true),
        std::move(interceptor_creators));
  }
  grpc::SecureChannelCredentials* AsSecureCredentials() override {
    return nullptr;
  }
  std::shared_ptr<grpc::Channel> CreateChannelImpl(
      const grpc::string& target,
      const grpc::ChannelArguments& args) override {
    return CreateChannelWithInterceptors(target, args, {});
  }
};

std::shared_ptr<grpc::ChannelCredentials> AyameChannelCredentials() {
  return std::shared_ptr<grpc::ChannelCredentials>(
      new AyameChannelCredentialsImpl());
}

/*

サーバ側:
  goa_create_ayame_transport 関数で ayame_transport を作ってから grpc_server_setup_transport を呼び出す
    - grpc_server_setup_transport 内で grpc_channel_create を呼んでサーバに設定してる

クライアント側:
  AyameChannelCredentials() で grpc::ChannelCredentials を継承した AyameChannelCredentialsImpl を生成して返す

トランスポート側:
  grpc_channel* goa_create_ayame_channel(const grpc_channel_args* channel_args, grpc_endpoint* ep, bool is_client) を実装する
    - goa_create_ayame_transport 関数を呼んで ayame_transport を作ってから grpc_channel_create を呼び出して、その grpc_channel* を返す
  grpc_transport* goa_create_ayame_transport(const char* target, const grpc_channel_args* channel_args, bool is_client) を実装する

InsecureChannelCredentialsImpl::CreateChannelWithInterceptors (cpp/client/insecure_credentials.cc)
-> grpc_insecure_channel_create(target.c_str(), &channel_args, nullptr) (core/ext/transport/chttp2/client/insecure/channel_create.cc)
-> grpc_core::CreateChannel(target, new_args) (core/ext/transport/chttp2/client/insecure/channel_create.cc)
-> grpc_channel_create(target, new_args, GRPC_CLIENT_CHANNEL, nullptr);

on_handshake_done
-> grpc_create_ayame_transport(args->args, args->endpoint, false, resource_user) でトランスポートを作る

*/

using SayHelloClient =
    ggrpc::ClientResponseReader<goa::HelloRequest, goa::HelloResponse>;

int main() {
  //rtc::LogMessage::LogToDebug(rtc::LS_INFO);
  //rtc::LogMessage::LogTimestamps();
  //rtc::LogMessage::LogThreads();

  //boost::asio::io_context ioc(1);
  //AyameSignaling::Create(ioc)->Connect(
  //    "wss://ayame-lite.shiguredo.jp/signaling",
  //    []() { std::cout << "OnConnected" << std::endl; });
  //ioc.run();

  // スレッド数1でクライアントマネージャを作る
  ggrpc::ClientManager cm(1);
  cm.Start();

  // 接続先の stub を作る
  std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(
      "wss://ayame-lite.shiguredo.jp/signaling", AyameChannelCredentials());
  std::unique_ptr<goa::Greeter::Stub> stub(goa::Greeter::NewStub(channel));

  // SayHello リクエストを送るクライアントを作る
  SayHelloClient::ConnectFunc connect = [stub = stub.get()](
                                            grpc::ClientContext* context,
                                            const goa::HelloRequest& req,
                                            grpc::CompletionQueue* cq) {
    return stub->AsyncSayHello(context, req, cq);
  };
  std::shared_ptr<SayHelloClient> client =
      cm.CreateResponseReader<goa::HelloRequest, goa::HelloResponse>(connect);

  // レスポンスが返ってきた時の処理
  client->SetOnFinish([](goa::HelloResponse resp, grpc::Status status) {
    //std::cout << resp.message() << std::endl;
    std::cout << "recv HelloResponse, : " << resp.DebugString();
  });

  // リクエスト送信
  goa::HelloRequest req;
  req.set_name("melpon");
  std::cout << "send HelloRequest, " << req.DebugString();
  client->Connect(req);

  std::this_thread::sleep_for(std::chrono::seconds(1));
}
