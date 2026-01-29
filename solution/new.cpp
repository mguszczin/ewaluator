#include <iostream>
#include <vector>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <cstring>
#include <signal.h>
#include <stdexcept>

namespace {
    
    using std::invalid_argument;
    using std::move;
    using std::string;
    using std::stringstream;
    using std::vector;

    volatile sig_atomic_t shutdown_requested = 0;

    struct SharedResources {
        sem_t sem_gpu;
        sem_t sem_cpu;
        sem_t sem_mem;
        volatile sig_atomic_t stop_flag;
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

    class Evaluator {
        private:

            enum class SubprocessType {
                POLICY,
                ENVIROMENT
            };

            static Evaluator *self;

            const EvaluatorData eval_info;
            int active_policy, active_env;
            SharedResources shared_mem;


            static void safe_write(int fd, const void* buf, size_t count)
            {
                size_t written = 0;
                const char* ptr = static_cast<const char*>(buf);
                while (written < count) {
                    ssize_t res = write(fd, ptr + written, count - written);
                    if (res < 0) {
                        if (errno == EINTR) continue;
                        return;
                    }
                    written += res;
                }
            }

            static ssize_t safe_read(int fd, void* buf, size_t count) {
                size_t read_bytes = 0;
                char* ptr = static_cast<char*>(buf);
                while (read_bytes < count) {
                    ssize_t res = read(fd, ptr + read_bytes, count - read_bytes);
                    if (res < 0) {
                        if (errno == EINTR) continue;
                        return -1;
                    }
                    if (res == 0) break;
                    read_bytes += res;
                }
                return read_bytes;
            }

            void cleanup()
            {

            }

            static void sigchild_handler(int)
            {
                if (!self) {
                    throw std::invalid_argument("Bad call to sigchild, handler");
                }
            }

            static void sigint_handler(int) {
                if (!self) {
                    throw std::invalid_argument("Bad call to sigint handler");
                }
                if (self->shared_mem) {
                    self->shared_mem->stop_flag = 1;
                }
            }

            void start_signal_handling() {
                struct sigaction sa_int = {}, sa_chld = {};

                sa_int.sa_handler = sigint_handler;
                sigemptyset(&sa_int.sa_mask);
                sa_int.sa_flags = 0;

                sa_chld.sa_handler = sigchild_handler;
                sigemptyset(&sa_chld.sa_mask);
                sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;

                if (!sigaction(SIGINT, &sa_int, nullptr) ||
                    !sigaction(SIGCHLD, &sa_chld, nullptr)) {
                        throw std::ios_base::failure("Signal action went wrong!!");
                }
            }

        public: 
            Evaluator(EvaluatorData args): eval_info(move(args)) 
                                        { self = this; };


            void start()
            {
                using std::getline;
                
                start_signal_handling();

                std::string testName;
                // Pętla czytająca testy
                while (!shared_mem->stop_flag && std::cin >> testName) {
                    // Oczekiwanie na wolne miejsce w "RAM" (maxActiveEnvironments)
                    // Jeśli brak miejsca, sem_wait zablokuje główny proces.
                    // Zwolnienie nastąpi w sigchild_handler po zakończeniu jakiegoś Nadzorcy.
                    if (sem_wait(&shared_mem->sem_mem) == -1) {
                        if (errno == EINTR) continue; // Przerwane sygnałem, sprawdzamy pętlę
                        break;
                    }

                    pid_t pid = fork();
                    if (pid == 0) {
                        // Proces Potomny (Nadzorca)
                        // Musimy zresetować handlery, aby Nadzorca domyślnie reagował na sygnały (lub je ignorował)
                        // Ale w zadaniu jest mowa, że evaluator ma posprzątać.
                        // Zostawmy domyślne zachowanie albo zignorujmy SIGINT w dziecku, 
                        // bo rodzic wyśle mu SIGTERM/KILL przy sprzątaniu.
                        signal(SIGINT, SIG_IGN); 
                        run_supervisor(testName); // To się kończy _exit(0)
                    } else if (pid < 0) {
                        // Błąd fork
                        sem_post(&shared_mem->sem_mem); // Oddajemy zasób
                    }
                    // Rodzic wraca natychmiast do czytania kolejnego testu
                }

                // Faza kończenia - czekamy na wszystkie dzieci
                // Flaga stop_flag mogła zostać ustawiona przez SIGINT
                
                // Zabijamy wszystkie pozostałe procesy w grupie (jeśli SIGINT)
                if (shared_mem->stop_flag) {
                    kill(0, SIGKILL); // Brutalne, ale skuteczne w ramach zadania na "jak najszybciej"
                }

                while (wait(nullptr) > 0); // Czekamy na wszystkie dzieci
                cleanup_shared_memory();
                
                if (shared_mem->stop_flag) exit(2);
                exit(0);
            }
    }
}

int main(int argc, char** argv)
{
    ParseReader reader(argc, argv);
    Evaluator eval(reader.get_eval_info());
    try {
        eval.start();
    } catch(...) {
        std::cerr << "OH NO EXCEPTION" << std::endl;
        return 1;
    }

}