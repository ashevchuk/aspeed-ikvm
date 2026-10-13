#pragma once

#include <mutex>
#include <string>
#include <vector>

struct ServerEntry {
  std::string id;
  std::string name;
  std::string host;
  std::string username;
  std::string password;
  std::vector<std::string> tags;
};

// Persists the server inventory to a JSON file. Access is serialized with a
// mutex since multiple HTTP worker threads may read/write concurrently.
class Inventory {
 public:
  explicit Inventory(std::string path);

  std::vector<ServerEntry> list() const;
  bool get(const std::string &id, ServerEntry &out) const;
  ServerEntry add(const std::string &name, const std::string &host,
                   const std::string &username, const std::string &password,
                   const std::vector<std::string> &tags);
  bool update(const std::string &id, const std::string &name, const std::string &host,
              const std::string &username, const std::string &password,
              const std::vector<std::string> &tags);
  bool remove(const std::string &id);

 private:
  void load();
  void save() const;

  std::string path_;
  std::vector<ServerEntry> servers_;
  mutable std::mutex mutex_;
};

// Single-admin credential store for the web UI itself (separate from IPMI
// credentials). Password is never kept in memory or on disk in plaintext.
class AuthStore {
 public:
  explicit AuthStore(std::string path);

  // Creates the store with a random password on first run. Returns the
  // generated plaintext password, or an empty string if the store already
  // existed.
  std::string ensure_bootstrapped();

  bool verify(const std::string &username, const std::string &password) const;

  // Overwrites the stored password (and its salt). Preserves the existing
  // username if the store already exists, otherwise defaults to "admin".
  void set_password(const std::string &password);

 private:
  void load();
  void save() const;

  std::string path_;
  std::string username_;
  std::string salt_hex_;
  std::string hash_hex_;
  bool loaded_ = false;
};
