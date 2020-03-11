//
#include <bela/base.hpp>
#include <bela/env.hpp>
#include <bela/finaly.hpp>
#include <bela/path.hpp>
#include <bela/strip.hpp>
#include <winhttp.h>
#include <cstdio>
#include <cstdlib>
#include "baulk.hpp"
#include "indicators.hpp"
#include "net.hpp"
#include "io.hpp"

namespace baulk::net {
inline void Free(HINTERNET &h) {
  if (h != nullptr) {
    WinHttpCloseHandle(h);
  }
}

struct UrlComponets {
  std::wstring host;
  std::wstring filename;
  std::wstring uri;
  int nPort{80};
  int nScheme{INTERNET_SCHEME_HTTPS};
  inline auto TlsFlag() const {
    return nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  }
};

inline std::wstring UrlPathName(std::wstring_view urlpath) {
  std::vector<std::wstring_view> pv = bela::SplitPath(urlpath);
  if (pv.empty()) {
    return L"index.html";
  }
  return std::wstring(pv.back());
}

inline bool CrackUrl(std::wstring_view url, UrlComponets &uc) {
  URL_COMPONENTSW urlcomp;
  ZeroMemory(&urlcomp, sizeof(urlcomp));
  urlcomp.dwStructSize = sizeof(urlcomp);
  urlcomp.dwSchemeLength = (DWORD)-1;
  urlcomp.dwHostNameLength = (DWORD)-1;
  urlcomp.dwUrlPathLength = (DWORD)-1;
  urlcomp.dwExtraInfoLength = (DWORD)-1;
  if (WinHttpCrackUrl(url.data(), static_cast<DWORD>(url.size()), 0,
                      &urlcomp) != TRUE) {
    return false;
  }
  uc.host.assign(urlcomp.lpszHostName, urlcomp.dwHostNameLength);
  std::wstring_view urlpath{urlcomp.lpszUrlPath, urlcomp.dwUrlPathLength};
  uc.filename = UrlPathName(urlpath);
  uc.uri =
      bela::StringCat(urlpath, std::wstring_view(urlcomp.lpszExtraInfo,
                                                 urlcomp.dwExtraInfoLength));
  uc.nPort = urlcomp.nPort;
  uc.nScheme = urlcomp.nScheme;
  return true;
}

inline void EnableTlsProxy(HINTERNET hSession) {
  auto https_proxy_env = bela::GetEnv(L"HTTPS_PROXY");
  if (!https_proxy_env.empty()) {
    WINHTTP_PROXY_INFOW proxy;
    proxy.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
    proxy.lpszProxy = https_proxy_env.data();
    proxy.lpszProxyBypass = nullptr;
    WinHttpSetOption(hSession, WINHTTP_OPTION_PROXY, &proxy, sizeof(proxy));
  }
  DWORD secure_protocols(WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 |
                         WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3);
  WinHttpSetOption(hSession, WINHTTP_OPTION_SECURE_PROTOCOLS, &secure_protocols,
                   sizeof(secure_protocols));
}

inline bool BodyLength(HINTERNET hReq, uint64_t &len) {
  wchar_t conlen[32];
  DWORD dwXsize = sizeof(conlen);
  if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CONTENT_LENGTH,
                          WINHTTP_HEADER_NAME_BY_INDEX, conlen, &dwXsize,
                          WINHTTP_NO_HEADER_INDEX) == TRUE) {
    return bela::SimpleAtoi({conlen, dwXsize / 2}, &len);
  }
  return false;
}

// https://developer.mozilla.org/en-US/docs/Web/HTTP/Headers/Content-Disposition
// update filename
inline bool Disposition(HINTERNET hReq, std::wstring &fn) {
  wchar_t diposition[MAX_PATH + 4];
  DWORD dwXsize = sizeof(diposition);
  if (WinHttpQueryHeaders(hReq, WINHTTP_QUERY_CUSTOM, L"Content-Disposition",
                          diposition, &dwXsize,
                          WINHTTP_NO_HEADER_INDEX) != TRUE) {
    return false;
  }
  std::vector<std::wstring_view> pvv =
      bela::StrSplit(diposition, bela::ByChar(';'), bela::SkipEmpty());
  constexpr std::wstring_view fns = L"filename=";
  for (auto e : pvv) {
    auto s = bela::StripAsciiWhitespace(e);
    if (bela::ConsumePrefix(&s, fns)) {
      bela::ConsumePrefix(&s, L"\"");
      bela::ConsumeSuffix(&s, L"\"");
      fn = s;
      return true;
    }
  }
  return false;
}

std::wstring flatten_http_headers(const headers_t &headers,
                                  const std::vector<std::wstring> &cookies) {
  std::wstring flattened_headers;
  for (const auto &[key, value] : headers) {
    bela::StrAppend(&flattened_headers, key, L": ", value, L"\r\n");
  }
  if (!cookies.empty()) {
    bela::StrAppend(&flattened_headers, L"Cookie: ",
                    bela::StrJoin(cookies, L"; "), L"\r\n");
  }
  return flattened_headers;
}

static inline void query_header_length(HINTERNET request_handle, DWORD header,
                                       DWORD &length) {
  WinHttpQueryHeaders(request_handle, header, WINHTTP_HEADER_NAME_BY_INDEX,
                      WINHTTP_NO_OUTPUT_BUFFER, &length,
                      WINHTTP_NO_HEADER_INDEX);
}

void Response::ParseHeadersString(std::wstring_view hdr) {
  constexpr std::wstring_view content_type = L"Content-Type";
  std::vector<std::wstring_view> hlines =
      bela::StrSplit(hdr, bela::ByString(L"\r\n"), bela::SkipEmpty());
  for (const auto ln : hlines) {
    if (auto pos = ln.find(':'); pos != std::wstring_view::npos) {
      auto k = bela::StripTrailingAsciiWhitespace(ln.substr(0, pos));
      auto v = bela::StripTrailingAsciiWhitespace(ln.substr(pos + 1));
      hkv.emplace(k, v);
    }
  }
}

std::optional<Response> HttpClient::WinRest(std::wstring_view method,
                                            std::wstring_view url,
                                            std::wstring_view contenttype,
                                            std::wstring_view body,
                                            bela::error_code &ec) {
  HINTERNET hSession = nullptr;
  HINTERNET hConnect = nullptr;
  HINTERNET hRequest = nullptr;
  auto deleter = bela::final_act([&] {
    Free(hSession);
    Free(hConnect);
    Free(hRequest);
  });
  UrlComponets uc;
  if (!CrackUrl(url, uc)) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  hSession = WinHttpOpen(baulk::UserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (hSession == nullptr) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  EnableTlsProxy(hSession);
  hConnect = WinHttpConnect(hSession, uc.host.data(), uc.nPort, 0);
  if (hConnect == nullptr) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  hRequest = WinHttpOpenRequest(hConnect, method.data(), uc.uri.data(), nullptr,
                                WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, uc.TlsFlag());
  if (hRequest == nullptr) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  if (!hkv.empty()) {
    auto flattened_headers = flatten_http_headers(hkv, cookies);
    if (WinHttpAddRequestHeaders(hRequest, flattened_headers.data(),
                                 static_cast<size_t>(flattened_headers.size()),
                                 WINHTTP_ADDREQ_FLAG_ADD) != TRUE) {
      ec = bela::make_system_error_code();
      return std::nullopt;
    }
  }
  if (!body.empty()) {
    auto addheader = bela::StringCat(
        L"Content-Type: ", contenttype.empty() ? contenttype : L"text/plain",
        L"\r\nContent-Length: ", body.size(), L"\r\n");
    if (WinHttpAddRequestHeaders(
            hRequest, addheader.data(), static_cast<size_t>(addheader.size()),
            WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE) != TRUE) {
      ec = bela::make_system_error_code();
      return std::nullopt;
    }
    if (WinHttpSendRequest(
            hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            const_cast<LPVOID>(reinterpret_cast<LPCVOID>(body.data())),
            body.size(), body.size(), 0) != TRUE) {
      ec = bela::make_system_error_code();
      return std::nullopt;
    }
  } else {
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != TRUE) {
      ec = bela::make_system_error_code();
      return std::nullopt;
    }
  }

  if (WinHttpReceiveResponse(hRequest, nullptr) != TRUE) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  Response resp;
  DWORD headerBufferLength = 0;
  query_header_length(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                      headerBufferLength);
  std::string hdbf;
  hdbf.resize(headerBufferLength);
  if (WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                          WINHTTP_HEADER_NAME_BY_INDEX, hdbf.data(),
                          &headerBufferLength,
                          WINHTTP_NO_HEADER_INDEX) != TRUE) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  resp.ParseHeadersString(std::wstring_view{
      reinterpret_cast<const wchar_t *>(hdbf.data()), headerBufferLength / 2});
  std::vector<char> readbuf;
  readbuf.reserve(64 * 1024);
  DWORD dwSize = sizeof(resp.statuscode);
  if (WinHttpQueryHeaders(
          hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
          nullptr, &resp.statuscode, &dwSize, nullptr) != TRUE) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  do {
    DWORD downloaded_size = 0;
    if (WinHttpQueryDataAvailable(hRequest, &dwSize) != TRUE) {
      ec = bela::make_system_error_code();
      return std::nullopt;
    }
    if (readbuf.size() < dwSize) {
      readbuf.resize(static_cast<size_t>(dwSize) * 2);
    }
    if (WinHttpReadData(hRequest, (LPVOID)readbuf.data(), dwSize,
                        &downloaded_size) != TRUE) {
      ec = bela::make_system_error_code();
      return std::nullopt;
    }
    resp.body.append(readbuf.data(), dwSize);
  } while (dwSize > 0);
  return std::make_optional(std::move(resp));
}

std::optional<std::wstring> WinGet(std::wstring_view url,
                                   std::wstring_view workdir,
                                   bool forceoverwrite, bela::error_code ec) {
  HINTERNET hSession = nullptr;
  HINTERNET hConnect = nullptr;
  HINTERNET hRequest = nullptr;
  auto deleter = bela::final_act([&] {
    Free(hSession);
    Free(hConnect);
    Free(hRequest);
  });
  UrlComponets uc;
  if (!CrackUrl(url, uc)) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  hSession = WinHttpOpen(baulk::UserAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                         WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (hSession == nullptr) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  EnableTlsProxy(hSession);
  hConnect = WinHttpConnect(hSession, uc.host.data(), uc.nPort, 0);
  if (hConnect == nullptr) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  hRequest = WinHttpOpenRequest(hConnect, L"GET", uc.uri.data(), nullptr,
                                WINHTTP_NO_REFERER,
                                WINHTTP_DEFAULT_ACCEPT_TYPES, uc.TlsFlag());
  if (hRequest == nullptr) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != TRUE) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  if (WinHttpReceiveResponse(hRequest, nullptr) != TRUE) {
    ec = bela::make_system_error_code();
    return std::nullopt;
  }
  baulk::ProgressBar bar;
  uint64_t blen = 0;
  if (BodyLength(hRequest, blen)) {
    bar.Maximum(blen);
  }
  Disposition(hRequest, uc.filename);

  auto dest = bela::PathCat(workdir, uc.filename);
  if (bela::PathExists(dest)) {
    if (!forceoverwrite) {
      ec = bela::make_error_code(ERROR_FILE_EXISTS, L"'", dest,
                                 L"' already exists");
      return std::nullopt;
    }
    if (DeleteFileW(dest.data()) != TRUE) {
      ec = bela::make_system_error_code();
      return std::nullopt;
    }
  }
  size_t total_downloaded_size = 0;
  DWORD dwSize = 0;
  std::vector<char> buf;
  buf.reserve(64 * 1024);
  auto file = baulk::io::FilePart::MakeFilePart(dest, ec);
  bar.FileName(uc.filename);
  bar.Execute();
  auto finish = bela::finally([&] {
    // finish progressbar
    bar.Finish();
  });
  do {
    DWORD downloaded_size = 0;
    if (WinHttpQueryDataAvailable(hRequest, &dwSize) != TRUE) {
      ec = bela::make_system_error_code();
      bar.MarkFault();
      return std::nullopt;
    }
    if (buf.size() < dwSize) {
      buf.resize(static_cast<size_t>(dwSize) * 2);
    }
    if (WinHttpReadData(hRequest, (LPVOID)buf.data(), dwSize,
                        &downloaded_size) != TRUE) {
      ec = bela::make_system_error_code();
      bar.MarkFault();
      return std::nullopt;
    }
    file->Write(buf.data(), downloaded_size);
    total_downloaded_size += downloaded_size;
    bar.Update(total_downloaded_size);
  } while (dwSize > 0);
  file->Finish();
  bar.MarkCompleted();
  return std::make_optional(std::move(dest));
}
} // namespace baulk::net