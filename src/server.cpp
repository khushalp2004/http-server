#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <zlib.h>

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#define BUFFER_SIZE 4096

std::string transform_to_lowercase(std::string &s) {
  std::string res = "";
  for (int i = 0; i < (int)s.length(); i++) {
    if (s[i] >= 'A' && s[i] <= 'Z') {
      res += s[i] - 'A' + 'a';
    } else {
      res += s[i];
    }
  }
  return res;
}

std::string gzip_compress(const std::string &input) {
  z_stream zs{};
  deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED,
               15 + 16,  // 15 window bits + 16 = gzip
               8, Z_DEFAULT_STRATEGY);

  zs.next_in = (Bytef *)input.data();
  zs.avail_in = input.size();

  int ret;
  char outbuffer[32768];
  std::string outstring;

  do {
    zs.next_out = reinterpret_cast<Bytef *>(outbuffer);
    zs.avail_out = sizeof(outbuffer);

    ret = deflate(&zs, Z_FINISH);
    outstring.append(outbuffer, sizeof(outbuffer) - zs.avail_out);
  } while (ret == Z_OK);

  deflateEnd(&zs);

  if (ret != Z_STREAM_END) throw std::runtime_error("gzip compression failed!");

  return outstring;
}

struct HttpResponseStartLine {
  std::string protocol;
  std::string status_code;
  std::string status_text;

  HttpResponseStartLine(int identifier, std::string new_protocol = "HTTP/1.1") {
    protocol = new_protocol;
    status_code = std::to_string(identifier);
    if (identifier == 404)
      status_text = "Not Found";
    else if (identifier == 200)
      status_text = "OK";
    else if (identifier == 201)
      status_text = "Created";
  }

  HttpResponseStartLine(std::string new_protocol, std::string new_status_code,
                        std::string new_status_text) {
    protocol = new_protocol;
    status_code = new_status_code;
    status_text = new_status_text;
  }
};

void add_response_header(std::map<std::string, std::string> &headers,
                         const std::string &key, const std::string &val) {
  headers[key] = val;
}

void send_response(int client_socket, HttpResponseStartLine start_line) {
  std::string res = "";
  res += start_line.protocol + " " + start_line.status_code + " " +
         start_line.status_text + "\r\n";
  res += "\r\n";
  send(client_socket, res.c_str(), res.length(), 0);
}

void send_response(int client_socket, HttpResponseStartLine start_line,
                   std::map<std::string, std::string> &headers) {
  std::string res = "";
  res += start_line.protocol + " " + start_line.status_code + " " +
         start_line.status_text + "\r\n";
  for (auto [key, val] : headers) {
    res += key + ": " + val + "\r\n";
  }
  res += "\r\n";
  send(client_socket, res.c_str(), res.length(), 0);
}

void send_response(int client_socket, HttpResponseStartLine start_line,
                   std::map<std::string, std::string> &headers,
                   const std::string &body_message) {
  std::string res = "";
  res += start_line.protocol + " " + start_line.status_code + " " +
         start_line.status_text + "\r\n";
  for (auto [key, val] : headers) {
    res += key + ": " + val + "\r\n";
  }
  res += "\r\n";
  res += body_message;
  send(client_socket, res.c_str(), res.length(), 0);
}

std::vector<std::string> split_compression_header(
    std::string compression_header, char delimiter) {
  std::vector<std::string> res;
  std::string cur = "";
  for (int i = 0; i < (int)compression_header.size(); i++) {
    if (compression_header[i] == ' ') continue;
    if (compression_header[i] == delimiter) {
      res.push_back(cur);
      cur.clear();
      continue;
    }
    cur += compression_header[i];
  }
  if (!cur.empty()) res.push_back(cur);
  return res;
}

void process_client(int client_socket, std::string &directory) {
  while (true) {
    std::string res = "";
    while (true) {
      char buffer[BUFFER_SIZE] = {0};
      int bytes_read = recv(client_socket, buffer, BUFFER_SIZE - 1, 0);
      if (bytes_read <= 0) {
        std::cerr << "Failed to receive data from client" << std::endl;
        close(client_socket);
        return;
      }
      buffer[bytes_read] = '\0';
      res += buffer;
      if (res.find("\r\n\r\n") != std::string::npos) break;
    }

    std::istringstream iss(res);

    std::string line;
    std::getline(iss, line);

    // Parse Request
    // Parse Request Start Line
    std::istringstream request_stream(line);
    std::string method, path, version;
    request_stream >> method >> path >> version;

    // Parse Request Headers
    std::map<std::string, std::string> request_headers;
    while (std::getline(iss, line) && line != "\r") {
      int colon_pos = line.find(':');
      if (colon_pos != std::string::npos) {
        std::string key = line.substr(0, colon_pos);
        std::string val = line.substr(colon_pos + 2);
        val.pop_back();  // need to pop the \r
        request_headers[transform_to_lowercase(key)] = val;
      }
    }

    // Parse Request Body
    std::string request_body = "";
    while (std::getline(iss, line)) {
      request_body += line + '\n';
    }
    request_body.pop_back();  // remove the last \n

    // Construct Response
    HttpResponseStartLine response_start_line = HttpResponseStartLine(200);
    std::map<std::string, std::string> response_headers;
    std::string response_body = "";

    bool compress_body = false;
    if (request_headers.find("accept-encoding") != request_headers.end() &&
        request_headers["accept-encoding"].find("gzip") != std::string::npos) {
      std::vector<std::string> compression_headers =
          split_compression_header(request_headers["accept-encoding"], ',');
      for (auto compression_header : compression_headers) {
        if (compression_header == "gzip") {
          compress_body = true;
          add_response_header(response_headers, "Content-Encoding", "gzip");
          break;
        }
      }
    }

    if (method == "POST") {
      if (path.find("/files") != std::string::npos) {
        if (directory == "") {
          response_start_line = HttpResponseStartLine(404);
        } else {
          std::string filename = path.substr(7);
          std::ofstream outputFile(directory + filename);
          outputFile << request_body;
          response_start_line = HttpResponseStartLine(201);
        }
      }
    } else if (method == "GET") {  // GET request handler
      if (path == "/")
        ;
      else if (path.substr(0, 5) == "/echo") {
        add_response_header(response_headers, "Content-Type", "text/plain");
        response_body = path.substr(6, (int)path.length() - 6);
      } else if (path.find("/user-agent") != std::string::npos &&
                 request_headers.find("user-agent") != request_headers.end()) {
        add_response_header(response_headers, "Content-Type", "text/plain");
        response_body = request_headers["user-agent"];
      } else if (path.find("/files") != std::string::npos) {
        std::string filename = path.substr(7);
        std::ifstream inputFile(directory + filename);
        if (!inputFile.is_open()) {
          response_start_line = HttpResponseStartLine(404);
        } else {
          std::string inputLine;
          std::string res = "";
          while (std::getline(inputFile, inputLine)) {
            res += inputLine + '\n';
          }
          res.pop_back();  // remove the last \n
          add_response_header(response_headers, "Content-Type",
                              "application/octet-stream");
          response_body = res;
        }
      } else {
        response_start_line = HttpResponseStartLine(404);
      }
    }

    if (compress_body) {
      response_body = gzip_compress(response_body);
    }

    add_response_header(response_headers, "Content-Length",
                        std::to_string(response_body.size()));

    if (request_headers.find("connection") != request_headers.end() && request_headers["connection"] == "close") {
      add_response_header(response_headers, "Connection", "close");
    }

    send_response(client_socket, response_start_line, response_headers,
                  response_body);
    
    if (request_headers.find("connection") != request_headers.end() && request_headers["connection"] == "close") {
      break;
    }
  }

  close(client_socket);
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // Process argument
  std::string directory = "";
  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];
    if (arg == "--directory") {
      if (i + 1 < argc) {
        directory = argv[i + 1];
      }
    }
  }

  // You can use print statements as follows for debugging, they'll be visible
  // when running tests.
  std::cout << "Logs from your program will appear here!\n";

  // Uncomment this block to pass the first stage

  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Failed to create server socket\n";
    return 1;
  }

  // Since the tester restarts your program quite often, setting SO_REUSEADDR
  // ensures that we don't run into 'Address already in use' errors
  int reuse = 1;
  if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) <
      0) {
    std::cerr << "setsockopt failed\n";
    return 1;
  }

  struct sockaddr_in server_addr;
  server_addr.sin_family = AF_INET;
  server_addr.sin_addr.s_addr = INADDR_ANY;
  server_addr.sin_port = htons(4221);

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) !=
      0) {
    std::cerr << "Failed to bind to port 4221\n";
    return 1;
  }

  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }

  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);

  std::cout << "Waiting for a client to connect...\n";

  while (true) {
    // Documentation related:
    // https://pubs.opengroup.org/onlinepubs/009604499/functions/accept.html
    int client_socket = accept(server_fd, (struct sockaddr *)&client_addr,
                               (socklen_t *)&client_addr_len);
    if (client_socket < 0) {
      std::cerr << "Failed to accept connection" << std::endl;
      return 1;
    }
    std::cout << "Client connected\n";
    std::thread t(process_client, client_socket, std::ref(directory));
    t.detach();
  }

  close(server_fd);

  return 0;
}