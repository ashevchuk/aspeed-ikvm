#pragma once

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

class IkvmError : public std::runtime_error {
 public:
  explicit IkvmError(const std::string &what) : std::runtime_error(what) {}
};

// A connected and authenticated AMI MegaRAC JViewer KVM-redirection session
// (TCP, BMC_IP:7578). session_token is the STOKEN from
// /rpc/getsessiontoken.asp, web_session_cookie is the SessionCookie value
// from /rpc/WEBSES/create.asp - both already required to reach this point
// via the existing HTTP login flow.
class IkvmSession {
 public:
  IkvmSession(const std::string &host, const std::string &session_token,
              const std::string &web_session_cookie);
  ~IkvmSession();

  IkvmSession(const IkvmSession &) = delete;
  IkvmSession &operator=(const IkvmSession &) = delete;

  int fd() const { return fd_; }

  // MD5(session_token), the RC4 key the BMC expects for encrypted
  // keyboard/mouse packets later on.
  const std::array<unsigned char, 16> &km_key() const { return km_key_; }

  // Blocks until one full video frame has been reassembled from its
  // ADVISER_VIDEO_FRAGMENT packets (see FragNumReader/FragReader.java).
  // Interleaved bandwidth-probe and mouse-cursor-shape packets are consumed
  // and discarded. The returned buffer is the frame as the BMC sends it: a
  // 39-byte VideoHdr ("ASP-2000" signature) followed by an ASP2000ImgHdr and
  // the compressed image data - not yet decoded.
  std::vector<unsigned char> read_video_frame();

  // True if the BMC has sent at least one byte we haven't read yet, waiting
  // up to timeout_ms. A caller that only calls read_video_frame() when this
  // is true never blocks waiting on a BMC that's gone quiet (e.g. an
  // unchanging screen) - it stays free to re-check other state, such as
  // whether the browser on the other end is still connected, in between.
  bool has_data(int timeout_ms);

  // Sends one USB HID boot-keyboard report (ADVISER_HID_PKT, wrapped in
  // AMI's IUSB envelope - see USBKeyboardRep.java/USBKeyProcessor.java).
  // modifiers is the standard HID modifier bitmask (LeftCtrl=0x01,
  // LeftShift=0x02, LeftAlt=0x04, LeftGUI=0x08, RightCtrl=0x10,
  // RightShift=0x20, RightAlt=0x40, RightGUI=0x80); keycode is a single USB
  // HID Keyboard/Keypad usage ID, or 0 for "no ordinary key held" (the real
  // client never reports more than one non-modifier key at a time either).
  void send_key_report(uint8_t modifiers, uint8_t keycode);

  // Sends one USB HID absolute-mouse report (ADVISER_HID_PKT, IUSB envelope
  // - see USBMouseRep.java's ABSreport()). buttons bit0=left, bit1=right,
  // bit2=middle. x/y are pre-scaled to the device's 0..32767 absolute
  // range (USBMouseRep.DIRABS_MAX_SCALED_{X,Y} = Short.MAX_VALUE) - the
  // caller maps its own coordinate space to that range, this just wraps and
  // sends it. Only meaningful when mouse_mode() == 2 (ABSOLUTE_MOUSE_MODE);
  // this class doesn't implement the relative-mode protocol.
  void send_mouse_report(uint8_t buttons, int16_t x, int16_t y);

  // USBMouseRep's RELATIVE_MOUSE_MODE(1) / ABSOLUTE_MOUSE_MODE(2); 3
  // (INVALID) until the BMC's unprompted mouse-mode announcement has been
  // seen by read_video_frame() - see USBMouseRep.java.
  uint8_t mouse_mode() const { return mouse_mode_; }

 private:
  int fd_ = -1;
  std::array<unsigned char, 16> km_key_{};
  uint32_t key_seq_ = 0;
  uint32_t mouse_seq_ = 0;
  uint8_t mouse_mode_ = 3;
};
