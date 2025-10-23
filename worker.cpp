#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>
#include <string>
#include <cstdlib>
#include <errno.h>
#include <climits>

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
 
         // increase process run time
        // increment run time by the quantum we received
        long long before_total = (long long)run_time_sec * NSEC_PER_SEC + (long long)run_time_nano;
        long long after_total = before_total + (long long)rcv_message.time_q;
        // normalize run_time
        run_time_sec = (int)(after_total / NSEC_PER_SEC);
        run_time_nano = (int)(after_total % NSEC_PER_SEC);
 
        // decide if we've reached the target interval
        if (after_total >= target_total_ns) {
            // how much of the last quantum was actually used to reach the target?
            long long used_in_last_quantum = target_total_ns - before_total;
            if (used_in_last_quantum < 0) used_in_last_quantum = 0;
            if (used_in_last_quantum > rcv_message.time_q) used_in_last_quantum = rcv_message.time_q;
 
            MessageBuffer term_msg;
            term_msg.mtype = (long)oss_pid;
            term_msg.time_q = - (int) used_in_last_quantum; // negative indicates termination + how many ns used in last quantum
 
            cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
                 << "SysClockS: " << *sec << " SysclockNano: " << *nano << " TargetInterval: " << target_seconds << "s " << target_nano << "ns" << endl
                 << "-- Terminating after " << run_time_sec << " seconds and " << used_in_last_quantum << " nanoseconds of run time." << endl;
 
            if (msgsnd(msgid, &term_msg, sizeof(term_msg.time_q), 0) == -1) {
                cerr << "msgsnd failed" << endl;
            }
            break;
        } else {
            // still need more time, reply that we used the full quantum
            MessageBuffer cont_msg;
            cont_msg.mtype = (long)oss_pid;
            cont_msg.time_q = rcv_message.time_q; // indicate we used the full quantum
            cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
                 << "SysClockS: " << *sec << " SysclockNano: " << *nano << endl
                 << "-- Continuing after " << run_time_sec << "s " << run_time_nano << "ns of run time." << endl;
            if (msgsnd(msgid, &cont_msg, sizeof(cont_msg.time_q), 0) == -1) {
                cerr << "msgsnd failed" << endl;
            }
        }
     }
     shmdt(clock);
     return 0;
}