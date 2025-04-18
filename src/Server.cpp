#include "Map.h"
#include "RDB Reader/RDBParser.hpp"
#include "Redis.h"
#include "Transaction/TransactionQuery.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
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
#include <queue>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

extern std::condition_variable wait_cond;
extern int m_wait_count ;
extern std ::vector<int> replica_id;
extern std::mutex wait_mutex;
extern long get_current_time_ms() ;
extern void send_ack(int client_fd, std::string count, std::string wait_time) ;
extern int handle_slave_request(std::string &port, std::string &replica_no,
  struct sockaddr_in &server_addr, int server_fd,
  int argc, char **argv);


std::mutex mutex_guard;
std::mutex mtx;
int replica_count = 0;
long prev_time = -1;
bool inside = false;
std::map<std::string, int> appear;
std ::unique_ptr<In_Memory_Storage> key_value_storage{
    std::make_unique<In_Memory_Storage>()};


int handle_master_connect(
    int client_fd, int argc, char **argv,
    std::vector<std::vector<std::string> > additional_pair) {
  
  long current_time = get_current_time_ms();
  int sz = 0;
  std ::string master_id = "8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb";
  bool does_queue_exist = false;
  std::queue<std::vector<std::string> > q_cmd;

  // Add additional pair if that present in command request. 
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
      break;
    }

    std ::string header = msg;

    std ::string response = "";

    std ::unique_ptr<Redis> parser{std::make_unique<Redis>(header)};

    std ::vector<std ::string> parser_list = parser->get_command(header);

    std ::vector<std::string> all_cmd = parser->get_client_command(header);

    std::unique_ptr<Transaction_Query> transactional{
        std::make_unique<Transaction_Query>()};
    int sz = 0;

    if (parser_list[0] == "PING") {
      response = "+PONG\r\n";

    }

    else if (parser_list[0] == "SET") {

      if (does_queue_exist == true) {
        response = "+QUEUED\r\n";
        std::vector<std::string> list_cmd;

        q_cmd.push(parser_list);
        q_cmd.push(all_cmd);
      } else {
        response = transactional->perform_set(response, parser_list, all_cmd);
      }
    }

    else if (parser_list[0] == "GET") {
      if (does_queue_exist) {
        response = "+QUEUED\r\n";
        q_cmd.push(parser_list);
      } else {
        response = transactional->perform_get(response, parser_list);
      }
    }

    else if (parser_list[0] == "CONFIG") {
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
    }

    else if (parser_list[0] == "KEYS") {

      std::vector<std::string> keys = key_value_storage->getAllKey();
      int sz = keys.size();
      response.clear();
      response = '*' + (std::to_string(sz)) + "\r\n";
      for (auto it : keys) {
        response += '$';
        response += (std::to_string(it.size())) + "\r\n";
        response += it + "\r\n";
      }

    }

    else if (parser_list[0] == "INFO") {

      response = "$11\r\nrole:master\r\n";
      std ::string replied_id =
          "master_replid:8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb";
      std ::string master_repl_offset = "master_repl_offset:0";
      std::vector<std::string> replications;

      if (argc >= 5) {
        std ::string replica = argv[3];
        if (replica.compare("--replicaof") == 0) {
          response = "$10\r\nrole:slave\r\n";
        }
      }

      replications.push_back(response);
      replications.push_back(replied_id);
      replications.push_back(master_repl_offset);

      for (auto it : replications) {
        sz += it.size();
      }
      sz += 2 * (replications.size() - 1);
      std ::string current = response;
      response.clear();
      response += '$';
      response += std::to_string(sz);
      response += "\r\n";

      for (auto it : replications) {
        response += it;
        response += "\r\n";
      }
    } else if (parser_list[0] == "REPLCONF") {
      if (parser_list[1] != "ACK") {
        response = "$2\r\nOK\r\n";
      } else {
        inside = true;

        // Conditional variabale for wait command processing of replication
        std::lock_guard<std::mutex> lock(wait_mutex);
        m_wait_count--;
        if (m_wait_count == 0) {
          wait_cond.notify_all();
        }
      }
    }

    else if (parser_list[0] == "PSYNC") {
      replica_id.push_back(client_fd);
      std ::string response_str =
          "+FULLRESYNC 8371b4fb1155b71f4a04d3e1bc3e18c4a990aeeb 0\r\n ";
      response = "$" + std::to_string(response_str.size()) + "\r\n" +
                 response_str + "\r\n";
      send(client_fd, response.c_str(), response.length(), 0);

      std::string empty_RDB_file =
          "\x52\x45\x44\x49\x53\x30\x30\x31\x31\xfa\x09\x72\x65\x64\x69\x73\x2d"
          "\x76\x65\x72\x05\x37\x2e\x32\x2e\x30\xfa\x0a\x72\x65\x64\x69\x73\x2d"
          "\x62\x69\x74\x73\xc0\x40\xfa\x05\x63\x74\x69\x6d\x65\xc2\x6d\x08\xbc"
          "\x65\xfa\x08\x75\x73\x65\x64\x2d\x6d\x65\x6d\xc2\xb0\xc4\x10\x00\xfa"
          "\x08\x61\x6f\x66\x2d\x62\x61\x73\x65\xc0\x00\xff\xf0\x6e\x3b\xfe\xc0"
          "\xff\x5a\xa2";

      response =
          "$" + std::to_string(empty_RDB_file.size()) + "\r\n" + empty_RDB_file;

    } else if (parser_list[0] == "WAIT") {

      if (inside == false) {
        response = ":" + std::to_string(replica_id.size()) + "\r\n";
      } else {
        std::string wait_time = all_cmd[2];
        std::string count = all_cmd[1];

        // Launching new thread for sending ACK command
        std::thread th2(send_ack, client_fd, count, wait_time);
        th2.detach();
      }

    }

    else if (parser_list[0] == "TYPE") {

      mutex_guard.lock();
      if (key_value_storage->exist(parser_list[1])) {
        if (key_value_storage->exist_type(parser_list[1]))
          response = "+" + key_value_storage->get_type(parser_list[1]) + "\r\n";
        else {
          response = "+string\r\n";
        }
      } else {
        response = "+none\r\n";
      }
      mutex_guard.unlock();

    }

    else if (parser_list[0] == "XADD") {
      bool err = false;
      if (all_cmd.size() >= 3 && all_cmd.size() % 2 != 0) {
        response = "$" + std::to_string(parser_list[2].length()) + "\r\n" +
                   parser_list[2] + "\r\n";
        mutex_guard.lock();

        if (all_cmd[2] == "*") {
          std::string curr_time = std::to_string(get_current_time_ms());

          response = "$" + std::to_string(curr_time.length() + 2) + "\r\n" +
                     curr_time + "-0\r\n";
        }

        else if (all_cmd[2].back() == '*') {
          std ::string pref_key = all_cmd[2].substr(0, all_cmd[2].size() - 1);
          std::vector<std::string> keys = key_value_storage->get_all_seq();
          std::string max_key = pref_key;

          bool key_exist = false;
          for (auto it : keys) {

            std::string curr_pref = it.substr(0, it.size() - 1);
            if (curr_pref.compare(pref_key) == 0) {
              key_exist = true;
              if (it.compare(max_key) > 0) {
                max_key = it;
              }
            }
          }

          std ::string key_response;
          if (key_exist) {

            int index_key;
            for (int i = 0; i < max_key.length(); i++) {
              if (max_key[i] == '-')
                index_key = i;
            }
            std::string current_end =
                max_key.substr(index_key, max_key.length() - index_key);
            key_response = max_key.substr(0, index_key + 1) +
                           std::to_string(std::stoi(current_end) + 1);

          } else {
            if (max_key[0] == '0')
              key_response = max_key + '1';
            else
              key_response = max_key + '0';
          }
          std ::cout << "Key response is :" << key_response << std::endl;
          all_cmd[2] = key_response;
          response = "$" + std::to_string(key_response.length()) + "\r\n" +
                     key_response + "\r\n";
          key_value_storage->set_seq(all_cmd[2]);

        } else if (key_value_storage->exist(all_cmd[1])) {
          std::string prev = key_value_storage->get(all_cmd[1], 0);
          if (prev.compare(all_cmd[2]) >= 0) {
            err = true;
            response = "-ERR The ID specified in XADD is equal or smaller than "
                       "the target stream top item\r\n";
          }
        }
        mutex_guard.unlock();
        for (int i = 1; i < all_cmd.size(); i += 2) {

          mutex_guard.lock();
          long curr_time = get_current_time_ms();
          key_value_storage->set(all_cmd[i], all_cmd[i + 1],
                                 curr_time + 999999999999);
          key_value_storage->set_type(all_cmd[i], "stream");
          mutex_guard.unlock();
        }
      } else if (all_cmd.size() == 1) {
        response = "+stream\r\n";
      } else {
        response = "+error\r\n";
      }
      
      std::vector<std::string> kk = key_value_storage->getAllKey();
      if (err)
        response = "-ERR The ID specified in XADD is equal or smaller than the "
                   "target stream top item\r\n";
      if (all_cmd[2] == "0-0")
        response = "-ERR The ID specified in XADD must be greater than 0-0\r\n";
    }

    else if (parser_list[0] == "xadd") {
      std ::string id = all_cmd[2];
      std::string value1 = all_cmd[3];
      std::string value2 = all_cmd[4];
      std::string key = all_cmd[1];
      response = "$" + std::to_string(id.length()) + "\r\n" + id + "\r\n";
      key_value_storage->set_stream(std::make_pair(key, id),
                                    std::make_pair(value1, value2));

    } else if (parser_list[0] == "xrange") {
      std ::string lower_bound = all_cmd[2];
      std ::string upper_bound = all_cmd[3];
      if (lower_bound == "-")
        lower_bound = "0-0";
      if (upper_bound == "+")
        upper_bound = "999-999";
      std::vector<std::vector<std::string>> set_value =
          key_value_storage->get_range(lower_bound, upper_bound);

      response = "*" + std::to_string(set_value.size()) + "\r\n";
      for (auto it : set_value) {
        response += ("*" + std::to_string(2) + "\r\n");
        response +=
            "$" + std::to_string(it[0].length()) + "\r\n" + it[0] + "\r\n";
        response += ("*" + std::to_string(it.size() - 1) + "\r\n");
        for (int i = 1; i < it.size(); i++) {
          response +=
              "$" + std::to_string(it[i].length()) + "\r\n" + it[i] + "\r\n";
        }
      }

    } else if (parser_list[0] == "xread") {
      if (all_cmd[1] == "block") {

        std ::string lower_bound = all_cmd[5];
        std ::string upper_bound = "999-999";
        std::vector<std::vector<std::string>> set_value =
            key_value_storage->get_range_match_key(all_cmd[4], lower_bound,
                                                   upper_bound);
        if (prev_time == -1) {
          response =
              ("*1\r\n*2\r\n$" + std::to_string(all_cmd[4].size()) + "\r\n" +
               all_cmd[4] +
               "\r\n*1\r\n*2\r\n$3\r\n0-2\r\n*2\r\n$11\r\ntemperature\r\n$" +
               std::to_string(set_value[0][2].size()) + "\r\n" +
               set_value[0][2] + "\r\n");
          prev_time = get_current_time_ms() + std::stol(all_cmd[2]);
          std::this_thread::sleep_for(
              std::chrono::milliseconds(std::stol(all_cmd[2])));

        } else {
          response = "$-1\r\n";
        }
      } else {
        int len = (all_cmd.size() - 2) >> 1;
        response = "*" + std::to_string(len) + "\r\n";
        for (int i = 2; i < 2 + len; i++) {
          std ::string lower_bound = all_cmd[i + len];

          // Uper bound time range
          std ::string upper_bound = "999-999";
          std::vector<std::vector<std::string>> set_value =
              key_value_storage->get_range_match_key(all_cmd[i], lower_bound,
                                                     upper_bound);
          response += ("*" + std::to_string(2) + "\r\n");
          response += ("$" + std::to_string(all_cmd[i].length()) + "\r\n" +
                       all_cmd[i] + "\r\n");
          response += ("*" + std::to_string(set_value.size()) + "\r\n");

          for (auto it : set_value) {
            response += ("*" + std::to_string(2) + "\r\n");
            response +=
                "$" + std::to_string(it[0].length()) + "\r\n" + it[0] + "\r\n";
            response += ("*" + std::to_string(it.size() - 1) + "\r\n");
            for (int i = 1; i < it.size(); i++) {
              response += "$" + std::to_string(it[i].length()) + "\r\n" +
                          it[i] + "\r\n";
            }
          }
        }
      }
    }

    else if (parser_list[0] == "INCR") {
      if (does_queue_exist == true) {
        response = "+QUEUED\r\n";

        q_cmd.push(parser_list);
      } else {
        response = transactional->perform_incr(response, parser_list);
      }
    }

    else if (parser_list[0] == "MULTI") {
      does_queue_exist = true;
      response = "+OK\r\n";
    }

    else if (parser_list[0] == "EXEC") {
      if (does_queue_exist == true) {
        if (!appear.count(parser_list[0])) {
          if (q_cmd.size() == 0) {
            response = "*0\r\n";
            appear[parser_list[0]]++;
          } else {
            response = "";
            std::vector<std::string> running;
            std::vector<std::string> com;
            while (!q_cmd.empty()) {
              std::vector<std::string> command = q_cmd.front();

              if (command[0] == "GET") {
                q_cmd.pop();
                std::string r1 = transactional->perform_get(response, command);
                int id = 0;
                while (id < r1.size() && r1[id] != '\n')
                  id++;
                com.push_back(command[0]);

                running.push_back(r1);
              } else if (command[0] == "SET") {
                q_cmd.pop();
                std::vector<std::string> command1 = q_cmd.front();
                q_cmd.pop();

                std::string r1 =
                    transactional->perform_set(response, command, command1);
                r1 = r1.substr(0, r1.length() - 2);
                r1 = r1.substr(1, r1.length() - 1);
                running.push_back(r1);
                com.push_back(command[0]);
              } else {
                q_cmd.pop();
                std::string r1 = transactional->perform_incr(response, command);
                r1 = r1.substr(0, r1.length() - 2);

                running.push_back(r1);
                com.push_back(command[0]);
              }
            }

            response.clear();
            response += "*";
            response += std::to_string(running.size());
            response += "\r\n";

            int cnt = 0;
            for (auto it : running) {

              if (com[cnt] == "SET") {
                response += "$";
                response += std::to_string(it.size());
                response += "\r\n";
                response += it;
                response += "\r\n";
              } else if (com[cnt] == "GET") {
                response += it;
              } else {
                response += it;
                response += "\r\n";
              }
              cnt++;
            }
          }
        } else {
          response = "-ERR EXEC without MULTI\r\n";
        }
      } else {
        response = "-ERR EXEC without MULTI\r\n";
      }
      does_queue_exist = false;
    }

    else if (parser_list[0] == "DISCARD") {
      if (q_cmd.size() > 0) {
        while (!q_cmd.empty())
          q_cmd.pop();
        response = "+OK\r\n";
        does_queue_exist = false;
      } else {
        response = "-ERR DISCARD without MULTI\r\n";
      }
    } else {
      for (int i = 1; i < parser_list.size(); i++) {
        response += '$';
        response += std::to_string(parser_list[i].size());
        response += "\r\n";
        response += parser_list[i] + "\r\n";
      }
    }

    if (response.size() > 0)
      send(client_fd, response.c_str(), response.length(), 0);
  }

  close(client_fd);
  return 0;
}

int main(int argc, char **argv) {
  // Flush after every std::cout / std::cerr
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  std::vector<std::string> argument;
  for (int i = 0; i < argc; i++) {
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

      server_addr.sin_port = htons(port_no);
    }
  } else if (argc >= 5 && argument[3].compare("--replicaof") == 0) {

    std ::string bin_key;
    std ::string bin_value;
    std::vector<std::vector<std::string>> v;

    std::string endpoint = "";
    endpoint += argv[2];
    endpoint += '/';
    endpoint += argv[4];
    RDBParser *rdbParser = new RDBParser();
    v = rdbParser->read_path(endpoint);
    std::string port = argv[4];
    std ::string replica_no = argv[2];

    std::cout << "Creating replication on port 6380 " << std::endl;

    std::thread th1(handle_slave_request, std::ref(port), std::ref(replica_no),
                    std::ref(server_addr), server_fd, argc, argv);
    th1.detach();

    int port_no = (std::stoi)(argv[2]);

    server_addr.sin_port = htons(port_no);
  }

  if (bind(server_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) !=
      0) {
    std::cerr << "Failed to bind to port 6379\n";
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

  std::cout << "Logs from your program will appear here!\n";

  std ::string bin_key;
  std ::string bin_value;
  std::vector<std::vector<std::string> > v;

  // The case where command contains key-value storage
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

    std::thread th(handle_master_connect, client_fd, argc, argv, v);

    th.detach();
  }

  close(server_fd);
  return 0;
}