#include <deque>
#include <iostream>
#include <signal.h>
#include <string>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <fcntl.h>
#include <sys/wait.h>
#include <set>
#include <queue>
#include <map>
#include <vector>

#include "defs.h"
#include "err.h"

namespace {

using std::cin;
using std::invalid_argument;
using std::move;
using std::pair;
using std::set;
using std::string;
using std::stringstream;
using std::to_string;
using std::queue;
using std::unordered_map;
using std::vector;
using std::map;

string read_buf(int fd, size_t size) { 
    std::string buf;
    buf.resize(size);

    ssize_t bytes_read = read(fd, &buf[0], size);

    if (bytes_read < 0) {
        throw std::ios_base::failure("Error in `read` inside `read_buf`");
    }

    return std::move(buf);
}

pair<int, int> create_pipe() {
    int pd[2];
    if (pipe(pd) != 0) {
        throw std::ios_base::failure(
        "pipe function wrong return code");
    }

    return pair{pd[0], pd[1]};
}

pid_t set_up_reader(int& out_names_read_fd) 
{
    auto [names_read, names_write] = create_pipe();
    auto [event_read, event_write] = create_pipe();

    pid_t pid;
    if ((pid = fork()) == -1) {
        throw std::ios_base::failure("Fork failed inside `set_up_reader()` function");
    }

    if (pid == 0) {
        close(names_read);
        close(event_read);

        string s_names_fd = to_string(names_write);
        string s_event_fd = to_string(event_write);

        char* args[] = {
            (char*)"./reader",
            (char*)s_names_fd.c_str(),
            (char*)s_event_fd.c_str(),
            nullptr
        };

        execv("./reader", args);

        perror("FATAL: execv failed in set_up_reader");
        exit(1);

    } else {
        close(names_write);
        close(event_write);

        if (dup2(event_read, STDIN_FILENO) == -1) {
            perror("dup2 failed");
            throw std::ios_base::failure("dup2 failed");
        }

        close(event_read);

        out_names_read_fd = names_read;
        fcntl(out_names_read_fd, F_SETFD, FD_CLOEXEC);

        return pid;
    }
}

struct EvaluatorData {
    string policy_path;
    string env_path;
    int max_concurrent_policy_calls;
    int max_concurrent_calls;
    int max_active_env;
    vector<string> extra_arguments;

    EvaluatorData(vector<string> args)
        : policy_path(move(args[1])), 
        env_path(move(args[2])), 
        max_concurrent_policy_calls(stoi(args[3])),
        max_concurrent_calls(stoi(args[4])),
        max_active_env(stoi(args[5]))
    {
        if (args.size() < 5) {
                throw invalid_argument(
                    "Evaluator Requires at least 5 arguments");
            }
            using std::stoi;
            std::copy(args.begin() + 6, args.end(), 
                                    std::back_inserter(extra_arguments));
    }
};

class Evaluator {
private:
    // --- KONFIGURACJA ---
    const EvaluatorData eval_info;
    int names_read_fd;      // Rura z nazwami (od Readera)
    pid_t reader_pid;

    size_t count_active_policies{0};
    size_t count_active_envs{0};
    size_t count_cpu_busy{0};

    std::map<int, event_data::Test> test_db;

    std::queue<int> q_new_tests;
    std::queue<int> q_waiting_for_gpu;
    std::queue<int> q_waiting_for_cpu;

    size_t next_test_id = 0;

        bool can_start_new_env() const {
            return (count_active_envs < eval_info.max_active_env) &&
                (count_cpu_busy < eval_info.max_concurrent_calls);
            }

        bool can_run_policy() const {
            return (count_active_policies < eval_info.max_concurrent_policy_calls) &&
                (count_cpu_busy < eval_info.max_concurrent_calls);
        }

        bool can_resume_env() const {
            return (count_cpu_busy < eval_info.max_concurrent_calls);
        }


        void handle_new_test() {
            string s = read_buf(names_read_fd, NAME_SIZE);

            event_data::Test t = 
                    event_data::Test{.test_id = next_test_id++, .name = s};

            test_db.emplace(t.test_id, t);

            
        }
    
    public: 
        Evaluator(EvaluatorData args): eval_info(move(args)) {};

        void start()
        {
            using event_data::Event;
            using event_data::EventType;

            set_up_reader(names_read_fd);

            Event event;
            while(cin.read(reinterpret_cast<char*>(&event), sizeof(event))) {
                if (event.type == EventType::NEW_TEST) {
                    handle_new_test();
                }
            }
        }
};

}

int main(int argc, char** argv)
{
    try {
        utils::ParseReader reader(argc, argv);
        Evaluator eval(EvaluatorData(reader.get_args()));
        eval.start();
    } catch(...) { // correct this
        std::cerr << "OH NO EXCEPTION" << std::endl;
        return 1;
    }

}