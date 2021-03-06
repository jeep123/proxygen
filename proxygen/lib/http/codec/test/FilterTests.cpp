/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <gtest/gtest.h>
#include <limits>
#include <proxygen/lib/http/HTTPHeaderSize.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <proxygen/lib/http/codec/FlowControlFilter.h>
#include <proxygen/lib/http/codec/HTTPChecks.h>
#include <proxygen/lib/http/codec/SPDYConstants.h>
#include <proxygen/lib/http/codec/test/MockHTTPCodec.h>
#include <proxygen/lib/http/codec/test/TestUtils.h>
#include <random>

using namespace proxygen;
using namespace std;
using namespace testing;

class MockFlowControlCallback: public FlowControlFilter::Callback {
 public:
  MOCK_METHOD0(onConnectionSendWindowOpen, void());
};

class FilterTest : public testing::Test {
 public:
  FilterTest():
      codec_(new MockHTTPCodec()),
      chain_(unique_ptr<HTTPCodec>(codec_)) {
    EXPECT_CALL(*codec_, setCallback(_))
      .WillRepeatedly(SaveArg<0>(&callbackStart_));
    chain_.setCallback(&callback_);
  }
 protected:

  MockHTTPCodec* codec_;
  HTTPCodec::Callback* callbackStart_;
  HTTPCodecFilterChain chain_;
  MockHTTPCodecCallback callback_;
  folly::IOBufQueue writeBuf_{folly::IOBufQueue::cacheChainLength()};
};

class HTTPChecksTest: public FilterTest {
 public:
  void SetUp() override {
    chain_.add<HTTPChecks>();
  }
};

template <int initSize>
class FlowControlFilterTest: public FilterTest {
 public:
  void SetUp() override {
    if (initSize > spdy::kInitialWindow) {
      // If the initial size is bigger than the default, a window update
      // will immediately be generated
      EXPECT_CALL(*codec_, generateWindowUpdate(_, 0, initSize -
                                                spdy::kInitialWindow))
        .WillOnce(InvokeWithoutArgs([this] () {
              writeBuf_.append(makeBuf(10));
              return 10;
            }));
    }
    EXPECT_CALL(*codec_, generateBody(_, _, _, _))
      .WillRepeatedly(Invoke([] (folly::IOBufQueue& writeBuf,
                                 HTTPCodec::StreamID stream,
                                 std::shared_ptr<folly::IOBuf> chain,
                                 bool eom) {
          auto len = chain->computeChainDataLength() + 4;
          writeBuf.append(makeBuf(len));
          return len;
      }));
    EXPECT_CALL(*codec_, isReusable()).WillRepeatedly(Return(true));

    // Construct flow control filter with capacity of 0, which will be
    // overridden to 65536, which is the minimum
    filter_ = new FlowControlFilter(flowCallback_, writeBuf_, codec_, initSize);
    chain_.addFilters(std::unique_ptr<FlowControlFilter>(filter_));
  }
  StrictMock<MockFlowControlCallback> flowCallback_;
  FlowControlFilter* filter_;
  int recvWindow_{initSize};
};

typedef FlowControlFilterTest<0> DefaultFlowControl;
typedef FlowControlFilterTest<1000000> BigWindow;

MATCHER(IsFlowException, "") {
  return arg->hasCodecStatusCode() &&
    arg->getCodecStatusCode() == ErrorCode::FLOW_CONTROL_ERROR &&
    !arg->hasHttpStatusCode() &&
    !arg->hasProxygenError();
}

TEST_F(DefaultFlowControl, flow_control_construct) {
  // Constructing the filter with a low capacity defaults to spdy's
  // initial capacity, so no window update should have been generated in
  // the constructor
  InSequence enforceSequence;
  ASSERT_EQ(writeBuf_.chainLength(), 0);

  // Our send window is limited to spdy::kInitialWindow
  chain_->generateBody(writeBuf_, 1,
                       makeBuf(spdy::kInitialWindow - 1), false);

  // the window isn't full yet, so getting a window update shouldn't give a
  // callback informing us that it is open again
  callbackStart_->onWindowUpdate(0, 1);

  // Now fill the window (2 more bytes)
  chain_->generateBody(writeBuf_, 1, makeBuf(2), false);
  // get the callback informing the window is open once we get a window update
  EXPECT_CALL(flowCallback_, onConnectionSendWindowOpen());
  callbackStart_->onWindowUpdate(0, 1);

  // Overflowing the window is fatal. Write 2 bytes (only 1 byte left in window)
  EXPECT_DEATH_NO_CORE(chain_->generateBody(writeBuf_, 1, makeBuf(2), false),
                       ".*");
}

TEST_F(DefaultFlowControl, send_update) {
  // Make sure we send a window update when the window decreases below half
  InSequence enforceSequence;
  EXPECT_CALL(callback_, onBody(_, _))
    .WillRepeatedly(Return());

  // Have half the window outstanding
  callbackStart_->onBody(1, makeBuf(spdy::kInitialWindow / 2 + 1));
  filter_->ingressBytesProcessed(writeBuf_, spdy::kInitialWindow / 2);

  // It should wait until the "+1" is ack'd to generate the coallesced update
  EXPECT_CALL(*codec_,
              generateWindowUpdate(_, 0, spdy::kInitialWindow / 2 + 1));
  filter_->ingressBytesProcessed(writeBuf_, 1);
}

TEST_F(BigWindow, recv_too_much) {
  // Constructing the filter with a large capacity causes a WINDOW_UPDATE
  // for stream zero to be generated
  ASSERT_GT(writeBuf_.chainLength(), 0);

  InSequence enforceSequence;
  EXPECT_CALL(callback_, onBody(_, _));
  EXPECT_CALL(callback_, onError(0, IsFlowException(), _));

  // Receive the max amount advertised
  callbackStart_->onBody(1, makeBuf(recvWindow_));
  ASSERT_TRUE(chain_->isReusable());
  // Receive 1 byte too much
  callbackStart_->onBody(1, makeBuf(1));
  ASSERT_FALSE(chain_->isReusable());
}

TEST_F(BigWindow, remote_increase) {
  // The remote side sends us a window update for stream=0, increasing our
  // available window
  InSequence enforceSequence;

  ASSERT_EQ(filter_->getAvailableSend(), spdy::kInitialWindow);
  callbackStart_->onWindowUpdate(0, 10);
  ASSERT_EQ(filter_->getAvailableSend(), spdy::kInitialWindow + 10);

  chain_->generateBody(writeBuf_, 1,
                       makeBuf(spdy::kInitialWindow + 10), false);
  ASSERT_EQ(filter_->getAvailableSend(), 0);

  // Now the remote side sends a HUGE update (just barely legal)
  // Since the window was full, this generates a callback from the filter
  // telling us the window is no longer full.
  EXPECT_CALL(flowCallback_, onConnectionSendWindowOpen());
  callbackStart_->onWindowUpdate(0, std::numeric_limits<int32_t>::max());
  ASSERT_EQ(filter_->getAvailableSend(), std::numeric_limits<int32_t>::max());

  // Now overflow it by 1
  EXPECT_CALL(callback_, onError(0, IsFlowException(), _));
  callbackStart_->onWindowUpdate(0, 1);
  ASSERT_FALSE(chain_->isReusable());
}

TEST_F(HTTPChecksTest, send_trace_body_death) {
  // It is NOT allowed to send a TRACE with a body.

  HTTPMessage msg = getPostRequest();
  msg.setMethod("TRACE");

  EXPECT_DEATH_NO_CORE(chain_->generateHeader(writeBuf_, 0, msg, 0), ".*");
}

TEST_F(HTTPChecksTest, send_get_body) {
  // It is allowed to send a GET with a content-length. It is up to the
  // server to ignore it.

  EXPECT_CALL(*codec_, generateHeader(_, _, _, _, _));

  HTTPMessage msg = getPostRequest();
  msg.setMethod("GET");

  chain_->generateHeader(writeBuf_, 0, msg, 0);
}

TEST_F(HTTPChecksTest, recv_trace_body) {
  // In proxygen, we deal with receiving a TRACE with a body by 400'ing it

  EXPECT_CALL(callback_, onError(_, _, _))
    .WillOnce(Invoke([] (HTTPCodec::StreamID,
                         std::shared_ptr<HTTPException> exc,
                         bool newTxn) {
                       ASSERT_TRUE(newTxn);
                       ASSERT_EQ(exc->getHttpStatusCode(), 400);
        }));

  auto msg = makePostRequest();
  msg->setMethod("TRACE");

  callbackStart_->onHeadersComplete(0, std::move(msg));
}
