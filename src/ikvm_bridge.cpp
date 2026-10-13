#include "ikvm_bridge.hpp"

#include <thread>

#include "ikvm_decoder.hpp"
#include "ikvm_session.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

std::string extract_field(const std::string &body, const std::string &key) {
  size_t pos = body.find(key);
  if (pos == std::string::npos) throw IkvmError("field " + key + " not found in BMC response");
  pos += key.size();
  size_t end = body.find('\'', pos);
  if (end == std::string::npos) throw IkvmError("malformed field " + key + " in BMC response");
  return body.substr(pos, end - pos);
}

// Mirrors ikvm.sh's HTTP login: create.asp hands back a session cookie,
// getsessiontoken.asp exchanges it for the STOKEN the :7578 TCP login
// expects.
void web_login(const ServerEntry &server, std::string &cookie, std::string &stoken) {
  httplib::Client http(server.host.c_str(), 80);

  std::string body = "WEBVAR_USERNAME=" + server.username + "&WEBVAR_PASSWORD=" + server.password;
  auto create_res = http.Post("/rpc/WEBSES/create.asp", body, "application/x-www-form-urlencoded");
  if (!create_res || create_res->status != 200) throw IkvmError("create.asp failed");
  cookie = extract_field(create_res->body, "'SESSION_COOKIE' : '");

  httplib::Headers headers = {{"Cookie", "SessionCookie=" + cookie}};
  auto token_res = http.Get("/rpc/getsessiontoken.asp", headers);
  if (!token_res || token_res->status != 200) throw IkvmError("getsessiontoken.asp failed");
  stoken = extract_field(token_res->body, "'STOKEN' : '");
}

uint16_t le16(const std::vector<uint8_t> &frame, size_t off) {
  return static_cast<uint16_t>(frame[off] | frame[off + 1] << 8);
}

uint32_t le32(const std::vector<uint8_t> &frame, size_t off) {
  return static_cast<uint32_t>(frame[off] | frame[off + 1] << 8 | frame[off + 2] << 16 | frame[off + 3] << 24);
}

constexpr size_t kAspHdrOffset = 39;    // VideoHdr size, see ikvm_session.hpp
constexpr size_t kCompressedOffset = 125;  // VideoHdr (39B) + ASP2000ImgHdr (86B)
constexpr int kPollTimeoutMs = 1000;

// Field offsets verified live against a BMC - see ikvm_decoder's capture
// test and the frame-layout comment on IkvmSession::read_video_frame().
IkvmFrameInfo parse_frame_info(const std::vector<uint8_t> &frame) {
  IkvmFrameInfo info;
  info.source_width = le16(frame, kAspHdrOffset + 4);
  info.source_height = le16(frame, kAspHdrOffset + 6);
  info.dest_width = le16(frame, kAspHdrOffset + 13);
  info.dest_height = le16(frame, kAspHdrOffset + 15);
  info.jpeg_scale_factor = frame[kAspHdrOffset + 43];
  info.jpeg_table_selector = frame[kAspHdrOffset + 44];
  info.jpeg_yuv_table_mapping = frame[kAspHdrOffset + 45];
  info.advance_table_selector = frame[kAspHdrOffset + 47];
  info.advance_scale_factor = frame[kAspHdrOffset + 48];
  info.number_of_mb = le32(frame, kAspHdrOffset + 49);
  info.rc4_enable = frame[kAspHdrOffset + 53];
  info.mode420 = frame[kAspHdrOffset + 55];
  info.compress_size = le32(frame, kAspHdrOffset + 69);
  return info;
}

constexpr char kMsgTypeKeyReport = 1;
constexpr size_t kKeyReportMsgSize = 3;  // type, modifiers, keycode

constexpr char kMsgTypeMouseReport = 2;
constexpr size_t kMouseReportMsgSize = 6;  // type, buttons, x (u16 LE), y (u16 LE)

constexpr uint8_t kFrameFormatRaw = 0;
constexpr uint8_t kFrameFormatJpeg = 1;

void append_bytes(void *context, void *data, int size) {
  static_cast<std::string *>(context)->append(static_cast<const char *>(data), static_cast<size_t>(size));
}

// RGB24 -> JPEG, straight into the outbound message buffer via stb's
// write-to-memory callback - no temp file, no intermediate copy.
std::string encode_jpeg(const uint8_t *rgb, int width, int height, int quality) {
  std::string jpeg;
  stbi_write_jpg_to_func(append_bytes, &jpeg, width, height, 3, rgb, quality);
  return jpeg;
}

constexpr uint8_t kAbsoluteMouseMode = 2;  // USBMouseRep.ABSOLUTE_MOUSE_MODE

}  // namespace

void ikvm_stream_console(const ServerEntry &server, httplib::ws::WebSocket &ws, bool compress, int jpeg_quality) {
  std::string cookie, stoken;
  web_login(server, cookie, stoken);

  IkvmSession session(server.host, stoken, cookie);
  IkvmDecoder decoder;

  // httplib only parses incoming frames (Ping/Pong/Close) inside ws.read();
  // nothing else drives that. Without this thread pumping it, a browser-side
  // close is never noticed and ws.is_open() stays true forever - this is the
  // only thing that makes it go false on a client-initiated disconnect. It
  // doubles as the keyboard-input path: sending on this thread while the
  // main loop reads video frames is safe, each direction of a TCP socket has
  // its own buffering and neither call touches the other's state.
  std::thread reader([&ws, &session] {
    std::string msg;
    while (ws.is_open()) {
      if (ws.read(msg) != httplib::ws::Binary || msg.empty()) continue;

      if (msg[0] == kMsgTypeKeyReport && msg.size() == kKeyReportMsgSize) {
        session.send_key_report(static_cast<uint8_t>(msg[1]), static_cast<uint8_t>(msg[2]));
      } else if (msg[0] == kMsgTypeMouseReport && msg.size() == kMouseReportMsgSize &&
                 session.mouse_mode() == kAbsoluteMouseMode) {
        uint8_t buttons = static_cast<uint8_t>(msg[1]);
        uint16_t x = static_cast<uint8_t>(msg[2]) | static_cast<uint16_t>(static_cast<uint8_t>(msg[3])) << 8;
        uint16_t y = static_cast<uint8_t>(msg[4]) | static_cast<uint16_t>(static_cast<uint8_t>(msg[5])) << 8;
        session.send_mouse_report(buttons, static_cast<int16_t>(x), static_cast<int16_t>(y));
      }
    }
  });

  auto stop_reader = [&] {
    ws.close();
    reader.join();
  };

  try {
    while (ws.is_open()) {
      // An idle screen can go a long time between frames. Polling with a
      // bound keeps this loop checking ws.is_open() regularly instead of
      // blocking in read_video_frame() long after the browser is gone.
      if (!session.has_data(kPollTimeoutMs)) continue;

      std::vector<uint8_t> frame = session.read_video_frame();
      if (frame.size() <= kCompressedOffset) continue;

      IkvmFrameInfo info = parse_frame_info(frame);
      std::vector<uint8_t> pixels =
          decoder.decode(info, frame.data() + kCompressedOffset, frame.size() - kCompressedOffset);

      std::string payload = compress ? encode_jpeg(pixels.data(), info.dest_width, info.dest_height, jpeg_quality)
                                      : std::string(reinterpret_cast<const char *>(pixels.data()), pixels.size());

      std::string message;
      message.reserve(5 + payload.size());
      message.push_back(static_cast<char>(compress ? kFrameFormatJpeg : kFrameFormatRaw));
      message.push_back(static_cast<char>(info.dest_width & 0xFF));
      message.push_back(static_cast<char>(info.dest_width >> 8));
      message.push_back(static_cast<char>(info.dest_height & 0xFF));
      message.push_back(static_cast<char>(info.dest_height >> 8));
      message.append(payload);

      if (!ws.send(message.data(), message.size())) break;
    }
  } catch (...) {
    stop_reader();
    throw;
  }

  stop_reader();
}
