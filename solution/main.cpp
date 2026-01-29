#include <deque>
#include <iostream>
#include <signal.h>
#include <string>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <sys/wait.h>
#include <vector>
#include <err.h>

namespace {
    
    using std::deque;
    using std::invalid_argument;
    using std::move;
    using std::string;
    using std::stringstream;
    using std::unordered_map;
    using std::vector;

    volatile sig_atomic_t shutdown_requested = 0;

    /** Enum representing possible status of our test */
    enum class TestStatus {
        WAITING = 0,
        ENVIROMENT_COMP,
        AFTER_ENVIROMENT,
        DURING_POLICY,
        COMPLETED
    };

    constexpr size_t TEST_STATUS_CNT = 5;

    /** Strucure representing our Test */
    struct TestData {
        string test_name;
        string test_finish;
        TestStatus status;
        size_t id;

        TestData(string command) : status(TestStatus::WAITING)
        {
            try {
                stringstream ss(command);
                ss >> test_name;
            } catch(...) {
                throw invalid_argument("Wrong Command");
            }
        }

        void advance(size_t a = 1) {
            if (status == TestStatus::COMPLETED) return;
            status = static_cast<TestStatus>(static_cast<size_t>(status) + a);
        }
    };

    class TestStorage {
        private:
            std::deque<TestData> tests;
            size_t current_test = 0;
            
            std::vector<std::vector<int>> counts;

            void update(const TestData& test) {

                size_t status_idx = static_cast<size_t>(test.status);
                if (status_idx < counts.size()) {
                    counts[status_idx].push_back(test.id);
                }
            }

        public:
            TestStorage() : counts(TEST_STATUS_CNT) {} 

            void add(TestData test) {
                test.id = current_test++;
                
                tests.push_back(std::move(test));
                TestData& test_r = tests.back();
                
                update(test_r);
            }
        };

    struct EvaluatorData {
        string policy_path;
        string env_path;
        int max_concurrent_policy_calls;
        int max_concurrent_calls;
        int max_active_env;
        vector<string> extra_arguments;

        EvaluatorData(string p_path, 
                    string e_path, 
                    int max_p_calls, 
                    int max_total_calls,
                    int max_envs, 
                    vector<string> extra_args = {})
            : policy_path(move(p_path)), 
            env_path(move(e_path)), 
            max_concurrent_policy_calls(max_p_calls),
            max_concurrent_calls(max_total_calls),
            max_active_env(max_envs),
            extra_arguments(move(extra_args)) 
        {}
};

    class ParseReader {
        private:
            vector<string> arguments;
        public:
            ParseReader(int argc, char** args)
            {
                for (int i = 1; i < argc; i++) {
                    arguments.push_back(string{args[i]});
                }
            }

            EvaluatorData get_eval_info()
            {
                if (arguments.size() < 5) {
                    throw invalid_argument(
                        "Evaluator Requires at least 5 arguments");
                }
                using std::stoi;
                const string p_path = arguments[1];
                const string e_path = arguments[2];
                int max_con_policy_calls = stoi(arguments[3]);
                int max_con_calls = stoi(arguments[4]);
                int max_act_env = stoi(arguments[5]);
                vector<string> additional;
                std::copy(arguments.begin() + 6, arguments.end(), 
                                        std::back_inserter(additional));
                return EvaluatorData(p_path, 
                                     e_path,
                                     max_con_policy_calls,
                                     max_con_calls,
                                     max_act_env,
                                     additional);
            }
        
    };

    void handle_subprocess(int a) {
    }

    class Evaluator {
        private:

            enum class SubprocessType {
                POLICY,
                ENVIROMENT
            };

            const EvaluatorData eval_info;
            int active_policy, active_env;

            std::unordered_map<int, SubprocessType> mapped_processes;
            TestStorage storage{};

            void cleanup()
            {

            }

            void handle_signal(int)
            {
                int saved_errno = errno;
                int status;
                pid_t pid;

                while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
                    
                }
                
                errno = saved_errno;
            }

            bool can_create_env()
            {
                return active_env < eval_info.max_active_env &&
                       (active_env + active_policy) < eval_info.max_concurrent_calls;
            }

            void create_env(TestData test)
            {
                test.advance(2);
                storage.add(test);

                int pid = fork();

                if (pid == -1) {
                    throw std::ios_base::failure("Fork error");
                }

                if (pid == 0) {
                vector<string> args_storage;
                args_storage.push_back(eval_info.env_path); 

                args_storage.push_back(test.test_name);

                args_storage.insert(args_storage.end(), 
                                    eval_info.extra_arguments.begin(), 
                                    eval_info.extra_arguments.end());

                vector<char*> c_args;
                for (const auto& s : args_storage) {
                    c_args.push_back(const_cast<char*>(s.c_str()));
                }
                c_args.push_back(nullptr);
                execv(eval_info.env_path.c_str(), c_args.data());
            } else {
                    mapped_processes.emplace(pid, SubprocessType::ENVIROMENT);
                }
            }

            void start_signal_handling() {
                struct sigaction sa = {};
                sa.sa_handler = &handle_subprocess;
                sigemptyset(&sa.sa_mask);
                sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;

                if (sigaction(SIGCHLD, &sa, nullptr) == -1) {
                    throw std::ios_base::failure("Error while initializing `sigaction`");
                }
            }

        public: 
            Evaluator(EvaluatorData args): eval_info(move(args)) {};


            void start()
            {
                using std::getline;
                
                start_signal_handling();

                string command;
                while(getline(std::cin, command)) {
                    TestData test{command};
                    // dodaj jakieś blokowanie
                    if (can_create_env()) {
                        create_env(test);
                    } else {
                        storage.add(test);
                    }
                }
            }
    };

}

int main(int argc, char** argv)
{
    ParseReader reader(argc, argv);
    Evaluator eval(reader.get_eval_info());
    try {
        eval.start();
    } catch(...) { // correct this
        std::cerr << "OH NO EXCEPTION" << std::endl;
        return 1;
    }

}