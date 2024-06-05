#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <grp.h>
#include <iostream>
#include <fstream>
#include <map>
#include <pwd.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include <readline/readline.h>
#include <readline/history.h>

using namespace std;

const string WHITE_SPACE = " \t\r\n";
const string SYMBOL = "|<>";

#define ANSI_COLOR_RESET   "\x1b[0m"
#define ANSI_COLOR_BLACK   "\x1b[30m"
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_WHITE   "\x1b[37m"

#define CMD_TYPE_NULL 0      // initial value
#define CMD_TYPE_EXEC 1      // common exec command
#define CMD_TYPE_PIPE 2      // pipe command
#define CMD_TYPE_REDIR_IN 4  // redirect using <
#define CMD_TYPE_REDIR_OUT 8 // redirect using >

#define MAX_ARGV_LEN 128
#define SHOW_PANIC true
#define SHOW_WAIT_PANIC false

#define REDIR_IN_OFLAG O_RDONLY
#define REDIR_OUT_OFLAG O_WRONLY | O_CREAT | O_TRUNC

int pipe_fd[2];

#define CHAR_BUF_SIZE 1024
char char_buf[CHAR_BUF_SIZE];


uid_t uid = -1;
passwd* pwd;
string username;
string hostname;
string home_dir;

map<string, string> alias_map;
map<string, string> help_map;
void panic(string hint, bool exit_ = false, int exit_code = 0) {
  if (SHOW_PANIC)
    cerr << "[!flickShell]: " << hint << endl;
  if (exit_)
    exit(exit_code);
}

void init_shell() {
  string filePath = home_dir + "/.flickshrc";
  ifstream usrConfig(filePath);
  if (!usrConfig.good()) {
    usrConfig.close();
    ofstream newConfig(filePath, std::ios::app);

    if (!newConfig.is_open()) {
      panic("Failed to open file: "+filePath);
      exit(1);
    }else{
      newConfig << "# ./flickshrc"<<endl;
    }

  }
}
void init_alias() { alias_map.insert({ {"ll", "ls -l"} }); }
void init_help() { help_map.insert({ {"cd","cd: cd [-L|[-P [-e]] [-@]] [dir]\n    Change the shell working directory.\n    Change the current directory to DIR.  The default DIR is the value of the\n    HOME shell variable."} }); }

// ==========================
// string utilities
// ==========================
bool is_white_space(char ch) { return WHITE_SPACE.find(ch) != -1; }

bool is_symbol(char ch) { return SYMBOL.find(ch) != -1; }

vector<string> string_split(const string& s, const string& delims) {
  vector<string> vec;
  int p = 0, q;
  while ((q = s.find_first_of(delims, p)) != string::npos) {
    if (q > p)
      vec.push_back(s.substr(p, q - p));
    p = q + 1;
  }
  if (p < s.length())
    vec.push_back(s.substr(p));
  return vec;
}

// this split function will protect string inside quote
vector<string> string_split_protect(const string& str, const string& delims) {
  vector<string> vec;
  string tmp = "";
  for (int i = 0; i < str.length(); i++) {
    if (is_white_space(str[i])) {
      vec.push_back(tmp);
      tmp = "";
    } else if (str[i] == '\"') {
      i++; // skip "
      while (str[i] != '\"' && i < str.length()) {
        tmp.push_back(str[i]);
        i++;
      }
      if (i == str.length())
        panic("unclosed quote");
    } else {
      tmp.push_back(str[i]);
    }
  }
  if (tmp.length() > 0) vec.push_back(tmp);
  return vec;
}

string string_split_last(const string& s, const string& delims) {
  int q;
  if ((q = s.find_last_of(delims)) != string::npos) {
    return s.substr(q + 1, s.length());
  }
  return s;
}

string string_split_first(const string& s, const string& delims) {
  int q;
  if ((q = s.find_first_of(delims)) != string::npos) {
    return s.substr(0, q);
  }
  return s;
}

string trim(const string& s) {
  if (s.length() == 0)
    return string(s);
  int p = 0, q = s.length() - 1;
  while (is_white_space(s[p]))
    p++;
  while (is_white_space(s[q]))
    q--;
  return s.substr(p, q - p + 1);
}

string prompt;

// ==========================
// show the command prompt in front of each line
// **example** [root@localhost tmp]>
// ==========================
void set_command_prompt() {
  uid_t new_uid = getuid();
  if (new_uid != uid) {
    uid = new_uid;
    pwd = getpwuid(uid);
    username = (pwd->pw_name);
    gethostname(char_buf, CHAR_BUF_SIZE);
    hostname = char_buf;
    // sometimes, hostname is like localhost.locald.xxx here, should split it
    hostname = string_split_first(hostname, ".");

    // consider home path (~)
    if (username == "root")
      home_dir = "/root"; // home for root
    else {
      home_dir = "/home/";
      home_dir.append(username); // home for other user
    }
  }

  // get current working directory
  getcwd(char_buf, CHAR_BUF_SIZE);
  string cwd(char_buf);

  if (cwd == home_dir)
    cwd = "~";
  else if (cwd != "/") {
    // consider root path (/)
    // keep only the last level of directory
    cwd = string_split_last(cwd, "/");
  }

  //prompt = getenv("P1");

  // output
  string x = "[";
  x.append(username);
  x.push_back('@');
  x.append(hostname);
  x.push_back(' ');
  x.append(cwd);
  x.append("]>");
  prompt = x;
}

// ==========================
// proxy functions
// ==========================
// wrapped fork function that panics
int fork_wrap() {
  int pid = fork();
  if (pid == -1)
    panic("fork failed.", true, 1);
  return pid;
}

// wrapped pipe function that panics
int pipe_wrap(int pipe_fd[2]) {
  int ret = pipe(pipe_fd);
  if (ret == -1)
    panic("pipe failed", true, 1);
  return ret;
}

// wrapped dup2 function that panics
int dup2_wrap(int fd1, int fd2) {
  int dup2_ret = dup2(fd1, fd2);
  if (dup2_ret < 0)
    panic("dup2 failed.", true, 1);
  return dup2_ret;
}

// wrapped open function that panics
int open_wrap(const char* file, int oflag) {
  int open_ret = open(file, oflag);
  if (open_ret < 0)
    panic("open failed.", true, 1);
  return open_ret;
}

// panic for wait status
void check_wait_status(int& wait_status) {
  if (WIFEXITED(wait_status) == 0) { // means abnormal exit
    char buf[8];
    sprintf(buf, "%d", WEXITSTATUS(wait_status));
    if (SHOW_WAIT_PANIC)
      panic("child exit with code " + string(buf));
  }
}

// ==========================
// command line parsing
// ==========================

// base class for any cmd
class cmd {
public:
  int type;
  cmd() { this->type = CMD_TYPE_NULL; }
};

// most common type of cmd
// argv[0] ...argv[1~n]
class exec_cmd : public cmd {
public:
  vector<string> argv;
  exec_cmd(vector<string>& argv) {
    this->type = CMD_TYPE_EXEC;
    this->argv = vector<string>(argv);
  }
};

// pipe cmd
// left | right
class pipe_cmd : public cmd {
public:
  cmd* left;
  cmd* right;
  pipe_cmd() { this->type = CMD_TYPE_PIPE; }
  pipe_cmd(cmd* left, cmd* right) {
    this->type = CMD_TYPE_PIPE;
    this->left = left;
    this->right = right;
  }
};

// redirect cmd
// ls > a.txt; some_program < b.txt
class redirect_cmd : public cmd {
public:
  cmd* cmd_;
  string file;
  int fd;
  redirect_cmd() {}
  redirect_cmd(int type, cmd* cmd_, string file, int fd) {
    this->type = type;
    this->cmd_ = cmd_;
    this->file = file;
    this->fd = fd;
  }
};

// parse seg as is exec_cmd
cmd* parse_exec_cmd(string seg) {
  seg = trim(seg);
  vector<string> argv = string_split_protect(seg, WHITE_SPACE);
  return new exec_cmd(argv);
}

// divide-and-conquer
// **test cases:**
// ls -a < a.txt | grep linux > b.txt
// some_bin "hello world" > b.txt > c.txt
cmd* parse(string line) {
  line = trim(line);
  string cur_read = "";
  cmd* cur_cmd = new cmd();
  int i = 0;
  while (i < line.length()) {
    if (line[i] == '<' || line[i] == '>') {
      cmd* lhs = parse_exec_cmd(cur_read); // [lhs] < (or >) [rhs]
      int j = i + 1;
      while (j < line.length() && !is_symbol(line[j]))
        j++;
      string file = trim(line.substr(i + 1, j - i));
      cur_cmd = new redirect_cmd(line[i] == '<' ? CMD_TYPE_REDIR_IN
        : CMD_TYPE_REDIR_OUT,
        lhs, file, -1); // fd wait for filling
      i = j;
    } else if (line[i] == '|') {
      cmd* rhs = parse(line.substr(i + 1)); // recursive
      if (cur_cmd->type == CMD_TYPE_NULL)
        cur_cmd = parse_exec_cmd(cur_read);
      cur_cmd = new pipe_cmd(cur_cmd, rhs);
      return cur_cmd;
    } else
      cur_read += line[i++];
  }
  if (cur_cmd->type == CMD_TYPE_NULL)
    return parse_exec_cmd(cur_read);
  else
    return cur_cmd;
}

// deal with builtin command
// returns: 0-nothing_done, 1-success, -1-failure
int process_builtin_command(string line) {
  vector<string> args = string_split(line, WHITE_SPACE);
  // 1 - cd
  if (args[0] == "cd") {
    string route;
    int chdir_ret;
    switch (args.size()) {
    case 1:
      chdir(home_dir.c_str()); // single cd means cd ~
      set_command_prompt();
      return 1;
    case 2:
      route = args[1];
      if (route.find("~") == 0) {
        route = home_dir + route.substr(1);
      }
      // change directory
      chdir_ret = chdir(route.c_str());
      if (chdir_ret < 0) {
        panic("chdir failed");
        return -1;
      }
      set_command_prompt();
      return 1; // successfully processed
    default:
      panic("too many arguments");
      return -1;
    }
  }
  // 2 - help
  if (args[0] == "help") {
    string key;
    switch (args.size()) {
    case 1:
      cout << "" << endl;
      return 1;
    case 2:
      if (args[1].size() > 256) {
        panic("input out of limit");
        return -1;
      }
      try {
        // 尝试直接访问元素
        key = args[1];
        std::string value = help_map.at(key);
        std::cout << value << std::endl;
      } catch (const std::out_of_range& e) {
        std::snprintf(char_buf, CHAR_BUF_SIZE, "no help topics match `%s'.  Try `help help' or `man -k %s' or `info %s'.", key.c_str(), key.c_str(), key.c_str());
        panic(char_buf);
      }
      return 1;
      //break;
    default:
      panic("too many arguments");
      return -1;
    }
  }
  // 3 - alias
  // 3.5 - unalias
  // 4 - history
  if (args[0] == "history") {
    if (args.size() != 1) {
      panic("too many arguments");
      return -1;
    }
    for (int i = history_base; i < history_length; ++i) {
      // 获取第i条历史记录
      HIST_ENTRY* history_entry = history_get(i);

      // 输出历史记录
      if (history_entry != NULL) {
        printf("%5d  %s\n", i + 1, history_entry->line);
      }
    }
    return 1;
  }

  // 5 - type

  // 6 - exit
  if (args[0] == "exit") {
    switch (args.size()) {
    case 1:
      cout << "Bye from flickShell." << endl;
      exit(0);
      break;
    case 2:
      try {
        int num = stoi(args[1]);
        exit(num);
      } catch (const invalid_argument& e) {
        panic("Invalid argument");
        return -1;
      }
      break;
    default:
      panic("too many arguments");
      return -1;
    }
  }
  // 下面几个命令没有要求实现
  // 7 - jobs
  // 8 - job_spec
  // 9 - exec
  return 0; // nothing done
}

// run some cmd
void run_cmd(cmd* cmd_) {
  switch (cmd_->type) {
  case CMD_TYPE_EXEC:
  {
    exec_cmd* ecmd = static_cast<exec_cmd*>(cmd_);
    // process alias
    if (alias_map.count(ecmd->argv[0]) != 0) {
      vector<string> arg0_replace =
        string_split(alias_map.at(ecmd->argv[0]), WHITE_SPACE);
      ecmd->argv.erase(ecmd->argv.begin());
      for (vector<string>::reverse_iterator it = arg0_replace.rbegin();
        it < arg0_replace.rend(); it++) {
        ecmd->argv.insert(ecmd->argv.begin(), (*it));
      }
    }
    // prepare vector<string> for execvp
    vector<char*> argv_c_str;
    for (int i = 0; i < ecmd->argv.size(); i++) {
      string arg_trim = trim(ecmd->argv[i]);
      if (arg_trim.length() > 0) { // skip blank string
        char* tmp = new char[MAX_ARGV_LEN];
        strcpy(tmp, arg_trim.c_str());
        argv_c_str.push_back(tmp);
      }
    }
    argv_c_str.push_back(NULL);
    char** argv_c_arr = &argv_c_str[0];
    // vscode made wrong marco expansion here
    // second argument is ok for char** rather than char *const (*(*)())[]
    int execvp_ret = execvp(argv_c_arr[0], argv_c_arr);
    if (execvp_ret < 0)
      panic("execvp failed");
    break;
  }
  case CMD_TYPE_PIPE:
  {
    pipe_cmd* pcmd = static_cast<pipe_cmd*>(cmd_);
    pipe_wrap(pipe_fd);
    // fork twice to run lhs and rhs of pipe
    if (fork_wrap() == 0) {
      // i'm a child, let's satisfy lhs
      close(pipe_fd[0]);
      dup2_wrap(pipe_fd[1], fileno(stdout)); // lhs_stdout -> pipe_write
      // close the original ones
      run_cmd(pcmd->left);
      close(pipe_fd[1]);
    }
    if (fork_wrap() == 0) {
      // i'm also a child, let's satisfy rhs
      close(pipe_fd[1]);
      dup2_wrap(pipe_fd[0], fileno(stdin)); // pipe_read -> rhs_stdin
      run_cmd(pcmd->right);
      close(pipe_fd[0]);
    }
    // really good. now we have lhs_stdout -> pipe -> rhs_stdin
    // if fork > 0, then i'm the father
    // let's wait for my children
    // close()会等待IO操作。
    close(pipe_fd[0]);
    close(pipe_fd[1]);
    // 等待两个子进程退出
    int wait_status_1, wait_status_2;
    wait(&wait_status_1);
    wait(&wait_status_2);
    // 打印子进程的退出信息
    check_wait_status(wait_status_1);
    check_wait_status(wait_status_2);
    break;
  }
  case CMD_TYPE_REDIR_IN:
    // 可以在这里初始化flag
  case CMD_TYPE_REDIR_OUT:
  {
    redirect_cmd* rcmd = static_cast<redirect_cmd*>(cmd_);
    if (fork_wrap() == 0) {
      // i'm a child, let's satisfy the file being redirected to (or from)
      if (rcmd->type == CMD_TYPE_REDIR_IN) {
        rcmd->fd = open_wrap(rcmd->file.c_str(), REDIR_IN_OFLAG);
        dup2_wrap(rcmd->fd, rcmd->type == fileno(stdin));
      } else {
        rcmd->fd = open_wrap(rcmd->file.c_str(), REDIR_OUT_OFLAG);
        dup2_wrap(rcmd->fd, rcmd->type == fileno(stdout));
      }
      run_cmd(rcmd->cmd_);
      close(rcmd->fd);
    }
    // if fork > 0, then i'm the father
    // let's wait for my children
    int wait_status;
    wait(&wait_status);
    check_wait_status(wait_status);
    break;
  }
  default:
    panic("unknown or null cmd type", true, 1);
  }
}

int main() {
  rl_initialize();
  using_history();
  fflush(stdout);
  rl_on_new_line();
  rl_bind_key('\t', rl_complete);

  set_command_prompt();
  init_shell();
  init_alias();
  init_help();

  string line;
  int wait_status;
  while (true) {
    line = trim(readline(prompt.c_str()));
    if (line.empty())continue;
    add_history(line.c_str());
    // deal with builtin commands
    if (process_builtin_command(line) > 0)
      continue;
    // fork a new me to execute the typed command
    if (fork_wrap() == 0) {
      cmd* cmd_ = parse(line);
      run_cmd(cmd_);
      exit(0); // child exit
    }
    wait(&wait_status);
    check_wait_status(wait_status);
  }
  return 0;
}
