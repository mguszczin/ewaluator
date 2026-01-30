#include <iostream>
#include <vector>
#include <string>
#include <map>
#include <queue>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <cstring>
#include <atomic>
#include <algorithm>
#include <functional>
#include <stdexcept>

#include "defs.h"
#include "err.h"

using namespace std;
using namespace event_data;


std::atomic<bool> stop_requested(false);

void handle_sigint(int) {
    stop_requested = true;
}

pair<int, int> create_pipe() {
    int pd[2];
    if (pipe(pd) != 0) throw std::ios_base::failure("Pipe failed");
    return {pd[0], pd[1]};
}

string read_fixed_string(int fd, size_t size) {
    string s(size, '\0');
    ssize_t r = read(fd, &s[0], size);
    if (r <= 0) return "";
    
    size_t len = strnlen(s.c_str(), r);
    s.resize(len);
    return s;
}

struct EvaluatorData {
    string policy_path;
    string env_path;
    size_t max_active_env;
    size_t max_calls;
    size_t max_policy;

    EvaluatorData(const vector<string>& args) {
        if (args.size() < 5) 
            throw std::invalid_argument("Too few arguments");
        policy_path = args[0];
        env_path = args[1];
        max_policy = stoul(args[2]);
        max_calls = stoul(args[3]);
        max_active_env = stoul(args[4]);
    }
};

class Evaluator {
private:
    EvaluatorData config;
    
    int names_read_fd;
    int event_write_fd;
    pid_t reader_pid;

    size_t active_calls = 0;
    size_t active_policy_calls = 0;
    size_t active_env = 0;
    
    queue<int> new_tests;
    queue<int> wait_env; // Waiting for transfer Policy -> Env
    queue<int> wait_policy; // Waiting for transfer Env -> Policy

    map<int, Test> tests;
    size_t next_test_id = 0;
    size_t next_print_id = 0;

    map<int, PolicyWorker> policy_pool;
    queue<int> free_workers;
    int next_worker_id = 0;

    map<EventType, function<void(Event)>> commands;

    void try_print_results() {
        while (tests.count(next_print_id)) {
            auto& t = tests[next_print_id];

            if (!t.is_calculated) {
                break;
            }

            cout << t.name << " "
                 << t.answer << endl;
            
            cleanup_test(next_print_id);
            next_print_id++;
        }
    }

    int spawn_policy_worker() {
        int pin[2], pout[2];

        if (pipe2(pin, O_CLOEXEC) == -1 || pipe2(pout, O_CLOEXEC) == -1) 
            syserr("pipe2 worker failed");

        pid_t pid = fork();
        if (pid == -1) syserr("fork worker failed");

        if (pid == 0) {
            dup2(pin[0], STDIN_FILENO);
            dup2(pout[1], STDOUT_FILENO);
            
            close(pin[0]); close(pin[1]);
            close(pout[0]); close(pout[1]);
            close(event_write_fd);
            
            execl(config.policy_path.c_str(), config.policy_path.c_str(), NULL);
            
            perror("exec policy failed");
            _exit(1);
        }

        close(pin[0]); 
        close(pout[1]);

        int id = next_worker_id++;
        PolicyWorker w;
        w.id = id;
        w.pid = pid;
        w.pipe_in = pin[1];
        w.pipe_out = pout[0];
        w.is_busy = false;

        policy_pool[id] = w;
        free_workers.push(id);
        
        return id;
    }

    pid_t spawn_reader(int& out_names_fd, int& out_event_write) {
        auto [name_reader, name_writer] = create_pipe();
        auto [event_reader, event_writer] = create_pipe();

        pid_t pid = fork();
        if (pid == -1) throw std::ios_base::failure("Fork reader failed");

        if (pid == 0) {
            close(name_reader); close(event_reader);
            execl("./reader", "./reader", 
                  to_string(name_writer).c_str(), 
                  to_string(event_writer).c_str(), NULL);
            syserr("exec reader failed");
        }

        close(name_writer); 
        if (dup2(event_reader, STDIN_FILENO) == -1) syserr("dup2 failed");
        close(event_reader);

        out_names_fd = name_reader;
        fcntl(out_names_fd, F_SETFD, FD_CLOEXEC);

        out_event_write = event_writer;
        return pid;
    }

    pid_t spawn_waiter(int fd_to_watch, int id) {
        pid_t worker = fork();
        if (worker == -1) throw std::ios_base::failure("fork waiter failed");

        if (worker == 0) { 
            if (dup2(fd_to_watch, STDIN_FILENO) == -1) exit(1);
            close(fd_to_watch);
            
            execl("./worker_waiter", "worker_waiter", 
                  to_string(event_write_fd).c_str(), 
                  to_string(id).c_str(), NULL);
            perror("exec waiter failed");
            exit(1);
        }
        return worker;
    }

    pid_t spawn_copier(int src, int dst, int id, char latch, size_t size) {
        pid_t worker = fork();
        if (worker == -1) syserr("fork copier failed");
        
        if (worker == 0) {
            dup2(src, STDIN_FILENO);
            dup2(dst, STDOUT_FILENO);
            close(src); close(dst);

            execl("./worker_copier", "worker_copier",
                  to_string(event_write_fd).c_str(),
                  to_string(id).c_str(),
                  to_string((int)latch).c_str(),
                  to_string(size).c_str(), NULL);
            perror("exec copier failed");
            _exit(1);
        }
        return worker;
    }

    bool can_start_policy() {
        return !wait_policy.empty() &&
            active_calls < config.max_calls &&
            active_policy_calls < config.max_policy;
    }

    void spawn_environment(int id) {
        auto& test = tests[id];
        
        int p_to_child[2], p_from_child[2];
        if (pipe(p_to_child) == -1 || pipe(p_from_child) == -1) syserr("pipe env");

        test.env_pid = fork();
        if (test.env_pid == -1) syserr("fork env");

        if (test.env_pid == 0) {
            dup2(p_to_child[0], STDIN_FILENO);
            dup2(p_from_child[1], STDOUT_FILENO);

            close(p_to_child[0]); close(p_to_child[1]);
            close(p_from_child[0]); close(p_from_child[1]);
            close(event_write_fd);

            execl(config.env_path.c_str(), config.env_path.c_str(), test.name.c_str(), NULL);
            syserr("exec env failed");
        }

        close(p_to_child[0]);
        close(p_from_child[1]);

        test.env_in = p_to_child[1];
        test.env_out = p_from_child[0];

        fcntl(test.env_in, F_SETFD, FD_CLOEXEC);
        fcntl(test.env_out, F_SETFD, FD_CLOEXEC);

        test.current_worker_pid = spawn_waiter(test.env_out, id);
        test.state = TestState::WAITING_FOR_ENV_OUTPUT;
    }

    void schedule() {
        if (stop_requested) return;

        while (!wait_env.empty() && active_calls < config.max_calls) {
            int id = wait_env.front(); wait_env.pop();
            auto& t = tests[id];
            auto& worker = policy_pool[t.assigned_worker_idx];

            active_calls++;
            t.state = TestState::WAITING_FOR_ENV_OUTPUT;
            t.current_worker_pid = spawn_copier(
                worker.pipe_out, t.env_in, id, t.latch_byte, ACTION_SIZE);
        }

        while (can_start_policy()) {
            int worker_id = -1;

            if (!free_workers.empty()) {
                worker_id = free_workers.front();
                free_workers.pop();
            } 
            else {
                worker_id = spawn_policy_worker();
                free_workers.pop();
            }

            int test_id = wait_policy.front(); wait_policy.pop();
            auto& t = tests[test_id];
            auto& worker = policy_pool[worker_id];

            worker.is_busy = true;
            t.assigned_worker_idx = worker_id;

            active_calls++;
            active_policy_calls++;
            t.state = TestState::WAITING_FOR_POLICY_OUTPUT;
            t.current_worker_pid = spawn_copier(t.env_out, worker.pipe_in, test_id, t.latch_byte, STATE_SIZE);
        }

        while (!new_tests.empty() && active_calls < config.max_calls && active_env < config.max_active_env) {
            int id = new_tests.front(); new_tests.pop();
            
            active_calls++; 
            active_env++;
            spawn_environment(id);
        }
    }

    void handle_new_test(Event e) {
        string name = read_fixed_string(names_read_fd, NAME_SIZE);
        size_t id = next_test_id++;
        
        Test t;
        t.test_id = id;
        t.name = name;
        
        tests[id] = t;
        new_tests.push(id);
    }

    void handle_data_ready(Event e) {
        auto& t = tests[e.test_id];
        t.latch_byte = e.data_byte;
        
        if (t.current_worker_pid > 0) {
            waitpid(t.current_worker_pid, NULL, 0);
            t.current_worker_pid = -1;
        }

        if (active_calls > 0) active_calls--;

        if (t.state == TestState::WAITING_FOR_ENV_OUTPUT) {
            if (e.data_byte == 'T') {
                t.answer = read_fixed_string(t.env_out, STATE_SIZE - 1);
                t.is_calculated = true;
                try_print_results();
            } else {
                t.state = TestState::QUEUED_FOR_GPU;
                wait_policy.push(e.test_id);
            }
        } 
        else if (t.state == TestState::WAITING_FOR_POLICY_OUTPUT) {
            t.state = TestState::QUEUED_FOR_CPU;
            if (active_policy_calls > 0) active_policy_calls--;
            wait_env.push(e.test_id);
        }
    }

    void handle_transfer_done(Event e) {
        auto& t = tests[e.test_id];
        
        if (t.current_worker_pid > 0) {
            waitpid(t.current_worker_pid, NULL, 0);
            t.current_worker_pid = -1;
        }

        if (t.state == TestState::QUEUED_FOR_GPU) {
            t.state = TestState::WAITING_FOR_POLICY_OUTPUT;
            
            auto& worker = policy_pool[t.assigned_worker_idx];
            t.current_worker_pid = spawn_waiter(worker.pipe_out, e.test_id);
        } 
        else if (t.state == TestState::QUEUED_FOR_CPU) {
            int wid = t.assigned_worker_idx;
            if (wid != -1) {
                policy_pool[wid].is_busy = false;
                free_workers.push(wid);
                t.assigned_worker_idx = -1;
            }

            t.state = TestState::WAITING_FOR_ENV_OUTPUT;
            t.current_worker_pid = spawn_waiter(t.env_out, e.test_id);
        }
    }

    void cleanup_test(int id) {
        auto& t = tests[id];
        
        if (t.env_pid > 0) waitpid(t.env_pid, NULL, 0);
        
        if (t.env_in != -1) close(t.env_in);
        if (t.env_out != -1) close(t.env_out);

        tests.erase(id);
        
        if (active_env > 0) active_env--;
    }

    // add proper waiting for the end of file

    void perform_shutdown() {
        for (auto& [id, t] : tests) {
            if (t.env_pid > 0) kill(t.env_pid, SIGINT);
            if (t.current_worker_pid > 0) kill(t.current_worker_pid, SIGINT);
            
            if (t.env_in != -1) close(t.env_in);
            if (t.env_out != -1) close(t.env_out);
        }
        
        for (auto& [wid, worker] : policy_pool) {
            kill(worker.pid, SIGINT);
            close(worker.pipe_in);
            close(worker.pipe_out);
        }

        close(names_read_fd);
        close(event_write_fd);
        kill(reader_pid, SIGINT);
        waitpid(reader_pid, NULL, 0);

        for (auto& [id, t] : tests) {
            if (t.env_pid > 0) waitpid(t.env_pid, NULL, 0);
            if (t.current_worker_pid > 0) waitpid(t.current_worker_pid, NULL, 0);
        }
        
        for (auto& [wid, worker] : policy_pool) {
            waitpid(worker.pid, NULL, 0);
        }
    }

public:
    Evaluator(EvaluatorData data) : config(std::move(data)) {
        commands = {
            { EventType::NEW_TEST, [this](Event e) { this->handle_new_test(e); }},
            { EventType::EVT_DATA_READY, [this](Event e) { this->handle_data_ready(e); }},
            { EventType::EVT_TRANSFER_DONE, [this](Event e) { this->handle_transfer_done(e); }},
            { EventType::STDIN_CLOSED, [](Event e) { stop_requested = true; }},
            { EventType::EVT_ERROR, [](Event e) {
                std::cerr << "Error reported in test " << e.test_id << std::endl;
            }}
        };
    }

    void run() {
        reader_pid = spawn_reader(names_read_fd, event_write_fd);
        
        Event e;
        while (true) {
            if (stop_requested) { perform_shutdown(); break; }

            ssize_t n = read(STDIN_FILENO, &e, sizeof(e));

            if (stop_requested || (n < 0 && errno == EINTR)) {
                perform_shutdown(); break;
            }
            if (n <= 0) break; // EOF

            if (commands.count(e.type)) {
                commands.at(e.type)(e);
            }
            schedule();
        }
        close(event_write_fd);
    }
};

int main(int argc, char* argv[]) {
    // Obsługa sygnałów
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN); 

    try {
        utils::ParseReader parser(argc, argv);
        EvaluatorData config(parser.get_args());
        Evaluator app(config);
        app.run();
    } catch (...) {
        cerr << "Unknown Error" << endl;
        return 1;
    }

    return 0;
}