// Copyright (C) 2013-2015 iFunFactory Inc. All Rights Reserved.
//
// This work is confidential and proprietary to iFunFactory Inc. and
// must not be used, disclosed, copied, or distributed without the prior
// consent of iFunFactory Inc.

#include "Engine.h"
#include "UnrealString.h"

#if PLATFORM_WINDOWS
#include "AllowWindowsPlatformTypes.h"
#include <process.h>
#include <WinSock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <algorithm>
#endif

#include <list>
#include <sstream>
#include <functional>
#include <queue>
#include <mutex>
#include <memory>

#include "curl/curl.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "./funapi_network.h"
#include "./funapi_utils.h"
#if PLATFORM_WINDOWS
#include "HideWindowsPlatformTypes.h"
#endif


namespace fun {

using std::map;


namespace {

////////////////////////////////////////////////////////////////////////////////
// Types.

typedef sockaddr_in Endpoint;
typedef helper::Binder1<int, void *> AsyncConnectCallback;
typedef helper::Binder1<ssize_t, void *> AsyncSendCallback;
typedef helper::Binder1<ssize_t, void *> AsyncReceiveCallback;
typedef helper::Binder1<int, void *> AsyncWebRequestCallback;
typedef helper::Binder2<void *, int, void *> AsyncWebResponseCallback;


enum FunapiTransportType {
  kTcp = 1,
  kUdp,
  kHttp,
};


enum FunapiTransportState {
  kDisconnected = 0,
  kConnecting,
  kConnected,
};


enum WebRequestState {
  kWebRequestStart = 0,
  kWebRequestEnd,
};



struct AsyncRequest {
  enum RequestType {
    kConnect = 0,
    kSend,
    kSendTo,
    kReceive,
    kReceiveFrom,
    kWebRequest,
  };

  RequestType type_;
  int sock_;

  struct {
    AsyncConnectCallback callback_;
    Endpoint endpoint_;
  } connect_;

  struct AsyncSendContext {
    AsyncSendCallback callback_;
    uint8_t *buf_;
    size_t buf_len_;
    size_t buf_offset_;
  } send_;

  struct AsyncReceiveContext {
    AsyncReceiveCallback callback_;
    uint8_t *buf_;
    size_t capacity_;
  } recv_;

  struct AsyncSendToContext : AsyncSendContext {
    Endpoint endpoint_;
  } sendto_;

  struct AsyncReceiveFromContext : AsyncReceiveContext {
    Endpoint endpoint_;
  } recvfrom_;

  struct WebRequestContext {
    AsyncWebRequestCallback request_callback_;
    AsyncWebResponseCallback receive_header_;
    AsyncWebResponseCallback receive_body_;
    const char *url_;
    const char *header_;
    uint8_t *body_;
    size_t body_len_;
  } web_request_;
};


typedef std::list<struct iovec> IoVecList;


////////////////////////////////////////////////////////////////////////////////
// Constants.

// Buffer-related constants.
const int kUnitBufferSize = 65536;
const int kUnitBufferPaddingSize = 1;

// Funapi header-related constants.
const char kHeaderDelimeter[] = "\n";
const char kHeaderFieldDelimeter[] = ":";
const char kVersionHeaderField[] = "VER";
const char kLengthHeaderField[] = "LEN";
const int kCurrentFunapiProtocolVersion = 1;

// Funapi message-related constants.
const char kMsgTypeBodyField[] = "_msgtype";
const char kSessionIdBodyField[] = "_sid";
const char kNewSessionMessageType[] = "_session_opened";
const char kSessionClosedMessageType[] = "_session_closed";



////////////////////////////////////////////////////////////////////////////////
// Global variables.

bool initialized = false;
bool curl_initialized = false;
time_t epoch = 0;
time_t funapi_session_timeout = 3600;

typedef std::list<AsyncRequest> AsyncQueue;
AsyncQueue async_queue;

std::queue< std::function<void()> > tasks_queue;
std::mutex tasks_queue_mutex;

bool async_thread_run = false;
#if PLATFORM_WINDOWS
HANDLE async_queue_thread;
HANDLE async_queue_mutex;
#else
pthread_t async_queue_thread;
pthread_mutex_t async_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t async_queue_cond = PTHREAD_COND_INITIALIZER;
#endif

size_t HttpResponseCb(void *data, size_t size, size_t count, void *cb) {
  AsyncWebResponseCallback *callback = (AsyncWebResponseCallback*)(cb);
  if (callback != NULL)
    (*callback)(data, size * count);
  return size * count;
}



////////////////////////////////////////////////////////////////////////////////
// Asynchronous operation related..

#if PLATFORM_WINDOWS
uint32_t WINAPI AsyncQueueThreadProc(LPVOID /*arg*/) {
#else
void *AsyncQueueThreadProc(void * /*arg*/) {
#endif
  LOG("Thread for async operation has been created.");

  while (async_thread_run) {
    // Waits until we have something to process.
    pthread_mutex_lock(&async_queue_mutex);
    while (async_thread_run && async_queue.empty()) {
#if PLATFORM_WINDOWS
      ReleaseMutex(async_queue_mutex);
      Sleep(1000);
      WaitForSingleObject(async_queue_mutex, INFINITE);
#else
      pthread_cond_wait(&async_queue_cond, &async_queue_mutex);
#endif
    }

    // Moves element to a worker queue and releaes the mutex
    // for contention prevention.
    AsyncQueue work_queue;
    work_queue.swap(async_queue);
    pthread_mutex_unlock(&async_queue_mutex);

    fd_set rset, wset;
    FD_ZERO(&rset);
    FD_ZERO(&wset);

    // Makes fd_sets for select().
    int max_fd = -1;
    for (AsyncQueue::const_iterator i = work_queue.begin();
         i != work_queue.end(); ++i) {

      switch (i->type_) {
      case AsyncRequest::kConnect:
      case AsyncRequest::kSend:
      case AsyncRequest::kSendTo:
        //LOG("Checking " << i->type_ << " with fd [" << i->sock_ << "]");
        FD_SET(i->sock_, &wset);
        max_fd = std::max(max_fd, i->sock_);
        break;
      case AsyncRequest::kReceive:
      case AsyncRequest::kReceiveFrom:
        //LOG("Checking " << i->type_ << " with fd [" << i->sock_ << "]");
        FD_SET(i->sock_, &rset);
        max_fd = std::max(max_fd, i->sock_);
        break;
      case AsyncRequest::kWebRequest:
        break;
      default:
        assert(false);
        break;
      }
    }

    // Waits until some events happen to fd in the sets.
    if (max_fd != -1) {
      struct timeval timeout = {0, 1};
      int r = select(max_fd + 1, &rset, &wset, NULL, &timeout);

      // Some fd can be invalidated if other thread closed.
      assert(r >= 0 || (r < 0 && errno == EBADF));
      // LOG("woke up: " << r);
    }

    // Checks if the fd is ready.
    for (AsyncQueue::iterator i = work_queue.begin(); i != work_queue.end(); ) {
      bool remove_request = true;
      // Ready. Handles accordingly.
      switch (i->type_) {
      case AsyncRequest::kConnect:
        if (!FD_ISSET(i->sock_, &wset)) {
          remove_request = false;
        } else {
          int e;
          socklen_t e_size = sizeof(e);
          int r = getsockopt(i->sock_, SOL_SOCKET, SO_ERROR, (char *)&e, &e_size);
          if (r == 0) {
            i->connect_.callback_(e);
          } else {
            assert(r < 0 && errno == EBADF);
          }
        }
        break;
      case AsyncRequest::kSend:
        if (!FD_ISSET(i->sock_, &wset)) {
          remove_request = false;
        } else {
          ssize_t nSent =
              send(i->sock_, (char *)i->send_.buf_ + i->send_.buf_offset_,
                  i->send_.buf_len_ - i->send_.buf_offset_, 0);
          if (nSent < 0) {
            i->send_.callback_(nSent);
          } else if (nSent + i->send_.buf_offset_ == i->send_.buf_len_) {
            i->send_.callback_(i->send_.buf_len_);
          } else {
            i->send_.buf_offset_ += nSent;
            remove_request = false;
          }
        }
        break;
      case AsyncRequest::kReceive:
        if (!FD_ISSET(i->sock_, &rset)) {
          remove_request = false;
        } else {
          ssize_t nRead =
              recv(i->sock_, (char *)i->recv_.buf_, i->recv_.capacity_, 0);
          i->recv_.callback_(nRead);
        }
        break;
      case AsyncRequest::kSendTo:
        if (!FD_ISSET(i->sock_, &wset)) {
          remove_request = false;
        } else {
          socklen_t len = sizeof(i->sendto_.endpoint_);
          ssize_t nSent =
              sendto(i->sock_, (char *)i->sendto_.buf_ + i->sendto_.buf_offset_,
                     i->sendto_.buf_len_ - i->sendto_.buf_offset_, 0,
                     (struct sockaddr *)&i->sendto_.endpoint_, len);
          if (nSent < 0) {
            i->sendto_.callback_(nSent);
          } else if (nSent + i->sendto_.buf_offset_ == i->sendto_.buf_len_) {
            i->sendto_.callback_(i->sendto_.buf_len_);
          } else {
            i->sendto_.buf_offset_ += nSent;
            remove_request = false;
          }
        }
        break;
      case AsyncRequest::kReceiveFrom:
        if (!FD_ISSET(i->sock_, &rset)) {
          remove_request = false;
        } else {
          socklen_t len = sizeof(i->recvfrom_.endpoint_);
          ssize_t nRead =
              recvfrom(i->sock_, (char *)i->recvfrom_.buf_, i->recvfrom_.capacity_, 0,
              (struct sockaddr *)&i->recvfrom_.endpoint_, &len);
          i->recvfrom_.callback_(nRead);
        }
        break;
      case AsyncRequest::kWebRequest: {
          CURL *ctx = curl_easy_init();
          if (ctx == NULL) {
            LOG("Unable to initialize cURL interface.");
            break;
          }

          i->web_request_.request_callback_(kWebRequestStart);

          struct curl_slist *chunk = NULL;
          chunk = curl_slist_append(chunk, i->web_request_.header_);
          curl_easy_setopt(ctx, CURLOPT_HTTPHEADER, chunk);

          curl_easy_setopt(ctx, CURLOPT_URL, i->web_request_.url_);
          curl_easy_setopt(ctx, CURLOPT_POST, 1L);
          curl_easy_setopt(ctx, CURLOPT_POSTFIELDS, i->web_request_.body_);
          curl_easy_setopt(ctx, CURLOPT_POSTFIELDSIZE, i->web_request_.body_len_);
          curl_easy_setopt(ctx, CURLOPT_HEADERDATA, &i->web_request_.receive_header_);
          curl_easy_setopt(ctx, CURLOPT_WRITEDATA, &i->web_request_.receive_body_);
          curl_easy_setopt(ctx, CURLOPT_WRITEFUNCTION, &HttpResponseCb);

          CURLcode res = curl_easy_perform(ctx);
          if (res != CURLE_OK) {
            LOG1("Error from cURL: %s", *FString(curl_easy_strerror(res)));
            assert(false);
          } else {
            i->web_request_.request_callback_(kWebRequestEnd);
          }

          curl_easy_cleanup(ctx);
          curl_slist_free_all(chunk);
        }
        break;
      }

      // If we should keep the fd, puts it back to the queue.
      if (remove_request) {
        AsyncQueue::iterator to_delete = i++;
        work_queue.erase(to_delete);
      } else {
        ++i;
      }
    }

    // Puts back requests that requires more work.
    // We should respect the order.
    pthread_mutex_lock(&async_queue_mutex);
    async_queue.splice(async_queue.begin(), work_queue);
    pthread_mutex_unlock(&async_queue_mutex);
  }

  LOG("Thread for async operation is terminating.");
  return 0;
}


void AsyncConnect(int sock,
    const Endpoint &endpoint, const AsyncConnectCallback &callback) {
  // Makes the fd non-blocking.
#if PLATFORM_WINDOWS
  u_long argp = 0;
  int flag = ioctlsocket(sock, FIONBIO, &argp);
  assert(flag >= 0);
  int rc;
#else
  int flag = fcntl(sock, F_GETFL);
  assert(flag >= 0);
  int rc = fcntl(sock, F_SETFL, O_NONBLOCK | flag);
  assert(rc >= 0);
#endif

  // Tries to connect.
  rc = connect(sock, reinterpret_cast<const struct sockaddr *>(&endpoint),
               sizeof(endpoint));
  assert(rc == 0 || (rc < 0 && errno == EINPROGRESS));

  LOG("Queueing 1 async connect.");

  AsyncRequest r;
  r.type_ = AsyncRequest::kConnect;
  r.sock_ = sock;
  r.connect_.callback_ = callback;
  r.connect_.endpoint_ = endpoint;

  pthread_mutex_lock(&async_queue_mutex);
  async_queue.push_back(r);
  pthread_cond_signal(&async_queue_cond);
  pthread_mutex_unlock(&async_queue_mutex);
}


void AsyncSend(int sock,
    const IoVecList &sending, AsyncSendCallback callback) {
  assert(!sending.empty());
  LOG1("Queueing %d async sends.", sending.size());

  pthread_mutex_lock(&async_queue_mutex);
  for (IoVecList::const_iterator itr = sending.begin(), itr_end = sending.end();
      itr != itr_end;
      ++itr) {
    const struct iovec &e = *itr;
    AsyncRequest r;
    r.type_ = AsyncRequest::kSend;
    r.sock_ = sock;
    r.send_.callback_ = callback;
    r.send_.buf_ = reinterpret_cast<uint8_t *>(e.iov_base);
    r.send_.buf_len_ = e.iov_len;
    r.send_.buf_offset_ = 0;
    async_queue.push_back(r);
  }
  pthread_cond_signal(&async_queue_cond);
  pthread_mutex_unlock(&async_queue_mutex);
}


void AsyncReceive(int sock,
    const struct iovec &receiving, AsyncReceiveCallback callback) {
  assert(receiving.iov_len != 0);
  LOG("Queueing 1 async receive.");

  AsyncRequest r;
  r.type_ = AsyncRequest::kReceive;
  r.sock_ = sock;
  r.recv_.callback_ = callback;
  r.recv_.buf_ = reinterpret_cast<uint8_t *>(receiving.iov_base);
  r.recv_.capacity_ = receiving.iov_len;

  pthread_mutex_lock(&async_queue_mutex);
  async_queue.push_back(r);
  pthread_cond_signal(&async_queue_cond);
  pthread_mutex_unlock(&async_queue_mutex);
}


void AsyncSendTo(int sock, const Endpoint &ep,
    const IoVecList &sending, AsyncSendCallback callback) {
  assert(!sending.empty());
  LOG1("Queueing %d async sends.", sending.size());

  pthread_mutex_lock(&async_queue_mutex);
  for (IoVecList::const_iterator itr = sending.begin(), itr_end = sending.end();
      itr != itr_end;
      ++itr) {
    const struct iovec &e = *itr;

    AsyncRequest r;
    r.type_ = AsyncRequest::kSendTo;
    r.sock_ = sock;
    r.sendto_.endpoint_ = ep;
    r.sendto_.callback_ = callback;
    r.sendto_.buf_offset_ = 0;
    r.sendto_.buf_len_ = e.iov_len;
    r.sendto_.buf_ = reinterpret_cast<uint8_t *>(e.iov_base);
    async_queue.push_back(r);
  }
  pthread_cond_signal(&async_queue_cond);
  pthread_mutex_unlock(&async_queue_mutex);
}


void AsyncReceiveFrom(int sock, const Endpoint &ep,
    const struct iovec &receiving, AsyncReceiveCallback callback) {
  assert(receiving.iov_len != 0);
  LOG("Queueing 1 async receive.");

  AsyncRequest r;
  r.type_ = AsyncRequest::kReceiveFrom;
  r.sock_ = sock;
  r.recvfrom_.endpoint_ = ep;
  r.recvfrom_.callback_ = callback;
  r.recvfrom_.buf_ = reinterpret_cast<uint8_t *>(receiving.iov_base);
  r.recvfrom_.capacity_ = receiving.iov_len;

  pthread_mutex_lock(&async_queue_mutex);
  async_queue.push_back(r);
  pthread_cond_signal(&async_queue_cond);
  pthread_mutex_unlock(&async_queue_mutex);
}


void AsyncWebRequest(const char* host_url, const IoVecList &sending,
    AsyncWebRequestCallback callback,
    AsyncWebResponseCallback receive_header,
    AsyncWebResponseCallback receive_body) {
  assert(!sending.empty());
  LOG1("Queueing %d async sends.", sending.size());

  pthread_mutex_lock(&async_queue_mutex);
  for (IoVecList::const_iterator itr = sending.begin(), itr_end = sending.end();
      itr != itr_end;
      ++itr) {
    const struct iovec &header = *itr;
    const struct iovec &body = *(++itr);

    AsyncRequest r;
    r.type_ = AsyncRequest::kWebRequest;
    r.web_request_.url_ = host_url;
    r.web_request_.request_callback_ = callback;
    r.web_request_.receive_header_ = receive_header;
    r.web_request_.receive_body_ = receive_body;
    r.web_request_.header_ = reinterpret_cast<char *>(header.iov_base);
    r.web_request_.body_ = reinterpret_cast<uint8_t *>(body.iov_base);
    r.web_request_.body_len_ = body.iov_len;
    async_queue.push_back(r);
  }
  pthread_cond_signal(&async_queue_cond);
  pthread_mutex_unlock(&async_queue_mutex);
}

}  // unnamed namespace



////////////////////////////////////////////////////////////////////////////////
// FunapiTransportBase implementation.

class FunapiTransportBase : std::enable_shared_from_this<FunapiTransportBase> {
 public:
  typedef FunapiTransport::OnReceived OnReceived;
  typedef FunapiTransport::OnStopped OnStopped;

  FunapiTransportBase(FunapiTransportType type);
  virtual ~FunapiTransportBase();

  void RegisterEventHandlers(const OnReceived &cb1, const OnStopped &cb2);

  void SendMessage(Json &message);
  void SendMessage(FunMessage &message);

 protected:
  void SendMessage(const char *body);

 private:
  virtual void EncodeThenSendMessage() = 0;

  bool TryToDecodeHeader();
  bool TryToDecodeBody();

 protected:
  typedef map<string, string> HeaderFields;

  void Init();
  bool EncodeMessage();
  bool DecodeMessage(int nRead);

  // Registered event handlers.
  OnReceived on_received_;
  OnStopped on_stopped_;

  // State-related.
#if PLATFORM_WINDOWS
  HANDLE mutex_;
#else
  mutable pthread_mutex_t mutex_;
#endif
  FunapiTransportType type_;
  FunapiTransportState state_;
  IoVecList sending_;
  IoVecList pending_;
  struct iovec receiving_;
  bool header_decoded_;
  int received_size_;
  int next_decoding_offset_;
  HeaderFields header_fields_;
};


FunapiTransportBase::FunapiTransportBase(FunapiTransportType type)
    : type_(type), state_(kDisconnected), header_decoded_(false),
      received_size_(0), next_decoding_offset_(0) {
  receiving_.iov_len = kUnitBufferSize;
  receiving_.iov_base =
      new uint8_t[receiving_.iov_len + kUnitBufferPaddingSize];
  assert(receiving_.iov_base);
}


FunapiTransportBase::~FunapiTransportBase() {
  header_fields_.clear();

  IoVecList::iterator itr, itr_end;

  for (itr = sending_.begin(), itr_end = sending_.end();
      itr != itr_end;
      ++itr) {
    struct iovec &e = *itr;
    delete [] reinterpret_cast<uint8_t *>(e.iov_base);
  }
  sending_.clear();

  for (itr = pending_.begin(), itr_end = pending_.end();
      itr != itr_end;
      ++itr) {
    struct iovec &e = *itr;
    delete [] reinterpret_cast<uint8_t *>(e.iov_base);
  }
  pending_.clear();

  if (receiving_.iov_base) {
    delete [] reinterpret_cast<uint8_t *>(receiving_.iov_base);
    receiving_.iov_base = NULL;
  }
}


void FunapiTransportBase::RegisterEventHandlers(
    const OnReceived &on_received, const OnStopped &on_stopped) {
  pthread_mutex_lock(&mutex_);
  on_received_ = on_received;
  on_stopped_ = on_stopped;
  pthread_mutex_unlock(&mutex_);
}


void FunapiTransportBase::Init() {
  // Resets states.
  header_decoded_ = false;
  received_size_ = 0;
  next_decoding_offset_ = 0;
  header_fields_.clear();
  for (IoVecList::iterator itr = sending_.begin(), itr_end = sending_.end();
      itr != itr_end;
      ++itr) {
    delete [] reinterpret_cast<uint8_t *>(itr->iov_base);
  }
  sending_.clear();
}


bool FunapiTransportBase::EncodeMessage() {
  assert(!sending_.empty());

  for (IoVecList::iterator itr = sending_.begin(), itr_end = sending_.end();
      itr != itr_end;
      ++itr) {
    struct iovec &body_as_bytes = *itr;

    char header[1024];
    size_t offset = 0;
    if (type_ == kTcp || type_ == kUdp) {
      offset += snprintf(header + offset, sizeof(header) - offset, "%s%s%d%s",
                         kVersionHeaderField, kHeaderFieldDelimeter,
                         kCurrentFunapiProtocolVersion, kHeaderDelimeter);
      offset += snprintf(header + offset, sizeof(header)- offset, "%s%s%lu%s",
                         kLengthHeaderField, kHeaderFieldDelimeter,
                         (unsigned long)body_as_bytes.iov_len, kHeaderDelimeter);
      offset += snprintf(header + offset, sizeof(header)- offset, "%s",
                          kHeaderDelimeter);
    }

    char *b = reinterpret_cast<char *>(body_as_bytes.iov_base);
    b[body_as_bytes.iov_len] = '\0';
    header[offset] = '\0';
    LOG1("Header to send: %s", *FString(header));
    LOG1("send message: %s", *FString(b));

    if (type_ == kTcp || type_ == kHttp) {
      struct iovec header_as_bytes;
      header_as_bytes.iov_len = offset;
      // LOG 출력을 위하여 null 문자를 위한 + 1
      header_as_bytes.iov_base = new uint8_t[header_as_bytes.iov_len + 1];
      memcpy(header_as_bytes.iov_base,
             header,
             header_as_bytes.iov_len + 1);

      sending_.insert(itr, header_as_bytes);
    } else if (type_ == kUdp) {
      uint8_t* bytes = new uint8_t[offset + body_as_bytes.iov_len];
      memcpy(bytes, header, offset);
      memcpy(&bytes[offset], body_as_bytes.iov_base, body_as_bytes.iov_len);
      delete [] reinterpret_cast<uint8_t *>(body_as_bytes.iov_base);
      body_as_bytes.iov_base = bytes;
      body_as_bytes.iov_len = offset + body_as_bytes.iov_len;
    }
  }

  return true;
}


bool FunapiTransportBase::DecodeMessage(int nRead) {
  if (nRead <= 0) {
    if (nRead < 0) {
      LOG1("receive failed: %s", *FString(strerror(errno)));
    } else {
      if (received_size_ > next_decoding_offset_) {
        LOG1("Buffer has %d bytes. But they failed to decode. Discarding.", (received_size_ - next_decoding_offset_));
      }
    }
    return false;
  }

  LOG2("Received %d bytes. Buffer has %d bytes.", nRead, (received_size_ - next_decoding_offset_));
  pthread_mutex_lock(&mutex_);
  received_size_ += nRead;

  // Tries to decode as many messags as possible.
  while (true) {
    if (header_decoded_ == false) {
      if (TryToDecodeHeader() == false) {
        break;
      }
    }
    if (header_decoded_) {
      if (TryToDecodeBody() == false) {
        break;
      }
    }
  }

  // Checks buvffer space before starting another async receive.
  if (receiving_.iov_len - received_size_ == 0) {
    // If there are space can be collected, compact it first.
    // Otherwise, increase the receiving buffer size.
    if (next_decoding_offset_ > 0) {
      LOG1("Compacting a receive buffer to save %d bytes.", next_decoding_offset_);
      uint8_t *base = reinterpret_cast<uint8_t *>(receiving_.iov_base);
      memmove(base, base + next_decoding_offset_,
              received_size_ - next_decoding_offset_);
      received_size_ -= next_decoding_offset_;
      next_decoding_offset_ = 0;
    } else {
      size_t new_size = receiving_.iov_len + kUnitBufferSize;
      LOG1("Resizing a buffer to %d bytes.", new_size);
      uint8_t *new_buffer = new uint8_t[new_size + kUnitBufferPaddingSize];
      memmove(new_buffer, receiving_.iov_base, received_size_);
      delete [] reinterpret_cast<uint8_t *>(receiving_.iov_base);
      receiving_.iov_base = new_buffer;
      receiving_.iov_len = new_size;
    }
  }
  pthread_mutex_unlock(&mutex_);
  return true;
}


void FunapiTransportBase::SendMessage(Json &message) {
  rapidjson::StringBuffer string_buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(string_buffer);
  message.Accept(writer);

  SendMessage(string_buffer.GetString());
}


void FunapiTransportBase::SendMessage(FunMessage &message) {
  string body = message.SerializeAsString();
  SendMessage(body.c_str());
}


void FunapiTransportBase::SendMessage(const char *body) {
  iovec body_as_bytes;
  body_as_bytes.iov_len = strlen(body);

  // EncodeThenSendMessage() 에서 LOG 출력을 하기 위하여 null 문자를 위한 + 1
  body_as_bytes.iov_base = new uint8_t[body_as_bytes.iov_len + 1];
  memcpy(body_as_bytes.iov_base, body, body_as_bytes.iov_len);

  bool sendable = false;

  pthread_mutex_lock(&mutex_);
  pending_.push_back(body_as_bytes);
  if (state_ == kConnected && sending_.size() == 0) {
    sending_.swap(pending_);
    sendable = true;
  }
  pthread_mutex_unlock(&mutex_);

  if (sendable)
      EncodeThenSendMessage();
}


// The caller must lock mutex_ before call this function.
bool FunapiTransportBase::TryToDecodeHeader() {
  LOG("Trying to decode header fields.");
  for (; next_decoding_offset_ < received_size_; ) {
    char *base = reinterpret_cast<char *>(receiving_.iov_base);
    char *ptr =
        std::search(base + next_decoding_offset_,
                    base + received_size_,
                    kHeaderDelimeter,
                    kHeaderDelimeter + sizeof(kHeaderDelimeter) - 1);

    ssize_t eol_offset = ptr - base;
    if (eol_offset >= received_size_) {
      // Not enough bytes. Wait for more bytes to come.
      LOG("We need more bytes for a header field. Waiting.");
      return false;
    }

    // Generates a null-termianted string by replacing the delimeter with \0.
    *ptr = '\0';
    char *line = base + next_decoding_offset_;
    LOG1("Header line: %s", *FString(line));

    ssize_t line_length = eol_offset - next_decoding_offset_;
    next_decoding_offset_ = eol_offset + 1;

    if (line_length == 0) {
      // End of header.
      header_decoded_ = true;
      LOG("End of header reached. Will decode body from now.");
      return true;
    }

    ptr = std::search(
        line, line + line_length, kHeaderFieldDelimeter,
        kHeaderFieldDelimeter + sizeof(kHeaderFieldDelimeter) - 1);
    assert((ptr - base) < eol_offset);

    // Generates null-terminated string by replacing the delimeter with \0.
    *ptr = '\0';
    char *e1 = line, *e2 = ptr + 1;
    while (*e2 == ' ' || *e2 == '\t') ++e2;
    LOG2("Decoded header field '%s' => '%s'", *FString(e1), *FString(e2));
    header_fields_[e1] = e2;
  }
  return false;
}


// The caller must lock mutex_ before call this function.
bool FunapiTransportBase::TryToDecodeBody() {
  // version header 읽기
  HeaderFields::const_iterator it = header_fields_.find(kVersionHeaderField);
  assert(it != header_fields_.end());
  int version = atoi(it->second.c_str());
  assert(version == kCurrentFunapiProtocolVersion);

  // length header 읽기
  it = header_fields_.find(kLengthHeaderField);
  int body_length = atoi(it->second.c_str());
  LOG2("We need %d bytes for a message body. Buffer has %d bytes.",
       body_length, (received_size_ - next_decoding_offset_));

  if (received_size_ - next_decoding_offset_ < body_length) {
    // Need more bytes.
    LOG("We need more bytes for a message body. Waiting.");
    return false;
  }

  if (body_length > 0) {
    assert(state_ == kConnected);

    if (state_ != kConnected) {
      LOG("unexpected message");
      return false;
    }

    char *base = reinterpret_cast<char *>(receiving_.iov_base);

    // Generates a null-termianted string for convenience.
    char tmp = base[next_decoding_offset_ + body_length];
    base[next_decoding_offset_ + body_length] = '\0';

    string buffer;
    buffer.assign(base + next_decoding_offset_);
    base[next_decoding_offset_ + body_length] = tmp;

    // Moves the read offset.
    next_decoding_offset_ += body_length;

    // The network module eats the fields and invokes registered handler
    LOG("Invoking a receive handler.");
    {
      std::unique_lock<std::mutex> lock(tasks_queue_mutex);
      tasks_queue.push(std::bind(on_received_, header_fields_, buffer));
    }
  }

  // Prepares for a next message.
  header_decoded_ = false;
  header_fields_.clear();
  return true;
}



////////////////////////////////////////////////////////////////////////////////
// FunapiTransportImpl implementation.

class FunapiTransportImpl : public FunapiTransportBase {
 public:
  FunapiTransportImpl(FunapiTransportType type,
                      const string &hostname_or_ip, uint16_t port);
  virtual ~FunapiTransportImpl();

  void Start();
  void Stop();
  bool Started() const;

 private:
  static void StartCbWrapper(int rc, void *arg);
  static void SendBytesCbWrapper(ssize_t rc, void *arg);
  static void ReceiveBytesCbWrapper(ssize_t nRead, void *arg);

  void StartCb(int rc);
  void SendBytesCb(ssize_t rc);
  void ReceiveBytesCb(ssize_t nRead);

  virtual void EncodeThenSendMessage();

  // State-related.
  Endpoint endpoint_;
  Endpoint recv_endpoint_;
  int sock_;
};


FunapiTransportImpl::FunapiTransportImpl(FunapiTransportType type,
                                         const string &hostname_or_ip,
                                         uint16_t port)
    : FunapiTransportBase(type), sock_(-1) {
  pthread_mutex_init(&mutex_, NULL);

  struct hostent *entry = gethostbyname(hostname_or_ip.c_str());
  assert(entry);

  struct in_addr **l = reinterpret_cast<struct in_addr **>(entry->h_addr_list);
  assert(l);
  assert(l[0]);

  if (type == kTcp) {
    endpoint_.sin_family = AF_INET;
    endpoint_.sin_addr = *l[0];
    endpoint_.sin_port = htons(port);
  } else if (type == kUdp) {
    endpoint_.sin_family = AF_INET;
    endpoint_.sin_addr = *l[0];
    endpoint_.sin_port = htons(port);

    recv_endpoint_.sin_family = AF_INET;
    recv_endpoint_.sin_addr.s_addr = htonl(INADDR_ANY);
    recv_endpoint_.sin_port = htons(port);
  }
}


FunapiTransportImpl::~FunapiTransportImpl() {
#if PLATFORM_WINDOWS
  WaitForSingleObject(mutex_, INFINITE);
  CloseHandle(mutex_);
#endif
}


void FunapiTransportImpl::Start() {
  pthread_mutex_lock(&mutex_);
  Init();

  // Initiates a new socket.
  if (type_ == kTcp) {
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    assert(sock_ >= 0);
    state_ = kConnecting;

    // connecting
    AsyncConnect(sock_, endpoint_,
                 AsyncConnectCallback(&FunapiTransportImpl::StartCbWrapper,
                 (void *) this));
  } else if (type_ == kUdp) {
    sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    assert(sock_ >= 0);
    state_ = kConnected;

    // Wait for message
    AsyncReceiveFrom(sock_, recv_endpoint_, receiving_,
                     AsyncReceiveCallback(&FunapiTransportImpl::ReceiveBytesCbWrapper,
                     (void *)this));
  }
  pthread_mutex_unlock(&mutex_);
}


void FunapiTransportImpl::Stop() {
  pthread_mutex_lock(&mutex_);
  state_ = kDisconnected;
  if (sock_ >= 0) {
#if PLATFORM_WINDOWS
    closesocket(sock_);
#else
    close(sock_);
#endif
    sock_ = -1;
  }
  pthread_mutex_unlock(&mutex_);
}


bool FunapiTransportImpl::Started() const {
  pthread_mutex_lock(&mutex_);
  bool r = (state_ == kConnected);
  pthread_mutex_unlock(&mutex_);
  return r;
}


void FunapiTransportImpl::StartCbWrapper(int rc, void *arg) {
  FunapiTransportImpl *obj = reinterpret_cast<FunapiTransportImpl *>(arg);
  obj->StartCb(rc);
}


void FunapiTransportImpl::SendBytesCbWrapper(ssize_t nSent, void *arg) {
  FunapiTransportImpl *obj = reinterpret_cast<FunapiTransportImpl *>(arg);
  obj->SendBytesCb(nSent);
}


void FunapiTransportImpl::ReceiveBytesCbWrapper(ssize_t nRead, void *arg) {
  FunapiTransportImpl *obj = reinterpret_cast<FunapiTransportImpl *>(arg);
  obj->ReceiveBytesCb(nRead);
}


void FunapiTransportImpl::StartCb(int rc) {
  LOG("StartCb called");

  if (rc != 0) {
    LOG1("Connect failed: %s", *FString(strerror(rc)));
    Stop();
    on_stopped_();
    return;
  }

  LOG("Connected.");

  // Makes a state transition.
  state_ = kConnected;

  // Start to handle incoming messages.
  AsyncReceive(
      sock_, receiving_,
      AsyncReceiveCallback(
          &FunapiTransportImpl::ReceiveBytesCbWrapper, (void *)this));
  LOG("Ready to receive");

  // Starts to process if there any data already queue.
  if (pending_.size() > 0) {
    LOG("Flushing pending messages.");
    sending_.swap(pending_);
    EncodeThenSendMessage();
  }
}


void FunapiTransportImpl::SendBytesCb(ssize_t nSent) {
  if (nSent < 0) {
    LOG1("send failed: %s", *FString(strerror(errno)));
    Stop();
    on_stopped_();
    return;
  }

  LOG1("Sent %d bytes", nSent);

  bool sendable = false;

  // Now releases memory that we have been holding for transmission.
  pthread_mutex_lock(&mutex_);
  assert(!sending_.empty());

  IoVecList::iterator itr = sending_.begin();
  delete [] reinterpret_cast<uint8_t *>(itr->iov_base);
  sending_.erase(itr);

  if (sending_.size() > 0) {
    if (type_ == kUdp) {
      AsyncSendTo(sock_, endpoint_, sending_,
          AsyncSendCallback(&FunapiTransportImpl::SendBytesCbWrapper,
          (void *) this));
    }
  } else if (pending_.size() > 0) {
    sending_.swap(pending_);
    sendable = true;
  }
  pthread_mutex_unlock(&mutex_);

  if (sendable)
      EncodeThenSendMessage();
}


void FunapiTransportImpl::ReceiveBytesCb(ssize_t nRead) {
  if (!DecodeMessage(nRead)) {
    if (nRead == 0)
      LOG1("Socket [%d] closed.", sock_);

    Stop();
    on_stopped_();
    return;
  }

  // Starts another async receive.
  pthread_mutex_lock(&mutex_);

  struct iovec residual;
  residual.iov_len = receiving_.iov_len - received_size_;
  residual.iov_base =
      reinterpret_cast<uint8_t *>(receiving_.iov_base) + received_size_;
  if (type_ == kTcp) {
    AsyncReceive(sock_, residual,
                 AsyncReceiveCallback(&FunapiTransportImpl::ReceiveBytesCbWrapper,
                 (void *) this));
  } else if (type_ == kUdp) {
    AsyncReceiveFrom(sock_, recv_endpoint_, residual,
                     AsyncReceiveCallback(&FunapiTransportImpl::ReceiveBytesCbWrapper,
                     (void *)this));
  }
  LOG1("Ready to receive more. We can receive upto %ld  more bytes.",
       (receiving_.iov_len - received_size_));
  pthread_mutex_unlock(&mutex_);
}


void FunapiTransportImpl::EncodeThenSendMessage() {
  assert(state_ == kConnected);

  if (!EncodeMessage()) {
    Stop();
    return;
  }

  assert(!sending_.empty());
  if (type_ == kTcp) {
    AsyncSend(sock_, sending_,
              AsyncSendCallback(&FunapiTransportImpl::SendBytesCbWrapper,
              (void *) this));
  } else if (type_ == kUdp) {
    AsyncSendTo(sock_, endpoint_, sending_,
                AsyncSendCallback(&FunapiTransportImpl::SendBytesCbWrapper,
                (void *) this));
  }
}



////////////////////////////////////////////////////////////////////////////////
// FunapiHttpTransportImpl implementation.

class FunapiHttpTransportImpl : public FunapiTransportBase {
 public:
  FunapiHttpTransportImpl(const string &hostname_or_ip, uint16_t port, bool https);
  virtual ~FunapiHttpTransportImpl();

  void Start();
  void Stop();
  bool Started() const;

 private:
  static void WebRequestCbWrapper(int state, void *arg);
  static void WebResponseHeaderCbWrapper(void *data, int len, void *arg);
  static void WebResponseBodyCbWrapper(void *data, int len, void *arg);

  virtual void EncodeThenSendMessage();

  void WebRequestCb(int state);
  void WebResponseHeaderCb(void *data, int len);
  void WebResponseBodyCb(void *data, int len);

  string host_url_;
  int recv_length_;
};


FunapiHttpTransportImpl::FunapiHttpTransportImpl(const string &hostname_or_ip,
                                                 uint16_t port, bool https)
    : FunapiTransportBase(kHttp), recv_length_(0) {
  pthread_mutex_init(&mutex_, NULL);
  char url[1024];
  sprintf(url, "%s://%s:%d/v%d/",
      https ? "https" : "http", hostname_or_ip.c_str(), port,
      kCurrentFunapiProtocolVersion);
  host_url_ = url;
  LOG1("Host url : %s", *FString(host_url_.c_str()));

  if (!curl_initialized)
    curl_global_init(CURL_GLOBAL_ALL);
}


FunapiHttpTransportImpl::~FunapiHttpTransportImpl() {
  if (curl_initialized) {
    curl_global_cleanup();
    curl_initialized = false;
  }

#if PLATFORM_WINDOWS
  WaitForSingleObject(mutex_, INFINITE);
  CloseHandle(mutex_);
#endif
}


void FunapiHttpTransportImpl::Start() {
  pthread_mutex_lock(&mutex_);
  Init();
  state_ = kConnected;
  recv_length_ = 0;
  LOG("Started.");
  pthread_mutex_unlock(&mutex_);
}


void FunapiHttpTransportImpl::Stop() {
  if (state_ == kDisconnected)
    return;

  pthread_mutex_lock(&mutex_);
  state_ = kDisconnected;
  LOG("Stopped.");

  // TODO : clear list

  on_stopped_();
  pthread_mutex_unlock(&mutex_);
}


bool FunapiHttpTransportImpl::Started() const {
  pthread_mutex_lock(&mutex_);
  bool r = (state_ == kConnected);
  pthread_mutex_unlock(&mutex_);
  return r;
}


void FunapiHttpTransportImpl::WebRequestCbWrapper(int state, void *arg) {
  FunapiHttpTransportImpl *obj = reinterpret_cast<FunapiHttpTransportImpl *>(arg);
  obj->WebRequestCb(state);
}


void FunapiHttpTransportImpl::WebResponseHeaderCbWrapper(void *data, int len, void *arg) {
  FunapiHttpTransportImpl *obj = reinterpret_cast<FunapiHttpTransportImpl *>(arg);
  obj->WebResponseHeaderCb(data, len);
}


void FunapiHttpTransportImpl::WebResponseBodyCbWrapper(void *data, int len, void *arg) {
  FunapiHttpTransportImpl *obj = reinterpret_cast<FunapiHttpTransportImpl *>(arg);
  obj->WebResponseBodyCb(data, len);
}


void FunapiHttpTransportImpl::EncodeThenSendMessage() {
  assert(state_ == kConnected);
  if (!EncodeMessage()) {
    Stop();
    return;
  }

  assert(!sending_.empty());

  AsyncWebRequest(host_url_.c_str(), sending_,
      AsyncWebRequestCallback(&FunapiHttpTransportImpl::WebRequestCbWrapper, (void *) this),
      AsyncWebResponseCallback(&FunapiHttpTransportImpl::WebResponseHeaderCbWrapper, (void *) this),
      AsyncWebResponseCallback(&FunapiHttpTransportImpl::WebResponseBodyCbWrapper, (void *) this));
}


void FunapiHttpTransportImpl::WebRequestCb(int state) {
  LOG1("WebRequestCb called. state: %d", state);
  if (state == kWebRequestStart) {
    recv_length_ = 0;
    return;
  } else if (state == kWebRequestEnd) {
    pthread_mutex_lock(&mutex_);
    assert(sending_.size() >= 2);

    IoVecList::iterator itr = sending_.begin();
    int index = 0;
    while (itr != sending_.end() && index < 2) {
      delete [] reinterpret_cast<uint8_t *>(itr->iov_base);
      itr = sending_.erase(itr);
      ++index;
    }

    std::stringstream version;
    version << kCurrentFunapiProtocolVersion;
    header_fields_[kVersionHeaderField] = version.str();
    std::stringstream length;
    length << recv_length_;
    header_fields_[kLengthHeaderField] = length.str();
    header_decoded_ = true;
    pthread_mutex_unlock(&mutex_);

    if (!DecodeMessage(recv_length_)) {
      Stop();
      on_stopped_();
      return;
    }

    LOG1("Ready to receive more. We can receive upto %ld more bytes.",
         (receiving_.iov_len - received_size_));
  }
}


void FunapiHttpTransportImpl::WebResponseHeaderCb(void *data, int len) {
  char buf[1024];
  memcpy(buf, data, len);
  buf[len-2] = '\0';

  char *ptr = std::search(buf, buf + len, kHeaderFieldDelimeter,
      kHeaderFieldDelimeter + sizeof(kHeaderFieldDelimeter) - 1);
  ssize_t eol_offset = ptr - buf;
  if (eol_offset >= len)
    return;

  // Generates null-terminated string by replacing the delimeter with \0.
  *ptr = '\0';
  const char *e1 = buf, *e2 = ptr + 1;
  while (*e2 == ' ' || *e2 == '\t') ++e2;
  LOG2("Decoded header field '%s' => '%s'", *FString(e1), *FString(e2));
  header_fields_[e1] = e2;
}


void FunapiHttpTransportImpl::WebResponseBodyCb(void *data, int len) {
  int offset = received_size_ + recv_length_;
  if (offset + len >= receiving_.iov_len) {
    size_t new_size = receiving_.iov_len + kUnitBufferSize;
    LOG1("Resizing a buffer to %dbytes.", new_size);
    uint8_t *new_buffer = new uint8_t[new_size + kUnitBufferPaddingSize];
    memmove(new_buffer, receiving_.iov_base, offset);
    delete [] reinterpret_cast<uint8_t *>(receiving_.iov_base);
    receiving_.iov_base = new_buffer;
    receiving_.iov_len = new_size;
  }

  uint8_t *buf = reinterpret_cast<uint8_t*>(receiving_.iov_base);
  memcpy(buf + offset, data, len);
  recv_length_ += len;
}



////////////////////////////////////////////////////////////////////////////////
// FunapiNetworkImpl implementation.

class FunapiNetworkImpl : std::enable_shared_from_this<FunapiNetworkImpl> {
 public:
  typedef FunapiTransport::HeaderType HeaderType;
  typedef FunapiNetwork::MessageHandler MessageHandler;
  typedef FunapiNetwork::OnSessionInitiated OnSessionInitiated;
  typedef FunapiNetwork::OnSessionClosed OnSessionClosed;

  FunapiNetworkImpl(FunapiTransport *transport, int type,
                    OnSessionInitiated on_session_initiated,
                    OnSessionClosed on_session_closed);

  ~FunapiNetworkImpl();

  void RegisterHandler(const std::string msg_type, const MessageHandler &handler);
  void Start();
  void Stop();
  void SendMessage(const string &msg_type, Json &body, const TransportProtocol protocol);
  void SendMessage(FunMessage& message, const TransportProtocol protocol);
  bool Started() const;
  bool Connected(const TransportProtocol protocol) const;
  void Update();
  void AttachTransport(FunapiTransport *transport);
  FunapiTransport* GetTransport(const TransportProtocol protocol) const;
  
 private:
  static void OnTransportReceivedWrapper(const HeaderType &header, const string &body, void *arg);
  static void OnTransportStoppedWrapper(void *arg);

  void OnTransportReceived(const HeaderType &header, const string &body);
  void OnTransportStopped();
  void OnNewSession(const std::string &msg_type, const std::vector<uint8_t>&v_body);
  void OnSessionTimedout(const std::string &msg_type, const std::vector<uint8_t>&v_body);

  bool started_;
  int encoding_type_;
  map<TransportProtocol, FunapiTransport*> transports_;
  OnSessionInitiated on_session_initiated_;
  OnSessionClosed on_session_closed_;
  string session_id_;
  typedef map<std::string, MessageHandler> MessageHandlerMap;
  MessageHandlerMap message_handlers_;
  time_t last_received_;
};



FunapiNetworkImpl::FunapiNetworkImpl(FunapiTransport *transport, int type,
                                     OnSessionInitiated on_session_initiated,
                                     OnSessionClosed on_session_closed)
    : started_(false), encoding_type_(type),
      on_session_initiated_(on_session_initiated),
      on_session_closed_(on_session_closed),
      session_id_(""), last_received_(0) {
  AttachTransport(transport);
}


FunapiNetworkImpl::~FunapiNetworkImpl() {
  message_handlers_.clear();

  for (auto iter : transports_)
  {
    delete iter.second;
  }
  transports_.clear();
}

void FunapiNetworkImpl::RegisterHandler(
    const string msg_type, const MessageHandler &handler) {
  LOG1("New handler for message type %s", *FString(msg_type.c_str()));
  message_handlers_[msg_type] = handler;
}


void FunapiNetworkImpl::Start() {
  // Installs event handlers.
  message_handlers_[kNewSessionMessageType] =
    [this](const std::string&s, const std::vector<uint8_t>&v) { OnNewSession(s, v); };
  message_handlers_[kSessionClosedMessageType] =
    [this](const std::string&s, const std::vector<uint8_t>&v) { OnSessionTimedout(s, v); };

  // Then, asks the transport to work.
  LOG("Starting a network module.");
  for (auto iter : transports_) {
    if (!iter.second->Started())
      iter.second->Start();
  }

  // Ok. We are ready.
  started_ = true;
  last_received_ = epoch = time(NULL);
}


void FunapiNetworkImpl::Stop() {
  LOG("Stopping a network module.");

  // Turns off the flag.
  started_ = false;

  // Then, requests the transport to stop.
  for (auto iter : transports_) {
    if (iter.second->Started())
      iter.second->Stop();
  }
}


void FunapiNetworkImpl::SendMessage(const string &msg_type, Json &body, const TransportProtocol protocol = TransportProtocol::kDefault) {  
  // Invalidates session id if it is too stale.
  time_t now = time(NULL);
  time_t delta = funapi_session_timeout;

  if (last_received_ != epoch && last_received_ + delta < now) {
    LOG("Session is too stale. The server might have invalidated my session. Resetting.");
    session_id_ = "";
  }

  // Encodes a messsage type.
  rapidjson::Value msg_type_node;
  msg_type_node = msg_type.c_str();
  body.AddMember(kMsgTypeBodyField, msg_type_node, body.GetAllocator());

  // Encodes a session id, if any.
  if (!session_id_.empty()) {
    rapidjson::Value session_id_node;
    session_id_node = session_id_.c_str();
    body.AddMember(kSessionIdBodyField, session_id_node, body.GetAllocator());
  }

  // Sends the manipulated JSON object through the transport.
  FunapiTransport* transport = GetTransport(protocol);
  if (transport) {
    transport->SendMessage(body);
  }
  else {
    LOG("Invaild Protocol - Transport is not founded");
  }
}


void FunapiNetworkImpl::SendMessage(FunMessage& message, const TransportProtocol protocol = TransportProtocol::kDefault) {
  // Invalidates session id if it is too stale.
  time_t now = time(NULL);
  time_t delta = funapi_session_timeout;

  if (last_received_ != epoch && last_received_ + delta < now) {
    LOG("Session is too stale. The server might have invalidated my session. Resetting.");
    session_id_ = "";
  }

  // Encodes a session id, if any.
  if (!session_id_.empty()) {
    message.set_sid(session_id_.c_str());
  }

  // Sends the manipulated Protobuf object through the transport.
  FunapiTransport *transport = GetTransport(protocol);
  if (transport) {
    transport->SendMessage(message);
  }
  else {
    LOG("Invaild Protocol - Transport is not founded");
  }
}


bool FunapiNetworkImpl::Started() const {
  return started_;
}


bool FunapiNetworkImpl::Connected(TransportProtocol protocol = TransportProtocol::kDefault) const {
  const FunapiTransport *transport = (const FunapiTransport*)GetTransport(protocol);

  if (transport)
    return transport->Started();

  return false;
}


void FunapiNetworkImpl::OnTransportReceivedWrapper(
    const HeaderType &header, const string &body, void *arg) {
  FunapiNetworkImpl *obj = reinterpret_cast<FunapiNetworkImpl *>(arg);
  return obj->OnTransportReceived(header, body);
}


void FunapiNetworkImpl::OnTransportStoppedWrapper(void *arg) {
  FunapiNetworkImpl *obj = reinterpret_cast<FunapiNetworkImpl *>(arg);
  return obj->OnTransportStopped();
}

void FunapiNetworkImpl::OnTransportReceived(
    const HeaderType &header, const string &body) {
  LOG("OnReceived invoked");

  last_received_ = time(NULL);

  string msg_type;
  string session_id;

  if (encoding_type_ == kJsonEncoding) {
    // Parses the given json string.
    Json json;
    json.Parse<0>(body.c_str());
    assert(json.IsObject());

    const rapidjson::Value &msg_type_node = json[kMsgTypeBodyField];
    assert(msg_type_node.IsString());
    msg_type = msg_type_node.GetString();
    json.RemoveMember(kMsgTypeBodyField);

    const rapidjson::Value &session_id_node = json[kSessionIdBodyField];
    assert(session_id_node.IsString());
    session_id = session_id_node.GetString();
    json.RemoveMember(kSessionIdBodyField);

  } else if (encoding_type_ == kProtobufEncoding) {
    FunMessage proto;
    proto.ParseFromString(body);

    msg_type = proto.msgtype();
    session_id = proto.sid();
  }

  if (session_id_.empty()) {
    session_id_ = session_id;
    LOG1("New session id: %s", *FString(session_id.c_str()));
    on_session_initiated_(session_id_);
  }

  if (session_id_ != session_id) {
    LOG2("Session id changed: %s => %s", *FString(session_id_.c_str()), *FString(session_id.c_str()));
    session_id_ = session_id;
    on_session_closed_();
    on_session_initiated_(session_id_);
  }

  MessageHandlerMap::iterator it = message_handlers_.find(msg_type);
  if (it == message_handlers_.end()) {
    LOG1("No handler for message '%s'. Ignoring.", *FString(msg_type.c_str()));
  } else {
    std::vector<uint8_t> v;
    std::copy(body.begin(), body.end(), std::back_inserter(v));
    v.push_back('\0');
    it->second(msg_type, v);
  }
}


void FunapiNetworkImpl::OnTransportStopped() {
  LOG("Transport terminated. Stopping. You may restart again.");
  Stop();
}


void FunapiNetworkImpl::OnNewSession(
  const std::string &msg_type, const std::vector<uint8_t>&v_body) {
  // ignore
}


void FunapiNetworkImpl::OnSessionTimedout(
  const std::string &msg_type, const std::vector<uint8_t>&v_body) {
  LOG("Session timed out. Resetting my session id. The server will send me another one next time.");

  session_id_ = "";
  on_session_closed_();
}

void FunapiNetworkImpl::Update() {
  // LOG("FunapiNetworkImpl::Update()");
  {
    std::unique_lock<std::mutex> lock(tasks_queue_mutex);
    while (!tasks_queue.empty())
    {
      (tasks_queue.front())();
      tasks_queue.pop();
    }
  }
}

void FunapiNetworkImpl::AttachTransport(FunapiTransport *transport) {
  transport->RegisterEventHandlers(
    FunapiTransport::OnReceived(
    &FunapiNetworkImpl::OnTransportReceivedWrapper, (void *) this),
    FunapiTransport::OnStopped(
    &FunapiNetworkImpl::OnTransportStoppedWrapper, (void *) this));

  if (GetTransport(transport->Protocol()) == nullptr)
  {
    transports_[transport->Protocol()] = transport;
  } else 
    delete transport;
}

FunapiTransport* FunapiNetworkImpl::GetTransport(const TransportProtocol protocol) const {
  if (protocol == TransportProtocol::kDefault) {
    return transports_.begin()->second;
  }

  auto iter = transports_.find(protocol);
  if (iter != transports_.end())
    return iter->second;

  return nullptr;
}

////////////////////////////////////////////////////////////////////////////////
// FunapiTcpTransport implementation.

FunapiTcpTransport::FunapiTcpTransport(
    const string &hostname_or_ip, uint16_t port)
    : impl_(new FunapiTransportImpl(kTcp, hostname_or_ip, port)) {
  if (!initialized) {
    LOG("You should call FunapiNetwork::Initialize() first.");
    assert(initialized);
  }
}


FunapiTcpTransport::~FunapiTcpTransport() {
  if (impl_) {
    delete impl_;
    impl_ = NULL;
  }
}


void FunapiTcpTransport::RegisterEventHandlers(
    const OnReceived &on_received, const OnStopped &on_stopped) {
  impl_->RegisterEventHandlers(on_received, on_stopped);
}


void FunapiTcpTransport::Start() {
  impl_->Start();
}


void FunapiTcpTransport::Stop() {
  impl_->Stop();
}


void FunapiTcpTransport::SendMessage(Json &message) {
  impl_->SendMessage(message);
}

void FunapiTcpTransport::SendMessage(FunMessage &message) {
  impl_->SendMessage(message);
}


bool FunapiTcpTransport::Started() const {
  return impl_->Started();
}

TransportProtocol FunapiTcpTransport::Protocol() const {
  return TransportProtocol::kTcp;
}


////////////////////////////////////////////////////////////////////////////////
// FunapiUdpTransport implementation.

FunapiUdpTransport::FunapiUdpTransport(
    const string &hostname_or_ip, uint16_t port)
    : impl_(new FunapiTransportImpl(kUdp, hostname_or_ip, port)) {
  if (!initialized) {
    LOG("You should call FunapiNetwork::Initialize() first.");
    assert(initialized);
  }
}


FunapiUdpTransport::~FunapiUdpTransport() {
  if (impl_) {
    delete impl_;
    impl_ = NULL;
  }
}


void FunapiUdpTransport::RegisterEventHandlers(
    const OnReceived &on_received, const OnStopped &on_stopped) {
  impl_->RegisterEventHandlers(on_received, on_stopped);
}


void FunapiUdpTransport::Start() {
  impl_->Start();
}


void FunapiUdpTransport::Stop() {
  impl_->Stop();
}


void FunapiUdpTransport::SendMessage(Json &message) {
  impl_->SendMessage(message);
}


void FunapiUdpTransport::SendMessage(FunMessage &message) {
  impl_->SendMessage(message);
}


bool FunapiUdpTransport::Started() const {
  return impl_->Started();
}

TransportProtocol FunapiUdpTransport::Protocol() const {
  return TransportProtocol::kUdp;
}


////////////////////////////////////////////////////////////////////////////////
// FunapiHttpTransport implementation.

FunapiHttpTransport::FunapiHttpTransport(
    const string &hostname_or_ip, uint16_t port, bool https /*= false*/)
    : impl_(new FunapiHttpTransportImpl(hostname_or_ip, port, https)) {
  if (!initialized) {
    LOG("You should call FunapiNetwork::Initialize() first.");
    assert(initialized);
  }
}


FunapiHttpTransport::~FunapiHttpTransport() {
  if (impl_) {
    delete impl_;
    impl_ = NULL;
  }
}


void FunapiHttpTransport::RegisterEventHandlers(
    const OnReceived &on_received, const OnStopped &on_stopped) {
  impl_->RegisterEventHandlers(on_received, on_stopped);
}


void FunapiHttpTransport::Start() {
  impl_->Start();
}


void FunapiHttpTransport::Stop() {
  impl_->Stop();
}


void FunapiHttpTransport::SendMessage(Json &message) {
  impl_->SendMessage(message);
}


void FunapiHttpTransport::SendMessage(FunMessage &message) {
  impl_->SendMessage(message);
}


bool FunapiHttpTransport::Started() const {
  return impl_->Started();
}

TransportProtocol FunapiHttpTransport::Protocol() const {
  return TransportProtocol::kHttp;
}


////////////////////////////////////////////////////////////////////////////////
// FunapiNetwork implementation.

void FunapiNetwork::Initialize(time_t session_timeout) {
  assert(!initialized);

  // Remembers the session timeout setting.
  funapi_session_timeout = session_timeout;

  // Creates a thread to handle async operations.
  async_thread_run = true;
#if PLATFORM_WINDOWS
  pthread_mutex_init(&async_queue_mutex, NULL);
  async_queue_thread = (HANDLE)_beginthreadex(NULL, 0, AsyncQueueThreadProc, NULL, 0, NULL);
  assert(async_queue_thread != 0);
#else
  int r = pthread_create(&async_queue_thread, NULL, AsyncQueueThreadProc, NULL);
  assert(r == 0);
#endif

  // Now ready.
  initialized = true;
}


void FunapiNetwork::Finalize() {
  assert(initialized);

  // Terminates the thread for async operations.
  async_thread_run = false;
#if PLATFORM_WINDOWS
  WaitForSingleObject(async_queue_thread, INFINITE);
  CloseHandle(async_queue_thread);

  WaitForSingleObject(async_queue_mutex, INFINITE);
  CloseHandle(async_queue_mutex);
#else
  pthread_cond_broadcast(&async_queue_cond);
  void *dummy;
  pthread_join(async_queue_thread, &dummy);
  (void) dummy;
#endif

  // All set.
  initialized = false;
}


FunapiNetwork::FunapiNetwork(
    FunapiTransport *transport, int type,
    const OnSessionInitiated &on_session_initiated,
    const OnSessionClosed &on_session_closed)
    : impl_(new FunapiNetworkImpl(transport, type,
        on_session_initiated, on_session_closed)) {
  // Makes sure we initialized the module.
  if (!initialized) {
    LOG("You should call FunapiNetwork::Initialize() first.");
    assert(initialized);
  }
}


FunapiNetwork::~FunapiNetwork() {
  if (impl_) {
    delete impl_;
    impl_ = NULL;
  }
}


void FunapiNetwork::RegisterHandler(
    const string msg_type, const MessageHandler &handler) {
  return impl_->RegisterHandler(msg_type, handler);
}


void FunapiNetwork::Start() {
  return impl_->Start();
}


void FunapiNetwork::Stop() {
  return impl_->Stop();
}


void FunapiNetwork::SendMessage(const string &msg_type, Json &body, const TransportProtocol protocol) {
  return impl_->SendMessage(msg_type, body, protocol);
}


void FunapiNetwork::SendMessage(FunMessage& message, const TransportProtocol protocol) {
  return impl_->SendMessage(message, protocol);
}


bool FunapiNetwork::Started() const {
  return impl_->Started();
}


bool FunapiNetwork::Connected(const TransportProtocol protocol) const {
  return impl_->Connected(protocol);
}

void FunapiNetwork::Update() {
  return impl_->Update();
}

void FunapiNetwork::AttachTransport(FunapiTransport *transport) {
  return impl_->AttachTransport(transport);
}

}  // namespace fun
