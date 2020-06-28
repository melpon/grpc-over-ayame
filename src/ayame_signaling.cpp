#include "ayame_signaling.h"

// WebRTC
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/create_peerconnection_factory.h>
#include <api/rtc_event_log/rtc_event_log_factory.h>
#include <api/task_queue/default_task_queue_factory.h>
#include <media/engine/webrtc_media_engine.h>
#include <rtc_base/logging.h>

// Boost
#include <boost/asio/connect.hpp>
#include <boost/beast/core/buffers_to_string.hpp>

using json = nlohmann::json;

class CreateSessionDescriptionThunk
    : public webrtc::CreateSessionDescriptionObserver {
 public:
  typedef std::function<void(webrtc::SessionDescriptionInterface*)>
      OnSuccessFunc;
  typedef std::function<void(webrtc::RTCError)> OnFailureFunc;

  static CreateSessionDescriptionThunk* Create(OnSuccessFunc on_success,
                                               OnFailureFunc on_failure) {
    return new rtc::RefCountedObject<CreateSessionDescriptionThunk>(
        std::move(on_success), std::move(on_failure));
  }

 protected:
  CreateSessionDescriptionThunk(OnSuccessFunc on_success,
                                OnFailureFunc on_failure)
      : on_success_(std::move(on_success)),
        on_failure_(std::move(on_failure)) {}
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    auto f = std::move(on_success_);
    if (f)
      f(desc);
  }
  void OnFailure(webrtc::RTCError error) override {
    RTC_LOG(LS_ERROR) << "Failed to create session description : "
                      << ToString(error.type()) << ": " << error.message();
    auto f = std::move(on_failure_);
    if (f)
      f(error);
  }

 private:
  OnSuccessFunc on_success_;
  OnFailureFunc on_failure_;
};

class SetSessionDescriptionThunk
    : public webrtc::SetSessionDescriptionObserver {
 public:
  typedef std::function<void()> OnSuccessFunc;
  typedef std::function<void(webrtc::RTCError)> OnFailureFunc;

  static SetSessionDescriptionThunk* Create(OnSuccessFunc on_success,
                                            OnFailureFunc on_failure) {
    return new rtc::RefCountedObject<SetSessionDescriptionThunk>(
        std::move(on_success), std::move(on_failure));
  }

 protected:
  SetSessionDescriptionThunk(OnSuccessFunc on_success, OnFailureFunc on_failure)
      : on_success_(std::move(on_success)),
        on_failure_(std::move(on_failure)) {}
  void OnSuccess() override {
    auto f = std::move(on_success_);
    if (f)
      f();
  }
  void OnFailure(webrtc::RTCError error) override {
    RTC_LOG(LS_ERROR) << "Failed to set session description : "
                      << ToString(error.type()) << ": " << error.message();
    auto f = std::move(on_failure_);
    if (f)
      f(error);
  }

 private:
  OnSuccessFunc on_success_;
  OnFailureFunc on_failure_;
};

std::shared_ptr<AyameSignaling> AyameSignaling::Create(
    boost::asio::io_context& ioc,
    AyameSignalingListener* listener) {
  return std::shared_ptr<AyameSignaling>(new AyameSignaling(ioc, listener));
}
AyameSignaling::AyameSignaling(boost::asio::io_context& ioc,
                               AyameSignalingListener* listener)
    : ioc_(ioc), resolver_(ioc), listener_(listener) {}

rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
AyameSignaling::CreatePeerConnectionFactory() {
  webrtc::PeerConnectionFactoryDependencies dependencies;

  network_thread_ = rtc::Thread::CreateWithSocketServer();
  network_thread_->Start();
  worker_thread_ = rtc::Thread::Create();
  worker_thread_->Start();
  signaling_thread_ = rtc::Thread::Create();
  signaling_thread_->Start();

  dependencies.network_thread = network_thread_.get();
  dependencies.worker_thread = worker_thread_.get();
  dependencies.signaling_thread = signaling_thread_.get();
  dependencies.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();
  dependencies.call_factory = webrtc::CreateCallFactory();
  dependencies.event_log_factory =
      absl::make_unique<webrtc::RtcEventLogFactory>(
          dependencies.task_queue_factory.get());

  //cricket::MediaEngineDependencies media_dependencies;
  //media_dependencies.task_queue_factory = dependencies.task_queue_factory.get();

  //media_dependencies.adm =
  //    webrtc::AudioDeviceModule::Create(webrtc::AudioDeviceModule::kDummyAudio,
  //                                      dependencies.task_queue_factory.get());
  //media_dependencies.audio_encoder_factory =
  //    webrtc::CreateBuiltinAudioEncoderFactory();
  //media_dependencies.audio_decoder_factory =
  //    webrtc::CreateBuiltinAudioDecoderFactory();

  //dependencies.media_engine =
  //    cricket::CreateMediaEngine(std::move(media_dependencies));

  auto factory =
      webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));

  webrtc::PeerConnectionFactoryInterface::Options factory_options;
  factory_options.disable_sctp_data_channels = false;
  factory_options.disable_encryption = false;
  factory_options.ssl_max_version = rtc::SSL_PROTOCOL_DTLS_12;
  factory->SetOptions(factory_options);

  return factory;
}

rtc::scoped_refptr<webrtc::PeerConnectionInterface>
AyameSignaling::CreatePeerConnection(
    webrtc::PeerConnectionInterface::IceServers ice_servers) {
  webrtc::PeerConnectionInterface::RTCConfiguration rtc_config;
  rtc_config.servers = ice_servers;
  rtc_config.enable_dtls_srtp = true;
  rtc_config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  webrtc::PeerConnectionDependencies dependencies(this);
  return factory_->CreatePeerConnection(rtc_config, std::move(dependencies));
}

bool AyameSignaling::Connect(std::string signaling_url) {
  if (!URLParts::Parse(signaling_url, parts_)) {
    return false;
  }

  if (parts_.scheme != "wss") {
    return false;
  }

  factory_ = CreatePeerConnectionFactory();
  if (factory_ == nullptr) {
    return false;
  }

  boost::asio::ssl::context ssl_ctx(boost::asio::ssl::context::tlsv12);
  ssl_ctx.set_default_verify_paths();
  ssl_ctx.set_options(boost::asio::ssl::context::default_workarounds |
                      boost::asio::ssl::context::no_sslv2 |
                      boost::asio::ssl::context::no_sslv3 |
                      boost::asio::ssl::context::single_dh_use);

  wss_.reset(new ssl_websocket_t(ioc_, ssl_ctx));
  wss_->write_buffer_bytes(8192);

  // SNI の設定を行う
  if (!SSL_set_tlsext_host_name(wss_->next_layer().native_handle(),
                                parts_.host.c_str())) {
    boost::system::error_code ec{static_cast<int>(::ERR_get_error()),
                                 boost::asio::error::get_ssl_category()};
    RTC_LOG(LS_ERROR) << "Failed SSL_set_tlsext_host_name: ec=" << ec;
    return false;
  }

  std::string port;
  if (parts_.port.empty()) {
    port = "443";
  } else {
    port = parts_.port;
  }

  // DNS ルックアップ
  resolver_.async_resolve(
      parts_.host, port,
      std::bind(&AyameSignaling::OnResolve, shared_from_this(),
                std::placeholders::_1, std::placeholders::_2));

  return true;
}

void AyameSignaling::Close() {
  wss_->async_close(boost::beast::websocket::close_code::normal,
                    std::bind(&AyameSignaling::OnClose, shared_from_this(),
                              std::placeholders::_1));
}

std::unique_ptr<AyameDataChannel> AyameSignaling::CreateDataChannel(
    std::string label) {
  if (!ready_) {
    return nullptr;
  }
  return std::unique_ptr<AyameDataChannel>(
      new AyameDataChannel(connection_->CreateDataChannel(label, nullptr)));
}

void AyameSignaling::OnResolve(
    boost::system::error_code ec,
    boost::asio::ip::tcp::resolver::results_type results) {
  if (ec) {
    RTC_LOG(LS_ERROR) << ec.message();
    return;
  }

  // DNS ルックアップで得られたエンドポイントに対して接続する
  boost::asio::async_connect(
      wss_->next_layer().next_layer(), results.begin(), results.end(),
      std::bind(&AyameSignaling::OnSSLConnect, shared_from_this(),
                std::placeholders::_1));
}

void AyameSignaling::OnSSLConnect(boost::system::error_code ec) {
  if (ec) {
    RTC_LOG(LS_ERROR) << ec.message();
    return;
  }

  // SSL のハンドシェイク
  wss_->next_layer().async_handshake(
      boost::asio::ssl::stream_base::client,
      std::bind(&AyameSignaling::OnSSLHandshake, shared_from_this(),
                std::placeholders::_1));
}

void AyameSignaling::OnSSLHandshake(boost::system::error_code ec) {
  if (ec) {
    RTC_LOG(LS_ERROR) << ec.message();
    return;
  }

  // Websocket のハンドシェイク
  wss_->async_handshake(parts_.host, parts_.path_query_fragment,
                        std::bind(&AyameSignaling::OnHandshake,
                                  shared_from_this(), std::placeholders::_1));
}

void AyameSignaling::OnHandshake(boost::system::error_code ec) {
  if (ec) {
    RTC_LOG(LS_ERROR) << ec.message();
    return;
  }

  DoRead();
  DoSendRegister();

  listener_->OnConnect();
}

void AyameSignaling::OnClose(boost::system::error_code ec) {
  if (ec) {
    RTC_LOG(LS_ERROR) << ec.message();
    return;
  }
}

void AyameSignaling::DoRead() {
  wss_->async_read(read_buffer_,
                   std::bind(&AyameSignaling::OnRead, shared_from_this(),
                             std::placeholders::_1, std::placeholders::_2));
}

void AyameSignaling::OnRead(boost::system::error_code ec,
                            std::size_t bytes_transferred) {
  if (ec == boost::asio::error::operation_aborted) {
    return;
  }

  if (ec == boost::beast::websocket::error::closed) {
    Close();
    return;
  }

  if (ec) {
    RTC_LOG(LS_ERROR) << ec.message();
    Close();
    return;
  }

  const auto text = boost::beast::buffers_to_string(read_buffer_.data());
  read_buffer_.consume(read_buffer_.size());

  RTC_LOG(LS_INFO) << __FUNCTION__ << ": text=" << text;

  auto js = json::parse(text);
  const std::string type = js["type"];
  if (type == "accept") {
    auto ice_servers = CreateIceServers(js);
    connection_ = CreatePeerConnection(ice_servers);

    bool is_exist_user = js["isExistUser"];
    if (is_exist_user) {
      datachannel_ = connection_->CreateDataChannel("init", nullptr);

      auto on_create_offer = [this](webrtc::SessionDescriptionInterface* desc) {
        connection_->SetLocalDescription(
            SetSessionDescriptionThunk::Create(nullptr, nullptr), desc);

        std::string sdp;
        desc->ToString(&sdp);

        json message = {{"type", webrtc::SdpTypeToString(desc->GetType())},
                        {"sdp", sdp}};
        SendText(message.dump());
      };
      webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
      //options.offer_to_receive_video = webrtc::PeerConnectionInterface::
      //    RTCOfferAnswerOptions::kOfferToReceiveMediaTrue;
      //options.offer_to_receive_audio = webrtc::PeerConnectionInterface::
      //    RTCOfferAnswerOptions::kOfferToReceiveMediaTrue;
      connection_->CreateOffer(
          CreateSessionDescriptionThunk::Create(on_create_offer, nullptr),
          options);
    }
    DoRead();
  } else if (type == "offer") {
    std::string sdp = js["sdp"];

    auto on_create_answer = [this](webrtc::SessionDescriptionInterface* desc) {
      connection_->SetLocalDescription(
          SetSessionDescriptionThunk::Create(nullptr, nullptr), desc);

      std::string sdp;
      desc->ToString(&sdp);

      json message = {{"type", webrtc::SdpTypeToString(desc->GetType())},
                      {"sdp", sdp}};
      SendText(message.dump());
    };
    auto on_set_offer = [this, on_create_answer]() {
      connection_->CreateAnswer(
          CreateSessionDescriptionThunk::Create(on_create_answer, nullptr),
          webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
    };

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &error);
    if (!session_description) {
      RTC_LOG(LS_ERROR) << __FUNCTION__
                        << "Failed to create session description: "
                        << error.description.c_str()
                        << "\nline: " << error.line.c_str();
      return;
    }
    connection_->SetRemoteDescription(
        SetSessionDescriptionThunk::Create(on_set_offer, nullptr),
        session_description.release());

    DoRead();
  } else if (type == "answer") {
    std::string sdp = js["sdp"];

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> session_description =
        webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, sdp, &error);
    if (!session_description) {
      RTC_LOG(LS_ERROR) << __FUNCTION__
                        << "Failed to create session description: "
                        << error.description.c_str()
                        << "\nline: " << error.line.c_str();
      return;
    }
    connection_->SetRemoteDescription(
        SetSessionDescriptionThunk::Create(nullptr, nullptr),
        session_description.release());

    DoRead();
  } else if (type == "candidate") {
    json ice = js["ice"];
    std::string sdp_mid = ice["sdpMid"].get<std::string>();
    int sdp_mlineindex = ice["sdpMLineIndex"].get<int>();
    std::string sdp = ice["candidate"].get<std::string>();

    webrtc::SdpParseError error;
    std::unique_ptr<webrtc::IceCandidateInterface> candidate(
        webrtc::CreateIceCandidate(sdp_mid, sdp_mlineindex, sdp, &error));
    if (!candidate.get()) {
      RTC_LOG(LS_ERROR) << "Can't parse received candidate message: "
                        << error.description.c_str()
                        << "\nline: " << error.line.c_str();
      return;
    }
    if (!connection_->AddIceCandidate(candidate.get())) {
      RTC_LOG(LS_WARNING) << __FUNCTION__
                          << "Failed to apply the received candidate : " << sdp;
      return;
    }

    if (!ready_) {
      ready_ = true;
      listener_->OnReady();
    }

    DoRead();
  } else if (type == "ping") {
    DoSendPong();
    DoRead();
  } else if (type == "bye") {
    connection_ = nullptr;
    Close();
  }
}

webrtc::PeerConnectionInterface::IceServers AyameSignaling::CreateIceServers(
    json json_message) {
  webrtc::PeerConnectionInterface::IceServers ice_servers;

  // 返却されてきた iceServers を セットする
  //if (json_message.contains("iceServers")) {
  //  auto jservers = json_message["iceServers"];
  //  if (jservers.is_array()) {
  //    for (auto jserver : jservers) {
  //      webrtc::PeerConnectionInterface::IceServer ice_server;
  //      if (jserver.contains("username")) {
  //        ice_server.username = jserver["username"].get<std::string>();
  //      }
  //      if (jserver.contains("credential")) {
  //        ice_server.password = jserver["credential"].get<std::string>();
  //      }
  //      auto jurls = jserver["urls"];
  //      for (const std::string url : jurls) {
  //        ice_server.urls.push_back(url);
  //      }
  //      ice_servers.push_back(ice_server);
  //    }
  //  }
  //}
  if (ice_servers.empty()) {
    // accept 時に iceServers が返却されてこなかった場合 google の stun server を用いる
    webrtc::PeerConnectionInterface::IceServer ice_server;
    ice_server.uri = "stun:stun.l.google.com:19302";
    ice_servers.push_back(ice_server);
  }

  return ice_servers;
}

void AyameSignaling::DoSendRegister() {
  std::string client_id;
  rtc::CreateRandomString(32, &client_id);

  json message = {
      {"type", "register"},
      {"clientId", client_id},
      {"roomId", "grpc-over-ayame-testroom"},
      {"ayameClient", "grpc-over-ayame"},
      {"libwebrtc", "libwebrtc"},
      {"environment", "environment"},
  };
  SendText(message.dump());
}

void AyameSignaling::DoSendPong() {
  json message = {{"type", "pong"}};
  SendText(message.dump());
}

void AyameSignaling::SendText(std::string text) {
  boost::asio::post(std::bind(&AyameSignaling::DoSendText, shared_from_this(),
                              std::move(text)));
}

void AyameSignaling::DoSendText(std::string text) {
  RTC_LOG(LS_INFO) << "SendText: " << text;
  bool empty = write_buffer_.empty();
  boost::beast::flat_buffer buffer;

  const auto n = boost::asio::buffer_copy(buffer.prepare(text.size()),
                                          boost::asio::buffer(text));
  buffer.commit(n);

  write_buffer_.push_back(std::move(buffer));

  if (empty) {
    DoWrite();
  }
}

void AyameSignaling::DoWrite() {
  auto& buffer = write_buffer_.front();

  wss_->text(true);
  wss_->async_write(buffer.data(),
                    std::bind(&AyameSignaling::OnWrite, shared_from_this(),
                              std::placeholders::_1, std::placeholders::_2));
}

void AyameSignaling::OnWrite(boost::system::error_code ec,
                             std::size_t bytes_transferred) {
  if (ec == boost::asio::error::operation_aborted) {
    return;
  }

  if (ec) {
    RTC_LOG(LS_ERROR) << "Failed to write: ec=" << ec;
    return;
  }

  write_buffer_.erase(write_buffer_.begin());

  if (!write_buffer_.empty()) {
    DoWrite();
  }
}

void AyameSignaling::OnIceCandidate(
    const webrtc::IceCandidateInterface* candidate) {
  std::string sdp;
  if (!candidate->ToString(&sdp)) {
    RTC_LOG(LS_ERROR) << "Failed to serialize candidate";
    return;
  }

  json message = {
      {"type", "candidate"},
  };
  message["ice"] = {{"candidate", sdp},
                    {"sdpMLineIndex", candidate->sdp_mline_index()},
                    {"sdpMid", candidate->sdp_mid()}};
  SendText(message.dump());
}
