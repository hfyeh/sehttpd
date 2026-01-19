#ifndef LIST_H
#define LIST_H

#include <stddef.h>

/**
 * @brief Circular Doubly Linked List implementation.
 *
 * This is based on the Linux Kernel's list implementation.
 * Unlike standard lists where data is inside the node, here the node (list_head)
 * is embedded inside the data structure.
 */

struct list_head {
    struct list_head *prev, *next;
};

typedef struct list_head list_head;

/**
 * @brief Initializes the list head.
 *
 * A header points to itself when empty.
 */
#define INIT_LIST_HEAD(ptr)           \
    do {                              \
        struct list_head *_ptr = ptr; \
        (_ptr)->next = (_ptr);        \
        (_ptr->prev) = (_ptr);        \
    } while (0)

/* Insert a new entry between two known consecutive entries */
static inline void __list_add(struct list_head *_new,
                              struct list_head *prev,
                              struct list_head *next)
{
    _new->next = next;
    next->prev = _new;
    prev->next = _new;
    _new->prev = prev;
}

/**
 * @brief Adds a new entry after the specified head.
 *
 * @param _new New entry to be added.
 * @param head List head to add it after.
 */
static inline void list_add(struct list_head *_new, struct list_head *head)
{
    __list_add(_new, head, head->next);
}

/**
 * @brief Adds a new entry before the specified head (at the tail).
 *
 * @param _new New entry to be added.
 * @param head List head to add it before.
 */
static inline void list_add_tail(struct list_head *_new, struct list_head *head)
{
    __list_add(_new, head->prev, head);
}

/* Delete an entry by making the prev/next entries point to each other */
static inline void __list_del(struct list_head *prev, struct list_head *next)
{
    prev->next = next;
    next->prev = prev;
}

/**
 * @brief Deletes entry from list.
 *
 * @param entry The element to delete from the list.
 */
static inline void list_del(struct list_head *entry)
{
    __list_del(entry->prev, entry->next);
}

/**
 * @brief Tests whether a list is empty.
 * @param head The list to test.
 * @return int 1 if empty, 0 otherwise.
 */
static inline int list_empty(struct list_head *head)
{
    return (head->next == head) && (head->prev == head);
}

/**
 * @brief Casts a member of a structure out to the containing structure.
 *
 * @param ptr    The pointer to the member.
 * @param type   The type of the container struct this is embedded in.
 * @param member The name of the member within the struct.
 *
 * Logic:
 * 1. Get the offset of 'member' inside 'type'.
 * 2. Subtract that offset from 'ptr' address.
 * 3. Cast the result to 'type *'.
 */
#define container_of(ptr, type, member)                      \
    ({                                                       \
        const typeof(((type *) 0)->member) *__mptr = (ptr);  \
        (type *) ((char *) __mptr - offsetof(type, member)); \
    })

/**
 * @brief Get the struct for this entry.
 * @param ptr    The &struct list_head pointer.
 * @param type   The type of the struct this is embedded in.
 * @param member The name of the list_head within the struct.
 */
#define list_entry(ptr, type, member) container_of(ptr, type, member)

/**
 * @brief Iterate over a list safe against removal of list entry.
 *
 * @param pos  The &struct list_head to use as a loop cursor.
 * @param n    Another &struct list_head to use as temporary storage.
 * @param head The head for your list.
 */
#define list_for_each_safe(pos, n, head)                   \
    for (pos = (head)->next, n = pos->next; pos != (head); \
         pos = n, n = pos->next)

#endif
