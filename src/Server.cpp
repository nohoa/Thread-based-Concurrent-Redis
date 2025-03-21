#include "Map.h"
#include "RDB Reader/RDBParser.hpp"
#include "Redis.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

std::mutex mutex_guard;
std ::string to_lower(std ::string s) {
  std ::string ans;
  for (auto x : s) {

    ans += (char)tolower(x);
  }
  return ans;
}

long get_current_time_ms() {
  auto time_now = std::chrono::system_clock::now();
  auto now_in_ms =
      std::chrono::time_point_cast<std::chrono::milliseconds>(time_now);
  auto value = now_in_ms.time_since_epoch();
  long current_time_in_ms = value.count();
  return current_time_in_ms;
}
void handle_connect(int client_fd, int argc, char **argv,
                    std::vector<std::vector<std::string>> additional_pair) {
  std ::unique_ptr<In_Memory_Storage> key_value_storage{
      std::make_unique<In_Memory_Storage>()};

  long current_time = get_current_time_ms();
  for (auto it : additional_pair) {
    if (it[2] == "-1") {
      key_value_storage->set(it[0], it[1], current_time + 999999999999);
    } else {

      key_value_storage->set(it[0], it[1], stol(it[2]));
    }
  }
  for (int i = 1; i < argc; i += 2) {
    if (i + 1 < argc) {
      std::string key = argv[i];
      std::string value = argv[i + 1];
      long current_time_in_ms = get_current_time_ms();
      key_value_storage->set(key.substr(2), value,
                             current_time_in_ms + 999999999999);
    }
  }
  while (true) {
    char msg[1024] = {};
    int rc = recv(client_fd, &msg, sizeof(msg), 0);

    if (rc <= 0) {
      close(client_fd);
      break;
    }
    // std::lock_guard<std::mutex> guard(mutex_guard);
    std ::string header = msg;

    std ::string response = "";

    std ::unique_ptr<Redis> parser{std::make_unique<Redis>(header)};

    std ::vector<std ::string> parser_list = parser->get_command(header);

    if (parser_list[0] == "PING") {
      response = "+PONG\r\n";
    } else if (parser_list[0] == "SET") {
      long current_time_in_ms = get_current_time_ms();

      response = "+OK\r\n";
      if (parser_list.size() > 3)
        key_value_storage->set(parser_list[1], parser_list[2],
                               current_time_in_ms + stoi(parser_list.back()));
      else
        key_value_storage->set(parser_list[1], parser_list[2],
                               current_time_in_ms + 999999999999);

    } else if (parser_list[0] == "GET") {
      long current_time_in_ms = get_current_time_ms();

      response = key_value_storage->get(parser_list[1], current_time_in_ms);

      if (response == "") {
        response = "$-1\r\n";
      } else {
        response =
            "$" + std::to_string(response.size()) + "\r\n" + response + "\r\n";
      }
    } else if (parser_list[0] == "CONFIG") {
      if (parser_list[1] == "GET") {
        response = key_value_storage->get(parser_list[2], 0);
        if (response == "") {
          response = "$-1\r\n";
        } else {
          response = "*2\r\n$" + std::to_string(parser_list[2].length()) +
                     "\r\n" + parser_list[2] + "\r\n$" +
                     std::to_string(response.size()) + "\r\n" + response +
                     "\r\n";
        }
      }
    } else if (parser_list[0] == "KEYS") {

      std::vector<std::string> keys = key_value_storage->getAllKey();
      int sz = keys.size();
      response.clear();
      response = '*' + (std::to_string(sz)) + "\r\n";
      for (auto it : keys) {
        response += '$';
        response += (std::to_string(it.size())) + "\r\n";
        response += it + "\r\n";
      }

    } else if (parser_list[0] == "INFO") {

      response = "$11\r\nrole:master\r\n";
      // bool is_replication = false;
      if (argc >= 5) {
        std ::string replica = argv[3];
        if (replica.compare("--replicaof") == 0) {
          // is_replication = true ;

          response = "$10\r\nrole:slave\r\n";
        }
      }
      //std ::cout << "response here" << std::endl;
      std ::string replied_id =
          "master_replid:8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb";
      std ::string master_repl_offset = "master_repl_offset:0";
      std::vector<std::string> replications;
      replications.push_back(response);
      replications.push_back(replied_id);
      replications.push_back(master_repl_offset);
      int sz = 0;
      for(auto it : replications){
        sz += it.size();
      }
      sz += 2*(replications.size()-1);
      std :: string current = response ;
      response.clear();
      response += '$';
      response += std::to_string(sz);
      response +="\r\n";
      //std :: cout << response << std::endl;
      for (auto it : replications) {
        //std :: cout << it << std::endl;
        //response += std::to_string(it.length());
        //response += "\r\n";
        response += it;
        response += "\r\n";
      }
    } else {
      for (int i = 1; i < parser_list.size(); i++) {
        response += '$';
        response += std::to_string(parser_list[i].size());
        response += "\r\n";
        response += parser_list[i] + "\r\n";
      }
    }

    send(client_fd, response.c_str(), response.length(), 0);
  }
  close(client_fd);
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  // for (int i = 0; i < argc; i++) {
  //   std ::cout << argv[i] << std ::endl;
  // }
  std::vector<std::string> argument ;
  for(int i = 0 ;i  < argc ;i ++){
    argument.push_back(argv[i]);
  }

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
  server_addr.sin_port = htons(6379);

  struct sockaddr_in replica_server_addr;
  if (argc == 3) {
    std ::string port_exist = argv[1];
    if (port_exist.compare("--port") == 0) {
      int port_no = (std::stoi)(argv[2]);
      // std :: cout << port_no << std :: endl;
      server_addr.sin_port = htons(port_no);
    }
  }
  else if(argc >= 5 && argument[3].compare("--replicaof") == 0){
    std::string port = argv[4];
     std :: cout << port  << std::endl;
    std :: string port_no = "";
    int id = port.size()-1;
    while(port[id]  >= '0' && port[id] <= '9'){
      port_no += port[id];
      id --;
    }
    std::reverse(port_no.begin(),port_no.end());
   // std :: cout << port_no << std::endl ;
    //std :: cout << port << std::endl;
    struct sockaddr_in back_up_addr;
    back_up_addr.sin_family = AF_INET;
    back_up_addr.sin_addr.s_addr = INADDR_ANY;
    back_up_addr.sin_port = htons(std::stoi(port_no));
    int backup_fd ;
    if((backup_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
       std :: cout << "Error  socket creation" << std :: endl;
       return - 1 ;
    }
    std :: cout << "Connecting to master " << std:: endl;

    int status ;
     if ((status
         = connect(backup_fd, (struct sockaddr*)&back_up_addr,
                   sizeof(back_up_addr)))
        < 0) {
        printf("\nConnection Failed \n");
    }
    else {
    std :: string SEND_PING  = "*1\r\n$4\r\nPING\r\n";
    send(backup_fd,SEND_PING.c_str(),SEND_PING.length(),0);
    server_addr.sin_port = htons(std::stoi(argv[2]));
    }

  }

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) !=
      0) {
    std::cerr << "Failed to bind to port 6379\n";
    return 1;
  }
  // else {
  //   server_addr.sin_port =  htons(6380);
  //   if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr))
  //   !=
  //     0) {
  //   std::cerr << "Failed to bind to port 6380\n";
  //   return 1;
  //     }
  // }

  int connection_backlog = 5;
  if (listen(server_fd, connection_backlog) != 0) {
    std::cerr << "listen failed\n";
    return 1;
  }

  struct sockaddr_in client_addr;
  int client_addr_len = sizeof(client_addr);
  std::cout << "Waiting for a client to connect...\n";
  // You can use print statements as follows for debugging, they'll be visible
  // when running tests.
  std::cout << "Logs from your program will appear here!\n";
  // Uncomment this block to pass the first stage
  //
  std ::string bin_key;
  std ::string bin_value;
  std::vector<std::vector<std::string> > v;

  if (argc >= 5) {
    std::string endpoint = "";
    endpoint += argv[2];
    endpoint += '/';
    endpoint += argv[4];
    RDBParser *rdbParser = new RDBParser();
    v = rdbParser->read_path(endpoint);
  }

  while (true) {

    int client_fd = accept(server_fd, (struct sockaddr *)&client_addr,
                           (socklen_t *)&client_addr_len);

    std::cout << "Client connected\n";

    std::thread th(handle_connect, client_fd, argc, argv, v);

    th.detach();
  }
  close(server_fd);
  return 0;
}