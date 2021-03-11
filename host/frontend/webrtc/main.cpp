/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <linux/input.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <android-base/logging.h>
#include <android-base/strings.h>
#include <gflags/gflags.h>
#include <libyuv.h>

#include "common/libs/fs/shared_buf.h"
#include "common/libs/fs/shared_fd.h"
#include "common/libs/utils/files.h"
#include "host/frontend/webrtc/audio_handler.h"
#include "host/frontend/webrtc/connection_observer.h"
#include "host/frontend/webrtc/display_handler.h"
#include "host/frontend/webrtc/lib/local_recorder.h"
#include "host/frontend/webrtc/lib/streamer.h"
#include "host/libs/audio_connector/server.h"
#include "host/libs/config/cuttlefish_config.h"
#include "host/libs/config/logging.h"
#include "host/libs/screen_connector/screen_connector.h"

DEFINE_int32(touch_fd, -1, "An fd to listen on for touch connections.");
DEFINE_int32(keyboard_fd, -1, "An fd to listen on for keyboard connections.");
DEFINE_int32(switches_fd, -1, "An fd to listen on for switch connections.");
DEFINE_int32(frame_server_fd, -1, "An fd to listen on for frame updates");
DEFINE_int32(kernel_log_events_fd, -1,
             "An fd to listen on for kernel log events.");
DEFINE_int32(command_fd, -1, "An fd to listen to for control messages");
DEFINE_string(action_servers, "",
              "A comma-separated list of server_name:fd pairs, "
              "where each entry corresponds to one custom action server.");
DEFINE_bool(write_virtio_input, true,
            "Whether to send input events in virtio format.");
DEFINE_int32(audio_server_fd, -1, "An fd to listen on for audio frames");

using cuttlefish::AudioHandler;
using cuttlefish::CfConnectionObserverFactory;
using cuttlefish::DisplayHandler;
using cuttlefish::webrtc_streaming::LocalRecorder;
using cuttlefish::webrtc_streaming::Streamer;
using cuttlefish::webrtc_streaming::StreamerConfig;

class CfOperatorObserver
    : public cuttlefish::webrtc_streaming::OperatorObserver {
 public:
  virtual ~CfOperatorObserver() = default;
  virtual void OnRegistered() override {
    LOG(VERBOSE) << "Registered with Operator";
  }
  virtual void OnClose() override {
    LOG(FATAL) << "Connection with Operator unexpectedly closed";
  }
  virtual void OnError() override {
    LOG(FATAL) << "Error encountered in connection with Operator";
  }
};

static std::vector<std::pair<std::string, std::string>> ParseHttpHeaders(
    const std::string& path) {
  auto fd = cuttlefish::SharedFD::Open(path, O_RDONLY);
  if (!fd->IsOpen()) {
    LOG(WARNING) << "Unable to open operator (signaling server) headers file, "
                    "connecting to the operator will probably fail: "
                 << fd->StrError();
    return {};
  }
  std::string raw_headers;
  auto res = cuttlefish::ReadAll(fd, &raw_headers);
  if (res < 0) {
    LOG(WARNING) << "Unable to open operator (signaling server) headers file, "
                    "connecting to the operator will probably fail: "
                 << fd->StrError();
    return {};
  }
  std::vector<std::pair<std::string, std::string>> headers;
  std::size_t raw_index = 0;
  while (raw_index < raw_headers.size()) {
    auto colon_pos = raw_headers.find(':', raw_index);
    if (colon_pos == std::string::npos) {
      LOG(ERROR)
          << "Expected to find ':' in each line of the operator headers file";
      break;
    }
    auto eol_pos = raw_headers.find('\n', colon_pos);
    if (eol_pos == std::string::npos) {
      eol_pos = raw_headers.size();
    }
    // If the file uses \r\n as line delimiters exclude the \r too.
    auto eov_pos = raw_headers[eol_pos - 1] == '\r'? eol_pos - 1: eol_pos;
    headers.emplace_back(
        raw_headers.substr(raw_index, colon_pos + 1 - raw_index),
        raw_headers.substr(colon_pos + 1, eov_pos - colon_pos - 1));
    raw_index = eol_pos + 1;
  }
  return headers;
}

std::unique_ptr<cuttlefish::AudioServer> CreateAudioServer() {
  cuttlefish::SharedFD audio_server_fd =
      cuttlefish::SharedFD::Dup(FLAGS_audio_server_fd);
  close(FLAGS_audio_server_fd);
  return std::make_unique<cuttlefish::AudioServer>(audio_server_fd);
}

int main(int argc, char** argv) {
  cuttlefish::DefaultSubprocessLogging(argv);
  ::gflags::ParseCommandLineFlags(&argc, &argv, true);

  cuttlefish::InputSockets input_sockets;

  input_sockets.touch_server = cuttlefish::SharedFD::Dup(FLAGS_touch_fd);
  input_sockets.keyboard_server = cuttlefish::SharedFD::Dup(FLAGS_keyboard_fd);
  input_sockets.switches_server = cuttlefish::SharedFD::Dup(FLAGS_switches_fd);
  auto control_socket = cuttlefish::SharedFD::Dup(FLAGS_command_fd);
  close(FLAGS_touch_fd);
  close(FLAGS_keyboard_fd);
  close(FLAGS_switches_fd);
  close(FLAGS_command_fd);
  // Accepting on these sockets here means the device won't register with the
  // operator as soon as it could, but rather wait until crosvm's input display
  // devices have been initialized. That's OK though, because without those
  // devices there is no meaningful interaction the user can have with the
  // device.
  input_sockets.touch_client =
      cuttlefish::SharedFD::Accept(*input_sockets.touch_server);
  input_sockets.keyboard_client =
      cuttlefish::SharedFD::Accept(*input_sockets.keyboard_server);
  input_sockets.switches_client =
      cuttlefish::SharedFD::Accept(*input_sockets.switches_server);

  std::thread touch_accepter([&input_sockets]() {
    for (;;) {
      input_sockets.touch_client =
          cuttlefish::SharedFD::Accept(*input_sockets.touch_server);
    }
  });
  std::thread keyboard_accepter([&input_sockets]() {
    for (;;) {
      input_sockets.keyboard_client =
          cuttlefish::SharedFD::Accept(*input_sockets.keyboard_server);
    }
  });
  std::thread switches_accepter([&input_sockets]() {
    for (;;) {
      input_sockets.switches_client =
          cuttlefish::SharedFD::Accept(*input_sockets.switches_server);
    }
  });

  auto kernel_log_events_client =
      cuttlefish::SharedFD::Dup(FLAGS_kernel_log_events_fd);
  close(FLAGS_kernel_log_events_fd);

  auto cvd_config = cuttlefish::CuttlefishConfig::Get();
  auto instance = cvd_config->ForDefaultInstance();
  auto screen_connector =
      cuttlefish::DisplayHandler::ScreenConnector::Get(FLAGS_frame_server_fd);

  StreamerConfig streamer_config;

  streamer_config.device_id = instance.webrtc_device_id();
  streamer_config.tcp_port_range = cvd_config->webrtc_tcp_port_range();
  streamer_config.udp_port_range = cvd_config->webrtc_udp_port_range();
  streamer_config.operator_server.addr = cvd_config->sig_server_address();
  streamer_config.operator_server.port = cvd_config->sig_server_port();
  streamer_config.operator_server.path = cvd_config->sig_server_path();
  streamer_config.operator_server.security =
      cvd_config->sig_server_strict()
          ? WsConnection::Security::kStrict
          : WsConnection::Security::kAllowSelfSigned;

  if (!cvd_config->sig_server_headers_path().empty()) {
    streamer_config.operator_server.http_headers =
        ParseHttpHeaders(cvd_config->sig_server_headers_path());
  }

  auto observer_factory = std::make_shared<CfConnectionObserverFactory>(
      input_sockets, kernel_log_events_client);

  auto streamer = Streamer::Create(streamer_config, observer_factory);
  CHECK(streamer) << "Could not create streamer";

  auto display_0 = streamer->AddDisplay(
      "display_0", screen_connector->ScreenWidth(0),
      screen_connector->ScreenHeight(0), cvd_config->dpi(), true);
  auto display_handler =
    std::make_shared<DisplayHandler>(display_0, std::move(screen_connector));

  std::unique_ptr<cuttlefish::webrtc_streaming::LocalRecorder> local_recorder;
  if (cvd_config->record_screen()) {
    int recording_num = 0;
    std::string recording_path;
    do {
      recording_path = instance.PerInstancePath("recording/recording_");
      recording_path += std::to_string(recording_num);
      recording_path += ".webm";
      recording_num++;
    } while (cuttlefish::FileExists(recording_path));
    local_recorder = LocalRecorder::Create(recording_path);
    CHECK(local_recorder) << "Could not create local recorder";

    streamer->RecordDisplays(*local_recorder);
    display_handler->IncClientCount();
  }

  observer_factory->SetDisplayHandler(display_handler);

  streamer->SetHardwareSpec("CPUs", cvd_config->cpus());
  streamer->SetHardwareSpec("RAM", std::to_string(cvd_config->memory_mb()) + " mb");

  std::string user_friendly_gpu_mode;
  if (cvd_config->gpu_mode() == cuttlefish::kGpuModeGuestSwiftshader) {
    user_friendly_gpu_mode = "SwiftShader (Guest CPU Rendering)";
  } else if (cvd_config->gpu_mode() == cuttlefish::kGpuModeDrmVirgl) {
    user_friendly_gpu_mode = "VirglRenderer (Accelerated Host GPU Rendering)";
  } else if (cvd_config->gpu_mode() == cuttlefish::kGpuModeGfxStream) {
    user_friendly_gpu_mode = "Gfxstream (Accelerated Host GPU Rendering)";
  } else {
    user_friendly_gpu_mode = cvd_config->gpu_mode();
  }
  streamer->SetHardwareSpec("GPU Mode", user_friendly_gpu_mode);

  std::shared_ptr<AudioHandler> audio_handler;
  if (cvd_config->enable_audio()) {
    auto audio_stream = streamer->AddAudioStream("audio");
    auto audio_server = CreateAudioServer();
    auto audio_source = streamer->GetAudioSource();
    audio_handler = std::make_shared<AudioHandler>(std::move(audio_server),
                                                   audio_stream, audio_source);
  }

  // Parse the -action_servers flag, storing a map of action server name -> fd
  std::map<std::string, int> action_server_fds;
  for (const std::string& action_server :
       android::base::Split(FLAGS_action_servers, ",")) {
    if (action_server.empty()) {
      continue;
    }
    const std::vector<std::string> server_and_fd =
        android::base::Split(action_server, ":");
    CHECK(server_and_fd.size() == 2)
        << "Wrong format for action server flag: " << action_server;
    std::string server = server_and_fd[0];
    int fd = std::stoi(server_and_fd[1]);
    action_server_fds[server] = fd;
  }

  for (const auto& custom_action : cvd_config->custom_actions()) {
    if (custom_action.shell_command) {
      if (custom_action.buttons.size() != 1) {
        LOG(FATAL) << "Expected exactly one button for custom action command: "
                   << *(custom_action.shell_command);
      }
      const auto button = custom_action.buttons[0];
      streamer->AddCustomControlPanelButton(button.command, button.title,
                                            button.icon_name,
                                            custom_action.shell_command);
    }
    if (custom_action.server) {
      if (action_server_fds.find(*(custom_action.server)) !=
          action_server_fds.end()) {
        LOG(INFO) << "Connecting to custom action server "
                  << *(custom_action.server);

        int fd = action_server_fds[*(custom_action.server)];
        cuttlefish::SharedFD custom_action_server = cuttlefish::SharedFD::Dup(fd);
        close(fd);

        if (custom_action_server->IsOpen()) {
          std::vector<std::string> commands_for_this_server;
          for (const auto& button : custom_action.buttons) {
            streamer->AddCustomControlPanelButton(button.command, button.title,
                                                  button.icon_name);
            commands_for_this_server.push_back(button.command);
          }
          observer_factory->AddCustomActionServer(custom_action_server,
                                                  commands_for_this_server);
        } else {
          LOG(ERROR) << "Error connecting to custom action server: "
                     << *(custom_action.server);
        }
      } else {
        LOG(ERROR) << "Custom action server not provided as command line flag: "
                   << *(custom_action.server);
      }
    }
  }

  std::shared_ptr<cuttlefish::webrtc_streaming::OperatorObserver> operator_observer(
      new CfOperatorObserver());
  streamer->Register(operator_observer);

  std::thread control_thread([control_socket, &local_recorder]() {
    if (!local_recorder) {
      return;
    }
    std::string message = "_";
    int read_ret;
    while ((read_ret = cuttlefish::ReadExact(control_socket, &message)) > 0) {
      LOG(VERBOSE) << "received control message: " << message;
      if (message[0] == 'C') {
        LOG(DEBUG) << "Finalizing screen recording...";
        local_recorder->Stop();
        LOG(INFO) << "Finalized screen recording.";
        message = "Y";
        cuttlefish::WriteAll(control_socket, message);
      }
    }
    LOG(DEBUG) << "control socket closed";
  });

  if (audio_handler) {
    audio_handler->Start();
  }
  display_handler->Loop();

  return 0;
}
