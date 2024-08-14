#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "./comm.h"
#include "./db.h"
#include "./server.h"

client_t *thread_list_head;
pthread_mutex_t thread_list_mutex = PTHREAD_MUTEX_INITIALIZER;

client_control_t client_control_inst = {PTHREAD_MUTEX_INITIALIZER,
                                        PTHREAD_COND_INITIALIZER, 0};

server_control_t serv_control = {PTHREAD_MUTEX_INITIALIZER,
                                 PTHREAD_COND_INITIALIZER, 0};

int accepting = 1;
//------------------------------------------------------------------------------------------------
// Client threads' constructor and main method

// Called by listener (in comm.c) to create a new client thread
void client_constructor(FILE *cxstr) {
    /*
     * TODO:
     * Part 1A:
     *  You should create a new client_t struct (see server.h) here and
     * initialize ALL of its fields. Remember that these initializations should
     * be error-checked.
     *
     *  Step 1. Allocate memory for a new client and set its connection stream
     *          to the input argument.
     *  Step 2. Initialize the client's list-related fields to a reasonable
     * default. Step 3. Create the new client thread running the `run_client`
     * routine. Step 4. Detach the new client thread.
     */
    client_t *client;
    if ((client = (client_t *)malloc(sizeof(client_t))) == NULL) {
        perror("malloc");
    }
    client->cxstr = cxstr;
    client->prev = client;
    client->next = client;
    int error;
    if ((error = pthread_create(&client->thread, 0, run_client, client)) != 0) {
        handle_error_en(error, "create");
    }
    if ((error = pthread_detach(client->thread)) != 0) {
        handle_error_en(error, "detach");
    }
    serv_control.num_client_threads++;
}

void cleanup_unlock_mutex(void *mutex) {
    pthread_mutex_unlock((pthread_mutex_t *)mutex);
}

// Code executed by a client thread
void *run_client(void *arg) {
    /*
     * TODO:
     * Part 1A:
     *  Step 1. For the passed-in client, loop calling `comm_serve` (in comm.c),
     * to output the previous response and then read in the client's next
     * command, until the client disconnects. Execute commands using
     * `interpret_command` (in db.c). Step 2. When the client is done sending
     * commands, call `thread_cleanup`.
     *
     * */
    if (accepting == 0) {
        client_destructor(arg);
        return (void *)-1;
    }
    char response[BUFLEN];
    char command[BUFLEN];
    memset(command, 0, BUFLEN);
    memset(response, 0, BUFLEN);

    pthread_mutex_lock(&thread_list_mutex);
    if (thread_list_head == NULL) {
        thread_list_head = arg;
    } else {
        thread_list_head->prev->next = arg;
        ((client_t *)arg)->prev = thread_list_head->prev;
        thread_list_head->prev = arg;
        ((client_t *)arg)->next = thread_list_head;
        thread_list_head = arg;
    }
    pthread_cleanup_push(thread_cleanup, arg);
    pthread_mutex_unlock(&thread_list_mutex);

    while (1) {
        if (comm_serve(((client_t *)arg)->cxstr, response, command) == -1) {
            break;
        }
        client_control_wait();
        interpret_command(command, response, BUFLEN);
    }
    pthread_cleanup_pop(1);
    /*
     * Part 1B: Before looping, add the passed-in client to the client list. Be
     * sure to protect access to the client list using `thread_list_mutex`.
     * */

    /*
     * Part 3A: Use `client_control_wait` to stop the client thread from
     * interpreting commands while the server is stopped.
     *
     * Part 3B: Support cancellation of the client thread by instead using
     * cleanup handlers to call `thread_cleanup`.
     *
     * Part 3C: Make sure that the server is still accepting clients before
     * adding a client to the client list (see step 2 in `main`). If not,
     * destroy the passed-in client and return.
     */
    return NULL;
}

//------------------------------------------------------------------------------------------------
// Methods for client thread cleanup, destruction, and cancellation

void client_destructor(client_t *client) {
    /*
     * TODO:
     * Part 1A: Free and close all resources associated with a client.
     * (Take a look at `comm_shutdown` in comm.c)
     */
    comm_shutdown(client->cxstr);
    free(client);
}

// Cleanup routine for client threads, called on cancels and exit.
void thread_cleanup(void *arg) {
    /*
     * TODO:
     * Part 1A: Call `client_destructor` on the passed-in client.
     * */

    pthread_mutex_lock(&thread_list_mutex);
    // Case where arg is only element in client list
    if (((client_t *)arg)->next == ((client_t *)arg)) {
        thread_list_head = NULL;
    } else {
        // arg is first element in list
        if (arg == thread_list_head) {
            thread_list_head = ((client_t *)arg)->next;
        }
        // Removing from client list
        ((client_t *)arg)->next->prev = ((client_t *)arg)->prev;
        ((client_t *)arg)->prev->next = ((client_t *)arg)->next;
    }
    pthread_mutex_unlock(&thread_list_mutex);
    pthread_mutex_lock(&serv_control.server_mutex);
    serv_control.num_client_threads--;
    if (serv_control.num_client_threads == 0) {
        int err;
        if ((err = pthread_cond_broadcast(&serv_control.server_cond)) != 0) {
            handle_error_en(err, "broadcast");
        }
    }
    pthread_mutex_unlock(&serv_control.server_mutex);
    client_destructor((client_t *)arg);
    /*
     * Part 1B: Remove the passed-in client from the client list before
     * destroying it. Note that the client must be in the list before this
     * routine is ever run. Be sure to protect access to the client list using
     * `thread_list_mutex`.
     */
}

void delete_all() {
    /*
     * TODO:
     * Part 3C: Cancel every thread in the client thread list with using
     * `pthread_cancel`.
     */
    pthread_mutex_lock(&thread_list_mutex);
    if (thread_list_head == NULL) {
        return;
    }
    client_t *client = thread_list_head;
    do {
        pthread_cancel(client->thread);
        client = client->next;
    } while (client != thread_list_head);
    thread_list_head = NULL;
    pthread_mutex_unlock(&thread_list_mutex);
}

//------------------------------------------------------------------------------------------------
// Methods for stop/go server commands

// Called by client threads to wait until progress is permitted
void client_control_wait() {
    /*
     * TODO:
     * Part 3A: Block the calling thread until the main thread calls
     * `client_control_release`. See the `client_control_t` struct.
     *
     * Part 3B: Support thread-safe cancellation of a client thread by
     * using cleanup handlers. (Remember that `pthread_cond_wait` is a
     * cancellation point!)
     */
    pthread_cleanup_push(cleanup_unlock_mutex, &client_control_inst.go_mutex);

    pthread_mutex_lock(&client_control_inst.go_mutex);
    while (client_control_inst.stopped) {
        int err;
        if ((err = pthread_cond_wait(&client_control_inst.go,
                                     &client_control_inst.go_mutex)) != 0) {
            handle_error_en(err, "wait");
        }
    }

    pthread_cleanup_pop(1);
}

// Called by main thread to stop client threads
void client_control_stop() {
    pthread_mutex_lock(&client_control_inst.go_mutex);
    client_control_inst.stopped = 1;
    pthread_mutex_unlock(&client_control_inst.go_mutex);
    /*
     * TODO:
     * Part 3A: Ensure that the next time client threads call
     * `client_control_wait` in `run_client`, they will block. See the
     * `client_control_t` struct.
     */
}

// Called by main thread to resume client threads
void client_control_release() {
    /*
     * TODO:
     * Part 3A: Allow clients that are blocked within `client_control_wait`
     * to continue. See the `client_control_t` struct.
     */
    pthread_mutex_lock(&client_control_inst.go_mutex);
    client_control_inst.stopped = 0;
    int err;
    if ((err = pthread_cond_broadcast(&client_control_inst.go)) != 0) {
        handle_error_en(err, "broadcast");
    }
    pthread_mutex_unlock(&client_control_inst.go_mutex);
}

//------------------------------------------------------------------------------------------------
// Main function

// The arguments to the server should be the port number.
int main(int argc, char *argv[]) {
    /*
     * TODO:
     * Part 1A:
     *  Step 1. Block SIGPIPE using `pthread_sigmask` so that the server does
     * not abort when a client disconnects. Step 2. Start a listener thread for
     * clients (see `start_listener` in comm.c). Step 3. Join with the listener
     * thread.
     *
     */
    if (argc != 2) {
        fprintf(stderr, "Usage: port\n");
        exit(1);
    }
    sigset_t mask;
    sigaddset(&mask, SIGPIPE);
    int error = 0;
    if ((error = pthread_sigmask(SIG_SETMASK, &mask, NULL)) != 0) {
        handle_error_en(error, "sigmask");
    }
    pthread_t listener_thread =
        start_listener(atoi(argv[1]), client_constructor);

    char buffer[512];
    char command[50], filename[50];

    // REPL
    while (1) {
        int length = read(0, buffer, 512);

        // TODO: CTRL-D case - Part 3C
        if (length == 0) {
            accepting = 0;
            break;
        }

        int vars_filled = sscanf(buffer, "%s %s", command, filename);
        if (vars_filled == 0) {
            continue;
        }
        // Print
        if (strcmp(command, "p") == 0) {
            if (vars_filled == 2) {
                db_print(filename);
            } else {
                db_print(NULL);
            }
        } else if (command[0] == 'p') {
            db_print(command + 1);
        }

        // Stop
        else if (strcmp(command, "s") == 0 || command[0] == 's') {
            client_control_stop();
            write(1, "stopping all clients\n",
                  strlen("stopping all clients\n"));
        }

        // Go
        else if (strcmp(command, "g") == 0 || command[0] == 'g') {
            client_control_release();
            write(1, "releasing all clients\n",
                  strlen("releasing all clients\n"));
        }
    }
    delete_all();
    pthread_mutex_lock(&serv_control.server_mutex);
    while (serv_control.num_client_threads > 0) {
        int err;
        if ((err = pthread_cond_wait(&serv_control.server_cond,
                                     &serv_control.server_mutex)) != 0) {
            handle_error_en(err, "wait");
        }
    }
    pthread_mutex_unlock(&serv_control.server_mutex);

    db_cleanup();

    pthread_cancel(listener_thread);
    pthread_mutex_destroy(&client_control_inst.go_mutex);
    pthread_mutex_destroy(&serv_control.server_mutex);
    pthread_mutex_destroy(&thread_list_mutex);
    pthread_cond_destroy(&client_control_inst.go);
    pthread_cond_destroy(&serv_control.server_cond);

    if ((error = pthread_join(listener_thread, NULL)) != 0) {
        handle_error_en(error, "join");
    }
    /*
     * Part 3A: Before joining the listener thread, loop for command line input
     * and handle any print, stop, and go command requests.
     *
     * Part 3C:
     *  Step 1. Modify the command line loop to break on receiving EOF.
     *  Step 2. After receiving EOF, use a thread-safe mechanism to indicate
     * that the server is no longer accepting clients, and then cancel all
     * client threads using `delete_all`. Think carefully about what happens at
     * the start of `run_client` and ensure that your mechanism does not allow
     * any way for a thread to add itself to the thread list after your
     * mechanism is activated. Step 3. After calling `delete_all`, make sure
     * that the thread list is empty using the `server_control_t` struct. (Note
     * that you will need to modify other functions for the struct to accurately
     * keep track of the number of threads in the list - where does it make
     * sense to modify the `num_client_threads` field?) Step 4. Once the thread
     * list is empty, cleanup the database, and then cancel and join with the
     * listener thread.
     */

    return 0;
}
