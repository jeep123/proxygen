/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <proxygen/lib/http/codec/experimental/HTTP2Codec.h>

#include <proxygen/lib/http/codec/experimental/HTTP2Constants.h>
#include <proxygen/lib/http/codec/SPDYUtil.h>

#include <folly/io/Cursor.h>
#include <folly/Random.h>

using namespace proxygen::compress;
using namespace folly::io;
using namespace folly;

using std::string;

namespace proxygen {

uint32_t HTTP2Codec::kHeaderSplitSize{http2::kMaxFramePayloadLength};

std::bitset<256> HTTP2Codec::perHopHeaderCodes_;

void HTTP2Codec::initPerHopHeaders() {
  // HTTP/1.x per-hop headers that have no meaning in HTTP/2
  perHopHeaderCodes_[HTTP_HEADER_CONNECTION] = true;
  perHopHeaderCodes_[HTTP_HEADER_HOST] = true;
  perHopHeaderCodes_[HTTP_HEADER_KEEP_ALIVE] = true;
  perHopHeaderCodes_[HTTP_HEADER_PROXY_CONNECTION] = true;
  perHopHeaderCodes_[HTTP_HEADER_TRANSFER_ENCODING] = true;
  perHopHeaderCodes_[HTTP_HEADER_UPGRADE] = true;
}

HTTP2Codec::HTTP2Codec(TransportDirection direction)
    : transportDirection_(direction),
      headerCodec_(direction),
      sessionClosing_(ClosingState::OPEN) {

  VLOG(4) << "creating " << getTransportDirectionString(direction)
          << " HTTP/2 codec";

  switch (transportDirection_) {
    case TransportDirection::DOWNSTREAM:
      nextEgressStreamID_ = 2;
      break;
    case TransportDirection::UPSTREAM:
      nextEgressStreamID_ = 1;
      break;
  }
}

HTTP2Codec::~HTTP2Codec() {}

// HTTPCodec API

bool HTTP2Codec::supportsStreamFlowControl() const {
  return true;
}

bool HTTP2Codec::supportsSessionFlowControl() const {
  return true;
}

HTTPCodec::StreamID HTTP2Codec::createStream() {
  auto ret = nextEgressStreamID_;
  nextEgressStreamID_ += 2;
  return ret;
}

bool HTTP2Codec::isBusy() const {
  return false;
}

size_t HTTP2Codec::onIngress(const folly::IOBuf& buf) {
  // TODO: ensure only 1 parse at a time on stack.

  Cursor cursor(&buf);
  size_t parsed = 0;
  ErrorCode connError = ErrorCode::NO_ERROR;
  for (auto bufLen = cursor.totalLength();
       connError == ErrorCode::NO_ERROR;
       bufLen = cursor.totalLength()) {
    if (needConnectionPreface_) {
      if (bufLen >= http2::kConnectionPreface.length()) {
        auto test = cursor.readFixedString(http2::kConnectionPreface.length());
        parsed += http2::kConnectionPreface.length();
        if (test != http2::kConnectionPreface) {
          connError = ErrorCode::PROTOCOL_ERROR;
        }
        needConnectionPreface_ = false;
      } else {
        break;
      }
    } else if (needHeader_) {
      // Waiting to parse the common frame header
      if (bufLen >= http2::kFrameHeaderSize) {
        needHeader_ = false;
        connError = parseFrameHeader(cursor, curHeader_);
        parsed += http2::kFrameHeaderSize;
        if (curHeader_.length > maxRecvFrameSize()) {
          connError = ErrorCode::FRAME_SIZE_ERROR;
        }
#ifndef NDEBUG
        receivedFrameCount_++;
#endif
      } else {
        break;
      }
    } else {
      // Already parsed the common frame header
      const auto frameLen = curHeader_.length;
      if (bufLen >= frameLen) {
        needHeader_ = true;
        connError = parseFrame(cursor);
        parsed += frameLen;
      } else {
        break;
      }
    }
  }
  checkConnectionError(connError);
  return parsed;
}

ErrorCode HTTP2Codec::parseFrame(folly::io::Cursor& cursor) {
  if (expectedContinuationStream_ != 0 &&
       (curHeader_.type != http2::FrameType::CONTINUATION ||
        expectedContinuationStream_ != curHeader_.stream)) {
    VLOG(4) << "Expected CONTINUATION with stream="
            << expectedContinuationStream_ << " got type="
            <<  getFrameTypeString(curHeader_.type)
            << " stream=" << curHeader_.stream;
    return ErrorCode::PROTOCOL_ERROR;
  }
  if ((expectedContinuationStream_ == 0 &&
       curHeader_.type == http2::FrameType::CONTINUATION)) {
    VLOG(4) << "Unexpected CONTINUATION stream=" << curHeader_.stream;
    return ErrorCode::PROTOCOL_ERROR;
  }
  ErrorCode err = ErrorCode::NO_ERROR;
  switch (curHeader_.type) {
    case http2::FrameType::DATA: err = parseData(cursor); break;
    case http2::FrameType::HEADERS: err = parseHeaders(cursor); break;
    case http2::FrameType::PRIORITY: err = parsePriority(cursor); break;
    case http2::FrameType::RST_STREAM: err = parseRstStream(cursor); break;
    case http2::FrameType::SETTINGS: err = parseSettings(cursor); break;
    case http2::FrameType::PUSH_PROMISE: err = parsePushPromise(cursor); break;
    case http2::FrameType::PING: err = parsePing(cursor); break;
    case http2::FrameType::GOAWAY: err = parseGoaway(cursor); break;
    case http2::FrameType::WINDOW_UPDATE: err = parseWindowUpdate(cursor);break;
    case http2::FrameType::CONTINUATION: err = parseContinuation(cursor); break;
    case http2::FrameType::ALTSVC:
      // fall through, unimplemented
    default:
      // Implementations MUST ignore and discard any frame that has a
      // type that is unknown
      VLOG(2) << "Skipping unknown frame type=" << (uint8_t)curHeader_.type;
      cursor.skip(curHeader_.length);
  }
  expectedContinuationStream_ =
    (frameAffectsCompression(curHeader_.type) &&
     !(curHeader_.flags & http2::END_HEADERS)) ? curHeader_.stream : 0;
  return err;
}

ErrorCode HTTP2Codec::handleEndStream() {
  // do we need to handle case where this stream has already aborted via
  // another callback (onHeadersComplete/onBody)?
  if (curHeader_.flags & http2::END_STREAM && callback_) {
    callback_->onMessageComplete(StreamID(curHeader_.stream), false);
  }
  return ErrorCode::NO_ERROR;
}

ErrorCode HTTP2Codec::parseData(Cursor& cursor) {
  std::unique_ptr<IOBuf> outData;
  VLOG(10) << "parsing DATA frame for stream=" << curHeader_.stream;
  auto ret = http2::parseData(cursor, curHeader_, outData);
  RETURN_IF_ERROR(ret);

  if (callback_) {
    callback_->onBody(StreamID(curHeader_.stream), std::move(outData));
  }
  return handleEndStream();
}

ErrorCode HTTP2Codec::parseHeaders(Cursor& cursor) {
  http2::PriorityUpdate priority;
  std::unique_ptr<IOBuf> headerBuf;
  VLOG(4) << "parsing HEADERS frame for stream=" << curHeader_.stream;
  auto err = http2::parseHeaders(cursor, curHeader_, priority, headerBuf);
  RETURN_IF_ERROR(err);
  if (transportDirection_ == TransportDirection::DOWNSTREAM) {
    RETURN_IF_ERROR(checkNewStream(curHeader_.stream));
  } else if ((curHeader_.stream & 0x1) == 0) {
    VLOG(4) << "Invalid HEADERS(reply) stream=" << curHeader_.stream;
    return ErrorCode::PROTOCOL_ERROR;
  }
  if (sessionClosing_ == ClosingState::CLOSED) {
    VLOG(4) << "Dropping HEADERS after final GOAWAY, stream="
            << curHeader_.stream;
    return ErrorCode::NO_ERROR;
  }
  err = parseHeadersImpl(cursor, std::move(headerBuf),
                         priority, boost::none);
  return err;
}

ErrorCode HTTP2Codec::parseContinuation(Cursor& cursor) {
  std::unique_ptr<IOBuf> headerBuf;
  VLOG(4) << "parsing CONTINUATION frame for stream=" << curHeader_.stream;
  auto err = http2::parseContinuation(cursor, curHeader_, headerBuf);
  RETURN_IF_ERROR(err);
  err = parseHeadersImpl(cursor, std::move(headerBuf),
                         boost::none, boost::none);
  return err;
}

ErrorCode HTTP2Codec::parseHeadersImpl(
  Cursor& cursor,
  std::unique_ptr<IOBuf> headerBuf,
  boost::optional<http2::PriorityUpdate> priority,
  boost::optional<uint32_t> promisedStream) {
  curHeaderBlock_.append(std::move(headerBuf));

  std::unique_ptr<HTTPMessage> msg;
  if (curHeader_.flags & http2::END_HEADERS) {
    // decompress headers
    Cursor headerCursor(curHeaderBlock_.front());
    auto result = headerCodec_.decode(headerCursor,
                                      curHeaderBlock_.chainLength());
    curHeaderBlock_.move();
    if (result.isError()) {
      VLOG(4) << "Failed decoding header block for stream="
              << curHeader_.stream;
      return ErrorCode::COMPRESSION_ERROR;
    }
    // Parse headers
    bool isRequest = (transportDirection_ == TransportDirection::DOWNSTREAM ||
                      promisedStream);
    auto parseResult = parseHeaderList(result.ok().headers, isRequest);
    if (parseResult.isError()) {
      HTTPException err(HTTPException::Direction::INGRESS,
                        "HTTP2Codec stream error: stream=",
                        curHeader_.stream, " status=", 400,
                        " error: ", parseResult.error());
      err.setHttpStatusCode(400);
      callback_->onError(curHeader_.stream, err, true);
      return ErrorCode::NO_ERROR;
    } else {
      msg = std::move(parseResult.ok());
    }
  } else {
    curHeaderBlock_.append(std::move(headerBuf));
  }

  // Report back what we've parsed
  if (callback_) {
    if (curHeader_.type == http2::FrameType::HEADERS) {
      if (curHeader_.flags & http2::PRIORITY) {
        DCHECK(priority);
        // callback_->onPriority(priority.get());
      }

      // callback checks total number of streams is smaller than settings max
      callback_->onMessageBegin(curHeader_.stream, msg.get());
    } else if (curHeader_.type == http2::FrameType::PUSH_PROMISE) {
      DCHECK(promisedStream);
      callback_->onPushMessageBegin(*promisedStream, curHeader_.stream,
                                    msg.get());
    }
    if (curHeader_.flags & http2::END_HEADERS && msg) {
      callback_->onHeadersComplete(curHeader_.stream, std::move(msg));
    }
    return handleEndStream();
  }
  return ErrorCode::NO_ERROR;
}

class HTTPRequestVerifier {
 public:
  explicit HTTPRequestVerifier(HTTPMessage& msg)
      :msg_(msg) {}

  bool setMethod(StringPiece method) {
    if (hasMethod_) {
      error = "Duplicate method";
      return false;
    }
    if (!SPDYUtil::validateMethod(method)) {
      error = "Invalid method";
      return false;
    }
    hasMethod_ = true;
    msg_.setMethod(method);
    return true;
  }

  bool setPath(StringPiece path) {
    if (hasPath_) {
      error = "Duplicate path";
      return false;
    }
    if (!SPDYUtil::validateURL(path)) {
      error = "Invalid url";
      return false;
    }
    hasPath_ = true;
    msg_.setURL(path.str());
    return true;
  }

  bool setScheme(StringPiece scheme) {
    if (hasScheme_) {
      error = "Duplicate scheme";
      return false;
    }
    // This just checks for alpha chars
    if (!SPDYUtil::validateMethod(scheme)) {
      error = "Invalid scheme";
      return false;
    }
    hasScheme_ = true;
    // TODO support non http/https schemes
    if (scheme == http2::kHttps) {
      msg_.setSecure(true);
    }
    return true;
  }

  bool setAuthority(StringPiece authority) {
    if (hasAuthority_) {
      error = "Duplicate authority";
      return false;
    }
    if (!SPDYUtil::validateHeaderValue(authority, SPDYUtil::STRICT)) {
      error = "Invalid authority";
      return false;
    }
    hasAuthority_ = true;
    msg_.getHeaders().add(HTTP_HEADER_HOST, authority.str());
    return true;
  }

  bool validate() {
    if (error.size()) {
      return false;
    }
    if (msg_.getMethod() == HTTPMethod::CONNECT &&
        (!hasMethod_ || !hasAuthority_ || hasScheme_ || hasPath_)) {
      error = folly::to<string>("Malformed CONNECT request m/a/s/p=",
                                hasMethod_, hasAuthority_,
                                hasScheme_, hasPath_);
    } else if (!hasMethod_ || !hasScheme_ || !hasPath_) {
      error = folly::to<string>("Malformed request m/a/s/p=",
                                hasMethod_, hasAuthority_,
                                hasScheme_, hasPath_);
    }
    return error.empty();
  }

  string error;

 private:
  HTTPMessage& msg_;
  bool hasMethod_{false};
  bool hasPath_{false};
  bool hasScheme_{false};
  bool hasAuthority_{false};
};


Result<std::unique_ptr<HTTPMessage>, string>
HTTP2Codec::parseHeaderList(const HeaderPieceList& list, bool isRequest) {
  auto msg = folly::make_unique<HTTPMessage>();
  HTTPHeaders& headers = msg->getHeaders();
  HTTPRequestVerifier verifier(*msg);
  bool hasStatus = false;
  bool regularHeaderSeen = false;

  for (unsigned i = 0; i < list.size(); i += 2) {
    const auto& name = list[i];
    const auto& value = list[i+1];

    VLOG(4) << "processing header name=" << name.str
            << " value=" << value.str;
    if (name.str.startsWith(':')) {
      if (regularHeaderSeen) {
        return folly::to<string>("Illegal pseudo header name=", name.str);
      }
      if (isRequest) {
        if (name.str == http2::kMethod) {
          if (!verifier.setMethod(value.str)) {
            break;
          }
        } else if (name.str == http2::kScheme) {
          if (!verifier.setScheme(value.str)) {
            break;
          }
        } else if (name.str == http2::kAuthority) {
          if (!verifier.setAuthority(value.str)) {
            break;
          }
        } else if (name.str == http2::kPath) {
          if (!verifier.setPath(value.str)) {
            break;
          }
        } else {
          return folly::to<string>("Invalid header name=", name.str);
        }
      } else {
        if (name.str == http2::kStatus) {
          if (hasStatus) {
            return string("Duplicate status");
          }
          hasStatus = true;
          int32_t code = -1;
          try {
            code = folly::to<unsigned int>(value.str);
          } catch (const std::range_error& ex) {
          }
          if (code >= 100 && code <= 999) {
            msg->setStatusCode(code);
            msg->setStatusMessage(HTTPMessage::getDefaultReason(code));
          } else {
            return folly::to<string>("Malformed status code=", value.str);
          }
        } else {
          return folly::to<string>("Invalid header name=", name.str);
        }
      }
    } else {
      regularHeaderSeen = true;
      if (name.str == "connection") {
        return string("HTTP/2 Message with Connection header");
      }
      bool nameOk = SPDYUtil::validateHeaderName(name.str);
      bool valueOk = SPDYUtil::validateHeaderValue(value.str, SPDYUtil::STRICT);
      headers.add(name.str, value.str);
      if (!nameOk || !valueOk) {
        return string("Bad header value");
      }
    }
  }
  if (isRequest) {
    auto combinedCookie = headers.combine(HTTP_HEADER_COOKIE, "; ");
    if (!combinedCookie.empty()) {
      headers.set(HTTP_HEADER_COOKIE, combinedCookie);
    }
    verifier.validate();
  } else if (!hasStatus) {
    return string("Malformed response, missing :status");
  }
  if (!verifier.error.empty()) {
    return verifier.error;
  }
  return std::move(msg);
}

ErrorCode HTTP2Codec::parsePriority(Cursor& cursor) {
  // will implement callbacks in subsequent diff
  VLOG(4) << "parsing PRIORITY frame for stream=" << curHeader_.stream;
  http2::PriorityUpdate pri;
  return http2::parsePriority(cursor, curHeader_, pri);
}

ErrorCode HTTP2Codec::parseRstStream(Cursor& cursor) {
  // rst for stream in idle state - protocol error
  VLOG(4) << "parsing RST_STREAM frame for stream=" << curHeader_.stream;
  ErrorCode statusCode = ErrorCode::NO_ERROR;
  auto err = http2::parseRstStream(cursor, curHeader_, statusCode);
  RETURN_IF_ERROR(err);
  if (callback_) {
    callback_->onAbort(curHeader_.stream, statusCode);
  }
  return ErrorCode::NO_ERROR;
}

ErrorCode HTTP2Codec::parseSettings(Cursor& cursor) {
  VLOG(4) << "parsing SETTINGS frame for stream=" << curHeader_.stream;
  std::deque<SettingPair> settings;
  auto err = http2::parseSettings(cursor, curHeader_, settings);
  RETURN_IF_ERROR(err);
  SettingsList settingsList;
  if (curHeader_.flags & http2::ACK) {
    // for stats
    if (callback_) {
      callback_->onSettingsAck();
    }
    return ErrorCode::NO_ERROR;
  }
  for (auto& setting: settings) {
    switch (setting.first) {
      case SettingsId::HEADER_TABLE_SIZE:
        // We could enforce an internal max rather than taking the max they
        // give us.
        VLOG(4) << "Setting header codec table size=" << setting.second;
        headerCodec_.setEncoderHeaderTableSize(setting.second);
        break;
      case SettingsId::ENABLE_PUSH:
        if (setting.second != 0 && setting.second != 1) {
          VLOG(4) << "Invalid ENABLE_PUSH setting=" << setting.second;
          return ErrorCode::PROTOCOL_ERROR;
        }
        break;
      case SettingsId::MAX_CONCURRENT_STREAMS:
        break;
      case SettingsId::INITIAL_WINDOW_SIZE:
        if (setting.second > http2::kMaxWindowUpdateSize) {
          VLOG(4) << "Invalid INITIAL_WINDOW_SIZE size=" << setting.second;
          return ErrorCode::PROTOCOL_ERROR;
        }
        break;
      case SettingsId::MAX_FRAME_SIZE:
        if (setting.second < http2::kMaxFramePayloadLengthMin ||
            setting.second > http2::kMaxFramePayloadLength) {
          VLOG(4) << "Invalid MAX_FRAME_SIZE size=" << setting.second;
          return ErrorCode::PROTOCOL_ERROR;
        }
        break;
      case SettingsId::MAX_HEADER_LIST_SIZE:
        break;
      default:
        // unknown setting
        break;
    }
    ingressSettings_.setSetting(setting.first, setting.second);
    settingsList.push_back(*ingressSettings_.getSetting(setting.first));
  }
  if (callback_) {
    callback_->onSettings(settingsList);
  }
  return ErrorCode::NO_ERROR;
}

ErrorCode HTTP2Codec::parsePushPromise(Cursor& cursor) {
  // stream id must be idle - protocol error
  // assoc-stream-id=closed/unknown - protocol error, unless rst_stream sent

  /*
   * What does "must handle" mean in the following context?  I have to
   * accept this as a valid pushed resource?

    However, an endpoint that has sent RST_STREAM on the associated
    stream MUST handle PUSH_PROMISE frames that might have been
    created before the RST_STREAM frame is received and processed.
  */
  if (transportDirection_ != TransportDirection::UPSTREAM) {
    VLOG(2) << "Received PUSH_PROMISE on DOWNSTREAM codec";
    return ErrorCode::PROTOCOL_ERROR;
  }
  if (egressSettings_.getSetting(SettingsId::ENABLE_PUSH, -1) != 1) {
    VLOG(2) << "Received PUSH_PROMISE on codec with push disabled";
    return ErrorCode::PROTOCOL_ERROR;
  }
  VLOG(4) << "parsing PUSH_PROMISE frame for stream=" << curHeader_.stream;
  uint32_t promisedStream;
  std::unique_ptr<IOBuf> headerBlockFragment;
  auto err = http2::parsePushPromise(cursor, curHeader_, promisedStream,
                                     headerBlockFragment);
  RETURN_IF_ERROR(err);
  RETURN_IF_ERROR(checkNewStream(promisedStream));
  if (sessionClosing_ == ClosingState::CLOSED) {
    VLOG(4) << "Dropping PUSH_PROMISE after final GOAWAY, stream=" <<
      curHeader_.stream;
    return ErrorCode::NO_ERROR;
  }
  err = parseHeadersImpl(cursor, std::move(headerBlockFragment), boost::none,
                         promisedStream);
  return err;
}

ErrorCode HTTP2Codec::parsePing(Cursor& cursor) {
  VLOG(4) << "parsing PING frame=";
  uint64_t opaqueData = 0;
  auto err = http2::parsePing(cursor, curHeader_, opaqueData);
  RETURN_IF_ERROR(err);
  if (callback_) {
    if (curHeader_.flags & http2::ACK) {
      callback_->onPingReply(opaqueData);
    } else {
      callback_->onPingRequest(opaqueData);
    }
  }
  return ErrorCode::NO_ERROR;
}

ErrorCode HTTP2Codec::parseGoaway(Cursor& cursor) {
  VLOG(4) << "parsing GOAWAY frame";
  uint32_t lastGoodStream = 0;
  ErrorCode statusCode = ErrorCode::NO_ERROR;
  std::unique_ptr<IOBuf> debugData;

  auto err = http2::parseGoaway(cursor, curHeader_, lastGoodStream, statusCode,
                                debugData);
  RETURN_IF_ERROR(err);
  if (lastGoodStream < ingressGoawayAck_) {
    ingressGoawayAck_ = lastGoodStream;
    // Drain all streams <= lastGoodStream
    // and abort streams > lastGoodStream
    if (callback_) {
      callback_->onGoaway(lastGoodStream, statusCode);
    }
  } else {
    LOG(WARNING) << "Received multiple GOAWAY with increasing ack";
  }
  return ErrorCode::NO_ERROR;
}

ErrorCode HTTP2Codec::parseWindowUpdate(Cursor& cursor) {
  VLOG(4) << "parsing WINDOW_UPDATE frame for stream=" << curHeader_.stream;
  uint32_t delta = 0;
  auto err = http2::parseWindowUpdate(cursor, curHeader_, delta);
  RETURN_IF_ERROR(err);
  if (delta == 0) {
    VLOG(4) << "Invalid 0 length delta for stream=" << curHeader_.stream;
    if (curHeader_.stream == 0) {
      return ErrorCode::PROTOCOL_ERROR;
    } else {
      // todo rst stream/protocol error
      return ErrorCode::NO_ERROR;
    }
  }
  if (callback_) {
    // if window exceeds 2^31-1, connection/stream error flow control error
    // must be checked in session/txn
    callback_->onWindowUpdate(curHeader_.stream, delta);
  }
  return ErrorCode::NO_ERROR;
}

ErrorCode HTTP2Codec::checkNewStream(uint32_t streamId) {
  if (streamId == 0 || streamId < lastStreamID_) {
    VLOG(4) << "Invalid new stream=" << streamId;
    return ErrorCode::PROTOCOL_ERROR;
  }
  bool odd = streamId & 0x01;
  bool push = (transportDirection_ == TransportDirection::UPSTREAM);
  lastStreamID_ = curHeader_.stream;

  if ((odd && push) || (!odd && !push)) {
    VLOG(4) << "Invalid new stream=" << streamId;
    return ErrorCode::PROTOCOL_ERROR;
  } else {
    return ErrorCode::NO_ERROR;
  }
}

bool HTTP2Codec::isReusable() const {
  return (sessionClosing_ == ClosingState::OPEN ||
          (transportDirection_ == TransportDirection::DOWNSTREAM &&
           isWaitingToDrain()))
    && (ingressGoawayAck_ == std::numeric_limits<uint32_t>::max());
}

bool HTTP2Codec::isWaitingToDrain() const {
  return sessionClosing_ == ClosingState::FIRST_GOAWAY_SENT;
}

size_t HTTP2Codec::generateConnectionPreface(folly::IOBufQueue& writeBuf) {
  writeBuf.append(http2::kConnectionPreface);
  return http2::kConnectionPreface.length();
}

void HTTP2Codec::generateHeader(folly::IOBufQueue& writeBuf,
                                StreamID stream,
                                const HTTPMessage& msg,
                                StreamID assocStream,
                                HTTPHeaderSize* size) {
  VLOG(4) << "generating " << ((assocStream != 0) ? "PUSH_PROMISE" : "HEADERS")
          << " for stream=" << stream;
  std::vector<Header> allHeaders;

  // The role of this local status string is to hold the generated
  // status code long enough for the header encoding.
  std::string status;

  if (msg.isRequest()) {
    DCHECK(transportDirection_ == TransportDirection::UPSTREAM ||
           assocStream != 0);
    const string& method = msg.getMethodString();
    const string& scheme = (msg.isSecure() ? http2::kHttps : http2::kHttp);
    const string& path = msg.getURL();
    const HTTPHeaders& headers = msg.getHeaders();
    const string& host = headers.getSingleOrEmpty(HTTP_HEADER_HOST);
    allHeaders.emplace_back(http2::kMethod, method);
    allHeaders.emplace_back(http2::kScheme, scheme);
    allHeaders.emplace_back(http2::kPath, path);
    if (!host.empty()) {
      allHeaders.emplace_back(http2::kAuthority, host);
    }
  } else {
    DCHECK(transportDirection_ == TransportDirection::DOWNSTREAM);
    status = folly::to<string>(msg.getStatusCode());
    allHeaders.emplace_back(http2::kStatus, status);
    // HEADERS frames do not include a version or reason string.
  }

  // Add the HTTP headers supplied by the caller, but skip
  // any per-hop headers that aren't supported in HTTP/2.
  msg.getHeaders().forEachWithCode(
    [&] (HTTPHeaderCode code,
         const string& name,
         const string& value) {

      if (perHopHeaderCodes_[code] || name.size() == 0 || name[0] == ':') {
        DCHECK(name.size() > 0) << "Empty header";
        DCHECK(name[0] != ':') << "Invalid header=" << name;
        return;
      }
      // Note this code will not drop headers named by Connection.  That's the
      // caller's job

      // see HTTP/2 spec, 8.1.2
      DCHECK(name != "TE" || value == "trailers");
      if ((name.size() > 0 && name[0] != ':') &&
          code != HTTP_HEADER_HOST) {
        allHeaders.emplace_back(code, name, value);
      }
    });

  headerCodec_.setEncodeHeadroom(http2::kFrameHeadersBaseMaxSize);
  auto out = headerCodec_.encode(allHeaders);
  if (size) {
    *size = headerCodec_.getEncodedSize();
  }

  IOBufQueue queue(IOBufQueue::cacheChainLength());
  queue.append(std::move(out));
  if (queue.chainLength() > 0) {

    auto chunk = queue.split(std::min(size_t(kHeaderSplitSize),
                                      queue.chainLength()));

    bool endHeaders = queue.chainLength() == 0;
    if (assocStream == 0) {
      http2::writeHeaders(writeBuf,
                          std::move(chunk),
                          stream,
                          boost::none,
                          http2::kNoPadding,
                          false, // TODO optimize including EOM in headers
                          endHeaders);
    } else {
      DCHECK(transportDirection_ == TransportDirection::DOWNSTREAM);
      http2::writePushPromise(writeBuf,
                              assocStream,
                              stream,
                              std::move(chunk),
                              http2::kNoPadding,
                              endHeaders);
    }

    while (!endHeaders) {
      chunk = queue.split(std::min(size_t(kHeaderSplitSize),
                                   queue.chainLength()));
      endHeaders = queue.chainLength() == 0;
      VLOG(4) << "generating CONTINUATION for stream=" << stream;
      http2::writeContinuation(writeBuf,
                               stream,
                               endHeaders,
                               std::move(chunk),
                               http2::kNoPadding);
    }
  }
}

size_t HTTP2Codec::generateBody(folly::IOBufQueue& writeBuf,
                                StreamID stream,
                                std::unique_ptr<folly::IOBuf> chain,
                                bool eom) {
  // todo: generate random padding for everything?
  size_t written = 0;

  IOBufQueue queue(IOBufQueue::cacheChainLength());
  queue.append(std::move(chain));
  while (queue.chainLength() > maxSendFrameSize()) {
    auto chunk = queue.split(maxSendFrameSize());
    written += http2::writeData(writeBuf, std::move(chunk), stream,
                                http2::kNoPadding, false);
  }

  return written + http2::writeData(writeBuf, queue.move(), stream,
                                    http2::kNoPadding, eom);
}

size_t HTTP2Codec::generateChunkHeader(folly::IOBufQueue& writeBuf,
                                       StreamID stream,
                                       size_t length) {
  // HTTP/2 has no chunk headers
  return 0;
}

size_t HTTP2Codec::generateChunkTerminator(folly::IOBufQueue& writeBuf,
                                           StreamID stream) {
  // HTTP/2 has no chunk terminators
  return 0;
}

size_t HTTP2Codec::generateTrailers(folly::IOBufQueue& writeBuf,
                                    StreamID stream,
                                    const HTTPHeaders& trailers) {
  return 0;
}

size_t HTTP2Codec::generateEOM(folly::IOBufQueue& writeBuf,
                               StreamID stream) {
  VLOG(4) << "sending EOM for stream=" << stream;
  return http2::writeData(writeBuf, nullptr, stream, http2::kNoPadding, true);
}

size_t HTTP2Codec::generateRstStream(folly::IOBufQueue& writeBuf,
                                     StreamID stream,
                                     ErrorCode statusCode) {
  VLOG(4) << "sending RST_STREAM for stream=" << stream
          << " with code=" << getErrorCodeString(statusCode);
  return http2::writeRstStream(writeBuf, stream, statusCode);
}

size_t HTTP2Codec::generateGoaway(folly::IOBufQueue& writeBuf,
                                  StreamID lastStream,
                                  ErrorCode statusCode) {
#ifndef NDEBUG
  CHECK(lastStream <= egressGoawayAck_) << "Cannot increase last good stream";
  egressGoawayAck_ = lastStream;
#endif
  if (sessionClosing_ == ClosingState::CLOSED) {
    VLOG(4) << "Not sending GOAWAY for closed session";
    return 0;
  }
  switch (sessionClosing_) {
    case ClosingState::OPEN:
      if (lastStream == std::numeric_limits<int32_t>::max() &&
          statusCode == ErrorCode::NO_ERROR) {
        sessionClosing_ = ClosingState::FIRST_GOAWAY_SENT;
      } else {
        // The user of this codec decided not to do the double goaway
        // drain, or this is not a graceful shutdown
        sessionClosing_ = ClosingState::CLOSED;
      }
      break;
    case ClosingState::FIRST_GOAWAY_SENT:
      sessionClosing_ = ClosingState::CLOSED;
      break;
    case ClosingState::CLOSED:
      LOG(FATAL) << "unreachable";
  }

  VLOG(4) << "Sending GOAWAY with last acknowledged stream="
          << lastStream << " with code=" << getErrorCodeString(statusCode);

  return http2::writeGoaway(writeBuf, lastStream, statusCode, nullptr);
}

size_t HTTP2Codec::generatePingRequest(folly::IOBufQueue& writeBuf) {
  // should probably let the caller specify when integrating with session
  // we know HTTPSession sets up events to track ping latency
  uint64_t opaqueData = folly::Random::rand64();
  VLOG(4) << "Generating ping request with opaqueData=" << opaqueData;
  return http2::writePing(writeBuf, opaqueData, false /* no ack */);
}

size_t HTTP2Codec::generatePingReply(folly::IOBufQueue& writeBuf,
                                     uint64_t uniqueID) {
  VLOG(4) << "Generating ping reply with opaqueData=" << uniqueID;
  return http2::writePing(writeBuf, uniqueID, true /* ack */);
}

size_t HTTP2Codec::generateSettings(folly::IOBufQueue& writeBuf) {
  std::deque<SettingPair> settings;
  for (auto& setting: egressSettings_.getAllSettings()) {
    if (setting.isSet) {
      settings.push_back(SettingPair(setting.id, setting.value));
      if (setting.id == SettingsId::HEADER_TABLE_SIZE) {
        headerCodec_.setDecoderHeaderTableMaxSize(setting.value);
      }
    }
  }
  VLOG(4) << "generating " << (unsigned)settings.size() << " settings";
  return http2::writeSettings(writeBuf, settings);
}

size_t HTTP2Codec::generateSettingsAck(folly::IOBufQueue& writeBuf) {
  VLOG(4) << "generating settings ack";
  return http2::writeSettingsAck(writeBuf);
}

size_t HTTP2Codec::generateWindowUpdate(folly::IOBufQueue& writeBuf,
                                        StreamID stream,
                                        uint32_t delta) {
  VLOG(4) << "generating window update for stream=" << stream
          << ": Processed " << delta << " bytes";
  return http2::writeWindowUpdate(writeBuf, stream, delta);
}

bool HTTP2Codec::checkConnectionError(ErrorCode err) {
  if (err != ErrorCode::NO_ERROR) {
    if (callback_) {
      HTTPException ex(HTTPException::Direction::INGRESS_AND_EGRESS,
                       "Connection error");
      ex.setCodecStatusCode(err);
      callback_->onError(0, ex, false);
    }
    return true;
  }
  return false;
}

}
