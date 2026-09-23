#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <map>
#include <vector>
#include <functional>
#include <zlib.h>


std::string g_directory;

// ---- Value object produced by parsing a raw request ----
struct HttpRequest {
  std::string method;
  std::string path;
  std::map<std::string, std::string> headers;
  std::string body;
};

// ---- Parses a raw HTTP request string into an HttpRequest ----
class HttpRequestParser {
public:
  static HttpRequest parse(const std::string &raw) {
    HttpRequest req;

    size_t methodEnd = raw.find(' ');
    if (methodEnd == std::string::npos) return req;
    req.method = raw.substr(0, methodEnd);

    size_t pathEnd = raw.find(' ', methodEnd + 1);
    if (pathEnd == std::string::npos) return req;
    req.path = raw.substr(methodEnd + 1, pathEnd - methodEnd - 1);

    size_t headers_end = raw.find("\r\n\r\n", pathEnd);
    if (headers_end == std::string::npos) return req;

    size_t headers_start = raw.find("\r\n", pathEnd) + 2;
    std::istringstream stream(raw.substr(headers_start, headers_end - headers_start));
    std::string line;
    while (std::getline(stream, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      size_t colon = line.find(": ");
      if (colon != std::string::npos) {
        req.headers[line.substr(0, colon)] = line.substr(colon + 2);
      }
    }

    int content_length = 0;
    auto it = req.headers.find("Content-Length");
    if (it != req.headers.end()) {
      content_length = std::stoi(it->second);
    }
    req.body = raw.substr(headers_end + 4, content_length);

    return req;
  }
};

// ---- Builds an HTTP response string without manual concatenation ----
class HttpResponseBuilder {
public:
  HttpResponseBuilder &status(int code, const std::string &text) {
    status_code_ = code;
    status_text_ = text;
    return *this;
  }

  HttpResponseBuilder &header(const std::string &key, const std::string &value) {
    headers_.emplace_back(key, value);
    return *this;
  }

  HttpResponseBuilder &body(const std::string &content) {
    body_ = content;
    return *this;
  }

  std::string build() const {
    std::ostringstream out;
    out << "HTTP/1.1 " << status_code_ << " " << status_text_ << "\r\n";
    for (const auto &[key, value] : headers_) {
      out << key << ": " << value << "\r\n";
    }
    out << "\r\n" << body_;
    return out.str();
  }

private:
  int status_code_ = 200;
  std::string status_text_ = "OK";
  std::vector<std::pair<std::string, std::string>> headers_;
  std::string body_;
};

using RouteHandler = std::function<HttpResponseBuilder(const HttpRequest &)>;

// Strategy pattern: each route owns its own handler, selected at request time
class Router {
public:
  enum class Match { Exact, Prefix, Contains };

  void add(const std::string &method, const std::string &pattern, Match match, RouteHandler handler) {
    routes_.push_back({method, pattern, match, std::move(handler)});
  }

  HttpResponseBuilder route(const HttpRequest &req) const {
    for (const auto &r : routes_) {
      if (r.method != req.method) continue;
      if (matches(req.path, r.pattern, r.match)) {
        return r.handler(req);
      }
    }
    return HttpResponseBuilder().status(404, "Not Found").header("Content-Length", "0");
  }

private:
  struct RouteEntry {
    std::string method;
    std::string pattern;
    Match match;
    RouteHandler handler;
  };

  static bool matches(const std::string &path, const std::string &pattern, Match match) {
    switch (match) {
      case Match::Exact: return path == pattern;
      case Match::Prefix: return path.rfind(pattern, 0) == 0;
      case Match::Contains: return path.find(pattern) != std::string::npos;
    }
    return false;
  }

  std::vector<RouteEntry> routes_;
};

//Route handlers (the "concrete strategies")
namespace handlers {

HttpResponseBuilder root(const HttpRequest &) {
  return HttpResponseBuilder().status(200, "OK").header("Content-Length", "0");
}

HttpResponseBuilder echo(const HttpRequest &req) {
  std::string content = req.path.substr(6);
  return HttpResponseBuilder()
      .status(200, "OK")
      .header("Content-Type", "text/plain")
      .header("Content-Length", std::to_string(content.size()))
      .body(content);
}
bool acceptsGzip(const HttpRequest &req){
  auto it = req.headers.find("Accept-Encoding");
  if (it == req.headers.end()) return false;

  const std::string &value = it->second;
  size_t start = 0;
  while (start <= value.size()) {
    size_t comma = value.find(',', start);
    size_t end = (comma == std::string::npos) ? value.size() : comma;

    size_t tokenStart = value.find_first_not_of(" \t", start);
    if (tokenStart != std::string::npos && tokenStart < end) {
      size_t tokenEnd = value.find_last_not_of(" \t", end - 1);
      if (value.compare(tokenStart, tokenEnd - tokenStart + 1, "gzip") == 0) {
        return true;
      }
    }

    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return false;
}

std::string gzipCompress(const std::string &input) {
  z_stream zs{};
  // 15 | 16 -> gzip wrapper instead of plain zlib
  if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 | 16, 8,
                    Z_DEFAULT_STRATEGY) != Z_OK) {
    return input;
  }

  zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
  zs.avail_in = static_cast<uInt>(input.size());

  std::string output;
  char buffer[4096];
  int ret;
  do {
    zs.next_out = reinterpret_cast<Bytef *>(buffer);
    zs.avail_out = sizeof(buffer);
    ret = deflate(&zs, Z_FINISH);
    output.append(buffer, sizeof(buffer) - zs.avail_out);
  } while (ret == Z_OK);

  deflateEnd(&zs);
  return output;
}

HttpResponseBuilder echoWithCompression(const HttpRequest &req) {
  std::string content = req.path.substr(6);
  HttpResponseBuilder response = HttpResponseBuilder()
      .status(200, "OK")
      .header("Content-Type", "text/plain");

  if (acceptsGzip(req)) {
    std::string compressed = gzipCompress(content);
    // std::cout << "compressed " << content.size() << " -> " << compressed.size() << "\n";
    response.header("Content-Encoding", "gzip")
        .header("Content-Length", std::to_string(compressed.size()))
        .body(compressed);
  } else {
    response.header("Content-Length", std::to_string(content.size()))
        .body(content);
  }
  return response;
}


HttpResponseBuilder userAgent(const HttpRequest &req) {
  std::string ua;
  auto it = req.headers.find("User-Agent");
  if (it != req.headers.end()) ua = it->second;
  return HttpResponseBuilder()
      .status(200, "OK")
      .header("Content-Type", "text/plain")
      .header("Content-Length", std::to_string(ua.size()))
      .body(ua);
}

HttpResponseBuilder getFile(const HttpRequest &req) {
  std::string filename = req.path.substr(7);
  std::string full_path = g_directory + "/" + filename;

  std::ifstream file(full_path, std::ios::binary);
  if (!file) {
    return HttpResponseBuilder().status(404, "Not Found").header("Content-Length", "0");
  }

  std::stringstream buffer;
  buffer << file.rdbuf();
  std::string file_contents = buffer.str();

  return HttpResponseBuilder()
      .status(200, "OK")
      .header("Content-Type", "application/octet-stream")
      .header("Content-Length", std::to_string(file_contents.size()))
      .body(file_contents);
}

HttpResponseBuilder postFile(const HttpRequest &req) {
  std::string filename = req.path.substr(7);
  std::string full_path = g_directory + "/" + filename;

  std::ofstream outfile(full_path, std::ios::binary);
  outfile << req.body;
  outfile.close();

  return HttpResponseBuilder().status(201, "Created").header("Content-Length", "0");
}

}  // namespace handlers

Router buildRouter() {
  Router router;
  router.add("GET", "/", Router::Match::Exact, handlers::root);
  router.add("GET", "/echo", Router::Match::Contains, handlers::echoWithCompression);
  router.add("GET", "/user-agent", Router::Match::Contains, handlers::userAgent);
  router.add("GET", "/files/", Router::Match::Prefix, handlers::getFile);
  router.add("POST", "/files/", Router::Match::Prefix, handlers::postFile);
  return router;
}

bool wantsConnectionClose(const HttpRequest &req) {
  auto it = req.headers.find("Connection");
  return it != req.headers.end() && it->second == "close";
}

void handle_client(int client_fd, const Router &router) {
  // keep the connection alive and just loop on it instead of closing after one request
  while (true) {
    size_t message_size = 1024;
    std::string message(message_size, '\0');
    ssize_t bytes_received = recv(client_fd, (void *)&message[0], message.size(), 0);
    if (bytes_received <= 0) {
      break;  // client hung up
    }
    message.resize(bytes_received);
    // std::cout << "recv " << bytes_received << " bytes\n";

    HttpRequest req = HttpRequestParser::parse(message);
    // std::cout << "req: " << req.method << " " << req.path << "\n";
    HttpResponseBuilder resp = router.route(req);

    bool close_connection = wantsConnectionClose(req);
    if (close_connection) {
      resp.header("Connection", "close");
    }

    std::string response = resp.build();
    // std::cout << response << "\n";
    send(client_fd, response.c_str(), response.size(), 0);

    if (close_connection) break;
  }

  close(client_fd);
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  
  std::cout << "Logs from your program will appear here!\n";

  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--directory" && i + 1 < argc) {
      g_directory = argv[i + 1];
    }
  }

  Router router = buildRouter();

  //syscall to create a new endpoint
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
   std::cerr << "Failed to create server socket\n";
   return 1;
  }

  //setting SO_REUSEADDR
  // ensures that we don't run into 'Address already in use' errors
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    std::cerr << "setsockopt failed\n";
    return 1;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  //htons() is a wrapper that enables to convert the port numer to network byte order(big-endian)
  server_addr.sin_port = htons(4221);

  if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
    std::cerr << "Failed to bind to port 4221\n";
    return 1;
  }

  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }

  std::cout << "Waiting for a client to connect...\n";

  while (true) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    int client_fd = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);
    if (client_fd < 0) {
      std::cerr << "accept failed\n";
      continue;
    }
    std::cout << "Client connected\n";
    // std::cout << "client_fd=" << client_fd << "\n";

    std::thread(handle_client, client_fd, std::ref(router)).detach();
  }

  close(server_fd);

  return 0;
}
