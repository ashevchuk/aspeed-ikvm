#include "config.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <random>
#include <sstream>
#include <stdexcept>

#include "json.hpp"
#include "sha256.hpp"

using json = nlohmann::json;

namespace {

std::string random_hex(size_t num_bytes) {
  std::random_device rd;
  std::uniform_int_distribution<int> dist(0, 255);
  static const char hexchars[] = "0123456789abcdef";
  std::string out(num_bytes * 2, '0');
  for (size_t i = 0; i < num_bytes; ++i) {
    unsigned char byte = static_cast<unsigned char>(dist(rd));
    out[i * 2] = hexchars[(byte >> 4) & 0xf];
    out[i * 2 + 1] = hexchars[byte & 0xf];
  }
  return out;
}

// Readable random password: alphanumeric, avoids characters easy to confuse.
std::string random_password(size_t len) {
  static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789";
  std::random_device rd;
  std::uniform_int_distribution<size_t> dist(0, sizeof(alphabet) - 2);
  std::string out(len, '0');
  for (size_t i = 0; i < len; ++i) out[i] = alphabet[dist(rd)];
  return out;
}

void write_file_atomic(const std::string &path, const std::string &content) {
  std::string tmp_path = path + ".tmp";
  {
    std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open " + tmp_path + " for writing");
    out << content;
  }
  chmod(tmp_path.c_str(), S_IRUSR | S_IWUSR);
  if (std::rename(tmp_path.c_str(), path.c_str()) != 0)
    throw std::runtime_error("cannot rename " + tmp_path + " to " + path);
}

bool file_exists(const std::string &path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

}  // namespace

// ---------------------------------------------------------------- Inventory

Inventory::Inventory(std::string path) : path_(std::move(path)) { load(); }

void Inventory::load() {
  std::lock_guard<std::mutex> lock(mutex_);
  servers_.clear();
  if (!file_exists(path_)) return;

  std::ifstream in(path_, std::ios::binary);
  json root;
  in >> root;
  for (const auto &item : root.value("servers", json::array())) {
    ServerEntry e;
    e.id = item.value("id", "");
    e.name = item.value("name", "");
    e.host = item.value("host", "");
    e.username = item.value("username", "");
    e.password = item.value("password", "");
    for (const auto &tag : item.value("tags", json::array())) e.tags.push_back(tag.get<std::string>());
    servers_.push_back(std::move(e));
  }
}

void Inventory::save() const {
  json root;
  root["servers"] = json::array();
  for (const auto &e : servers_) {
    json item;
    item["id"] = e.id;
    item["name"] = e.name;
    item["host"] = e.host;
    item["username"] = e.username;
    item["password"] = e.password;
    item["tags"] = e.tags;
    root["servers"].push_back(item);
  }
  write_file_atomic(path_, root.dump(2));
}

std::vector<ServerEntry> Inventory::list() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return servers_;
}

bool Inventory::get(const std::string &id, ServerEntry &out) const {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto &e : servers_) {
    if (e.id == id) {
      out = e;
      return true;
    }
  }
  return false;
}

ServerEntry Inventory::add(const std::string &name, const std::string &host,
                             const std::string &username, const std::string &password,
                             const std::vector<std::string> &tags) {
  std::lock_guard<std::mutex> lock(mutex_);
  ServerEntry e;
  e.id = random_hex(4);
  e.name = name;
  e.host = host;
  e.username = username;
  e.password = password;
  e.tags = tags;
  servers_.push_back(e);
  save();
  return e;
}

bool Inventory::update(const std::string &id, const std::string &name, const std::string &host,
                        const std::string &username, const std::string &password,
                        const std::vector<std::string> &tags) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto &e : servers_) {
    if (e.id == id) {
      e.name = name;
      e.host = host;
      e.username = username;
      e.password = password;
      e.tags = tags;
      save();
      return true;
    }
  }
  return false;
}

bool Inventory::remove(const std::string &id) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto before = servers_.size();
  servers_.erase(std::remove_if(servers_.begin(), servers_.end(),
                                 [&](const ServerEntry &e) { return e.id == id; }),
                 servers_.end());
  if (servers_.size() == before) return false;
  save();
  return true;
}

// ---------------------------------------------------------------- AuthStore

AuthStore::AuthStore(std::string path) : path_(std::move(path)) {}

void AuthStore::load() {
  std::ifstream in(path_, std::ios::binary);
  json root;
  in >> root;
  username_ = root.value("username", "admin");
  salt_hex_ = root.value("salt", "");
  hash_hex_ = root.value("hash", "");
  loaded_ = true;
}

void AuthStore::save() const {
  json root;
  root["username"] = username_;
  root["salt"] = salt_hex_;
  root["hash"] = hash_hex_;
  write_file_atomic(path_, root.dump(2));
}

std::string AuthStore::ensure_bootstrapped() {
  if (file_exists(path_)) {
    load();
    return "";
  }

  username_ = "admin";
  salt_hex_ = random_hex(16);
  std::string password = random_password(16);
  hash_hex_ = sha256_hex(salt_hex_ + password);
  loaded_ = true;
  save();
  return password;
}

bool AuthStore::verify(const std::string &username, const std::string &password) const {
  if (!loaded_) return false;
  if (username != username_) return false;
  return sha256_hex(salt_hex_ + password) == hash_hex_;
}

void AuthStore::set_password(const std::string &password) {
  if (!loaded_ && file_exists(path_)) load();
  if (!loaded_) username_ = "admin";

  salt_hex_ = random_hex(16);
  hash_hex_ = sha256_hex(salt_hex_ + password);
  loaded_ = true;
  save();
}
