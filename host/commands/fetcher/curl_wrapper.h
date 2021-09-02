//
// Copyright (C) 2019 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <string>

#include <curl/curl.h>
#include <json/json.h>

namespace cuttlefish {

template <typename T>
struct CurlResponse {
  bool HttpInfo() { return http_code >= 100 && http_code <= 199; }
  bool HttpSuccess() { return http_code >= 200 && http_code <= 299; }
  bool HttpRedirect() { return http_code >= 300 && http_code <= 399; }
  bool HttpClientError() { return http_code >= 400 && http_code <= 499; }
  bool HttpServerError() { return http_code >= 500 && http_code <= 599; }

  T data;
  long http_code;
};

class CurlWrapper {
public:
  CurlWrapper();
  ~CurlWrapper();
  CurlWrapper(const CurlWrapper&) = delete;
  CurlWrapper& operator=(const CurlWrapper*) = delete;
  CurlWrapper(CurlWrapper&&) = default;

  CurlResponse<std::string> DownloadToFile(const std::string& url,
                                           const std::string& path);
  CurlResponse<std::string> DownloadToFile(
      const std::string& url, const std::string& path,
      const std::vector<std::string>& headers);
  CurlResponse<std::string> DownloadToString(const std::string& url);
  CurlResponse<std::string> DownloadToString(
      const std::string& url, const std::vector<std::string>& headers);
  CurlResponse<Json::Value> DownloadToJson(const std::string& url);
  CurlResponse<Json::Value> DownloadToJson(
      const std::string& url, const std::vector<std::string>& headers);

 private:
  CURL* curl;
};

}
