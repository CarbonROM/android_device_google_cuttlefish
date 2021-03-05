/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <fcntl.h>
#include <sys/poll.h>
#include <unistd.h>
#include <ios>
#include <mutex>

#include <android-base/logging.h>
#include <gflags/gflags.h>
#include <thread>

#include "common/libs/fs/shared_buf.h"
#include "common/libs/fs/shared_fd.h"
#include "host/libs/config/cuttlefish_config.h"
#include "host/libs/config/logging.h"

DEFINE_int32(bt_in, -1, "A pipe for bt communication");
DEFINE_int32(bt_out, -1, "A pipe for bt communication");
DEFINE_int32(hci_port, -1, "A port for bt hci command");
DEFINE_int32(link_port, -1, "A pipe for bt link layer command");
DEFINE_int32(test_port, -1, "A pipe for rootcanal test channel");

void openSocket(cuttlefish::SharedFD* fd, int port) {
  static std::mutex mutex;
  std::unique_lock<std::mutex> lock(mutex);
  *fd = cuttlefish::SharedFD::SocketLocalClient(port, SOCK_STREAM);
}

int main(int argc, char** argv) {
  cuttlefish::DefaultSubprocessLogging(argv);
  gflags::ParseCommandLineFlags(&argc, &argv, true);
  auto bt_in = cuttlefish::SharedFD::Dup(FLAGS_bt_in);
  if (!bt_in->IsOpen()) {
    LOG(ERROR) << "Error dupping fd " << FLAGS_bt_in << ": "
               << bt_in->StrError();
    return 1;
  }
  close(FLAGS_bt_in);

  auto bt_out = cuttlefish::SharedFD::Dup(FLAGS_bt_out);
  if (!bt_out->IsOpen()) {
    LOG(ERROR) << "Error dupping fd " << FLAGS_bt_out << ": "
               << bt_out->StrError();
    return 1;
  }
  close(FLAGS_bt_out);
  cuttlefish::SharedFD sock;
  openSocket(&sock, FLAGS_hci_port);

  auto guest_to_host = std::thread([&]() {
    while (true) {
      char buf[1024];
      auto read = bt_in->Read(buf, sizeof(buf));
      if (cuttlefish::WriteAll(sock, buf, read) == -1) {
        openSocket(&sock, FLAGS_hci_port);
      }
    }
  });

  auto host_to_guest = std::thread([&]() {
    while (true) {
      char buf[1024];
      auto read = sock->Read(buf, sizeof(buf));
      if (read == -1) {
        openSocket(&sock, FLAGS_hci_port);
        continue;
      }
      cuttlefish::WriteAll(bt_out, buf, read);
    }
  });
  guest_to_host.join();
  host_to_guest.join();
}