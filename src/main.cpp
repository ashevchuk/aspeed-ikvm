#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <iostream>

#include "api.hpp"
#include "config.hpp"
#include "httplib.h"

namespace {
constexpr const char *kDataDir = "data";
constexpr const char *kServersPath = "data/servers.json";
constexpr const char *kAuthPath = "data/auth.json";
constexpr const char *kWebDir = "web";
constexpr int kDefaultPort = 8080;

// Reads a line from the terminal with echo disabled, like passwd(1).
std::string read_password_no_echo(const char *prompt) {
  std::cout << prompt << std::flush;

  termios old_term{};
  tcgetattr(STDIN_FILENO, &old_term);
  termios no_echo_term = old_term;
  no_echo_term.c_lflag &= ~ECHO;
  tcsetattr(STDIN_FILENO, TCSANOW, &no_echo_term);

  std::string password;
  std::getline(std::cin, password);

  tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
  std::cout << "\n";
  return password;
}

int run_set_password(const std::string &auth_path) {
  mkdir(kDataDir, S_IRWXU);

  std::string password = read_password_no_echo("New admin password: ");
  std::string confirm = read_password_no_echo("Confirm password: ");

  if (password.empty()) {
    std::cerr << "Password cannot be empty.\n";
    return 1;
  }
  if (password != confirm) {
    std::cerr << "Passwords did not match.\n";
    return 1;
  }

  AuthStore auth(auth_path);
  auth.set_password(password);
  std::cout << "Admin password updated.\n";
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc > 1 && std::strcmp(argv[1], "--set-password") == 0) {
    return run_set_password(kAuthPath);
  }

  int port = kDefaultPort;
  if (argc > 1) port = std::atoi(argv[1]);

  mkdir(kDataDir, S_IRWXU);

  Inventory inventory(kServersPath);
  AuthStore auth(kAuthPath);

  std::string generated_password = auth.ensure_bootstrapped();
  if (!generated_password.empty()) {
    std::cout << "First run: created admin account.\n"
              << "  username: admin\n"
              << "  password: " << generated_password << "\n"
              << "Save this password now, it will not be shown again.\n"
              << std::flush;
  }

  httplib::Server svr;
  svr.set_mount_point("/", kWebDir);

  ApiServer api(inventory, auth);
  api.register_routes(svr);

  // stdout is block-buffered (not line-buffered) when it isn't a tty, e.g.
  // under systemd - without this the lines above can sit unflushed for the
  // life of the process, since svr.listen() below never returns.
  std::cout << "srvman listening on http://0.0.0.0:" << port << "\n" << std::flush;
  svr.listen("0.0.0.0", port);
  return 0;
}
