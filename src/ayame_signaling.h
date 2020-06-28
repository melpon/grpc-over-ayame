#ifndef AYAME_WEBSOCKET_CLIENT_
#define AYAME_WEBSOCKET_CLIENT_

#include <algorithm>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>

// boost
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/multi_buffer.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/beast/websocket/stream.hpp>

// nlohmann/json
#include <nlohmann/json.hpp>

// WebRTC
#include <api/peer_connection_interface.h>
#include <api/scoped_refptr.h>
#include <rtc_base/thread.h>

#include "url_parts.h"

//class DataChannelObserver : public webrtc::DataChannelObserver {
//  webrtc::DataChannelInterface* dc_;
//
// public:
//  DataChannelObserver(webrtc::DataChannelInterface* dc) : dc_(dc) {}
//  void OnStateChange() override {
//    RTC_LOG(LS_INFO) << "DataChannel State: "
//                     << webrtc::DataChannelInterface::DataStateString(
//                            dc_->state());
//    if (dc_->state() == webrtc::DataChannelInterface::kOpen) {
//      dc_->Send(webrtc::DataBuffer("hogefuga"));
//    }
//  }
//  void OnMessage(const webrtc::DataBuffer& buffer) override {
//    RTC_LOG(LS_INFO) << "DataChannel OnMessage: "
//                     << std::string(buffer.data.data<char>(),
//                                    buffer.data.size());
//  }
//  void OnBufferedAmountChange(uint64_t sent_data_size) override {}
//
// protected:
//  virtual ~DataChannelObserver() = default;
//};

class AyameDataChannel : public webrtc::DataChannelObserver {
 public:
  typedef std::function<void()> OnStateChangeFunc;
  typedef std::function<void(const webrtc::DataBuffer& buffer)> OnMessageFunc;
  typedef std::function<void(uint64_t sent_data_size)>
      OnBufferedAmountChangeFunc;
  AyameDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> dc)
      : dc_(dc) {
    dc_->RegisterObserver(this);
  }
  ~AyameDataChannel() { dc_->UnregisterObserver(); }

  void SetOnStateChange(OnStateChangeFunc f) {
    on_state_change_ = std::move(f);
  }
  void SetOnMessage(OnMessageFunc f) { on_message_ = std::move(f); }
  void SetOnBufferedAmountChange(OnBufferedAmountChangeFunc f) {
    on_buffered_amount_change_ = std::move(f);
  }

  void OnStateChange() override {
    if (on_state_change_)
      on_state_change_();
  }
  void OnMessage(const webrtc::DataBuffer& buffer) override {
    if (on_message_)
      on_message_(buffer);
  }
  void OnBufferedAmountChange(uint64_t sent_data_size) override {
    if (on_buffered_amount_change_)
      on_buffered_amount_change_(sent_data_size);
  }

  void Send(const webrtc::DataBuffer& buffer) { dc_->Send(buffer); }
  webrtc::DataChannelInterface::DataState state() const { return dc_->state(); }

 private:
  rtc::scoped_refptr<webrtc::DataChannelInterface> dc_;
  OnStateChangeFunc on_state_change_;
  OnMessageFunc on_message_;
  OnBufferedAmountChangeFunc on_buffered_amount_change_;
};

class AyameSignalingListener {
 public:
  ~AyameSignalingListener() {}
  virtual void OnConnect() = 0;
  virtual void OnReady() = 0;
  virtual void OnDataChannel(
      std::unique_ptr<AyameDataChannel> data_channel) = 0;
};

class AyameSignaling : public std::enable_shared_from_this<AyameSignaling>,
                       public webrtc::PeerConnectionObserver {
 public:
  static std::shared_ptr<AyameSignaling> Create(
      boost::asio::io_context& ioc,
      AyameSignalingListener* listener);
  AyameSignaling(boost::asio::io_context& ioc,
                 AyameSignalingListener* listener);

  bool Connect(std::string signaling_url);
  void Close();
  std::unique_ptr<AyameDataChannel> CreateDataChannel(std::string label);

 private:
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface>
  CreatePeerConnectionFactory();
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> CreatePeerConnection(
      webrtc::PeerConnectionInterface::IceServers ice_servers);
  webrtc::PeerConnectionInterface::IceServers CreateIceServers(
      nlohmann::json js);

 private:
  void OnResolve(boost::system::error_code ec,
                 boost::asio::ip::tcp::resolver::results_type results);
  void OnSSLConnect(boost::system::error_code ec);
  void OnSSLHandshake(boost::system::error_code ec);
  void OnHandshake(boost::system::error_code ec);

  void OnClose(boost::system::error_code ec);

  void DoRead();
  void OnRead(boost::system::error_code ec, std::size_t bytes_transferred);

  void DoSendRegister();
  void DoSendPong();

  void SendText(std::string text);
  void DoSendText(std::string text);
  void DoWrite();
  void OnWrite(boost::system::error_code ec, std::size_t bytes_transferred);

 private:
  static std::string IceConnectionStateToString(
      webrtc::PeerConnectionInterface::IceConnectionState state) {
    switch (state) {
      case webrtc::PeerConnectionInterface::kIceConnectionNew:
        return "new";
      case webrtc::PeerConnectionInterface::kIceConnectionChecking:
        return "checking";
      case webrtc::PeerConnectionInterface::kIceConnectionConnected:
        return "connected";
      case webrtc::PeerConnectionInterface::kIceConnectionCompleted:
        return "completed";
      case webrtc::PeerConnectionInterface::kIceConnectionFailed:
        return "failed";
      case webrtc::PeerConnectionInterface::kIceConnectionDisconnected:
        return "disconnected";
      case webrtc::PeerConnectionInterface::kIceConnectionClosed:
        return "closed";
      case webrtc::PeerConnectionInterface::kIceConnectionMax:
        return "max";
    }
    return "unknown";
  }
  static std::string SignalingStateToString(
      webrtc::PeerConnectionInterface::SignalingState state) {
    switch (state) {
      case webrtc::PeerConnectionInterface::kStable:
        return "kStable";
      case webrtc::PeerConnectionInterface::kHaveLocalOffer:
        return "kHaveLocalOffer";
      case webrtc::PeerConnectionInterface::kHaveLocalPrAnswer:
        return "kHaveLocalPrAnswer";
      case webrtc::PeerConnectionInterface::kHaveRemoteOffer:
        return "kHaveRemoteOffer";
      case webrtc::PeerConnectionInterface::kHaveRemotePrAnswer:
        return "kHaveRemotePrAnswer";
      case webrtc::PeerConnectionInterface::kClosed:
        return "kClosed";
    }
    return "unknown";
  }

  // webrtc::PeerConnectionObserver
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState new_state) override {
    RTC_LOG(LS_INFO) << "signaling state: "
                     << SignalingStateToString(new_state);
  }
  void OnDataChannel(
      rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
    RTC_LOG(LS_INFO) << "opened DataChannel: " << data_channel->label();
    if (data_channel->label() == "init") {
      datachannel_ = data_channel;
    } else {
      std::unique_ptr<AyameDataChannel> p(new AyameDataChannel(data_channel));
      listener_->OnDataChannel(std::move(p));
    }
  }
  void OnRenegotiationNeeded() override {}
  void OnStandardizedIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
    RTC_LOG(LS_INFO) << "ice state: " << IceConnectionStateToString(new_state);
  }
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState new_state) override {}
  void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override;
  void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver)
      override {}
  void OnRemoveTrack(
      rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override {}

 private:
  boost::asio::io_context& ioc_;
  boost::asio::ip::tcp::resolver resolver_;
  AyameSignalingListener* listener_;

  std::string signaling_url_;
  URLParts parts_;

  typedef boost::beast::websocket::stream<
      boost::asio::ssl::stream<boost::asio::ip::tcp::socket>>
      ssl_websocket_t;

  std::unique_ptr<ssl_websocket_t> wss_;

  boost::beast::multi_buffer read_buffer_;
  std::vector<boost::beast::flat_buffer> write_buffer_;

  std::unique_ptr<rtc::Thread> network_thread_;
  std::unique_ptr<rtc::Thread> worker_thread_;
  std::unique_ptr<rtc::Thread> signaling_thread_;
  rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  rtc::scoped_refptr<webrtc::PeerConnectionInterface> connection_;

  webrtc::PeerConnectionInterface::IceConnectionState rtc_state_;

  rtc::scoped_refptr<webrtc::DataChannelInterface> datachannel_;
  bool ready_ = false;
};

#endif  // AYAME_WEBSOCKET_CLIENT_
