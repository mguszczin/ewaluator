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
#include "error.h"

using namespace std;
using namespace event_data;


atomic<bool> stop_requested(false);
atomic<bool> shut_soft(false);

void handle_sigint(int) {
    stop_requested = true;
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
    
    int names_read_fd = -1;
    int event_write_fd = -1;
    pid_t reader_pid = -1;

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

    void try_to_close(int& fd) {
        if (fd > 0) ASSERT_SYS_OK(close(fd));
        fd = -1;
    }

    void try_to_waitpid(int& pid) {
        if (pid > 0) ASSERT_SYS_OK(waitpid(pid, NULL, 0));
        pid = -1;
    }

    void try_to_pipe(int (&pd)[2]) {
        if (pipe(pd) == -1) {
            terminate_evaluator();
            throw std::ios_base::failure("pipe2_failed");
        }
    }

    void try_to_pipe2(int (&pd)[2]) {
        if (pipe2(pd, O_CLOEXEC) == -1) {
            terminate_evaluator();
            throw std::ios_base::failure("pipe2_failed");
        }
    }

    void try_to_dup2(int from, int to) {
        if (dup2(from, to) == -1) {
            terminate_evaluator();
            throw(std::ios_base::failure("dup2_failed"));
        }
    }

    pid_t try_to_fork() {
        pid_t pid = fork();

        if (pid == -1) {
            terminate_evaluator();
            throw std::ios_base::failure("Fork failed");
        }
    }

    void try_print_results() {
        while (tests.count(next_print_id)) {
            auto& t = tests[next_print_id];

            if (!t.is_calculated) {
                break;
            }

            cout << t.name << " "
                 << t.answer << endl;
            
            tests.erase(next_print_id);
            next_print_id++;
        }
    }

    int spawn_policy_worker() {
        int pin[2], pout[2];

        try_to_pipe2(pin);
        try {
            try_to_pipe2(pout);
        } catch(...) {
            try_to_close(pin[0]);
            try_to_close(pin[1]);
            throw;
        }

        pid_t pid = try_to_fork();

        if (pid == 0) {
            ASSERT_SYS_OK(dup2(pin[0], STDIN_FILENO));
            ASSERT_SYS_OK(dup2(pout[1], STDOUT_FILENO));

            ASSERT_SYS_OK(close(pin[0]));
            ASSERT_SYS_OK(close(pin[1]));
            ASSERT_SYS_OK(close(pout[0])); 
            ASSERT_SYS_OK(close(pout[1]));
            ASSERT_SYS_OK(close(event_write_fd));
            
            ASSERT_SYS_OK(execl(config.policy_path.c_str(), config.policy_path.c_str(), NULL));
            
            perror("exec policy failed");
            exit(1);
        }

        try_to_close(pin[0]); 
        try_to_close(pout[1]);

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
        int f[2], s[2];
        try_to_pipe(f);
        try {
            try_to_pipe(s);
        } catch(...) {
            try_to_close(f[0]);
            try_to_close(f[1]);
        }

        auto [name_reader, name_writer] = f;
        auto [event_reader, event_writer] = s;

        pid_t pid = try_to_fork();

        if (pid == 0) {
            ASSERT_SYS_OK(close(name_reader));
            ASSERT_SYS_OK(close(event_reader));

            ASSERT_SYS_OK(execl("./reader", "./reader", 
                  to_string(name_writer).c_str(), 
                  to_string(event_writer).c_str(), NULL));
            syserr("exec reader failed");
        }

        try_to_close(name_writer); 
        try_to_dup2(event_reader, STDIN_FILENO);
        try_to_close(event_reader);

        out_names_fd = name_reader;
        fcntl(out_names_fd, F_SETFD, FD_CLOEXEC);

        out_event_write = event_writer;
        return pid;
    }

    pid_t spawn_waiter(int fd_to_watch, int id) {
        pid_t worker = try_to_fork();

        if (worker == 0) { 
            ASSERT_SYS_OK(dup2(fd_to_watch, STDIN_FILENO));
            ASSERT_SYS_OK(close(fd_to_watch));
            
            ASSERT_SYS_OK(execl("./worker_waiter", "worker_waiter", 
                  to_string(event_write_fd).c_str(), 
                  to_string(id).c_str(), NULL));
            perror("exec waiter failed");
            exit(1);
        }
        return worker;
    }

    pid_t spawn_copier(int src, int dst, int id, char latch, size_t size) {
        pid_t worker = try_to_fork();
        
        if (worker == 0) {
            ASSERT_SYS_OK(dup2(src, STDIN_FILENO));
            ASSERT_SYS_OK(dup2(dst, STDOUT_FILENO));
            ASSERT_SYS_OK(close(src)); 
            ASSERT_SYS_OK(close(dst));

            ASSERT_SYS_OK(execl("./worker_copier", "worker_copier",
                  to_string(event_write_fd).c_str(),
                  to_string(id).c_str(),
                  to_string((int)latch).c_str(),
                  to_string(size).c_str(), NULL));
            perror("exec copier failed");
            exit(1);
        }
        return worker;
    }

    void spawn_environment(int id) {
        auto& test = tests[id];
        
        int p_to_child[2], p_from_child[2];
        try_to_pipe2(p_to_child);
        try {
            try_to_pipe2(p_from_child);
        } catch(...) {
            try_to_close(p_from_child[0]);
            try_to_close(p_from_child[1]);
        }

        test.env_pid = try_to_fork();

        if (test.env_pid == 0) {
            ASSERT_SYS_OK(dup2(p_to_child[0], STDIN_FILENO));
            ASSERT_SYS_OK(dup2(p_from_child[1], STDOUT_FILENO));

            ASSERT_SYS_OK(close(p_to_child[0]));
            ASSERT_SYS_OK(close(p_to_child[1]));
            ASSERT_SYS_OK(close(p_from_child[0])); 
            ASSERT_SYS_OK(close(p_from_child[1]));
            ASSERT_SYS_OK(close(event_write_fd));

            execl(config.env_path.c_str(), config.env_path.c_str(), test.name.c_str(), NULL);
            syserr("exec env failed");
        }

        try_to_close(p_to_child[0]);
        try_to_close(p_from_child[1]);

        test.env_in = p_to_child[1];
        test.env_out = p_from_child[0];

        test.current_worker_pid = spawn_waiter(test.env_out, id);
        test.state = TestState::WAITING_FOR_ENV_OUTPUT;
    }

    bool can_start_policy() {
        return !wait_policy.empty() &&
            active_calls < config.max_calls &&
            active_policy_calls < config.max_policy;
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
        
        try_to_waitpid(t.current_worker_pid);

        if (active_calls > 0) active_calls--;

        if (t.state == TestState::WAITING_FOR_ENV_OUTPUT) {
            if (e.data_byte == 'T') {
                t.answer = read_fixed_string(t.env_out, STATE_SIZE - 1);
                t.is_calculated = true;
                cleanup_test(t.test_id);
                try_print_results();
            } else {
                t.state = TestState::QUEUED_FOR_ENV;
                wait_policy.push(e.test_id);
            }
        } 
        else if (t.state == TestState::WAITING_FOR_POLICY_OUTPUT) {
            t.state = TestState::QUEUED_FOR_ENV;
            if (active_policy_calls > 0) active_policy_calls--;
            wait_env.push(e.test_id);
        }
    }

    void handle_transfer_done(Event e) {
        auto& t = tests[e.test_id];
        
        if (t.current_worker_pid > 0) {
            try_to_waitpid(t.current_worker_pid);
            t.current_worker_pid = -1;
        }

        if (t.state == TestState::QUEUED_FOR_ENV) {
            t.state = TestState::WAITING_FOR_POLICY_OUTPUT;
            
            auto& worker = policy_pool[t.assigned_worker_idx];
            t.current_worker_pid = spawn_waiter(worker.pipe_out, e.test_id);
        } 
        else if (t.state == TestState::QUEUED_FOR_ENV) {
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
        
        if (t.env_pid > 0) try_to_waitpid(t.env_pid);
        
        if (t.env_in != -1) try_to_close(t.env_in);
        if (t.env_out != -1) try_to_close(t.env_out);
        
        if (active_env > 0) active_env--;
    }

    void soft_shutdown() {
        shut_soft = true;
        kill(reader_pid, SIGINT);
        try_to_waitpid(reader_pid);
        reader_pid = -1;
    }

    void clean_all() {
        for (auto& [id, t] : tests) {
            try_to_close(t.env_in);
            try_to_close(t.env_out);
        }
        
        for (auto& [wid, worker] : policy_pool) {
            try_to_close(worker.pipe_in);
            try_to_close(worker.pipe_out);
        }

        try_to_close(names_read_fd);
        try_to_close(event_write_fd);

        try_to_waitpid(reader_pid);

        for (auto& [id, t] : tests) {
            try_to_waitpid(t.env_pid);
            try_to_waitpid(t.current_worker_pid);
        }
        
        for (auto& [wid, worker] : policy_pool) {
            try_to_waitpid(worker.pid);
        }
    }

    void terminate_evaluator() {
        for (auto& [id, t] : tests) {
            if (t.env_pid > 0) kill(t.env_pid, SIGINT);
            if (t.current_worker_pid > 0) kill(t.current_worker_pid, SIGINT);
        }
        
        for (auto& [wid, worker] : policy_pool) {
            kill(worker.pid, SIGINT);
        }

        kill(reader_pid, SIGINT);

        clean_all();
    }

public:
    Evaluator(EvaluatorData data) : config(std::move(data)) {
        commands = {
            { EventType::NEW_TEST, [this](Event e) { this->handle_new_test(e); }},
            { EventType::EVT_DATA_READY, [this](Event e) { this->handle_data_ready(e); }},
            { EventType::EVT_TRANSFER_DONE, [this](Event e) { this->handle_transfer_done(e); }},
            { EventType::STDIN_CLOSED, [this](Event e) { this->soft_shutdown(); }},
            { EventType::EVT_ERROR, [this](Event e) {
                std::cerr << "Error reported in test " << e.test_id << std::endl;
                this->terminate_evaluator();
                throw std::ios_base::failure("Failure reported");
            }}
        };
    }

    void run() {
        reader_pid = spawn_reader(names_read_fd, event_write_fd);
        
        Event e;
        while (!shut_soft || active_env > 0) {
            if (stop_requested) { terminate_evaluator(); break; }

            ssize_t n = read(STDIN_FILENO, &e, sizeof(e));
            
            if (stop_requested || (n < 0)) {
                terminate_evaluator(); break;
            }
            if (n <= 0) break; // EOF

            if (e.type == EventType::NEW_TEST && shut_soft) {
                continue;
            }

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
        if (stop_requested)
            return 2;
    } catch (...) {
        cerr << "Unknown Error" << endl;
        return 1;
    }

    return 0;
}