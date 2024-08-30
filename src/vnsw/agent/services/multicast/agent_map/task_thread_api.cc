/*
 * Copyright (c) 2017 Juniper Networks, Inc. All rights reserved.
 */

#ifdef __cplusplus
extern "C" {
#endif
#include "task_thread_api.h"
#ifdef __cplusplus
}
#endif

void thread_new_circular_thread(task_thread *head)
{
    if (!head) {
        return;
    }

    head->next = head;
    head->prev = head;

    return;
}

boolean thread_circular_thread_empty(task_thread *head)
{
    return (head->next == head) ? TRUE : FALSE;
}

boolean thread_circular_thread_head(task_thread *head, task_thread *node)
{
    return (head == node) ? TRUE : FALSE;
}

boolean thread_node_on_thread(const task_thread *node)
{
    return (node->next) ? TRUE : FALSE;
}

task_thread *thread_circular_top(task_thread *head)
{
    if (head->next == head) {
        return NULL;
    }

    return head->next;
}

void thread_circular_add_top(task_thread *head, task_thread *node)
{
    node->next = head->next;
    node->prev = head;
    head->next = node;
    node->next->prev = node;

    return;
}

void thread_circular_add_bottom(task_thread *head, task_thread *node)
{
    thread_circular_add_top(head->prev, node);
}

task_thread *thread_next_node(task_thread *node)
{
    return node->next;
}

task_thread *thread_circular_thread_next(task_thread *head, task_thread *node)
{
    if (!node) {
        return (head->next == head) ? NULL : head->next;
    }

    return (node->next == head) ? NULL : node->next;
}

void thread_remove(task_thread *node)
{
    if (!node->next) {
        return;
    }

    node->next->prev = node->prev;
    node->prev->next = node->next;

    node->next = NULL;
    node->prev = NULL;
}

task_thread *thread_circular_dequeue_top(task_thread *head)
{
    task_thread *current = NULL;

    if (head->next == head) {
        return NULL;
    }

    current = head->next;
    thread_remove(current);

    return current;
}

