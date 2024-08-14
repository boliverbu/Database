#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "./db.h"

#define MAXLEN 256

// The root node of the binary tree, unlike all
// other nodes in the tree, this one is never
// freed (it's allocated in the data region).
node_t head = {"", "", 0, 0};

// Global Mutex for Coarse Grain locking
pthread_rwlock_t db_lock = PTHREAD_RWLOCK_INITIALIZER;

//------------------------------------------------------------------------------------------------
// Constructor, destructor, and cleanup methods

node_t *node_constructor(char *arg_key, char *arg_value, node_t *arg_left,
                         node_t *arg_right) {
    size_t key_len = strlen(arg_key);
    size_t val_len = strlen(arg_value);

    if (key_len > MAXLEN || val_len > MAXLEN) return 0;

    node_t *new_node = (node_t *)malloc(sizeof(node_t));

    if (new_node == NULL) return 0;

    if ((new_node->key = (char *)malloc(key_len + 1)) == NULL) {
        free(new_node);
        return 0;
    }
    if ((new_node->value = (char *)malloc(val_len + 1)) == NULL) {
        free(new_node->key);
        free(new_node);
        return 0;
    }

    if ((snprintf(new_node->key, MAXLEN, "%s", arg_key)) < 0) {
        free(new_node->value);
        free(new_node->key);
        free(new_node);
        return 0;
    }
    if ((snprintf(new_node->value, MAXLEN, "%s", arg_value)) < 0) {
        free(new_node->value);
        free(new_node->key);
        free(new_node);
        return 0;
    }

    new_node->lchild = arg_left;
    new_node->rchild = arg_right;
    return new_node;
}

void node_destructor(node_t *node) {
    if (node->key != NULL) free(node->key);
    if (node->value != NULL) free(node->value);
    free(node);
}

/* Recursively destroys node and all its children. */
void db_cleanup_recurs(node_t *node) {
    if (node == NULL) {
        return;
    }

    db_cleanup_recurs(node->lchild);
    db_cleanup_recurs(node->rchild);

    node_destructor(node);
}

void db_cleanup() {
    db_cleanup_recurs(head.lchild);
    db_cleanup_recurs(head.rchild);
}

//------------------------------------------------------------------------------------------------
// Database modifiers and accessors

node_t *search(char *key, node_t *parent, node_t **parentpp) {
    /*
     * TODO:
     * Part 2: Make this thread safe!
     */
    node_t *next;
    if (strcmp(key, parent->key) < 0) {
        next = parent->lchild;
    } else {
        next = parent->rchild;
    }

    node_t *result;
    if (next == NULL) {
        result = NULL;
    } else {
        if (strcmp(key, next->key) == 0) {
            result = next;
        } else {
            return search(key, next, parentpp);
        }
    }

    if (parentpp != NULL) {
        *parentpp = parent;
    }
    return result;
}

void db_query(char *key, char *result, int len) {
    /*
     * TODO:
     * Part 2: Make this thread safe!
     */
    pthread_rwlock_rdlock(&db_lock);
    node_t *target = search(key, &head, NULL);
    if (target == NULL) {
        snprintf(result, len, "not found");
    } else {
        snprintf(result, len, "%s", target->value);
    }
    pthread_rwlock_unlock(&db_lock);
}

int db_add(char *key, char *value) {
    /*
     * TODO:
     * Part 2: Make this thread safe!
     */
    pthread_rwlock_wrlock(&db_lock);
    node_t *parent;
    node_t *target;
    if ((target = search(key, &head, &parent)) != NULL) {
        pthread_rwlock_unlock(&db_lock);
        return 0;
    }

    node_t *newnode = node_constructor(key, value, NULL, NULL);

    if (strcmp(key, parent->key) < 0)
        parent->lchild = newnode;
    else
        parent->rchild = newnode;
    pthread_rwlock_unlock(&db_lock);
    return 1;
}

int db_remove(char *key) {
    /*
     * TODO:
     * Part 2: Make this thread safe!
     */
    node_t *parent;  // parent of the node to delete
    node_t *dnode;   // node to delete
    pthread_rwlock_wrlock(&db_lock);
    // first, find the node to be removed
    if ((dnode = search(key, &head, &parent)) == NULL) {
        // it's not there
        pthread_rwlock_unlock(&db_lock);
        return 0;
    }

    // We found it. If the target has no right child, then we can simply replace
    // its parent's pointer to the target with the target's own left child.

    if (dnode->rchild == NULL) {
        if (strcmp(dnode->key, parent->key) < 0)
            parent->lchild = dnode->lchild;
        else
            parent->rchild = dnode->lchild;

        // done with dnode
        node_destructor(dnode);
    } else if (dnode->lchild == NULL) {
        // ditto if the target has no left child
        if (strcmp(dnode->key, parent->key) < 0)
            parent->lchild = dnode->rchild;
        else
            parent->rchild = dnode->rchild;

        // done with dnode
        node_destructor(dnode);
    } else {
        // Find the lexicographically smallest node in the right subtree and
        // replace the node to be deleted with that node. This new node thus is
        // lexicographically smaller than all nodes in its right subtree, and
        // greater than all nodes in its left subtree

        node_t *next = dnode->rchild;
        node_t **pnext = &dnode->rchild;

        while (next->lchild != NULL) {
            // work our way down the lchild chain, finding the smallest node
            // in the subtree.
            node_t *nextl = next->lchild;
            pnext = &next->lchild;
            next = nextl;
        }

        // replace next's position on right subtree with its right child
        *pnext = next->rchild;

        // replace dnode with the contents of next
        dnode->key = realloc(dnode->key, strlen(next->key) + 1);
        dnode->value = realloc(dnode->value, strlen(next->value) + 1);

        snprintf(dnode->key, MAXLEN, "%s", next->key);
        snprintf(dnode->value, MAXLEN, "%s", next->value);

        node_destructor(next);
    }
    pthread_rwlock_unlock(&db_lock);
    return 1;
}

//------------------------------------------------------------------------------------------------
// Printing methods and their helpers

static inline void print_spaces(int lvl, FILE *out) {
    for (int i = 0; i < lvl; i++) {
        fprintf(out, " ");
    }
}

/* helper function for db_print */
void db_print_recurs(node_t *node, int lvl, FILE *out) {
    /*
     * TODO:
     * Part 2: Make this thread safe!
     */
    print_spaces(lvl, out);  // print spaces to differentiate levels

    // print node's key/value, or (root) if it's the root
    if (node == NULL) {
        fprintf(out, "(null)\n");
        return;
    }

    if (node == &head) {
        fprintf(out, "(root)\n");
    } else {
        fprintf(out, "%s %s\n", node->key, node->value);
    }

    db_print_recurs(node->lchild, lvl + 1, out);
    db_print_recurs(node->rchild, lvl + 1, out);
}

int db_print(char *filename) {
    FILE *out;
    if (filename == NULL) {
        db_print_recurs(&head, 0, stdout);
        return 0;
    }

    // skip over leading whitespace
    while (isspace(*filename)) {
        filename++;
    }

    if (*filename == '\0') {
        db_print_recurs(&head, 0, stdout);
        return 0;
    }

    if ((out = fopen(filename, "w+")) == NULL) {
        return -1;
    }

    db_print_recurs(&head, 0, out);
    fclose(out);

    return 0;
}

//------------------------------------------------------------------------------------------------
// Command interpreting

/*
 * Interprets the given command string and writes up to len bytes into response,
 * where len is the buffer size.
 */
void interpret_command(char *command, char *response, int len) {
    char value[MAXLEN];
    char ibuf[MAXLEN];
    char name[MAXLEN];
    int sscanf_ret;

    if (strlen(command) <= 1) {
        snprintf(response, len, "ill-formed command");
        return;
    }

    // which command is it?
    switch (command[0]) {
        case 'q':
            // Query
            sscanf_ret = sscanf(&command[1], "%255s", name);
            if (sscanf_ret < 1) {
                snprintf(response, len, "ill-formed command");
                return;
            }
            db_query(name, response, len);
            if (strlen(response) == 0) {
                snprintf(response, len, "not found");
            }
            return;

        case 'a':
            // Add to the database
            sscanf_ret = sscanf(&command[1], "%255s %255s", name, value);
            if (sscanf_ret < 2) {
                snprintf(response, len, "ill-formed command");
                return;
            }
            if (db_add(name, value)) {
                snprintf(response, len, "added");
            } else {
                snprintf(response, len, "already in database");
            }
            return;

        case 'd':
            // Delete from the database
            sscanf_ret = sscanf(&command[1], "%255s", name);
            if (sscanf_ret < 1) {
                snprintf(response, len, "ill-formed command");
                return;
            }
            if (db_remove(name)) {
                snprintf(response, len, "removed");
            } else {
                snprintf(response, len, "not in database");
            }
            return;

        case 'f':
            // process the commands in a file (silently)
            sscanf_ret = sscanf(&command[1], "%255s", name);
            if (sscanf_ret < 1) {
                snprintf(response, len, "ill-formed command");
                return;
            }

            FILE *finput = fopen(name, "r");
            if (!finput) {
                snprintf(response, len, "bad file name");
                return;
            }
            while (fgets(ibuf, sizeof(ibuf), finput) != 0) {
                pthread_testcancel();  // fgets is not a cancellation point
                interpret_command(ibuf, response, len);
            }
            fclose(finput);
            snprintf(response, len, "file processed");
            return;

        default:
            snprintf(response, len, "ill-formed command");
            return;
    }
}
