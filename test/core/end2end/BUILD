# Copyright 2016 gRPC authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

load("//bazel:grpc_build_system.bzl", "grpc_cc_library", "grpc_cc_test", "grpc_package")
load(":generate_tests.bzl", "grpc_end2end_tests")

licenses(["notice"])

grpc_package(name = "test/core/end2end")

grpc_cc_library(
    name = "cq_verifier",
    srcs = ["cq_verifier.cc"],
    hdrs = ["cq_verifier.h"],
    language = "C++",
    visibility = ["//test:__subpackages__"],
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_library(
    name = "ssl_test_data",
    srcs = [
        "data/client_certs.cc",
        "data/server1_cert.cc",
        "data/server1_key.cc",
        "data/test_root_cert.cc",
    ],
    hdrs = ["data/ssl_test_data.h"],
    language = "C++",
    visibility = ["//test:__subpackages__"],
)

grpc_cc_library(
    name = "http_proxy",
    srcs = ["fixtures/http_proxy_fixture.cc"],
    hdrs = ["fixtures/http_proxy_fixture.h"],
    language = "C++",
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_library(
    name = "proxy",
    srcs = ["fixtures/proxy.cc"],
    hdrs = ["fixtures/proxy.h"],
    language = "C++",
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_library(
    name = "local_util",
    srcs = ["fixtures/local_util.cc"],
    hdrs = [
        "end2end_tests.h",
        "fixtures/local_util.h",
    ],
    language = "C++",
    deps = [
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "bad_server_response_test",
    srcs = ["bad_server_response_test.cc"],
    language = "C++",
    deps = [
        ":cq_verifier",
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "connection_refused_test",
    srcs = ["connection_refused_test.cc"],
    language = "C++",
    deps = [
        ":cq_verifier",
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "dualstack_socket_test",
    srcs = ["dualstack_socket_test.cc"],
    language = "C++",
    tags = ["no_windows"],
    deps = [
        ":cq_verifier",
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "goaway_server_test",
    srcs = ["goaway_server_test.cc"],
    language = "C++",
    deps = [
        ":cq_verifier",
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "inproc_callback_test",
    srcs = ["inproc_callback_test.cc"],
    language = "C++",
    uses_polling = False,
    deps = [
        ":end2end_tests",
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "invalid_call_argument_test",
    srcs = ["invalid_call_argument_test.cc"],
    language = "C++",
    deps = [
        ":cq_verifier",
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "multiple_server_queues_test",
    srcs = ["multiple_server_queues_test.cc"],
    language = "C++",
    deps = [
        ":cq_verifier",
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_cc_test(
    name = "no_server_test",
    srcs = ["no_server_test.cc"],
    language = "C++",
    deps = [
        ":cq_verifier",
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)

grpc_end2end_tests()

grpc_cc_test(
    name = "h2_ssl_session_reuse_test",
    srcs = ["h2_ssl_session_reuse_test.cc"],
    data = [
        "//src/core/tsi/test_creds:ca.pem",
        "//src/core/tsi/test_creds:client.key",
        "//src/core/tsi/test_creds:client.pem",
        "//src/core/tsi/test_creds:server1.key",
        "//src/core/tsi/test_creds:server1.pem",
    ],
    external_deps = [
        "gtest",
    ],
    language = "C++",
    deps = [
        ":end2end_tests",
        "//:gpr",
        "//:grpc",
        "//test/core/util:grpc_test_util",
    ],
)
