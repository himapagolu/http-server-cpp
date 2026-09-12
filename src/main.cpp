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

void handle_client(int client_fd) {
  size_t message_size = 1024;
  std::string message(message_size, '\0');
  recv(client_fd, (void*)&message[0], message.size(), 0);

  std::string path;
  size_t methodEnd = message.find(" ");
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

  std::string dir;
    size_t dir_idx = message.find(" ");
  if (dir_idx != std::string::npos) {
    int start = dir_idx + 1;
    int end = message.find(" ", start);

    if (end != std::string::npos) {
      dir = message.substr(start, end - start);
    }
  }

   std::ifstream file("dir");


  std::string response;
  if (path == "/") {
    response = "HTTP/1.1 200 OK\r\n\r\n";
  } else if (path.find("/echo") != std::string::npos) {
    std::string content = path.substr(6);
    response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length:" + std::to_string(content.size()) + "\r\n\r\n" + content;
  } else if (path.find("/user-agent") != std::string::npos) {
    response = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length:" + std::to_string(user_agt.size()) + "\r\n\r\n" + user_agt;
  } else if( dir!= " " && file){
    response = "HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length:" + 13 + "\r\n\r\n" + Hello, World!

  }else {
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
