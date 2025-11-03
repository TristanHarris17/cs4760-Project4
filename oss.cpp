#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>
#include <string>
#include <cstdlib>
#include <sys/wait.h>
#include <vector>
#include <iomanip>
#include <signal.h>
#include <random>
#include <fstream>
#include <sstream>
#include <climits>
#include <algorithm>

using namespace std;

struct PCB {
    bool occupied;
    pid_t pid;
    int start_sec;
    int start_nano;
    int messagesSent;
    int workerID;
    int serviceTimeSec; // total seconds process has been "scheduled"
    int serviceTimeNano; // total nanoseconds process has been "scheduled"
    int eventWaitSec; // time in seconds the process will become unblocked
    int eventWaitNano; // time in nanoseconds the process will become unblocked
    int blocked; // 1 if blocked, 0 if not
    long long last_scheduled_ns; // last scheduled time in nanoseconds
};

struct MessageBuffer {
    long mtype;
    int time_q; // time quantum in nanoseconds
};

// Globals
key_t sh_key = ftok("oss.cpp", 0);
int shmid = shmget(sh_key, sizeof(int)*2, IPC_CREAT | 0666);
int *shm_clock;
int *sec;
vector <PCB> table(20);
const int increment_amount = 10000;
const int time_quantum = 50000000; // 50 ms in nanoseconds
static int nextWorkerID = 0;

// setup message queue
key_t msg_key = ftok("oss.cpp", 1);
int msgid = msgget(msg_key, IPC_CREAT | 0666);

// global log stream and helper so other functions can log to the same place as main
ofstream log_fs;
static const size_t MAX_LOG_LINES = 10000;
static size_t log_lines_written = 0;
static inline void oss_log_msg(const string &s) {
    // always print to stdout
    cout << s;
    if (!log_fs.is_open()) return;
    size_t newlines = count(s.begin(), s.end(), '\n'); // count how many new lines this message contains
    if (log_lines_written >= MAX_LOG_LINES) return; // if limit is reached, skip
    if (log_lines_written + newlines > MAX_LOG_LINES) return; // skip message if it would exceed limit
    // else write the whole message and update counter
    log_fs << s;
    log_fs.flush();
    log_lines_written += newlines;
}

void increment_clock(int* sec, int* nano, long long inc_ns) {
    const long long NSEC_PER_SEC = 1000000000LL;
    if (inc_ns <= 0) inc_ns = 1; // guard against non-positive increments
    long long total = (long long)(*nano) + inc_ns;
    *sec += (int)(total / NSEC_PER_SEC);
    *nano = (int)(total % NSEC_PER_SEC);
}

// convert float time interval to seconds and nanoseconds and return nannoseconds
int seconds_conversion(float interval) {
    int seconds = (int)interval;
    float fractional = interval - (float)seconds;
    int nanoseconds = (int)(fractional * 1e9);
    return nanoseconds;
}

// check if any child has terminated, return pid if so, else -1
pid_t child_Terminated() {
    int status;
    pid_t result = waitpid(-1, &status, WNOHANG);
    if (result > 0) {
        return result;
    }
    return -1;
}

pid_t launch_worker(float time_limit) {
    pid_t worker_pid = fork();
    if (worker_pid < 0) {
        cerr << "fork failed" << endl;
        exit(1);
    }

    if (worker_pid == 0) {
        string arg_sec = to_string((int)time_limit);
        string arg_nsec = to_string(seconds_conversion(time_limit));
        char* args[] = {
            (char*)"./worker",
            const_cast<char*>(arg_sec.c_str()),
            const_cast<char*>(arg_nsec.c_str()),
            NULL
        };
        execv(args[0], args);
        cerr << "Exec failed" << endl;
        exit(1);
    }
    return worker_pid;
}

// find an empty PCB slot, return index or -1 if none found
int find_empty_pcb(const vector<PCB> &table) {
    for (size_t i = 0; i < table.size(); ++i) {
        if (!table[i].occupied) {
            return i;
        }
    }
    return -1;
}

int remove_pcb(vector<PCB> &table, pid_t pid) {
    for (size_t i = 0; i < table.size(); ++i) {
        if (table[i].occupied && table[i].pid == pid) {
            table[i].occupied = false;
            return i;
        }
    }
    return -1;
}

void print_process_table(const std::vector<PCB> &table) {
    using std::cout;
    using std::endl;
    cout << std::left
         << std::setw(6)  << "Index"
         << std::setw(10) << "Occ"
         << std::setw(12) << "PID"
         << std::setw(12) << "StartSec"
         << std::setw(12) << "StartNano"
         << std::setw(12) << "MsgsSent" << endl;
    cout << std::string(64, '-') << endl;

    for (size_t i = 0; i < table.size(); ++i) {
        const PCB &p = table[i];
        cout << std::left << std::setw(6) << i
             << std::setw(10) << (p.occupied ? 1 : 0);
        if (p.occupied) {
            cout << std::setw(12) << p.pid
                 << std::setw(12) << p.start_sec
                 << std::setw(12) << p.start_nano
                 << std::setw(12) << p.messagesSent;
        } else {
            cout << std::setw(12) << "-" << std::setw(12) << "-" << std::setw(12) << "-" << std::setw(12) << "-";
        }
        cout << endl;
    }
    cout << endl;
}

void signal_handler(int sig) {
    if (sig == SIGALRM || sig == SIGINT) {
        cout << "Received SIGALRM or SIGINT, terminating all child processes..." << endl;
        // Terminate all child processes and clean up shared memory
        shmdt(shm_clock);
        shmctl(shmid, IPC_RMID, nullptr);
        msgctl(msgid, IPC_RMID, nullptr);
        kill(0, SIGTERM); 
        exit(0);
    }
}

void exit_handler() {
    shmdt(shm_clock);
    shmctl(shmid, IPC_RMID, nullptr);
    msgctl(msgid, IPC_RMID, nullptr);
    exit(1);
}

pid_t select_next_worker(const vector<PCB> &table) {
    const long long NSEC_PER_SEC = 1000000000LL;

    // read current simulated time
    if (shm_clock == nullptr) return (pid_t)-1;
    long long current_total = (long long)shm_clock[0] * NSEC_PER_SEC + (long long)shm_clock[1];

    // collect (ratio, index) for every occupied and unblocked process
    vector<pair<double,int>> entries;
    entries.reserve(table.size());
    for (size_t i = 0; i < table.size(); ++i) {
        const PCB &p = table[i];
        if (!p.occupied || p.blocked) continue;

        long long service_ns = (long long)p.serviceTimeSec * NSEC_PER_SEC + (long long)p.serviceTimeNano;
        long long start_total = (long long)p.start_sec * NSEC_PER_SEC + (long long)p.start_nano;
        long long time_in_system = current_total - start_total;

        double ratio;
        if (time_in_system <= 0) {
            // denominator zero or negative ratio  = 0
            ratio = 0.0;
        } else {
            ratio = (double)service_ns / (double)time_in_system;
        }

        entries.emplace_back(ratio, (int)i);
    }

    // sort by ratio ascending, tie-break by PID ascending
    sort(entries.begin(), entries.end(), [&](const pair<double,int>& a, const pair<double,int>& b){
        if (a.first != b.first) return a.first < b.first;
        return table[a.second].pid < table[b.second].pid;
    });

    // build a single log message for the ratios and write it via oss_log_msg
    {
        ostringstream ss;
        ss << "OSS: Scheduling ratios at " << shm_clock[0] << "s " << shm_clock[1] << "ns\n";
        for (const auto &e : entries) {
            int idx = e.second;
            const PCB &p = table[idx];
            ss << "  idx=" << idx
               << " pid=" << p.pid
               << " ratio=" << std::fixed << std::setprecision(6) << e.first
               << " service=" << p.serviceTimeSec << "s" << p.serviceTimeNano << "ns"
               << " start=" << p.start_sec << "s" << p.start_nano << "ns"
               << "\n";
        }
        ss << std::flush;
        oss_log_msg(ss.str());
    }

    if (entries.empty()) return (pid_t)-1;

    // return pid of smallest-ratio entry
    return table[entries.front().second].pid;
}

// helper to detect empty/blank optarg
static inline bool optarg_blank(const char* s) {
    return (s == nullptr) || (s[0] == '\0');
}

void unblock_ready_processes(std::vector<PCB> &table, int *sec, int *nano) {
    const long long NSEC_PER_SEC = 1000000000LL;
    long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
    std::ostringstream ss;

    for (size_t i = 0; i < table.size(); ++i) {
        PCB &p = table[i];
        if (p.occupied && p.blocked) {
            long long event_total = (long long)p.eventWaitSec * NSEC_PER_SEC + (long long)p.eventWaitNano;
            if (current_total >= event_total) {
                p.blocked = 0;
                p.eventWaitSec = 0;
                p.eventWaitNano = 0;
                ss << "OSS: Unblocking worker " << p.workerID << " PID " << p.pid
                   << " at " << *sec << "s " << *nano << "ns." << std::endl;
            }
        }
    }

    if (!ss.str().empty()) {
        oss_log_msg(ss.str());
    }
}

int main(int argc, char* argv[]) {
    //parse command line args
    int proc = -1;
    int simul = -1;
    float time_limit = -1;
    float launch_interval = -1;
    string log_file = "";
    int opt;

    while((opt = getopt(argc, argv, "hn:s:t:i:f:")) != -1) {
        switch(opt) {
            case 'h': {
                cout << "Usage: oss -n proc -s simul -t time_limit -i launch_interval\n"
                    << "Options:\n"
                    << "  -h                Show this help message and exit\n"
                    << "  -n proc           Total number of worker processes to launch (non-negative integer)\n"
                    << "  -s simul          Maximum number of simultaneous worker processes (positive integer)\n"
                    << "  -t time_limit     Time limit for each worker process in seconds (non-negative float)\n"
                    << "  -i launch_interval Interval between launching worker processes in seconds (non-negative float)\n"
                    << "  -f logfile        Log file name (optional)\n"
                    << "Example:\n"
                    << "  ./oss -n 10 -s 3 -t 2.5 -i 0.5 -f oss.log\n";
                exit_handler();
            }
            case 'n': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -n requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    int val = stoi(optarg);
                    if (val < 0) throw invalid_argument("negative");
                    proc = val;
                } catch (...) {
                    cerr << "Error: -n must be a non-negative integer." << endl;
                    exit_handler();
                }
                 break;
            }
            case 's': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -s requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    int val = stoi(optarg);
                    if (val <= 0) throw invalid_argument("non-positive");
                    simul = val;
                } catch (...) {
                    cerr << "Error: -s must be a positive integer." << endl;
                    exit_handler();
                }
                 break;
            }
            case 't': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -t requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    float val = stof(optarg);
                    if (val < 0.0f) throw invalid_argument("negative");
                    time_limit = val;
                } catch (...) {
                    cerr << "Error: -t must be a non-negative number." << endl;
                    exit_handler();
                }
                 break;
            }
            case 'i': {
                if (optarg_blank(optarg)) {
                    cerr << "Error: -i requires a non-blank argument." << endl;
                    exit_handler();
                }
                try {
                    float val = stof(optarg);
                    if (val < 0.0f) throw invalid_argument("negative");
                    launch_interval = val;
                } catch (...) {
                    cerr << "Error: -i must be a non-negative number." << endl;
                    exit_handler();
                }
                 break;
            }
            case 'f': {
                // Optional: handle log file name if needed
                if (!optarg_blank(optarg)) log_file = optarg;
                else {
                    cerr << "Error: -f requires a non-blank filename." << endl;
                    exit_handler();
                }
                 break;
            }
            default:
                cerr << "Error: Unknown option or missing argument." << endl;
                exit_handler();
        }
    }

    // final validation of required options
    if (proc == -1 || simul == -1 || time_limit < 0.0f || launch_interval < 0.0f) {
        cerr << "Error: Missing required options. Usage: ./oss -n proc -s simul -t time_limit -i launch_interval [-f logfile]" << endl;
        exit_handler();
    }

    // attach shared memory to shm_ptr
    shm_clock = (int*) shmat(shmid, nullptr, 0);
    if (shm_clock == (int*) -1) {
        cerr << "shmat";
        exit_handler();
    }

    // pointers to seconds and nanoseconds in shared memory
    int *sec = &(shm_clock[0]);
    int *nano = &(shm_clock[1]);
    *sec = *nano = 0;

    // print interval using simulated clock: 0.5 seconds
    const long long NSEC_PER_SEC = 1000000000LL;
    const long long PRINT_INTERVAL_NANO = 500000000LL;
    long long next_print_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano) + PRINT_INTERVAL_NANO;

    // signal handling
    signal(SIGALRM, signal_handler);
    signal(SIGINT, signal_handler);
    alarm(3);

    // Initialize random number generator
    random_device rd;
    mt19937 gen(rd());
    uniform_real_distribution<double> dis(0, time_limit);

    // open log file if specified
    if (!log_file.empty()) {
        log_fs.open(log_file);
        if (!log_fs) {
            cerr << "Error: Could not open log file " << log_file << endl;
            exit(1);
        }
    }
 
    // helper to log messages originating from OSS (writes to stdout and to log file if open)
    // main still uses a local lambda name `oss_log` in many places; keep it forwarding to the global file
    auto oss_log = [&](const string &s) {
        cout << s;
        if (log_fs.is_open()) log_fs << s;
    };

    // oss starting message
    {
        ostringstream ss;
        ss << "OSS starting, PID:" << getpid() << " PPID:" << getppid() << endl
           << "Called With:" << endl
           << "-n: " << proc << endl
           << "-s: " << simul << endl
           << "-t: " << time_limit << endl
           << "-i: " << launch_interval << endl;
        oss_log(ss.str());
    }
 
    int launched_processes = 0;
    int running_processes = 0;

    long long launch_interval_nano = (long long)(launch_interval * 1e9); // convert launch interval to nanoseconds
    long long next_launch_total = 0; 

    MessageBuffer sndMessage;

    // statistics
    int message_count = 0;
    long long total_blocked_time_ns = 0;
    long long total_cpu_time_ns = 0;
    long long total_idle_time_ns = 0;
    long long total_wait_time_ns = 0;


    while (launched_processes < proc || running_processes > 0) {
        // Check if it's time to launch a new worker
        long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
        if (launched_processes < proc && running_processes < simul && current_total >= next_launch_total) {
            float worker_time = dis(gen);
            pid_t worker_pid = launch_worker(worker_time);

            // Find empty slot in PCB array and populate it with new process info
            int pcb_index = find_empty_pcb(table);
            table[pcb_index].occupied = true;
            table[pcb_index].pid = worker_pid;
            table[pcb_index].start_sec = *sec;
            table[pcb_index].start_nano = *nano;
            table[pcb_index].messagesSent = 0;
            table[pcb_index].workerID = ++nextWorkerID;

            launched_processes++;
            running_processes++;

            // Update the next allowed launch time
            next_launch_total = current_total + launch_interval_nano;
            print_process_table(table);
        }

        // check if any processes should be changed from blocked to ready
        unblock_ready_processes(table, sec, nano);

        // check if all processes are blocked
        bool all_blocked = true;
        for (const PCB &p : table) {
            if (p.occupied && p.blocked == 0) {
                all_blocked = false;
                break;
            }
        }
        if (all_blocked) {
            // find the process with the earliest unblock time
            long long earliest_unblock_total = LLONG_MAX;
            for (const PCB &p : table) {
                if (p.occupied && p.blocked) {
                    long long event_total = (long long)p.eventWaitSec * NSEC_PER_SEC + (long long)p.eventWaitNano;
                    if (event_total < earliest_unblock_total) {
                        earliest_unblock_total = event_total;
                    }
                }
            }
            // advance clock to that time
            if (earliest_unblock_total != LLONG_MAX) {
                long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
                if (earliest_unblock_total > current_total) {
                    long long diff = earliest_unblock_total - current_total;
                    increment_clock(sec, nano, diff);
                    // log clock advancement
                    total_idle_time_ns += diff;
                    {
                        ostringstream ss;
                        ss << "OSS: All processes blocked, advancing clock to "
                           << *sec << "s " << *nano << "ns " << endl;
                        oss_log(ss.str());
                    }
                    unblock_ready_processes(table, sec, nano);
                }
            }
        }

        // send message to next worker in round-robin fashion
        pid_t next_worker_pid = select_next_worker(table);
        int target_idx = -1;

        if (next_worker_pid != (pid_t)-1) {
            // find the index in the PCB table for printing
            for (size_t i = 0; i < table.size(); ++i) {
                if (table[i].occupied && table[i].pid == next_worker_pid) {
                    target_idx = (int)i;
                    break;
                }
            }

            {
                ostringstream ss;
                ss << "OSS: Scheduling worker " << "PID " << next_worker_pid <<  " at " << *sec << " seconds and " << *nano << " nanoseconds." << endl;
                oss_log(ss.str());
            }

            // prepare and send message to the selected worker
            sndMessage.mtype = next_worker_pid;
            sndMessage.time_q = time_quantum; // time quantum in nanoseconds
            if (msgsnd(msgid, &sndMessage, sizeof(sndMessage.time_q), 0) == -1) {
                cerr << "msgsnd";
                exit_handler();
            }
            
            // increment message count for this worker
            table[target_idx].messagesSent++;
            message_count++;

            // update total wait time
            long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
            long long wait_time = current_total - table[target_idx].last_scheduled_ns;
            if (wait_time > 0) {
                total_wait_time_ns += wait_time;
            }

            // receive reply from worker we just pinged
            MessageBuffer rcvMessage;
            if (msgrcv(msgid, &rcvMessage, sizeof(rcvMessage.time_q), getpid(), 0) == -1) {
                cerr << "msgrcv";
                exit_handler();
            }

            table[target_idx].last_scheduled_ns = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano); // update last scheduled time

            // If worker reported it is done, clean up PCB and counters
            if (rcvMessage.time_q < 0) {
                int worker_runtime = rcvMessage.time_q / -1; // convert to positive
                increment_clock(sec, nano, worker_runtime); // advance clock by worker runtime
                {
                    ostringstream ss;
                    ss << "OSS: Worker " << "PID " << next_worker_pid << " terminated after running for " << worker_runtime << " nanoseconds." << endl;
                    oss_log(ss.str());
                }
                wait(0);
                remove_pcb(table, next_worker_pid);
                running_processes = max(0, running_processes - 1);  
            }
            else if (rcvMessage.time_q == time_quantum) { // if worker is continuing
                increment_clock(sec, nano, rcvMessage.time_q); // increment clock by time quantum
                total_cpu_time_ns += rcvMessage.time_q;
                // update service time for the worker
                table[target_idx].serviceTimeNano += rcvMessage.time_q;
                if (table[target_idx].serviceTimeNano >= NSEC_PER_SEC) {
                    table[target_idx].serviceTimeSec += table[target_idx].serviceTimeNano / NSEC_PER_SEC;
                    table[target_idx].serviceTimeNano = table[target_idx].serviceTimeNano % NSEC_PER_SEC;
                }  
                {
                    ostringstream ss;
                    ss << "OSS: Received reply from worker " << "PID " << next_worker_pid << " ran for " << rcvMessage.time_q << " nanoseconds." << endl;
                    oss_log(ss.str());
                } 
            }
            else if (rcvMessage.time_q > 0 && rcvMessage.time_q < time_quantum) { // worker decided to block
                increment_clock(sec, nano, rcvMessage.time_q); // increment clock by reported time
                // update service time for the worker
                table[target_idx].serviceTimeNano += rcvMessage.time_q;
                if (table[target_idx].serviceTimeNano >= NSEC_PER_SEC) {
                    table[target_idx].serviceTimeSec += table[target_idx].serviceTimeNano / NSEC_PER_SEC;
                    table[target_idx].serviceTimeNano = table[target_idx].serviceTimeNano % NSEC_PER_SEC;
                }
                // set process to blocked state for 0.6 seconds
                table[target_idx].blocked = 1;
                total_blocked_time_ns += 600000000LL; // 0.6 seconds in nanoseconds
                // calculate unblock time (current time + 0.6 seconds)
                long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
                long long unblock_total = current_total + 600000000LL; // 0.6 seconds in nanoseconds
                table[target_idx].eventWaitSec = (int)(unblock_total / NSEC_PER_SEC);
                table[target_idx].eventWaitNano = (int)(unblock_total % NSEC_PER_SEC);
                {
                    ostringstream ss;
                    ss << "OSS: Worker " << "PID " << next_worker_pid
                    << " is blocked until " << table[target_idx].eventWaitSec << "s "
                    << table[target_idx].eventWaitNano << "ns." << endl;
                    oss_log(ss.str());
                }
            }
        }

        // call print_process_table every half-second of simulated time
        {
            long long current_total = (long long)(*sec) * NSEC_PER_SEC + (long long)(*nano);
            while (current_total >= next_print_total) {
                print_process_table(table);
                next_print_total += PRINT_INTERVAL_NANO;
            }
        }

        // increment simulated clock by fixed amount
        increment_clock(sec, nano, 1000); 
    }

    {
        ostringstream ss;
        ss << "OSS terminating after reaching process limit and all workers have finished." << endl;
        ss << "Number of processes launched: " << launched_processes << endl;
        ss << "Number of messages sent: " << message_count << endl;
        ss << "Total CPU time: " << total_cpu_time_ns / 1e9 << " seconds" << endl;
        ss << "total idle time: " << total_idle_time_ns / 1e9 << " seconds" << endl;
        ss << "Total blocked time: " << total_blocked_time_ns / 1e9 << " seconds" << endl;
        ss << "Average blocked time per process: " 
           << (launched_processes > 0 ? (total_blocked_time_ns / launched_processes) / 1e9 : 0.0) 
           << " seconds" << endl;
        ss << "Average wait time per process: " 
           << (launched_processes > 0 ? (total_wait_time_ns / launched_processes) / 1e9 : 0.0) 
           << " seconds" << endl;

        double total_time_simulated = (double)(*sec) + ((double)(*nano) / 1e9);
        double cpu_utilization = (total_time_simulated > 0.0) ? 
                                 ((double)total_cpu_time_ns / 1e9) / total_time_simulated * 100.0 : 0.0;
        ss << "CPU Utilization: " << fixed << setprecision(2) << cpu_utilization << "%" << endl;

        oss_log(ss.str());
    }
 
     shmdt(shm_clock);
     shmctl(shmid, IPC_RMID, nullptr);
     msgctl(msgid, IPC_RMID, nullptr);
     return 0;
 }