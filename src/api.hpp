#pragma once

#include "config.hpp"
#include "httplib.h"

// Wires the REST API routes (inventory CRUD + IPMI power/sensors/SEL) and
// HTTP Basic Auth onto an httplib::Server instance.
class ApiServer {
 public:
  ApiServer(Inventory &inventory, AuthStore &auth);

  void register_routes(httplib::Server &svr);

 private:
  bool check_auth(const httplib::Request &req, httplib::Response &res) const;

  void handle_list_servers(const httplib::Request &req, httplib::Response &res);
  void handle_create_server(const httplib::Request &req, httplib::Response &res);
  void handle_update_server(const httplib::Request &req, httplib::Response &res);
  void handle_delete_server(const httplib::Request &req, httplib::Response &res);
  void handle_power_status(const httplib::Request &req, httplib::Response &res);
  void handle_power_action(const httplib::Request &req, httplib::Response &res);
  void handle_sensors(const httplib::Request &req, httplib::Response &res);
  void handle_sel(const httplib::Request &req, httplib::Response &res);
  void handle_console(const httplib::Request &req, httplib::ws::WebSocket &ws);

  Inventory &inventory_;
  AuthStore &auth_;
};
