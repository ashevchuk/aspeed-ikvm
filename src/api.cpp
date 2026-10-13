#include "api.hpp"

#include <algorithm>
#include <cstdlib>

#include "ikvm_bridge.hpp"
#include "ikvm_session.hpp"
#include "ipmi_monitor.hpp"
#include "ipmi_power.hpp"
#include "json.hpp"

using json = nlohmann::json;

namespace {

std::string base64_decode(const std::string &in) {
  static const std::string alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::vector<int> table(256, -1);
  for (size_t i = 0; i < alphabet.size(); ++i) table[static_cast<unsigned char>(alphabet[i])] = static_cast<int>(i);

  std::string out;
  int val = 0, valb = -8;
  for (unsigned char c : in) {
    if (table[c] == -1) break;
    val = (val << 6) + table[c];
    valb += 6;
    if (valb >= 0) {
      out.push_back(static_cast<char>((val >> valb) & 0xFF));
      valb -= 8;
    }
  }
  return out;
}

json server_to_json(const ServerEntry &e) {
  json j;
  j["id"] = e.id;
  j["name"] = e.name;
  j["host"] = e.host;
  j["username"] = e.username;
  j["tags"] = e.tags;
  return j;
}

IpmiCredentials credentials_from(const ServerEntry &e) { return {e.host, e.username, e.password}; }

void send_json_error(httplib::Response &res, int status, const std::string &message) {
  json j;
  j["error"] = message;
  res.status = status;
  res.set_content(j.dump(), "application/json");
}

PowerAction parse_power_action(const std::string &s) {
  if (s == "on") return PowerAction::On;
  if (s == "off") return PowerAction::Off;
  if (s == "cycle") return PowerAction::Cycle;
  if (s == "reset") return PowerAction::HardReset;
  if (s == "soft-shutdown") return PowerAction::SoftShutdown;
  throw std::invalid_argument("unknown power action: " + s);
}

}  // namespace

ApiServer::ApiServer(Inventory &inventory, AuthStore &auth) : inventory_(inventory), auth_(auth) {}

bool ApiServer::check_auth(const httplib::Request &req, httplib::Response &res) const {
  const std::string prefix = "Basic ";
  std::string header = req.get_header_value("Authorization");

  if (header.compare(0, prefix.size(), prefix) == 0) {
    std::string decoded = base64_decode(header.substr(prefix.size()));
    size_t colon = decoded.find(':');
    if (colon != std::string::npos) {
      std::string user = decoded.substr(0, colon);
      std::string pass = decoded.substr(colon + 1);
      if (auth_.verify(user, pass)) return true;
    }
  }

  res.set_header("WWW-Authenticate", "Basic realm=\"srvman\"");
  send_json_error(res, 401, "authentication required");
  return false;
}

void ApiServer::register_routes(httplib::Server &svr) {
  svr.set_pre_routing_handler([this](const httplib::Request &req, httplib::Response &res) {
    if (req.path.compare(0, 5, "/api/") == 0 && !check_auth(req, res)) {
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  svr.Get("/api/servers", [this](const httplib::Request &req, httplib::Response &res) {
    handle_list_servers(req, res);
  });
  svr.Post("/api/servers", [this](const httplib::Request &req, httplib::Response &res) {
    handle_create_server(req, res);
  });
  svr.Put(R"(/api/servers/([A-Za-z0-9]+))", [this](const httplib::Request &req, httplib::Response &res) {
    handle_update_server(req, res);
  });
  svr.Delete(R"(/api/servers/([A-Za-z0-9]+))", [this](const httplib::Request &req, httplib::Response &res) {
    handle_delete_server(req, res);
  });
  svr.Get(R"(/api/servers/([A-Za-z0-9]+)/power)",
          [this](const httplib::Request &req, httplib::Response &res) { handle_power_status(req, res); });
  svr.Post(R"(/api/servers/([A-Za-z0-9]+)/power)",
           [this](const httplib::Request &req, httplib::Response &res) { handle_power_action(req, res); });
  svr.Get(R"(/api/servers/([A-Za-z0-9]+)/sensors)",
          [this](const httplib::Request &req, httplib::Response &res) { handle_sensors(req, res); });
  svr.Get(R"(/api/servers/([A-Za-z0-9]+)/sel)",
          [this](const httplib::Request &req, httplib::Response &res) { handle_sel(req, res); });
  svr.WebSocket(R"(/api/servers/([A-Za-z0-9]+)/console)",
                [this](const httplib::Request &req, httplib::ws::WebSocket &ws) { handle_console(req, ws); });
}

void ApiServer::handle_list_servers(const httplib::Request &, httplib::Response &res) {
  json arr = json::array();
  for (const auto &e : inventory_.list()) arr.push_back(server_to_json(e));
  res.set_content(arr.dump(), "application/json");
}

void ApiServer::handle_create_server(const httplib::Request &req, httplib::Response &res) {
  json body;
  try {
    body = json::parse(req.body);
  } catch (const json::exception &) {
    return send_json_error(res, 400, "invalid JSON body");
  }

  std::string name = body.value("name", "");
  std::string host = body.value("host", "");
  std::string username = body.value("username", "");
  std::string password = body.value("password", "");
  if (name.empty() || host.empty()) return send_json_error(res, 400, "name and host are required");

  std::vector<std::string> tags;
  for (const auto &t : body.value("tags", json::array())) tags.push_back(t.get<std::string>());

  ServerEntry e = inventory_.add(name, host, username, password, tags);
  res.status = 201;
  res.set_content(server_to_json(e).dump(), "application/json");
}

void ApiServer::handle_update_server(const httplib::Request &req, httplib::Response &res) {
  std::string id = req.matches[1];
  json body;
  try {
    body = json::parse(req.body);
  } catch (const json::exception &) {
    return send_json_error(res, 400, "invalid JSON body");
  }

  ServerEntry existing;
  if (!inventory_.get(id, existing)) return send_json_error(res, 404, "server not found");

  std::string name = body.value("name", existing.name);
  std::string host = body.value("host", existing.host);
  std::string username = body.value("username", existing.username);
  std::string password = body.value("password", existing.password);

  std::vector<std::string> tags = existing.tags;
  if (body.contains("tags")) {
    tags.clear();
    for (const auto &t : body["tags"]) tags.push_back(t.get<std::string>());
  }

  inventory_.update(id, name, host, username, password, tags);
  ServerEntry updated;
  inventory_.get(id, updated);
  res.set_content(server_to_json(updated).dump(), "application/json");
}

void ApiServer::handle_delete_server(const httplib::Request &req, httplib::Response &res) {
  std::string id = req.matches[1];
  if (!inventory_.remove(id)) return send_json_error(res, 404, "server not found");
  res.status = 204;
}

void ApiServer::handle_power_status(const httplib::Request &req, httplib::Response &res) {
  std::string id = req.matches[1];
  ServerEntry e;
  if (!inventory_.get(id, e)) return send_json_error(res, 404, "server not found");

  try {
    PowerStatus status = ipmi_get_power_status(credentials_from(e));
    json j;
    j["power_on"] = status.power_on;
    res.set_content(j.dump(), "application/json");
  } catch (const IpmiError &ex) {
    send_json_error(res, 502, ex.what());
  }
}

void ApiServer::handle_power_action(const httplib::Request &req, httplib::Response &res) {
  std::string id = req.matches[1];
  ServerEntry e;
  if (!inventory_.get(id, e)) return send_json_error(res, 404, "server not found");

  json body;
  try {
    body = json::parse(req.body);
  } catch (const json::exception &) {
    return send_json_error(res, 400, "invalid JSON body");
  }

  PowerAction action;
  try {
    action = parse_power_action(body.value("action", ""));
  } catch (const std::invalid_argument &ex) {
    return send_json_error(res, 400, ex.what());
  }

  try {
    ipmi_chassis_control(credentials_from(e), action);
    json j;
    j["ok"] = true;
    res.set_content(j.dump(), "application/json");
  } catch (const IpmiError &ex) {
    send_json_error(res, 502, ex.what());
  }
}

void ApiServer::handle_sensors(const httplib::Request &req, httplib::Response &res) {
  std::string id = req.matches[1];
  ServerEntry e;
  if (!inventory_.get(id, e)) return send_json_error(res, 404, "server not found");

  try {
    json arr = json::array();
    for (const auto &r : ipmi_read_sensors(credentials_from(e))) {
      json j;
      j["name"] = r.name;
      j["category"] = r.category;
      j["units"] = r.units;
      j["state"] = r.state;
      if (r.has_value) j["value"] = r.value;
      if (r.has_lower_warning) j["lower_warning"] = r.lower_warning;
      if (r.has_lower_critical) j["lower_critical"] = r.lower_critical;
      if (r.has_upper_warning) j["upper_warning"] = r.upper_warning;
      if (r.has_upper_critical) j["upper_critical"] = r.upper_critical;
      arr.push_back(j);
    }
    res.set_content(arr.dump(), "application/json");
  } catch (const IpmiError &ex) {
    send_json_error(res, 502, ex.what());
  }
}

void ApiServer::handle_console(const httplib::Request &req, httplib::ws::WebSocket &ws) {
  std::string id = req.matches[1];
  ServerEntry e;
  if (!inventory_.get(id, e)) {
    ws.close(httplib::ws::CloseStatus::PolicyViolation, "server not found");
    return;
  }

  int quality = 75;
  if (req.has_param("quality")) {
    int parsed = std::atoi(req.get_param_value("quality").c_str());
    quality = std::max(1, std::min(100, parsed));
  }

  try {
    ikvm_stream_console(e, ws, req.has_param("compress"), quality);
  } catch (const IkvmError &ex) {
    ws.close(httplib::ws::CloseStatus::InternalError, ex.what());
  }
}

void ApiServer::handle_sel(const httplib::Request &req, httplib::Response &res) {
  std::string id = req.matches[1];
  ServerEntry e;
  if (!inventory_.get(id, e)) return send_json_error(res, 404, "server not found");

  unsigned int limit = 100;
  if (req.has_param("limit")) limit = static_cast<unsigned int>(std::stoul(req.get_param_value("limit")));

  try {
    json arr = json::array();
    for (const auto &entry : ipmi_read_sel(credentials_from(e), limit)) {
      json j;
      j["record_id"] = entry.record_id;
      j["timestamp"] = entry.timestamp;
      j["sensor_type"] = entry.sensor_type;
      j["description"] = entry.description;
      arr.push_back(j);
    }
    res.set_content(arr.dump(), "application/json");
  } catch (const IpmiError &ex) {
    send_json_error(res, 502, ex.what());
  }
}
