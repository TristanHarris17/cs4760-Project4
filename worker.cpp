#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>
#include <string>
#include <cstdlib>
#include <errno.h>
#include <climits>
#include <random>

using namespace std;

struct MessageBuffer {
    long mtype;
    int time_q; // time quantum in nanoseconds
};

int main(int argc, char* argv[]) {
    key_t sh_key = ftok("oss.cpp", 0);

    // create/get shared memory
    int shmid = shmget(sh_key, sizeof(int)*2, 0666);
    if (shmid == -1) {
        cerr << "shmget";
        exit(1);
    }

    // attach shared memory to shm_ptr
    int* clock = (int*) shmat(shmid, nullptr, 0);
    if (clock == (int*) -1) {
        cerr << "shmat";
        exit(1);
    }

    int *sec = &(clock[0]);
    int *nano = &(clock[1]);
    
    // get target time from command line args
    int target_seconds = stoi(argv[1]);
    int target_nano = stoi(argv[2]);

    // setup message queue
    key_t msg_key = ftok("oss.cpp", 1);
    int msgid = msgget(msg_key, 0666);
    if (msgid == -1) {
        cerr << "msgget";
        exit(1);
    }

    // initalize random number generator
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<> blocked_dist(0, 99); // 10% chance to block
    uniform_int_distribution<> block_time_dist(1000000, 40000000); // block time between 1ms and 40ms

    // Print starting message
    cout << "Worker starting, " << "PID:" << getpid() << " PPID:" << getppid() << endl
         << "Called With:" << endl
         << "Interval: " << target_seconds << " seconds, " << target_nano << " nanoseconds" << endl;

    // target interval (how much CPU time this worker needs) in seconds/nanoseconds
    // we'll compare accumulated run_time against this target interval
    const long long NSEC_PER_SEC = 1000000000LL;
    long long target_total_ns = (long long)target_seconds * NSEC_PER_SEC + (long long)target_nano;

    int run_time_sec = 0;
    int run_time_nano = 0;
 
     // worker just staring message
     cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
          << "SysClockS: " << *sec << " SysclockNano: " << *nano << " TermTimeS: " << target_seconds << " TermTimeNano: " << target_nano << endl
          << "--Just Starting" << endl;
 
     // message-driven loop: block until OSS tells us to check the clock
     MessageBuffer rcv_message;
     pid_t oss_pid = getppid();
 
     while (true) {
         // block until oss sends a message addressed to this worker (mtype == this pid)
         if (msgrcv(msgid, &rcv_message, sizeof(rcv_message.time_q), getpid(), 0) == -1) {
             if (errno == EINTR) continue;
             cerr << "msgrcv failed" << endl;
             break;
         }
 
        // compute run-time totals before consuming any of this quantum
        const long long NSEC_PER_SEC = 1000000000LL;
        long long before_total = (long long)run_time_sec * NSEC_PER_SEC + (long long)run_time_nano;
 
        // get random number to decide if process should become blocked
        int rand_num = blocked_dist(gen);
 
        // decide if we've reached the target interval
        if (before_total + (long long)rcv_message.time_q >= target_total_ns) {
            // how much of the last quantum was actually used to reach the target?
            long long used_in_last_quantum = target_total_ns - before_total;
            if (used_in_last_quantum < 0) used_in_last_quantum = 0;
            if (used_in_last_quantum > rcv_message.time_q) used_in_last_quantum = rcv_message.time_q;
 
            // update run_time to reflect actual usage
            long long new_total = before_total + used_in_last_quantum;
            run_time_sec = (int)(new_total / NSEC_PER_SEC);
            run_time_nano = (int)(new_total % NSEC_PER_SEC);
 
            MessageBuffer term_msg;
            term_msg.mtype = (long)oss_pid;
            term_msg.time_q = - (int) used_in_last_quantum; // negative indicates termination + how many ns used in last quantum
 
            cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
                 << "SysClockS: " << *sec << " SysclockNano: " << *nano << " TargetInterval: " << target_seconds << "s " << target_nano << "ns" << endl
                 << "-- Terminating after " << run_time_sec << "s " << run_time_nano << "ns of total run time." << endl;
 
            if (msgsnd(msgid, &term_msg, sizeof(term_msg.time_q), 0) == -1) {
                cerr << "msgsnd failed" << endl;
            }
            break;
        } else if (rand_num < 10) { // 10% chance to block
            // decide how much time was actually used before blocking
            int time_used = block_time_dist(gen);
            if (time_used > rcv_message.time_q) time_used = rcv_message.time_q;
 
            // update run_time to reflect the actual used time before blocking
            long long new_total = before_total + (long long)time_used;
            run_time_sec = (int)(new_total / NSEC_PER_SEC);
            run_time_nano = (int)(new_total % NSEC_PER_SEC);
 
            MessageBuffer block_msg;
            block_msg.mtype = (long)oss_pid;
            block_msg.time_q = time_used; // random time used before blocking
            cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
                 << "SysClockS: " << *sec << " SysclockNano: " << *nano << endl
                 << "-- Blocking after " << run_time_sec << "s " << run_time_nano << "ns of total run time." << endl;
            if (msgsnd(msgid, &block_msg, sizeof(block_msg.time_q), 0) == -1) {
                cerr << "msgsnd failed" << endl;
            }
        } else {
            // used the entire quantum; update run_time accordingly
            long long used = (long long)rcv_message.time_q;
            long long new_total = before_total + used;
            run_time_sec = (int)(new_total / NSEC_PER_SEC);
            run_time_nano = (int)(new_total % NSEC_PER_SEC);
 
            MessageBuffer cont_msg;
            cont_msg.mtype = (long)oss_pid;
            cont_msg.time_q = rcv_message.time_q; // indicate we used the full quantum
            cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
                 << "SysClockS: " << *sec << " SysclockNano: " << *nano << endl
                 << "-- Continuing with " << run_time_sec << "s " << run_time_nano << "ns of total run time." << endl;
            if (msgsnd(msgid, &cont_msg, sizeof(cont_msg.time_q), 0) == -1) {
                cerr << "msgsnd failed" << endl;
            }
        }
     }
     shmdt(clock);
     return 0;
}