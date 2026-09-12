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


std::string g_directory;

void handle_client(int client_fd) {
  size_t message_size = 1024;
  std::string message(message_size, '\0');
  recv(client_fd, (void*)&message[0], message.size(), 0);

  std::string method;
  size_t methodEnd = message.find(" ");
  if (methodEnd != std::string::npos) {
    method = message.substr(0, methodEnd);
  }

  std::string path;
    if (methodEnd != std::string::npos) {
    int start = methodEnd + 1;
    int end = message.find(" ", start);

    if (end != std::string::npos) {
      path = message.substr(start, end - start);
    }
  }

  std::string user_agt;
  size_t user_agent_text_start = message.find("User-Agent: ");
  if (user_agent_text_start != std::string::npos) {
    int start = user_agent_text_start + 12;
    int end = message.find("\r", start);

    if (end != std::string::npos) {
      user_agt = message.substr(start, end - start);
    }
  }
  int content_length = 0;
  size_t content_length_pos = message.find("Content-Length: ");
  if (content_length_pos != std::string::npos) {
    int start = content_length_pos + 16;
    int end = message.find("\r", start);

    if (end != std::string::npos) {
      content_length = std::stoi(message.substr(start, end - start));
    }
  }

  std::string body;
  size_t headers_end = message.find("\r\n\r\n");
  if (headers_end != std::string::npos) {
    body = message.substr(headers_end + 4, content_length);
  }
  
  std::string response;
  if (method == "GET" && path == "/") {
    response = "HTTP/1.1 200 OK\r\n\r\n";
  } else if (method == "GET" && path.find("/echo") != std::string::npos) {
    std::string content = path.substr(6);
    response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length:" + std::to_string(content.size()) + "\r\n\r\n" + content;
  } else if (method == "GET" && path.find("/user-agent") != std::string::npos) {
    response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length:" + std::to_string(user_agt.size()) + "\r\n\r\n" + user_agt;
  } else if (method == "GET" && path.find("/files/") == 0) {
    std::string filename = path.substr(7);
    std::string full_path = g_directory + "/" + filename;
    std::ifstream file(full_path, std::ios::binary);
    if (file) {
      std::uintmax_t size = std::filesystem::file_size(full_path);
      std::stringstream buffer;
      buffer << file.rdbuf();
      std::string file_contents = buffer.str();

      response = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: " + std::to_string(size) + "\r\n\r\n" + file_contents;
    }else{
       response = "HTTP/1.1 404 Not Found\r\n\r\n"; 
    }
  }else if (method == "POST" && path.find("/files/") == 0) {
    std::string filename = path.substr(7);
    std::string full_path = g_directory + "/" + filename;

    std::ofstream outfile(full_path, std::ios::binary);
    outfile << body;
    outfile.close();

    response = "HTTP/1.1 201 Created\r\n\r\n";

  }else{
    response = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
  }

  send(client_fd, response.c_str(), response.size(), 0);
  
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // You can use print statements as follows for debugging, they'll be visible when running tests.
  std::cout << "Logs from your program will appear here!\n";

  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--directory" && i + 1 < argc) {
      g_directory = argv[i + 1];
    }
  }



  //syscall to create a new endpoint
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
   std::cerr << "Failed to create server socket\n";
   return 1;
  }

  // Since the tester restarts your program quite often, setting SO_REUSEADDR
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

    std::thread(handle_client, client_fd).detach();
  }

  close(server_fd);

  return 0;
}
