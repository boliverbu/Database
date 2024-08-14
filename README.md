```
 ____        _        _
|  _ \  __ _| |_ __ _| |__   __ _ ___  ___
| | | |/ _` | __/ _` | '_ \ / _` / __|/ _ \
| |_| | (_| | || (_| | |_) | (_| \__ \  __/
|____/ \__,_|\__\__,_|_.__/ \__,_|___/\___|
```

My code works by first creating a listener thread through start_listener, which 
listens for any client connections. Each client calls run_client, which is in
a doubly linked client list and uses interpret command to run any valid input given by the client.
I only have coarse grain locking implemented and my code doesn't handle SIGINT, but otherwise
my code works properly. I added a cleanup_unlock_mutex helper function which is called
in client_control_wait to guarantee the mutex is unlocked.
