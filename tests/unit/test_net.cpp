/// \file W1 传输层单测（docs/13 §5-W1）：回环连接/收发/超时 + 帧传输。

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "net/wire.hpp"
#include "platform/net.hpp"

namespace {

using rtdb::Err;
using rtdb::platform::TcpListener;
using rtdb::platform::TcpSocket;

/// 建立回环连接对（监听 → 连接 → 接受），返回 (客户端, 服务端)。
bool MakeLoopbackPair(TcpSocket* cli, TcpSocket* srv) {
    auto lr = TcpListener::Listen("127.0.0.1", 0);
    if (!lr.IsOk()) return false;
    TcpListener lst = lr.TakeValue();
    auto cr = TcpSocket::Connect("127.0.0.1", lst.port(), 3000);
    if (!cr.IsOk()) return false;
    auto ar = lst.Accept(3000);
    if (!ar.IsOk()) return false;
    *cli = cr.TakeValue();
    *srv = ar.TakeValue();
    cli->SetTimeout(3000);
    srv->SetTimeout(3000);
    return true;
}

TEST(NetTcpTest, LoopbackEchoRoundTrip) {
    TcpSocket cli;
    TcpSocket srv;
    ASSERT_TRUE(MakeLoopbackPair(&cli, &srv));

    const char msg[] = "ping-pong";
    EXPECT_EQ(cli.SendAll(msg, sizeof(msg)), Err::kOk);
    char buf[sizeof(msg)] = {0};
    EXPECT_EQ(srv.RecvExact(buf, sizeof(buf)), Err::kOk);
    EXPECT_STREQ(buf, msg);

    const char reply[] = "pong";
    EXPECT_EQ(srv.SendAll(reply, sizeof(reply)), Err::kOk);
    char rbuf[sizeof(reply)] = {0};
    EXPECT_EQ(cli.RecvExact(rbuf, sizeof(rbuf)), Err::kOk);
    EXPECT_STREQ(rbuf, reply);
}

TEST(NetTcpTest, AcceptTimeout) {
    auto lr = TcpListener::Listen("", 0);  // 空地址 = 仅回环
    ASSERT_TRUE(lr.IsOk());
    TcpListener lst = lr.TakeValue();
    ASSERT_NE(lst.port(), 0);

    auto ar = lst.Accept(50);
    EXPECT_FALSE(ar.IsOk());
    EXPECT_EQ(ar.Error(), Err::kTimeout);

    // 关闭后的监听器不再可用
    lst.Close();
    EXPECT_FALSE(lst.valid());
}

TEST(NetTcpTest, RecvTimeoutThenPeerClose) {
    TcpSocket cli;
    TcpSocket srv;
    ASSERT_TRUE(MakeLoopbackPair(&cli, &srv));
    cli.SetTimeout(60);

    char c = 0;
    EXPECT_EQ(cli.RecvExact(&c, 1), Err::kTimeout);

    srv.Close();  // 对端关闭 → EOF
    EXPECT_EQ(cli.RecvExact(&c, 1), Err::kNetworkError);
}

TEST(NetTcpTest, ConnectRefused) {
    auto lr = TcpListener::Listen("127.0.0.1", 0);
    ASSERT_TRUE(lr.IsOk());
    TcpListener lst = lr.TakeValue();
    const uint16_t port = lst.port();
    lst.Close();  // 释放端口 → 该端口大概率无监听

    // 注：部分 Windows 环境对回环拒连的 RST 有秒级延迟（安全软件/协议栈），
    // 预算放宽到 5s，确保观察到的是"拒连"而非"预算耗尽"。
    auto cr = TcpSocket::Connect("127.0.0.1", port, 5000);
    EXPECT_FALSE(cr.IsOk());
    EXPECT_EQ(cr.Error(), Err::kNetworkError);
}

TEST(NetFrameTest, FrameOverSocket) {
    TcpSocket cli;
    TcpSocket srv;
    ASSERT_TRUE(MakeLoopbackPair(&cli, &srv));

    std::vector<uint8_t> payload(1000);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>(i * 31);
    }
    rtdb::net::FrameHeader h;
    h.func = 11;
    h.session = 42;
    h.payload_len = static_cast<uint32_t>(payload.size());
    ASSERT_EQ(rtdb::net::WriteFrame(cli, h, payload.data()), Err::kOk);

    rtdb::net::FrameHeader rh;
    std::vector<uint8_t> rpayload;
    ASSERT_EQ(rtdb::net::ReadFrame(srv, 1u << 20, &rh, &rpayload), Err::kOk);
    EXPECT_EQ(rh.func, 11);
    EXPECT_EQ(rh.session, 42u);
    EXPECT_EQ(rpayload, payload);
}

TEST(NetFrameTest, BadCrcRejected) {
    TcpSocket cli;
    TcpSocket srv;
    ASSERT_TRUE(MakeLoopbackPair(&cli, &srv));

    // 直接构造 CRC 错误帧（合法头 + 未填 crc 的载荷）
    rtdb::net::FrameHeader h;
    h.func = 2;
    h.payload_len = 3;
    h.crc = 0;
    uint8_t hdr[rtdb::net::kFrameHeaderBytes];
    rtdb::net::EncodeFrameHeader(h, hdr);
    ASSERT_EQ(cli.SendAll(hdr, sizeof(hdr)), Err::kOk);
    const uint8_t body[3] = {1, 2, 3};
    ASSERT_EQ(cli.SendAll(body, sizeof(body)), Err::kOk);

    rtdb::net::FrameHeader rh;
    std::vector<uint8_t> rpayload;
    EXPECT_EQ(rtdb::net::ReadFrame(srv, 1024, &rh, &rpayload), Err::kProtocolError);
}

TEST(NetFrameTest, OversizePayloadHeaderRejected) {
    TcpSocket cli;
    TcpSocket srv;
    ASSERT_TRUE(MakeLoopbackPair(&cli, &srv));

    rtdb::net::FrameHeader h;
    h.func = 2;
    h.payload_len = 128;
    h.crc = 0;
    uint8_t hdr[rtdb::net::kFrameHeaderBytes];
    rtdb::net::EncodeFrameHeader(h, hdr);
    ASSERT_EQ(cli.SendAll(hdr, sizeof(hdr)), Err::kOk);

    rtdb::net::FrameHeader rh;
    std::vector<uint8_t> rpayload;
    // 读取端上限 64 < 载荷 128 → 头部即拒
    EXPECT_EQ(rtdb::net::ReadFrame(srv, 64, &rh, &rpayload), Err::kProtocolError);
}

TEST(NetFrameTest, PeerCloseMidFrameFails) {
    TcpSocket cli;
    TcpSocket srv;
    ASSERT_TRUE(MakeLoopbackPair(&cli, &srv));

    const uint8_t half_header[10] = {0};
    ASSERT_EQ(cli.SendAll(half_header, sizeof(half_header)), Err::kOk);
    cli.Close();  // 半帧后断开

    rtdb::net::FrameHeader rh;
    std::vector<uint8_t> rpayload;
    EXPECT_EQ(rtdb::net::ReadFrame(srv, 1024, &rh, &rpayload), Err::kNetworkError);
}

}  // namespace
