#include <iostream>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <unistd.h>
#include <string>
#include <cstdlib>
#include <errno.h>

using namespace std;

struct MessageBuffer {
    long mtype;
    int process_running; // 1 if running, 0 if not
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

    // calculate termination time
    int end_seconds = *sec + target_seconds;
    int end_nano = *nano + target_nano;
    if (end_nano >= 1000000000) {
        end_seconds += end_nano / 1000000000;
        end_nano = end_nano % 1000000000;
    }

    // worker just staring message
    cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
         << "SysClockS: " << *sec << " SysclockNano: " << *nano << " TermTimeS: " << end_seconds << " TermTimeNano: " << end_nano << endl
         << "--Just Starting" << endl;

    // message-driven loop: block until OSS tells us to check the clock
    MessageBuffer msg;
    pid_t oss_pid = getppid();
    int message_count = 0;

    while (true) {
        // block until oss sends a message addressed to this worker (mtype == this pid)
        if (msgrcv(msgid, &msg, sizeof(msg.process_running), getpid(), 0) == -1) {
            if (errno == EINTR) continue;
            cerr << "msgrcv failed" << endl;
            break;
        }

        // Print message received
        cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
         << "SysClockS: " << *sec << " SysclockNano: " << *nano << " TermTimeS: " << end_seconds << " TermTimeNano: " << end_nano << endl
         << "-- " << ++message_count << " messages received from oss" << endl;

        // After receiving the ping, check if it's time to terminate
        bool should_terminate = ((*sec > end_seconds) || (*sec == end_seconds && *nano >= end_nano));

        if (should_terminate) {
            // print terminating message
            cout << "Worker PID:" << getpid() << " PPID:" << getppid() << endl
                 << "SysClockS: " << *sec << " SysclockNano: " << *nano << " TermTimeS: " << end_seconds << " TermTimeNano: " << end_nano << endl
                 << "--Terminating after sending message back to oss after " << message_count << " received messages." << endl;

            // notify OSS that this process is no longer running (process_running = 0)
            MessageBuffer reply;
            reply.mtype = (long)oss_pid;
            reply.process_running = 0;
            if (msgsnd(msgid, &reply, sizeof(reply.process_running), 0) == -1) {
                cerr << "msgsnd failed" << endl;
            }
            break; // exit loop and terminate
        } else {
            // notify OSS that this process is still running (process_running = 1)
            MessageBuffer reply;
            reply.mtype = (long)oss_pid;
            reply.process_running = 1;
            if (msgsnd(msgid, &reply, sizeof(reply.process_running), 0) == -1) {
                cerr << "msgsnd failed" << endl;
            }
        }
    }
    shmdt(clock);
    return 0;
}