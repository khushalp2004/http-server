#include <iostream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Failed to create server socket\n";
    return 1;
  }

  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    std::cerr << "setsockopt failed\n";
    return 1;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(4221);

  if (bind(server_fd, (struct sockaddr *) &server_addr, sizeof(server_addr)) != 0) {
    std::cerr << "Failed to bind to port 4221\n";
    return 1;
  }

  if (listen(server_fd, 5) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }

  std::cout << "Waiting for clients...\n";

  while (true) {
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    int client_socket = accept(server_fd, (struct sockaddr *) &client_addr, &client_addr_len);

    if (client_socket < 0) {
      std::cerr << "Failed to accept connection\n";
      continue;
    }

    std::cout << "Client connected\n";

    char buffer[4096] = {0};
    int bytes_read = read(client_socket, buffer, sizeof(buffer) - 1);
    if (bytes_read < 0) {
      std::cerr << "Failed to read request\n";
      close(client_socket);
      continue;
    }

    std::string request(buffer);
    std::cout << "Request:\n" << request << "\n";

    std::string response;
    // Extract the path from the first line: e.g., "GET / HTTP/1.1"
    size_t path_start = request.find(' ');
    size_t path_end = request.find(' ', path_start + 1);

    std::string path = request.substr(path_start + 1, path_end - path_start - 1);

    if (path == "/") {
      response = "HTTP/1.1 200 OK\r\n\r\n";
    } else {
      response = "HTTP/1.1 404 Not Found\r\n\r\n";
    }

    send(client_socket, response.c_str(), response.size(), 0);
    close(client_socket);
  }

  close(server_fd);
  return 0;
}
