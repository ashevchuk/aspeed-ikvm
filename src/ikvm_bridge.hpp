#pragma once

#include "config.hpp"
#include "httplib.h"

// Logs into a BMC's AMI MegaRAC KVM video channel (the HTTP session
// handshake, then the :7578 TCP protocol handled by IkvmSession), decodes
// frames server-side, and relays them to a browser WebSocket, while
// relaying keyboard/mouse input the other way, until either side
// disconnects.
//
// Outbound (server -> browser) message: format byte (0 = raw RGB24, 1 =
// JPEG), dest_width (u16 LE), dest_height (u16 LE), then the payload - for
// format 0, dest_width * dest_height * 3 raw RGB24 bytes, row-major, no
// padding; for format 1, a JPEG-encoded image of that size.
//
// Inbound (browser -> server) messages, by first byte:
//   1: USB HID key report - modifiers bitmask, keycode (3 bytes total).
//      See IkvmSession::send_key_report.
//   2: USB HID absolute-mouse report - buttons bitmask, x (u16 LE), y
//      (u16 LE), both pre-scaled to the device's 0..32767 range (6 bytes
//      total). Dropped if the BMC hasn't announced absolute mouse mode -
//      see IkvmSession::send_mouse_report / mouse_mode.
//
// compress and jpeg_quality (1-100, ignored unless compress is set): chosen
// once for the whole session via query params on the WebSocket URL, not
// mid-stream - the raw-RGB path is the known-good fallback, so switching
// formats never needs to be a race with in-flight frames.
void ikvm_stream_console(const ServerEntry &server, httplib::ws::WebSocket &ws, bool compress, int jpeg_quality);
