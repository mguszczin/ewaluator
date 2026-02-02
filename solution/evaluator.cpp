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
#include <libgen.h>
#include <limits.h>
#include <system_error>
#include <type_traits>
#include <utility>
#include <cerrno>
#include <numeric>

#include "defs.h"
#include "error.h"

/* Not my proudest moment. */
using namespace std;
using namespace event_data;


atomic<bool> stop_requested(false);
atomic<bool> shut_soft(false);

namespace forked_processes {

    void go_to_reader(int event_pipe_fd) {
        signal(SIGINT, SIG_DFL);
        if (event_pipe_fd < 0) {
            cerr << "Error: File descriptor cannot be negative." << endl;
            exit(1);
        }

        string command;

        while (cin >> command) {
            
            vector<char> buffer(NAME_SIZE, 0);
            
            size_t len = command.length();
            if (len > NAME_SIZE) len = NAME_SIZE; 

            command.copy(buffer.data(), len);

            if (!utils::write_all(STDOUT_FILENO, buffer.data(), NAME_SIZE)) {
                break;
            }

            Event e = {}; 
            e.type = EventType::NEW_TEST;
            e.test_id = -1;
            e.data_byte = 0;
            
            if (!utils::write_all(event_pipe_fd, &e, sizeof(e))) {
                break;
            }
        }

        Event e = {};
        e.type = EventType::STDIN_CLOSED;
        e.test_id = -1;
        e.data_byte = 0;

        utils::write_all(event_pipe_fd, &e, sizeof(e));

        close(event_pipe_fd);
        
        exit(0);
    }

    void handle_wakeup(int) { }

    void go_to_persistent_waiter(int id, bool test) {
        sigset_t mask, oldmask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGUSR1);
        sigprocmask(SIG_BLOCK, &mask, &oldmask);

        // Setup handler
        struct sigaction sa = {};
        sa.sa_handler = handle_wakeup;
        sigaction(SIGUSR1, &sa, nullptr);

        signal(SIGINT, SIG_DFL);
        
        char c = 0;
        ssize_t r;

        while (true) {
            do {
                r = read(STDIN_FILENO, &c, 1);
            } while (r == -1 && errno == EINTR);

            if (r <= 0) {
                break; 
            }

            Event e = {}; 
            if (test) e.test_id = id, e.policy_id = -1;
            else e.policy_id = id, e.test_id = -1;
            e.type = EventType::EVT_DATA_READY;
            e.data_byte = c;

            if (!utils::write_all(STDOUT_FILENO, &e, sizeof(e))) {
                break;
            }
            sigsuspend(&oldmask);
        }
        
        exit(0);
    }
}


/**
 * Overrided function for sending signal to evaluator.
 * 
 * If `stop_requested` is set our program will exit with code 2.
 */
void handle_sigint(int) {
    stop_requested = true;
}

/**
 * Gets the current directory of our exectuable.
 * 
 * Used to call `execl`
 */
string get_executable_dir(const char* argv0) {
    char path[PATH_MAX];
    if (realpath(argv0, path)) {
        return string(dirname(path));
    }
    return ".";
}

/**
 * Enum representing possible states of Test
 */
enum class TestState {
        CREATED,
        WAITING_FOR_ENV_OUTPUT,    // Env is calculating, Waiter is listening on Env Output
        QUEUED_FOR_POLICY,          // Env finished, waiting for Policy slot
        WAITING_FOR_POLICY_OUTPUT, // Policy is calculating, Waiter is listening on Policy Output
        QUEUED_FOR_ENV,            // Policy finished, waiting for Env slot
        FINISHED
    };


/**
 * Data Structure representing test given from intput.
 * 
 * It stores:
 * - `test_id` to identify the test and the order it was put in
 *    (used later in printing output)
 * - `name` - we need to remember this in order to print output.
 * -  `answer` we will store final answer here.
 * - `current_action_tmp` that's temporary variable which will carry the
 *    output of policy.
 * - `current_state_tmp` also a temporary that will carry the output of 
 *    but of `enviroment`
 * - `latch_byte_tmp` see `waiter.cpp`.
 * -  `state` current state of our Test structure.
 */
struct Test {
    ssize_t test_id;
    string name;
    string answer="";
    
    string current_action_tmp ="";
    string current_state_tmp ="";
    char latch_byte_tmp = 0; 

    TestState state = TestState::CREATED;

    // Process IDs (Needed for SIGINT cleanup)
    pid_t env_pid = -1;
    pid_t current_waiter_pid = -1;
    int assigned_worker_idx = -1;

    // Pipe File Descriptors (Parent's end)
    int env_in = -1;  // We write Actions here
    int env_out = -1; // We read States from here
};

/** 
 * Data structure representing constraints from our evaluator.
 * */
struct EvaluatorData {
    string binary_path;
    string policy_path;
    string env_path;
    size_t max_active_env;
    size_t max_calls;
    size_t max_policy;
    vector<string> additional;

    EvaluatorData(const vector<string>& args) {
        if (args.size() < 5) 
            throw std::invalid_argument("Too few arguments");
        policy_path = args[1];
        env_path = args[2];
        max_policy = stoul(args[3]);
        max_calls = stoul(args[4]);
        max_active_env = stoul(args[5]);
        binary_path = get_executable_dir(args[0].c_str());
        additional.assign(args.begin() + 6, args.end());
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
    queue<int> wait_env;   // Waiting for transfer Policy -> Env
    queue<int> wait_policy; // Waiting for transfer Env -> Policy

    map<int, Test> tests;
    ssize_t next_test_id = 0;
    ssize_t next_print_id = 0;

    map<int, PolicyWorker> policy_pool;
    queue<int> free_workers;
    int next_worker_id = 0;

    map<EventType, function<void(Event)>> commands;

    using pipe_t = array<int, 2>;

    template <typename BufferT, typename SyscallFunc>
    void io_loop(int fd, BufferT* buffer, size_t size, SyscallFunc syscall, 
                 const std::string& err_msg, bool is_read_mode) {
        
        size_t processed = 0;

        while (processed < size) {
            ssize_t res = syscall(fd, buffer + processed, size - processed);

            if (res > 0) {
                processed += res;
            } 
            else if (res == 0) {
                if (is_read_mode) {
                    terminate_evaluator();
                    throw std::ios_base::failure(err_msg + ": unexpected EOF");
                }
            } 
            else {
                if (errno == EINTR) {
                    break;
                }
                terminate_evaluator();
                throw std::system_error(errno, std::system_category(), err_msg);
            }
        }
    }

    std::string read_fixed_string(int fd, size_t size) {
        if (size == 0) return "";
        std::string s(size, '\0');

        io_loop(fd, s.data(), size, read, "read failed", true);
        
        return s;
    }

    void write_fixed_string(int fd, const std::string& s) {
        if (s.empty()) return;

        io_loop(fd, s.data(), s.size(), write, "write failed", false);
    }

    template <typename F, typename... Args,
    typename = std::enable_if_t<std::is_invocable_v<F, Args...>>>
    decltype(auto) try_sys(F&& f, Args&&... args) {
        auto ret = f(std::forward<Args>(args)...);

        if (ret == -1) {
            terminate_evaluator();
            throw std::system_error(errno, std::system_category(), "Failure in system call");
        }
        
        return ret;
    }

    void safe_close(int& fd) {
        fd = fd >= 0 ? (try_sys(close, fd), -1): -1;
    }

    void safe_waitpid(pid_t& pid) {
        pid = pid >= 0 ? ([pid]() 
                        {ASSERT_SYS_OK(waitpid(pid, nullptr, 0)); }() , -1)
                        : -1;
    }

    void safe_pipe(pipe_t& pd, std::initializer_list<int> opt) {
        try_sys(pipe2, 
                pd.data(), 
                accumulate(opt.begin(), opt.end(), 0, std::bit_or<int>{}));
    }
    void safe_pipe(pipe_t& pd) {
        try_sys(pipe, 
                pd.data());
    }

    void safe_dup2(int from, int to) {
        try_sys(dup2, from, to);
    }

    pid_t safe_fork() {
       return try_sys(fork);
    }

    template <typename F, typename... Args>
    void try_multiple_sys(F&& f, Args&&... args) {
        ((f(args)), ...);
    }

    const function<void(int)> assert_and_close =
        [this](int fd) { ASSERT_SYS_OK(close(fd)); };

    void create_double_pipe(pipe_t& f, pipe_t& s) {
        safe_pipe(f, {O_CLOEXEC});
        try {
            safe_pipe(s, {O_CLOEXEC});
        } catch(...) {
            try_multiple_sys(
                assert_and_close, 
                f[0], f[1]
            );
            throw;
        }
    }

    /**
     * FUNCTIONS FOR FORKED PROCESSES
     */


    const function<void(initializer_list<int>)> assert_and_close_l = 
        [this] (initializer_list<int> li) 
            { for (int f : li) assert_and_close(f); };

    void dup_and_close(int from_fd, int to_fd) {
        ASSERT_SYS_OK(dup2(from_fd, to_fd));
        assert_and_close(from_fd);
    }

    void try_print_results() {
        while (tests.count(next_print_id)) {
            auto& t = tests.at(next_print_id);

            if (t.state != TestState::FINISHED) {
                break;
            }
            /*No need to do newline because t.answer already should
             * have new line */
            cout << t.name << " "
                 << t.answer;
            cout.flush();
            
            tests.erase(next_print_id);
            next_print_id++;
        }
    }

    void remove_cloexec(int fd) {
        int flags = try_sys(fcntl, fd, F_GETFD);
        flags &= ~FD_CLOEXEC;
        try_sys(fcntl, fd, F_SETFD, flags);
    }

    pid_t spawn_reader(int& out_names_fd, int& out_event_write) {

        static string reader_path = config.binary_path + "/reader";

        pipe_t f, s;
        create_double_pipe(f, s);
        auto [name_reader, name_writer] = f;
        auto [event_reader, event_writer] = s;
        remove_cloexec(event_writer);

        pid_t pid = safe_fork();

        if (pid == 0) {
            assert_and_close_l({name_reader, event_reader});
            dup_and_close(name_writer, STDOUT_FILENO);

            forked_processes::go_to_reader(event_writer);
            syserr("exec reader failed");
        }

        // moze zmien to 
        safe_dup2(event_reader, STDIN_FILENO);
        safe_close(event_reader);

        safe_close(name_writer); 

        out_names_fd = name_reader;
        out_event_write = event_writer;
        return pid;
    }

   void proper_exec(const std::vector<std::string> &dynamic_args, std::initializer_list<const char *> fixed_args) {
        std::vector<char*> argv;
        
        for (auto arg : fixed_args) {
            argv.push_back(const_cast<char*>(arg));
        }
        for (const auto &arg : dynamic_args) {
            argv.push_back(const_cast<char*>(arg.c_str()));
        }
        argv.push_back(nullptr);

        if (argv.size() > 0 && argv[0] != nullptr) {
            ASSERT_SYS_OK(execv(argv[0], argv.data()));
        } else {
            errno = EINVAL;
            ASSERT_SYS_OK(-1);
        }
    }

    pid_t spawn_waiter(int fd_to_watch, int id, bool is_test) {
        pid_t worker = safe_fork();

        static string waiter_path = config.binary_path + "/waiter";

        if (worker == 0) { 
            dup_and_close(fd_to_watch, STDIN_FILENO);
            dup_and_close(event_write_fd, STDOUT_FILENO);
            
            forked_processes::go_to_persistent_waiter(id, is_test);
            exit(1);
        }
        return worker;
    }

    void spawn_policy_worker() {
        static string reader_path = config.binary_path + "/reader";

        pipe_t pin, pout;
        create_double_pipe(pin, pout);

        int id = next_worker_id++;

        pid_t pid = safe_fork();
        if (pid == 0) {
            dup_and_close(pin[0], STDIN_FILENO);
            dup_and_close(pout[1], STDOUT_FILENO);
            
            assert_and_close(event_write_fd);
            proper_exec(config.additional, {
                config.policy_path.c_str(),
                to_string(id).c_str()
            });
            
            perror("exec policy failed");
            exit(1);
        }

        safe_close(pin[0]); 
        safe_close(pout[1]);
        cerr << "waiter spawned" << endl;
        pid_t waiter_pid = spawn_waiter(pout[0], id, false);

        PolicyWorker w{
                .id = id,
                .pid = pid,
                .pipe_in = pin[1],
                .pipe_out = pout[0],
                .is_busy = false,
                .waiter_pid = waiter_pid };
        policy_pool[id] = w;
        free_workers.push(id);
    }

    void spawn_environment(int id) {
        auto& test = tests[id];
        pipe_t p_to_child, p_from_child;
        create_double_pipe(p_to_child, p_from_child);

        test.env_pid = safe_fork();

        if (test.env_pid == 0) {
            dup_and_close(p_to_child[0], STDIN_FILENO);
            dup_and_close(p_from_child[1], STDOUT_FILENO);
            assert_and_close(event_write_fd);

            proper_exec(config.additional, {
                config.env_path.c_str(),
                test.name.c_str()
            });
            syserr("exec env failed");
        }

        test.env_in = p_to_child[1];
        test.env_out = p_from_child[0];

        safe_close(p_to_child[0]);
        safe_close(p_from_child[1]);
        test.state = TestState::WAITING_FOR_ENV_OUTPUT;
        test.current_waiter_pid = spawn_waiter(test.env_out, id, true);
    }

    bool can_calc_action() {
        return !wait_policy.empty() &&
            active_calls < config.max_calls &&
            active_policy_calls < config.max_policy;
    }

    bool can_calc_state() {
        return !wait_env.empty() && active_calls < config.max_calls;
    }

    bool can_create_new_env() {
        return !new_tests.empty() && 
               active_calls < config.max_calls && 
               active_env < config.max_active_env;
    }

    void schedule() {
        if (stop_requested) return;

        while (can_calc_state()) {
            int id = wait_env.front(); wait_env.pop();
            auto& t = tests[id];

            active_calls++;
            t.state = TestState::WAITING_FOR_ENV_OUTPUT;

            write_fixed_string(t.env_in, t.current_action_tmp);
            t.current_action_tmp.clear();
        }

        while (can_calc_action()) {
            if (free_workers.empty()) spawn_policy_worker();

            int worker_id = free_workers.front(); free_workers.pop();
            int test_id = wait_policy.front(); wait_policy.pop();

            auto& t = tests[test_id];
            auto& worker = policy_pool[worker_id];

            worker.is_busy = true;
            t.assigned_worker_idx = worker_id;
            worker.current_test_id = t.test_id;

            active_calls++;
            active_policy_calls++;
            t.state = TestState::WAITING_FOR_POLICY_OUTPUT;
            write_fixed_string(worker.pipe_in, t.current_state_tmp);
            t.current_state_tmp.clear();
        }

        while (can_create_new_env()) {
            int id = new_tests.front(); new_tests.pop();
            active_calls++; 
            active_env++;
            spawn_environment(id);
        }
    }

    void handle_data_ready(Event e) {
        ssize_t id;
        if (e.test_id == -1) {
            PolicyWorker& p = policy_pool[e.policy_id];
            id = p.current_test_id;
            p.current_test_id = -1;
        }else id = e.test_id;
        auto& t = tests[id];
        t.latch_byte_tmp = e.data_byte;

        cerr << "HELLO GOT SOME INPUT"  << " " << t.test_id<< endl;

        if (active_calls > 0) active_calls--;
        if (t.state == TestState::WAITING_FOR_ENV_OUTPUT) {
            cerr << "START READING" << endl;
            string tmp = string{e.data_byte} 
                + read_fixed_string(t.env_out, STATE_SIZE);
            kill(t.current_waiter_pid, SIGUSR1);
            cerr << "END READING" << endl;
            if (e.data_byte == 'T') {
                t.answer = move(tmp);
                cleanup_test(t.test_id);
                try_print_results();
            } else {
                t.state = TestState::QUEUED_FOR_POLICY;
                wait_policy.push(t.test_id);
                t.current_state_tmp = move(tmp);
            }
            
        } else if (t.state == TestState::WAITING_FOR_POLICY_OUTPUT) {
            auto& worker = policy_pool[t.assigned_worker_idx];
            string tmp = string{e.data_byte}
                + read_fixed_string(worker.pipe_out, ACTION_SIZE);

            kill(worker.waiter_pid, SIGUSR1);
            free_policy(t.assigned_worker_idx);
            t.state = TestState::QUEUED_FOR_ENV;
            wait_env.push(t.test_id);
            t.current_action_tmp = move(tmp);
        } else {
            syserr("undefined behaviour in data_ready");
        }
    }

    void handle_new_test([[maybe_unused]] Event e) {
        ssize_t id = next_test_id;
        next_test_id++;
        tests[id] = Test{
            .test_id = id,
            .name = read_fixed_string(names_read_fd, NAME_SIZE)
        };
        new_tests.push(id);
    }

    void free_policy(int& idx) {
        if (active_policy_calls > 0) active_policy_calls--;
        auto &w = policy_pool[idx];
        idx = -1;

        w.is_busy = false;
        free_workers.push(w.id);
    }

    void cleanup_test(int id) {
        if (active_env > 0) active_env--;
        auto& t = tests[id];
        t.state = TestState::FINISHED;
        
        // we don't need to kill because the enviroment
        // should shut itself
        safe_kill(t.current_waiter_pid);
        safe_waitpid(t.current_waiter_pid);
        safe_waitpid(t.env_pid);
        safe_close(t.env_in);
        safe_close(t.env_out);
    }

    void safe_kill(pid_t pid) {
        if (pid >= 0) ASSERT_SYS_OK(kill(pid, SIGINT));
    }

    void soft_shutdown() {
        shut_soft = true;
        safe_kill(reader_pid);
        safe_waitpid(reader_pid);
    }

    const function<void(int&)> assert_and_close_ter = 
        [this](int &fd) { fd = fd >= 0 ? (assert_and_close(fd), -1) : -1; };

    /**
     * We notify all workers that all work is finished by sending them sigkill.
     * 
     * Then we wait for reader_pid, 
     * 
     * make sure `safe_close()` isn't recursive. 
     */
    void clean_all() {
        for (auto& [wid, worker] : policy_pool) {
            safe_kill(worker.pid);
            safe_kill(worker.waiter_pid);
        }
        safe_waitpid(reader_pid);
        for (auto& [wid, worker] : policy_pool) {
            safe_waitpid(worker.pid);
            safe_waitpid(worker.waiter_pid);
            assert_and_close_ter(worker.pipe_in);
            assert_and_close_ter(worker.pipe_out);
        }

        for (auto& [id, t] : tests) {
            safe_waitpid(t.env_pid);
            safe_waitpid(t.current_waiter_pid);
            assert_and_close_ter(t.env_in);
            assert_and_close_ter(t.env_out);
        }

        assert_and_close_ter(names_read_fd);
        assert_and_close_ter(event_write_fd);
    }

    void terminate_evaluator() {
        for (auto& [id, t] : tests) {
            safe_kill(t.env_pid);
            safe_kill(t.current_waiter_pid);
        }

        safe_kill(reader_pid);

        clean_all();
    }

public:
    Evaluator(EvaluatorData data) : config(std::move(data)) {
        commands = {
            { EventType::NEW_TEST, [this](Event e) { this->handle_new_test(e); }},
            { EventType::EVT_DATA_READY, [this](Event e) { this->handle_data_ready(e); }},
            { EventType::STDIN_CLOSED, [this]([[maybe_unused]]Event e) { this->soft_shutdown(); }},
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
            if (stop_requested) { 
                terminate_evaluator(); 
                return; 
            }

            ssize_t n = read(STDIN_FILENO, &e, sizeof(e));

            if (n == -1 && errno == EINTR) {
                if (stop_requested) {
                    terminate_evaluator();
                    return;
                }
                continue;
            }

            if (stop_requested) {
                terminate_evaluator();
                return;
            }

            if (n <= 0) {
                if (!shut_soft || active_env > 0) {
                    terminate_evaluator();
                    throw std::ios_base::failure("Read error or unexpected EOF"); 
                }
                break; 
            }

            if (e.type == EventType::NEW_TEST && shut_soft) {
                continue;
            }

            if (commands.count(e.type)) {
                commands.at(e.type)(e);
            }
            
            schedule();
        }
        clean_all();
    }
}; // chceck safe_kill and so on.

int main(int argc, char* argv[]) {
    // Obsługa sygnałów
    struct sigaction sa;
    sa.sa_handler = handle_sigint;
    ASSERT_SYS_OK(sigemptyset(&sa.sa_mask));
    sa.sa_flags = 0;
    ASSERT_SYS_OK(sigaction(SIGINT, &sa, nullptr));
    try {
        utils::ParseReader parser(argc, argv);
        EvaluatorData config(parser.get_args());
        Evaluator app(config);
        app.run();
        if (stop_requested)
            return 2;
    } catch (const std::exception& e) {
        cerr << "Błąd: " << e.what() << endl;
        return 1;
    }
     catch (...) {
        return 1;
    }

    return 0;
}