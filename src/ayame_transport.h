#include <string>
#include <thread>

// WebRTC
#include <rtc_base/logging.h>

// gRPC
#include <grpc/support/alloc.h>
#include <grpc/support/port_platform.h>
#include <grpc/support/string_util.h>
#include <grpc/support/sync.h>
#include <grpc/support/time.h>
#include <grpcpp/grpcpp.h>

#include "src/core/lib/channel/channel_args.h"
#include "src/core/lib/gprpp/manual_constructor.h"
#include "src/core/lib/slice/slice_internal.h"
#include "src/core/lib/surface/api_trace.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/surface/channel_stack_type.h"
#include "src/core/lib/surface/server.h"
#include "src/core/lib/transport/connectivity_state.h"
#include "src/core/lib/transport/error_utils.h"
#include "src/core/lib/transport/transport_impl.h"

// boost

class AyameTransport : public grpc_transport, public AyameSignalingListener {
 public:
  AyameTransport(const grpc_transport_vtable* vtable,
                 const char* target,
                 grpc_channel_args* args,
                 bool is_client)
      : is_client_(is_client),
        state_tracker_(is_client ? "ayame_client" : "ayame_server",
                       GRPC_CHANNEL_CONNECTING),
        ioc_(1) {
    this->vtable = vtable;
    gpr_ref_init(&refs_, 1);

    ayame_ = AyameSignaling::Create(ioc_, this);
  }

  std::unique_ptr<AyameDataChannel> CreateDataChannel(std::string label) {
    return ayame_->CreateDataChannel(label);
  }

  void Start(const std::string& signaling_url) {
    // Ayame に接続してシグナリングする
    // シグナリングが完了したら GRPC_CHANNEL_READY にする
    th_.reset(new std::thread([this, signaling_url]() {
      grpc_core::ExecCtx exec_ctx;
      ayame_->Connect(signaling_url);
      ioc_.run();
    }));
  }

  static AyameTransport* Create(const grpc_transport_vtable* vtable,
                                const char* target,
                                grpc_channel_args* args,
                                bool is_client) {
    void* p = gpr_malloc(sizeof(AyameTransport));
    return new (p) AyameTransport(vtable, target, args, is_client);
  }

  void Ref() { gpr_ref(&refs_); }

  void Unref() {
    if (!gpr_unref(&refs_)) {
      return;
    }
    this->~AyameTransport();
    gpr_free(this);
  }

  void Destroy() { this->Unref(); }

  void PerformOp(grpc_transport_op* op) {
    if (op->start_connectivity_watch != nullptr) {
      state_tracker_.AddWatcher(op->start_connectivity_watch_state,
                                std::move(op->start_connectivity_watch));
    }
    if (op->stop_connectivity_watch != nullptr) {
      state_tracker_.RemoveWatcher(op->stop_connectivity_watch);
    }
    if (op->set_accept_stream) {
      accept_stream_cb_ = op->set_accept_stream_fn;
      accept_stream_data_ = op->set_accept_stream_user_data;
    }
    if (op->on_consumed) {
      grpc_core::ExecCtx::Run(DEBUG_LOCATION, op->on_consumed, GRPC_ERROR_NONE);
    }

    bool do_close = false;
    if (op->goaway_error != GRPC_ERROR_NONE) {
      do_close = true;
      //GRPC_ERROR_UNREF(op->goaway_error);
    }
    if (op->disconnect_with_error != GRPC_ERROR_NONE) {
      do_close = true;
      //GRPC_ERROR_UNREF(op->disconnect_with_error);
    }

    if (do_close) {
      state_tracker_.SetState(GRPC_CHANNEL_SHUTDOWN, "close transport");
    }
  }

  void OnConnect() override {
    state_tracker_.SetState(GRPC_CHANNEL_READY, "ready transport");
  }
  void OnReady() override {
    for (auto f : ready_waited_) {
      f();
    }
    ready_waited_.clear();
  }
  void OnDataChannel(std::unique_ptr<AyameDataChannel> data_channel) override {
    grpc_core::ExecCtx exec_ctx;
    accept_stream_cb_(accept_stream_data_, this, data_channel.release());
  }

  void AddOnReady(std::function<void()> f) { ready_waited_.push_back(f); }

 private:
  gpr_refcount refs_;
  bool is_client_;
  grpc_core::ConnectivityStateTracker state_tracker_;

  boost::asio::io_context ioc_;
  std::unique_ptr<std::thread> th_;
  std::shared_ptr<AyameSignaling> ayame_;

  void (*accept_stream_cb_)(void* user_data,
                            grpc_transport* transport,
                            const void* server_data);
  void* accept_stream_data_;

  std::vector<std::function<void()>> ready_waited_;
};

class AyameStream {
 public:
  AyameStream(AyameTransport* t,
              grpc_stream_refcount* refcount,
              const void* server_data,
              grpc_core::Arena* arena)
      : refs_(refcount), transport_(t), arena_(arena) {
    is_client_ = server_data == nullptr;
    transport_->Ref();
    if (server_data == nullptr) {
      dc_ = transport_->CreateDataChannel("ayame_transport");
    } else {
      dc_ = std::unique_ptr<AyameDataChannel>((AyameDataChannel*)server_data);
    }

    if (!dc_) {
      transport_->AddOnReady([this]() {
        if (pending_op_ != nullptr) {
          PerformOp(pending_op_);
        }
      });
    } else {
      InitDataChannel();
    }
  }
  void InitDataChannel() {
    dc_->SetOnStateChange([this]() {
      RTC_LOG(LS_INFO) << "DataChannel State: "
                       << webrtc::DataChannelInterface::DataStateString(
                              dc_->state());
      if (dc_->state() == webrtc::DataChannelInterface::kOpen &&
          pending_op_ != nullptr) {
        grpc_core::ExecCtx exec_ctx;
        PerformOp(pending_op_);
      }
    });
    dc_->SetOnMessage([this](const webrtc::DataBuffer& buffer) {
      grpc_core::ExecCtx exec_ctx;
      std::lock_guard<std::mutex> guard(mutex_);
      if (!func_queue_.empty()) {
        auto f = std::move(func_queue_.front());
        func_queue_.pop_front();
        f(buffer);
        return;
      }
      data_queue_.push_back(std::move(buffer));
    });
  }

  ~AyameStream() {
    if (recv_inited_) {
      grpc_slice_buffer_destroy_internal(&recv_message_);
    }

    transport_->Unref();

    if (closure_at_destroy_ != nullptr) {
      grpc_core::ExecCtx::Run(DEBUG_LOCATION, closure_at_destroy_,
                              GRPC_ERROR_NONE);
    }
  }

#ifndef NDEBUG
#define STREAM_REF(refs, reason) grpc_stream_ref(refs, reason)
#define STREAM_UNREF(refs, reason) grpc_stream_unref(refs, reason)
#else
#define STREAM_REF(refs, reason) grpc_stream_ref(refs)
#define STREAM_UNREF(refs, reason) grpc_stream_unref(refs)
#endif
  void Ref(const char* reason) { STREAM_REF(refs_, reason); }
  void Unref(const char* reason) { STREAM_UNREF(refs_, reason); }
#undef STREAM_REF
#undef STREAM_UNREF

  void Destroy(grpc_closure* then_schedule_closure) {
    closure_at_destroy_ = then_schedule_closure;
    this->~AyameStream();
  }

  static void DoNothing(void* /*arg*/, grpc_error* /*error*/) {}

  void PerformOp(grpc_transport_stream_op_batch* op) {
    if (pending_op_ != nullptr && pending_op_ == op) {
      RTC_LOG(LS_INFO) << "rerun pended op";
    } else {
      RTC_LOG(LS_INFO)
          << "perform_stream_op 0x" << (void*)this
          << (is_client_ ? " client" : " server")
          << (op->cancel_stream ? " cancel" : "")
          << (op->send_initial_metadata ? " send_initial_metadata" : "")
          << (op->send_message ? " send_message" : "")
          << (op->send_trailing_metadata ? " send_trailing_metadata" : "")
          << (op->recv_initial_metadata ? " recv_initial_metadata" : "")
          << (op->recv_message ? " recv_message" : "")
          << (op->recv_trailing_metadata ? " recv_trailing_metadata" : "");
    }

    if (dc_ == nullptr) {
      dc_ = transport_->CreateDataChannel("ayame_transport");
      if (dc_) {
        InitDataChannel();
      }
    }
    // DataChannel が初期化されていないので後で処理する
    if (!dc_ || dc_->state() != webrtc::DataChannelInterface::kOpen) {
      if (pending_op_ != nullptr && pending_op_ != op) {
        RTC_LOG(LS_ERROR) << "has pending op";
        return;
      }

      RTC_LOG(LS_INFO) << "pending op";
      pending_op_ = op;
      return;
    }
    pending_op_ = nullptr;

    grpc_error* error = GRPC_ERROR_NONE;
    grpc_closure* on_complete = op->on_complete;

    bool needs_close = false;

    if (op->send_initial_metadata) {
      grpc_metadata_batch* md_batch =
          op->payload->send_initial_metadata.send_initial_metadata;
      nlohmann::json jmeta;
      for (grpc_linked_mdelem* md = md_batch->list.head; md != nullptr;
           md = md->next) {
        char* key = grpc_slice_to_c_string(GRPC_MDKEY(md->md));
        char* value = grpc_slice_to_c_string(GRPC_MDVALUE(md->md));
        jmeta[key] = value;
        gpr_free(key);
        gpr_free(value);
      }
      RTC_LOG(LS_INFO) << "send_initial_metadata: " << jmeta.dump();
      dc_->Send(webrtc::DataBuffer(jmeta.dump()));
    }
    if (op->send_message) {
      auto& message = op->payload->send_message.send_message;
      size_t remaining = message->length();
      do {
        grpc_slice message_slice;
        grpc_closure unused;
        GPR_ASSERT(message->Next(SIZE_MAX, &unused));
        grpc_error* error = message->Pull(&message_slice);
        if (error != GRPC_ERROR_NONE) {
          RTC_LOG(LS_ERROR) << "Pull failed";
          break;
        }
        int length = GRPC_SLICE_LENGTH(message_slice);
        char* buf = grpc_slice_to_c_string(message_slice);
        webrtc::DataBuffer dbuf(std::string(buf, length));
        gpr_free(buf);
        dc_->Send(dbuf);
        grpc_slice_unref(message_slice);
        remaining -= GRPC_SLICE_LENGTH(message_slice);
      } while (remaining > 0);
      message.reset();
    }
    if (op->send_trailing_metadata) {
      grpc_metadata_batch* md_batch =
          op->payload->send_trailing_metadata.send_trailing_metadata;
      nlohmann::json jmeta;
      for (grpc_linked_mdelem* md = md_batch->list.head; md != nullptr;
           md = md->next) {
        char* key = grpc_slice_to_c_string(GRPC_MDKEY(md->md));
        char* value = grpc_slice_to_c_string(GRPC_MDVALUE(md->md));
        jmeta[key] = value;
        gpr_free(key);
        gpr_free(value);
      }
      RTC_LOG(LS_INFO) << "send_trailing_metadata: " << jmeta.dump();
      dc_->Send(webrtc::DataBuffer(jmeta.dump()));
    }

    if (op->send_initial_metadata || op->send_message ||
        op->send_trailing_metadata) {
      grpc_core::ExecCtx::Run(DEBUG_LOCATION, op->on_complete,
                              GRPC_ERROR_REF(error));
    }

    if (op->recv_initial_metadata) {
      WaitForRecv([this, op](const webrtc::DataBuffer& data) {
        grpc_error* error = GRPC_ERROR_NONE;
        grpc_metadata_batch* md_batch =
            op->payload->recv_initial_metadata.recv_initial_metadata;
        std::string text(data.data.cdata<char>(), data.data.size());
        auto js = nlohmann::json::parse(text);
        // md_batch にメタデータを設定していく
        for (const auto& e : js.items()) {
          const std::string& key = e.key();
          grpc_slice gkey =
              grpc_slice_from_copied_buffer(key.c_str(), key.size());
          const std::string& value = e.value().get<std::string>();
          grpc_slice gvalue =
              grpc_slice_from_copied_buffer(value.c_str(), value.size());
          grpc_mdelem md = grpc_mdelem_from_slices(grpc_slice_intern(gkey),
                                                   grpc_slice_intern(gvalue));
          grpc_linked_mdelem* linked = static_cast<grpc_linked_mdelem*>(
              arena_->Alloc(sizeof(grpc_linked_mdelem)));
          error = grpc_metadata_batch_add_tail(md_batch, linked, md);
          if (error != GRPC_ERROR_NONE) {
            break;
          }
        }
        grpc_core::ExecCtx::Run(
            DEBUG_LOCATION,
            op->payload->recv_initial_metadata.recv_initial_metadata_ready,
            GRPC_ERROR_REF(error));
      });
    }

    if (op->recv_message) {
      WaitForRecv([this, op](const webrtc::DataBuffer& data) {
        grpc_error* error = GRPC_ERROR_NONE;
        if (recv_inited_) {
          grpc_slice_buffer_destroy_internal(&recv_message_);
        }
        grpc_slice_buffer_init(&recv_message_);
        recv_inited_ = true;

        grpc_slice slice = grpc_slice_from_copied_buffer(
            data.data.cdata<char>(), data.data.size());
        grpc_slice_buffer_add(&recv_message_, slice);

        recv_stream_.Init(&recv_message_, 0);
        op->payload->recv_message.recv_message->reset(recv_stream_.get());

        grpc_core::ExecCtx::Run(DEBUG_LOCATION,
                                op->payload->recv_message.recv_message_ready,
                                GRPC_ERROR_REF(error));
      });
    }

    if (op->recv_trailing_metadata) {
      WaitForRecv([this, op](const webrtc::DataBuffer& data) {
        grpc_error* error = GRPC_ERROR_NONE;
        grpc_metadata_batch* md_batch =
            op->payload->recv_trailing_metadata.recv_trailing_metadata;
        std::string text(data.data.cdata<char>(), data.data.size());
        auto js = nlohmann::json::parse(text);
        // md_batch にメタデータを設定していく
        for (const auto& e : js.items()) {
          const std::string& key = e.key();
          grpc_slice gkey =
              grpc_slice_from_copied_buffer(key.c_str(), key.size());
          const std::string& value = e.value().get<std::string>();
          grpc_slice gvalue =
              grpc_slice_from_copied_buffer(value.c_str(), value.size());
          grpc_mdelem md = grpc_mdelem_from_slices(grpc_slice_intern(gkey),
                                                   grpc_slice_intern(gvalue));
          grpc_linked_mdelem* linked = static_cast<grpc_linked_mdelem*>(
              arena_->Alloc(sizeof(grpc_linked_mdelem)));
          error = grpc_metadata_batch_add_tail(md_batch, linked, md);
          if (error != GRPC_ERROR_NONE) {
            break;
          }
        }
        grpc_core::ExecCtx::Run(
            DEBUG_LOCATION,
            op->payload->recv_trailing_metadata.recv_trailing_metadata_ready,
            GRPC_ERROR_REF(error));
      });
    }
  }

  void WaitForRecv(std::function<void(const webrtc::DataBuffer& data)> f) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!data_queue_.empty()) {
      auto data = std::move(data_queue_.front());
      data_queue_.pop_front();
      f(data);
      return;
    }
    func_queue_.push_back(std::move(f));
  }

 private:
  AyameTransport* transport_;
  grpc_stream_refcount* refs_;
  std::unique_ptr<AyameDataChannel> dc_;
  grpc_closure* closure_at_destroy_ = nullptr;
  bool is_client_;
  grpc_core::Arena* arena_;

  std::mutex mutex_;
  std::deque<webrtc::DataBuffer> data_queue_;
  std::deque<std::function<void(const webrtc::DataBuffer&)>> func_queue_;

  grpc_slice_buffer recv_message_;
  bool recv_inited_ = false;
  grpc_core::ManualConstructor<grpc_core::SliceBufferByteStream> recv_stream_;

  grpc_transport_stream_op_batch* pending_op_ = nullptr;
};

int init_stream(grpc_transport* gt,
                grpc_stream* gs,
                grpc_stream_refcount* refcount,
                const void* server_data,
                grpc_core::Arena* arena) {
  AyameTransport* t = static_cast<AyameTransport*>(gt);
  AyameStream* s = new (gs) AyameStream(t, refcount, server_data, arena);
  s->Ref("AyameStream::ctor");
  return 0;  // return value is not important
}

void set_pollset(grpc_transport* /*gt*/,
                 grpc_stream* /*gs*/,
                 grpc_pollset* /*pollset*/) {}

void set_pollset_set(grpc_transport* /*gt*/,
                     grpc_stream* /*gs*/,
                     grpc_pollset_set* /*pollset_set*/) {}

void perform_stream_op(grpc_transport* gt,
                       grpc_stream* gs,
                       grpc_transport_stream_op_batch* op) {
  AyameStream* s = reinterpret_cast<AyameStream*>(gs);
  s->PerformOp(op);
}

void perform_transport_op(grpc_transport* gt, grpc_transport_op* op) {
  AyameTransport* t = static_cast<AyameTransport*>(gt);
  t->PerformOp(op);
}

void destroy_stream(grpc_transport* /*gt*/,
                    grpc_stream* gs,
                    grpc_closure* then_schedule_closure) {
  AyameStream* s = reinterpret_cast<AyameStream*>(gs);
  s->Destroy(then_schedule_closure);
}

void destroy_transport(grpc_transport* gt) {
  AyameTransport* t = static_cast<AyameTransport*>(gt);
  t->Destroy();
}

grpc_endpoint* get_endpoint(grpc_transport* /*t*/) {
  return nullptr;
}

const grpc_transport_vtable ayame_vtable = {
    sizeof(AyameStream),  "ayame",         init_stream,
    set_pollset,          set_pollset_set, perform_stream_op,
    perform_transport_op, destroy_stream,  destroy_transport,
    get_endpoint};

grpc_transport* goa_create_ayame_transport(const char* target,
                                           grpc_channel_args* args,
                                           bool is_client) {
  return AyameTransport::Create(&ayame_vtable, target, args, is_client);
}

grpc_channel* goa_create_ayame_channel(const char* target,
                                       grpc_channel_args* args,
                                       bool is_client) {
  grpc_core::ExecCtx exec_ctx;
  grpc_transport* transport =
      goa_create_ayame_transport(target, args, is_client);

  grpc_arg default_authority_arg;
  default_authority_arg.type = GRPC_ARG_STRING;
  default_authority_arg.key = (char*)GRPC_ARG_DEFAULT_AUTHORITY;
  default_authority_arg.value.string = (char*)"ayame.authority";
  grpc_channel_args* client_args =
      grpc_channel_args_copy_and_add(args, &default_authority_arg, 1);
  grpc_channel* channel = grpc_channel_create(
      "ayame", client_args, GRPC_CLIENT_DIRECT_CHANNEL, transport);
  grpc_channel_args_destroy(client_args);

  ((AyameTransport*)transport)->Start(target);

  return channel;
}
