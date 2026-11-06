// unit_tests.cpp - tests for framing, validation and hashing.
//
// Framing tests use socketpair() so both ends are real sockets in one
// process: we can write bytes in any pattern (one at a time, two frames at
// once, a lying length header) and check what recv_frame() returns.
#include "config.h"
#include "crypto.h"
#include "net.h"
#include "util.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (cond) ++g_pass;                                               \
        else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static std::string raw_frame(net::FrameType t, const std::string& body) {
    uint32_t len = htonl(static_cast<uint32_t>(body.size() + 1));
    std::string out(reinterpret_cast<char*>(&len), 4);
    out += static_cast<char>(t);
    out += body;
    return out;
}

static void test_frames() {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    net::Frame f;

    // 1. Normal round trip.
    CHECK(net::send_text(sv[0], "LOGIN|alice|pw"));
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::OK);
    CHECK(f.type == net::FrameType::TEXT && f.body == "LOGIN|alice|pw");

    // 2. One frame delivered one byte at a time (TCP splitting a message).
    std::string wire = raw_frame(net::FrameType::TEXT, "split-message-test");
    std::thread writer([&] {
        for (char c : wire) {
            ::send(sv[0], &c, 1, 0);
            usleep(200);
        }
    });
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::OK);
    CHECK(f.body == "split-message-test");
    writer.join();

    // 3. Two frames delivered in one write (TCP merging messages).
    std::string two = raw_frame(net::FrameType::TEXT, "first") + raw_frame(net::FrameType::TEXT, "second");
    ::send(sv[0], two.data(), two.size(), 0);
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::OK && f.body == "first");
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::OK && f.body == "second");

    // 4. Binary piece with zero bytes survives intact (binary safety).
    std::string bin(1000, '\0');
    for (size_t i = 0; i < bin.size(); ++i) bin[i] = static_cast<char>(i % 256);
    CHECK(net::send_frame(sv[0], net::FrameType::PIECE, bin));
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::OK);
    CHECK(f.type == net::FrameType::PIECE && f.body == bin);

    // 5. A full 512 KB piece.
    std::string piece(cfg::PIECE_SIZE, 'x');
    std::thread big([&] { net::send_frame(sv[0], net::FrameType::PIECE, piece); });
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::OK && f.body.size() == cfg::PIECE_SIZE);
    big.join();

    // 6. Sender refuses a body above MAX_FRAME.
    CHECK(!net::send_text(sv[0], std::string(cfg::MAX_FRAME + 1, 'a')));

    close(sv[0]);
    close(sv[1]);

    // 7. A header claiming 4 GB is rejected without allocating.
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    uint32_t huge = htonl(0xFFFFFFFFu);
    ::send(sv[0], &huge, 4, 0);
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::ERROR);
    close(sv[0]);
    close(sv[1]);

    // 8. Unknown frame type is rejected.
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    std::string bad = raw_frame(static_cast<net::FrameType>(9), "x");
    ::send(sv[0], bad.data(), bad.size(), 0);
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::ERROR);
    close(sv[0]);
    close(sv[1]);

    // 9. Zero length (no type byte) is rejected.
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    uint32_t zero = 0;
    ::send(sv[0], &zero, 4, 0);
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::ERROR);
    close(sv[0]);
    close(sv[1]);

    // 10. Peer closes mid-frame -> ERROR; clean close before a frame -> CLOSED.
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    std::string partial = raw_frame(net::FrameType::TEXT, "cut-short").substr(0, 7);
    ::send(sv[0], partial.data(), partial.size(), 0);
    close(sv[0]);
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::ERROR);
    close(sv[1]);
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    close(sv[0]);
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::CLOSED);
    close(sv[1]);

    // 11. Receive timeout reports TIMEOUT, not an error or a hang.
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    net::set_recv_timeout(sv[1], 100);
    CHECK(net::recv_frame(sv[1], f) == net::RecvStatus::TIMEOUT);
    close(sv[0]);
    close(sv[1]);
}

static void test_validation() {
    CHECK(util::valid_name("report.pdf"));
    CHECK(util::valid_name("my_group-1"));
    CHECK(!util::valid_name(""));
    CHECK(!util::valid_name("."));
    CHECK(!util::valid_name(".."));
    CHECK(!util::valid_name("../etc"));
    CHECK(!util::valid_name("a/b"));
    CHECK(!util::valid_name("a|b"));
    CHECK(!util::valid_name("a b"));
    CHECK(!util::valid_name(std::string("a\0b", 3)));
    CHECK(!util::valid_name(std::string(65, 'a')));
    CHECK(util::valid_name(std::string(64, 'a')));

    CHECK(util::valid_password("s3cret!#"));
    CHECK(!util::valid_password("has space"));
    CHECK(!util::valid_password("pipe|char"));

    uint64_t v = 0;
    CHECK(util::parse_u64("0", v) && v == 0);
    CHECK(util::parse_u64("4294967296", v) && v == 4294967296ULL);
    CHECK(util::parse_u64("18446744073709551615", v) && v == UINT64_MAX);
    CHECK(!util::parse_u64("18446744073709551616", v));  // overflow
    CHECK(!util::parse_u64("NOTNUM", v));                // crashed the old tracker
    CHECK(!util::parse_u64("-1", v));
    CHECK(!util::parse_u64("", v));
    CHECK(!util::parse_u64("12a", v));

    std::string ip;
    int port = 0;
    CHECK(net::parse_addr("127.0.0.2:9000", ip, port) && ip == "127.0.0.2" && port == 9000);
    CHECK(!net::parse_addr("127.0.0.1:0", ip, port));
    CHECK(!net::parse_addr("127.0.0.1:70000", ip, port));
    CHECK(!net::parse_addr("localhost:80", ip, port));
    CHECK(!net::parse_addr("1.2.3.4", ip, port));

    auto parts = util::split("a||b|", '|');
    CHECK(parts.size() == 4 && parts[1].empty() && parts[3].empty());
}

static void test_pieces_and_hashes() {
    CHECK(util::piece_count(0) == 0);
    CHECK(util::piece_count(1) == 1);
    CHECK(util::piece_count(cfg::PIECE_SIZE) == 1);
    CHECK(util::piece_count(cfg::PIECE_SIZE + 1) == 2);
    CHECK(util::piece_count(cfg::MAX_FILE_SIZE) == 8192);
    CHECK(util::piece_length(cfg::PIECE_SIZE + 1, 1) == 1);
    // Offset of the last piece of a 4 GiB file is past INT32_MAX: 64-bit math.
    CHECK(8191ULL * cfg::PIECE_SIZE == 4294443008ULL);
    CHECK(util::piece_length(cfg::MAX_FILE_SIZE, 8191) == cfg::PIECE_SIZE);

    // Known test vectors.
    CHECK(crypto::sha1_hex("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
    CHECK(crypto::sha256_hex("abc") ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    crypto::Sha1Stream s;
    s.update("a", 1);
    s.update("bc", 2);
    CHECK(s.final_hex() == "a9993e364706816aba3e25717850c26c9cd0d89d");

    // Salts: random, so the same password gives different stored hashes.
    std::string s1 = crypto::random_hex(cfg::SALT_BYTES), s2 = crypto::random_hex(cfg::SALT_BYTES);
    CHECK(s1.size() == 32 && s1 != s2);
    CHECK(crypto::hash_password(s1, "pw") != crypto::hash_password(s2, "pw"));
    CHECK(crypto::ct_equal(crypto::hash_password(s1, "pw"), crypto::hash_password(s1, "pw")));
    CHECK(!crypto::ct_equal(crypto::hash_password(s1, "pw"), crypto::hash_password(s1, "pX")));
}

int main() {
    test_frames();
    test_validation();
    test_pieces_and_hashes();
    std::printf("unit tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
