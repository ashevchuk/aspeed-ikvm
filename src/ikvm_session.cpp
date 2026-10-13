#include "ikvm_session.hpp"

#include <gcrypt.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>

namespace {

constexpr int kKvmPort = 7578;
constexpr size_t kHeaderSize = 7;

// IVTPPktHdr packet type codes (see AMI JViewer's IVTPPktHdr.java).
constexpr uint8_t kAdviserValidateVideoSession = 34;
constexpr uint8_t kAdviserWebSession = 52;
constexpr uint8_t kAdviserValidateVideoSessionResponse = 35;
constexpr uint8_t kAdviserVideoFragment = 5;
constexpr uint8_t kAdviserBwDetectRes = 32;
constexpr uint8_t kAdviserAddNewMouseCursor = 48;
constexpr uint8_t kAdviserGetUsbMouseMode = 33;

// ASTCursorPktHdr.HDR_SIZE: fixed sub-header in front of a mouse-cursor-shape
// packet's pattern data.
constexpr uint32_t kCursorHeaderSize = 57;

// High bit of the little-endian fragment number marks the last fragment of
// a frame (FragReader.java: `fragNum & 0x8000`).
constexpr uint16_t kLastFragmentFlag = 0x8000;

constexpr uint8_t kAdviserSetBandwidth = 7;
constexpr uint32_t kBandwidth100Mbps = 0xC80000;

constexpr uint8_t kAdviserHidPkt = 6;
constexpr size_t kKeyReportPacketSize = 48;
constexpr size_t kMouseReportPacketSize = 45;
// Checksum covers IUSB_HDR_SIZE (32) bytes starting right after the
// IVTPPktHdr, i.e. packet[7..39) - see USBKeyboardRep.java's report().
constexpr size_t kChecksumStart = 7;
constexpr size_t kChecksumEnd = 39;

int connect_tcp(const std::string &host, int port) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;

  addrinfo *result = nullptr;
  int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &result);
  if (rc != 0) throw IkvmError("resolving " + host + " failed: " + gai_strerror(rc));

  int fd = -1;
  for (addrinfo *ai = result; ai != nullptr; ai = ai->ai_next) {
    fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    if (fd < 0) continue;
    if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(result);

  if (fd < 0) throw IkvmError("connecting to " + host + ":" + std::to_string(port) + " failed: " + strerror(errno));
  return fd;
}

void send_all(int fd, const void *data, size_t len) {
  const char *p = static_cast<const char *>(data);
  size_t sent = 0;
  while (sent < len) {
    ssize_t n = send(fd, p + sent, len - sent, 0);
    if (n <= 0) throw IkvmError(std::string("send to KVM port failed: ") + strerror(errno));
    sent += static_cast<size_t>(n);
  }
}

void recv_exact(int fd, void *data, size_t len) {
  char *p = static_cast<char *>(data);
  size_t got = 0;
  while (got < len) {
    ssize_t n = recv(fd, p + got, len - got, 0);
    if (n < 0) throw IkvmError(std::string("recv from KVM port failed: ") + strerror(errno));
    if (n == 0) throw IkvmError("KVM port closed the connection");
    got += static_cast<size_t>(n);
  }
}

void discard_exact(int fd, size_t len) {
  char scratch[4096];
  while (len > 0) {
    size_t chunk = len < sizeof(scratch) ? len : sizeof(scratch);
    recv_exact(fd, scratch, chunk);
    len -= chunk;
  }
}

uint16_t read_u16le(int fd) {
  uint8_t buf[2];
  recv_exact(fd, buf, sizeof(buf));
  return static_cast<uint16_t>(buf[0]) | static_cast<uint16_t>(buf[1]) << 8;
}

// Little-endian: type(1B) + pktSize(4B) + status(2B), per IVTPPktHdr.java.
std::array<uint8_t, kHeaderSize> pack_header(uint8_t type, uint32_t size, uint16_t status) {
  std::array<uint8_t, kHeaderSize> hdr;
  hdr[0] = type;
  hdr[1] = static_cast<uint8_t>(size);
  hdr[2] = static_cast<uint8_t>(size >> 8);
  hdr[3] = static_cast<uint8_t>(size >> 16);
  hdr[4] = static_cast<uint8_t>(size >> 24);
  hdr[5] = static_cast<uint8_t>(status);
  hdr[6] = static_cast<uint8_t>(status >> 8);
  return hdr;
}

struct ParsedHeader {
  uint8_t type;
  uint32_t size;
  uint16_t status;
};

ParsedHeader read_header(int fd) {
  std::array<uint8_t, kHeaderSize> hdr;
  recv_exact(fd, hdr.data(), hdr.size());
  ParsedHeader out;
  out.type = hdr[0];
  out.size = static_cast<uint32_t>(hdr[1]) | static_cast<uint32_t>(hdr[2]) << 8 |
             static_cast<uint32_t>(hdr[3]) << 16 | static_cast<uint32_t>(hdr[4]) << 24;
  out.status = static_cast<uint16_t>(hdr[5]) | static_cast<uint16_t>(hdr[6]) << 8;
  return out;
}

std::array<unsigned char, 16> md5(const std::string &data) {
  if (!gcry_check_version(nullptr)) throw IkvmError("libgcrypt version check failed");
  std::array<unsigned char, 16> digest;
  gcry_md_hash_buffer(GCRY_MD_MD5, digest.data(), data.data(), data.size());
  return digest;
}

}  // namespace

IkvmSession::IkvmSession(const std::string &host, const std::string &session_token,
                          const std::string &web_session_cookie) {
  fd_ = connect_tcp(host, kKvmPort);

  try {
    km_key_ = md5(session_token);

    std::array<uint8_t, kHeaderSize> login_hdr = pack_header(kAdviserValidateVideoSession, 16, 0);
    send_all(fd_, login_hdr.data(), login_hdr.size());
    send_all(fd_, km_key_.data(), km_key_.size());

    std::array<uint8_t, kHeaderSize> web_hdr =
        pack_header(kAdviserWebSession, static_cast<uint32_t>(web_session_cookie.size()), 0);
    send_all(fd_, web_hdr.data(), web_hdr.size());
    send_all(fd_, web_session_cookie.data(), web_session_cookie.size());

    ParsedHeader response = read_header(fd_);
    if (response.type != kAdviserValidateVideoSessionResponse) {
      throw IkvmError("unexpected response type " + std::to_string(response.type) + " during login");
    }
    if (response.size != 1) {
      throw IkvmError("unexpected login response payload size " + std::to_string(response.size));
    }
    // Per JViewerApp.OnValidateVideoSessionResp: 0 means authentication
    // failure (invalid session token), any nonzero value means success.
    uint8_t login_status = 0;
    recv_exact(fd_, &login_status, 1);
    if (login_status == 0) {
      throw IkvmError("KVM login rejected: invalid session token");
    }

    // CfgBandwidth.java: the real client always reports its measured link
    // speed right after login - the BMC doesn't start streaming otherwise.
    std::array<uint8_t, kHeaderSize> bw_hdr = pack_header(kAdviserSetBandwidth, 4, 0);
    send_all(fd_, bw_hdr.data(), bw_hdr.size());
    uint8_t bw_payload[4] = {
        static_cast<uint8_t>(kBandwidth100Mbps), static_cast<uint8_t>(kBandwidth100Mbps >> 8),
        static_cast<uint8_t>(kBandwidth100Mbps >> 16), static_cast<uint8_t>(kBandwidth100Mbps >> 24)};
    send_all(fd_, bw_payload, sizeof(bw_payload));
  } catch (...) {
    close(fd_);
    throw;
  }
}

IkvmSession::~IkvmSession() {
  if (fd_ >= 0) close(fd_);
}

std::vector<unsigned char> IkvmSession::read_video_frame() {
  std::vector<unsigned char> frame;

  for (;;) {
    ParsedHeader hdr = read_header(fd_);

    if (hdr.type == kAdviserVideoFragment) {
      uint16_t frag_num = read_u16le(fd_);
      if (frame.empty() && (frag_num & ~kLastFragmentFlag) != 0) {
        throw IkvmError("video stream out of sync: first fragment of a frame has nonzero index");
      }
      uint32_t frag_len = hdr.size - 2;
      size_t old_size = frame.size();
      frame.resize(old_size + frag_len);
      recv_exact(fd_, frame.data() + old_size, frag_len);
      if (frag_num & kLastFragmentFlag) return frame;
      continue;
    }

    // Bandwidth-probe echo (NullReader.java): `status` repeats of `size`
    // bytes, all discarded - this client never triggers autoDetect().
    if (hdr.type == kAdviserBwDetectRes) {
      for (uint16_t i = 0; i < hdr.status; ++i) discard_exact(fd_, hdr.size);
      continue;
    }

    // Mouse-cursor-shape packet (ASTCursorHdrReader.java): fixed sub-header
    // plus pattern data, discarded - cursor rendering isn't implemented yet.
    if (hdr.type == kAdviserAddNewMouseCursor) {
      discard_exact(fd_, kCursorHeaderSize);
      if (hdr.size > kCursorHeaderSize) discard_exact(fd_, hdr.size - kCursorHeaderSize);
      continue;
    }

    // Mouse-mode announcement (KVMClient.onControlMessage case 33): the BMC
    // pushes this unprompted after login, there's no explicit request for
    // it (see JViewerApp.OnGetMouseMode). 1-byte payload: USBMouseRep's
    // RELATIVE_MOUSE_MODE=1 / ABSOLUTE_MOUSE_MODE=2 / INVALID_MOUSE_MODE=3.
    if (hdr.type == kAdviserGetUsbMouseMode && hdr.size == 1) {
      recv_exact(fd_, &mouse_mode_, 1);
      continue;
    }

    // Generic control message (CtrlReader.java): just a payload to skip.
    if (hdr.size > 0) discard_exact(fd_, hdr.size);
  }
}

void IkvmSession::send_key_report(uint8_t modifiers, uint8_t keycode) {
  std::array<uint8_t, kKeyReportPacketSize> packet{};

  std::array<uint8_t, kHeaderSize> hdr = pack_header(kAdviserHidPkt, 41, 0);
  std::copy(hdr.begin(), hdr.end(), packet.begin());

  // "IUSB    " - USBMessage.m_signature.
  const uint8_t signature[8] = {'I', 'U', 'S', 'B', ' ', ' ', ' ', ' '};
  std::copy(std::begin(signature), std::end(signature), packet.begin() + 7);

  packet[15] = 1;
  packet[16] = 0;
  packet[17] = 32;  // IUSB_HDR_SIZE
  // packet[18] is the checksum byte, filled in below.
  packet[19] = 9;  // int32 LE
  packet[23] = 0;
  packet[24] = 48;  // IUSB_DEVICE_KEYBD
  packet[25] = 16;  // IUSB_PROTO_KEYBD_DATA
  packet[26] = 0x80;
  packet[27] = 2;  // IUSB_KEYBD_DEVNUM
  packet[28] = 0;  // IUSB_KEYBD_IFNUM
  packet[31] = static_cast<uint8_t>(key_seq_);
  packet[32] = static_cast<uint8_t>(key_seq_ >> 8);
  packet[33] = static_cast<uint8_t>(key_seq_ >> 16);
  packet[34] = static_cast<uint8_t>(key_seq_ >> 24);
  packet[39] = 8;  // USB_PKT_KEYBDREP_SIZE
  // USB HID boot-keyboard report: modifiers, reserved (auto-keybreak flag,
  // unused here), then up to 6 keycodes - this client, like the real one,
  // only ever fills the first.
  packet[40] = modifiers;
  packet[41] = 0;
  packet[42] = keycode;

  uint8_t sum = 0;
  for (size_t i = kChecksumStart; i < kChecksumEnd; ++i) sum += packet[i];
  packet[18] = static_cast<uint8_t>(-static_cast<int>(sum));

  ++key_seq_;
  send_all(fd_, packet.data(), packet.size());
}

void IkvmSession::send_mouse_report(uint8_t buttons, int16_t x, int16_t y) {
  std::array<uint8_t, kMouseReportPacketSize> packet{};

  std::array<uint8_t, kHeaderSize> hdr = pack_header(kAdviserHidPkt, 38, 0);
  std::copy(hdr.begin(), hdr.end(), packet.begin());

  const uint8_t signature[8] = {'I', 'U', 'S', 'B', ' ', ' ', ' ', ' '};
  std::copy(std::begin(signature), std::end(signature), packet.begin() + 7);

  packet[15] = 1;
  packet[16] = 0;
  packet[17] = 32;  // IUSB_HDR_SIZE
  // packet[18] is the checksum byte, filled in below.
  packet[19] = 6;  // int32 LE
  packet[23] = 0;
  packet[24] = 49;  // IUSB_DEVICE_MOUSE
  packet[25] = 32;  // IUSB_PROTO_MOUSE_DATA
  packet[26] = 0x80;
  packet[27] = 2;  // IUSB_MOUSE_DEVNUM
  packet[28] = 1;  // IUSB_MOUSE_IFNUM
  packet[31] = static_cast<uint8_t>(mouse_seq_);
  packet[32] = static_cast<uint8_t>(mouse_seq_ >> 8);
  packet[33] = static_cast<uint8_t>(mouse_seq_ >> 16);
  packet[34] = static_cast<uint8_t>(mouse_seq_ >> 24);
  packet[39] = 5;  // USB_PKT_MOUSE_ABS_REP_SIZE
  packet[40] = buttons;
  packet[41] = static_cast<uint8_t>(x);
  packet[42] = static_cast<uint8_t>(static_cast<uint16_t>(x) >> 8);
  packet[43] = static_cast<uint8_t>(y);
  packet[44] = static_cast<uint8_t>(static_cast<uint16_t>(y) >> 8);

  uint8_t sum = 0;
  for (size_t i = kChecksumStart; i < kChecksumEnd; ++i) sum += packet[i];
  packet[18] = static_cast<uint8_t>(-static_cast<int>(sum));

  ++mouse_seq_;
  send_all(fd_, packet.data(), packet.size());
}

bool IkvmSession::has_data(int timeout_ms) {
  pollfd pfd{fd_, POLLIN, 0};
  int rv = poll(&pfd, 1, timeout_ms);
  if (rv < 0) throw IkvmError(std::string("poll on KVM port failed: ") + strerror(errno));
  return rv > 0;
}
