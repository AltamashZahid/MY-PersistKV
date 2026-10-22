// PKQL shell.
//   pkql <dir>                 interactive
//   pkql <dir> script.pkql     run a script file
//   pkql <dir> -c "<script>"   run statements from the command line

#include <fstream>
#include <iostream>
#include <sstream>

#include "pkql/engine.h"

namespace {

bool runScript(pkql::Engine& engine, const std::string& text) {
  try {
    for (const pkql::Result& r : engine.run(text)) std::cout << r.format() << "\n";
    return true;
  } catch (const std::exception& e) {
    std::cout << "error: " << e.what() << "\n";
    return false;
  }
}

// A line that ends with one of these continues on the next line.
bool continues(const std::string& buf) {
  int depth = 0;
  bool in_str = false;
  char quote = 0;
  for (char c : buf) {
    if (in_str) {
      if (c == quote) in_str = false;
      continue;
    }
    if (c == '"' || c == '\'') {
      in_str = true;
      quote = c;
    } else if (c == '(' || c == '[' || c == '{') {
      depth++;
    } else if (c == ')' || c == ']' || c == '}') {
      depth--;
    }
  }
  if (depth > 0) return true;
  size_t end = buf.find_last_not_of(" \t\r\n");
  if (end == std::string::npos) return false;
  return std::string("|,=+-*/(:").find(buf[end]) != std::string::npos;
}

void printHelp() {
  std::cout << R"(PKQL: a pipeline query language over the PersistKV storage engine.
Statements:
  table users (id: int key, name: text, city: text, age: int?)
  index users_city on users (city)
  insert users {id: 1, name: "Ann", city: "Pune", age: 31}, {id: 2, name: "Bo", city: "Delhi"}
  from users | filter age > 30 | sort -age | take 10
  from orders | join users on orders.user_id == users.id | group users.city { n = count() }
  from users | filter id == 1 | update age = age + 1
  from users | filter age == null | delete
  let adults = from users | filter age >= 18        (then: from adults | ...)
  analyze users          gather statistics for the optimizer
  explain <query>        show the plan;  explain analyze <query> also runs it
  show tables | describe users | drop table t | drop index i
End a line with | (or leave a bracket open) to continue on the next line.
)";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: pkql <database dir> [script.pkql | -c \"statements\"]\n";
    return 2;
  }
  try {
    pkql::Engine engine(argv[1]);
    if (argc >= 4 && std::string(argv[2]) == "-c") return runScript(engine, argv[3]) ? 0 : 1;
    if (argc >= 3) {
      std::ifstream in(argv[2]);
      if (!in) {
        std::cerr << "cannot read " << argv[2] << "\n";
        return 2;
      }
      std::stringstream ss;
      ss << in.rdbuf();
      return runScript(engine, ss.str()) ? 0 : 1;
    }

    std::cout << "PKQL on PersistKV. Type `help` for examples, `exit` to quit.\n";
    std::string buf, line;
    while (true) {
      std::cout << (buf.empty() ? "pkql> " : "  ...> ") << std::flush;
      if (!std::getline(std::cin, line)) break;
      if (buf.empty()) {
        if (line == "exit" || line == "quit") break;
        if (line == "help") {
          printHelp();
          continue;
        }
      }
      buf += line + "\n";
      if (continues(buf)) continue;
      if (buf.find_first_not_of(" \t\r\n") != std::string::npos) runScript(engine, buf);
      buf.clear();
    }
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
