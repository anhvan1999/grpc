//
//
// Copyright 2022 gRPC authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//

#include <grpc/support/port_platform.h>

#include <stdlib.h>
#include <string.h>

#include <thread>

#include <gmock/gmock.h>

#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"

#include <grpc/grpc.h>
#include <grpc/grpc_posix.h>
#include <grpc/grpc_security.h>

#include "src/core/ext/transport/chttp2/transport/chttp2_transport.h"
#include "src/core/ext/transport/chttp2/transport/frame_goaway.h"
#include "src/core/ext/transport/chttp2/transport/frame_ping.h"
#include "src/core/lib/channel/channel_stack_builder.h"
#include "src/core/lib/config/core_configuration.h"
#include "src/core/lib/gprpp/host_port.h"
#include "src/core/lib/iomgr/endpoint_pair.h"
#include "src/core/lib/slice/slice.h"
#include "src/core/lib/slice/slice_string_helpers.h"
#include "src/core/lib/surface/channel.h"
#include "src/core/lib/surface/server.h"
#include "test/core/end2end/cq_verifier.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"
#include "test/core/util/test_tcp_server.h"

namespace grpc_core {
namespace {

void* Tag(intptr_t t) { return reinterpret_cast<void*>(t); }

class GracefulShutdownTest : public ::testing::Test {
 protected:
  GracefulShutdownTest() { SetupAndStart(); }

  ~GracefulShutdownTest() override { ShutdownAndDestroy(); }

  // Sets up the client and server
  void SetupAndStart() {
    ExecCtx exec_ctx;
    cq_ = grpc_completion_queue_create_for_next(nullptr);
    cqv_ = cq_verifier_create(cq_);
    grpc_arg server_args[] = {
        grpc_channel_arg_integer_create(
            const_cast<char*>(GRPC_ARG_HTTP2_BDP_PROBE), 0),
        grpc_channel_arg_integer_create(
            const_cast<char*>(GRPC_ARG_KEEPALIVE_TIME_MS), INT_MAX)};
    grpc_channel_args server_channel_args = {GPR_ARRAY_SIZE(server_args),
                                             server_args};
    // Create server
    server_ = grpc_server_create(&server_channel_args, nullptr);
    auto* core_server = Server::FromC(server_);
    grpc_server_register_completion_queue(server_, cq_, nullptr);
    grpc_server_start(server_);
    fds_ = grpc_iomgr_create_endpoint_pair("fixture", nullptr);
    auto* transport = grpc_create_chttp2_transport(core_server->channel_args(),
                                                   fds_.server, false);
    grpc_endpoint_add_to_pollset(fds_.server, grpc_cq_pollset(cq_));
    GPR_ASSERT(core_server->SetupTransport(transport, nullptr,
                                           core_server->channel_args(),
                                           nullptr) == GRPC_ERROR_NONE);
    grpc_chttp2_transport_start_reading(transport, nullptr, nullptr, nullptr);
    // Start polling on the client
    client_poll_thread_ = absl::make_unique<std::thread>([this]() {
      grpc_completion_queue* client_cq =
          grpc_completion_queue_create_for_next(nullptr);
      {
        ExecCtx exec_ctx;
        grpc_endpoint_add_to_pollset(fds_.client, grpc_cq_pollset(client_cq));
        grpc_endpoint_add_to_pollset(fds_.server, grpc_cq_pollset(client_cq));
      }
      while (!shutdown_) {
        GPR_ASSERT(
            grpc_completion_queue_next(
                client_cq, grpc_timeout_milliseconds_to_deadline(10), nullptr)
                .type == GRPC_QUEUE_TIMEOUT);
      }
      grpc_completion_queue_destroy(client_cq);
    });
    // Write connection prefix and settings frame
    constexpr char kPrefix[] =
        "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n\x00\x00\x00\x04\x00\x00\x00\x00\x00";
    Write(absl::string_view(kPrefix, sizeof(kPrefix) - 1));
    // Start reading on the client
    grpc_slice_buffer_init(&read_buffer_);
    GRPC_CLOSURE_INIT(&on_read_done_, OnReadDone, this, nullptr);
    grpc_endpoint_read(fds_.client, &read_buffer_, &on_read_done_, false);
  }

  // Shuts down and destroys the client and server.
  void ShutdownAndDestroy() {
    shutdown_ = true;
    ExecCtx exec_ctx;
    grpc_endpoint_shutdown(
        fds_.client, GRPC_ERROR_CREATE_FROM_STATIC_STRING("Client shutdown"));
    ExecCtx::Get()->Flush();
    client_poll_thread_->join();
    GPR_ASSERT(read_end_notification_.WaitForNotificationWithTimeout(
        absl::Seconds(5)));
    grpc_endpoint_destroy(fds_.client);
    ExecCtx::Get()->Flush();
    // Shutdown and destroy server
    grpc_server_shutdown_and_notify(server_, cq_, Tag(1000));
    CQ_EXPECT_COMPLETION(cqv_, Tag(1000), true);
    cq_verify(cqv_);
    grpc_server_destroy(server_);
    cq_verifier_destroy(cqv_);
    grpc_completion_queue_destroy(cq_);
  }

  static void OnReadDone(void* arg, grpc_error_handle error) {
    GracefulShutdownTest* self = static_cast<GracefulShutdownTest*>(arg);
    if (error == GRPC_ERROR_NONE) {
      {
        absl::MutexLock lock(&self->mu_);
        for (size_t i = 0; i < self->read_buffer_.count; ++i) {
          absl::StrAppend(&self->read_bytes_,
                          StringViewFromSlice(self->read_buffer_.slices[i]));
        }
        self->read_cv_.SignalAll();
      }
      grpc_slice_buffer_reset_and_unref(&self->read_buffer_);
      grpc_endpoint_read(self->fds_.client, &self->read_buffer_,
                         &self->on_read_done_, false);
    } else {
      grpc_slice_buffer_destroy(&self->read_buffer_);
      self->read_end_notification_.Notify();
    }
  }

  // Waits for \a bytes to show up in read_bytes_
  void WaitForReadBytes(absl::string_view bytes) {
    std::atomic<bool> done{false};
    {
      absl::MutexLock lock(&mu_);
      while (!absl::StrContains(read_bytes_, bytes)) {
        read_cv_.WaitWithTimeout(&mu_, absl::Seconds(5));
      }
    }
    done = true;
  }

  void WaitForGoaway(uint32_t last_stream_id) {
    grpc_slice_buffer buffer;
    grpc_slice_buffer_init(&buffer);
    grpc_chttp2_goaway_append(last_stream_id, 0, grpc_empty_slice(), &buffer);
    std::string expected_bytes;
    for (size_t i = 0; i < buffer.count; ++i) {
      absl::StrAppend(&expected_bytes, StringViewFromSlice(buffer.slices[i]));
    }
    grpc_slice_buffer_destroy(&buffer);
    WaitForReadBytes(expected_bytes);
  }

  void WaitForPing(uint64_t opaque_data) {
    grpc_slice ping_slice = grpc_chttp2_ping_create(0, opaque_data);
    WaitForReadBytes(StringViewFromSlice(ping_slice));
  }

  void SendPingAck(uint64_t opaque_data) {
    grpc_slice ping_slice = grpc_chttp2_ping_create(1, opaque_data);
    Write(StringViewFromSlice(ping_slice));
  }

  // This is a blocking call. It waits for the write callback to be invoked
  // before returning. (In other words, do not call this from a thread that
  // should not be blocked, for example, a polling thread.)
  void Write(absl::string_view bytes) {
    ExecCtx exec_ctx;
    grpc_slice slice =
        StaticSlice::FromStaticBuffer(bytes.data(), bytes.size()).TakeCSlice();
    grpc_slice_buffer buffer;
    grpc_slice_buffer_init(&buffer);
    grpc_slice_buffer_add(&buffer, slice);
    WriteBuffer(&buffer);
    grpc_slice_buffer_destroy(&buffer);
  }

  void WriteBuffer(grpc_slice_buffer* buffer) {
    absl::Notification on_write_done_notification_;
    GRPC_CLOSURE_INIT(&on_write_done_, OnWriteDone,
                      &on_write_done_notification_, nullptr);
    grpc_endpoint_write(fds_.client, buffer, &on_write_done_, nullptr);
    ExecCtx::Get()->Flush();
    GPR_ASSERT(on_write_done_notification_.WaitForNotificationWithTimeout(
        absl::Seconds(5)));
  }

  static void OnWriteDone(void* arg, grpc_error_handle error) {
    GPR_ASSERT(error == GRPC_ERROR_NONE);
    absl::Notification* on_write_done_notification_ =
        static_cast<absl::Notification*>(arg);
    on_write_done_notification_->Notify();
  }

  grpc_endpoint_pair fds_;
  grpc_server* server_ = nullptr;
  grpc_completion_queue* cq_ = nullptr;
  cq_verifier* cqv_ = nullptr;
  std::unique_ptr<std::thread> client_poll_thread_;
  std::atomic<bool> shutdown_{false};
  grpc_closure on_read_done_;
  absl::Mutex mu_;
  absl::CondVar read_cv_;
  absl::Notification read_end_notification_;
  grpc_slice_buffer read_buffer_;
  std::string read_bytes_ ABSL_GUARDED_BY(mu_);
  grpc_closure on_write_done_;
};

TEST_F(GracefulShutdownTest, GracefulGoaway) {
  // Initiate shutdown on the server
  grpc_server_shutdown_and_notify(server_, cq_, Tag(1));
  // Wait for first goaway
  WaitForGoaway((1u << 31) - 1);
  // Wait for the ping
  WaitForPing(0);
  // Reply to the ping
  SendPingAck(0);
  // Wait for final goaway
  WaitForGoaway(0);
  // The shutdown should successfully complete.
  CQ_EXPECT_COMPLETION(cqv_, Tag(1), true);
  cq_verify(cqv_);
}

TEST_F(GracefulShutdownTest, RequestStartedBeforeFinalGoaway) {
  grpc_call_error error;
  grpc_call* s;
  grpc_call_details call_details;
  grpc_metadata_array request_metadata_recv;
  grpc_call_details_init(&call_details);
  grpc_metadata_array_init(&request_metadata_recv);
  error = grpc_server_request_call(server_, &s, &call_details,
                                   &request_metadata_recv, cq_, cq_, Tag(100));
  GPR_ASSERT(GRPC_CALL_OK == error);
  // Initiate shutdown on the server
  grpc_server_shutdown_and_notify(server_, cq_, Tag(1));
  // Wait for first goaway
  WaitForGoaway((1u << 31) - 1);
  // Wait for the ping
  WaitForPing(0);
  // Start a request
  constexpr char kRequestFrame[] =
      "\x00\x00\xbe\x01\x05\x00\x00\x00\x01"
      "\x10\x05:path\x08/foo/bar"
      "\x10\x07:scheme\x04http"
      "\x10\x07:method\x04POST"
      "\x10\x0a:authority\x09localhost"
      "\x10\x0c"
      "content-type\x10"
      "application/grpc"
      "\x10\x14grpc-accept-encoding\x15identity,deflate,gzip"
      "\x10\x02te\x08trailers"
      "\x10\x0auser-agent\x17grpc-c/0.12.0.0 (linux)";
  Write(absl::string_view(kRequestFrame, sizeof(kRequestFrame) - 1));
  // Reply to the ping
  SendPingAck(0);
  // Wait for final goaway with last stream ID 1 to show that the HTTP2
  // transport accepted the stream.
  WaitForGoaway(1);
  // TODO(yashykt): The surface layer automatically cancels calls received after
  // shutdown has been called. Once that is fixed, this should be a success.
  CQ_EXPECT_COMPLETION(cqv_, Tag(100), 0);
  // The shutdown should successfully complete.
  CQ_EXPECT_COMPLETION(cqv_, Tag(1), true);
  cq_verify(cqv_);
  grpc_metadata_array_destroy(&request_metadata_recv);
  grpc_call_details_destroy(&call_details);
}

TEST_F(GracefulShutdownTest, RequestStartedAfterFinalGoawayIsIgnored) {
  // Start a request before shutdown to make sure that the connection stays
  // alive.
  grpc_call_error error;
  grpc_call* s;
  grpc_call_details call_details;
  grpc_metadata_array request_metadata_recv;
  grpc_call_details_init(&call_details);
  grpc_metadata_array_init(&request_metadata_recv);
  error = grpc_server_request_call(server_, &s, &call_details,
                                   &request_metadata_recv, cq_, cq_, Tag(100));
  GPR_ASSERT(GRPC_CALL_OK == error);
  // Send the request from the client.
  constexpr char kRequestFrame[] =
      "\x00\x00\xbe\x01\x05\x00\x00\x00\x01"
      "\x10\x05:path\x08/foo/bar"
      "\x10\x07:scheme\x04http"
      "\x10\x07:method\x04POST"
      "\x10\x0a:authority\x09localhost"
      "\x10\x0c"
      "content-type\x10"
      "application/grpc"
      "\x10\x14grpc-accept-encoding\x15identity,deflate,gzip"
      "\x10\x02te\x08trailers"
      "\x10\x0auser-agent\x17grpc-c/0.12.0.0 (linux)";
  Write(absl::string_view(kRequestFrame, sizeof(kRequestFrame) - 1));
  CQ_EXPECT_COMPLETION(cqv_, Tag(100), 1);
  cq_verify(cqv_);

  // Initiate shutdown on the server
  grpc_server_shutdown_and_notify(server_, cq_, Tag(1));
  // Wait for first goaway
  WaitForGoaway((1u << 31) - 1);
  // Wait for the ping
  WaitForPing(0);
  // Reply to the ping
  SendPingAck(0);
  // Wait for final goaway
  WaitForGoaway(1);

  // Send another request from the client which should be ignored.
  constexpr char kNewRequestFrame[] =
      "\x00\x00\xbe\x01\x05\x00\x00\x00\x03"
      "\x10\x05:path\x08/foo/bar"
      "\x10\x07:scheme\x04http"
      "\x10\x07:method\x04POST"
      "\x10\x0a:authority\x09localhost"
      "\x10\x0c"
      "content-type\x10"
      "application/grpc"
      "\x10\x14grpc-accept-encoding\x15identity,deflate,gzip"
      "\x10\x02te\x08trailers"
      "\x10\x0auser-agent\x17grpc-c/0.12.0.0 (linux)";
  Write(absl::string_view(kNewRequestFrame, sizeof(kNewRequestFrame) - 1));

  // Finish the accepted request.
  grpc_op ops[3];
  grpc_op* op;
  memset(ops, 0, sizeof(ops));
  op = ops;
  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  op->data.send_status_from_server.trailing_metadata_count = 0;
  op->data.send_status_from_server.status = GRPC_STATUS_UNIMPLEMENTED;
  grpc_slice status_details = grpc_slice_from_static_string("xyz");
  op->data.send_status_from_server.status_details = &status_details;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  op->op = GRPC_OP_RECV_CLOSE_ON_SERVER;
  int was_cancelled = 2;
  op->data.recv_close_on_server.cancelled = &was_cancelled;
  op->flags = 0;
  op->reserved = nullptr;
  op++;
  error = grpc_call_start_batch(s, ops, static_cast<size_t>(op - ops), Tag(101),
                                nullptr);
  GPR_ASSERT(GRPC_CALL_OK == error);
  CQ_EXPECT_COMPLETION(cqv_, Tag(101), true);
  // The shutdown should successfully complete.
  CQ_EXPECT_COMPLETION(cqv_, Tag(1), true);
  cq_verify(cqv_);
  grpc_call_unref(s);
  grpc_metadata_array_destroy(&request_metadata_recv);
  grpc_call_details_destroy(&call_details);
}

// Make sure that the graceful goaway eventually makes progress even if a client
// does not respond to the ping.
TEST_F(GracefulShutdownTest, UnresponsiveClient) {
  absl::Time initial_time = absl::Now();
  // Initiate shutdown on the server
  grpc_server_shutdown_and_notify(server_, cq_, Tag(1));
  // Wait for first goaway
  WaitForGoaway((1u << 31) - 1);
  // Wait for the ping
  WaitForPing(0);
  // Wait for final goaway without sending a ping ACK.
  WaitForGoaway(0);
  EXPECT_GE(absl::Now() - initial_time,
            absl::Seconds(20) -
                absl::Seconds(
                    1) /* clock skew between threads due to time caching */);
  // The shutdown should successfully complete.
  CQ_EXPECT_COMPLETION(cqv_, Tag(1), true);
  cq_verify(cqv_);
}

}  // namespace
}  // namespace grpc_core

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  grpc::testing::TestEnvironment env(argc, argv);
  grpc_init();
  int result = RUN_ALL_TESTS();
  grpc_shutdown();
  return result;
}
