/*
 * <search.h> binary trees for VLC on the PS5 (VLC's plugin, option and hotkey
 * registries): the console's libc doesn't export them to a title. Its own file
 * so the Linux test build can link it in place of glibc's (host/build-app.sh
 * VLCPS5_OWN_TSEARCH=1) and run VLC on it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <search.h>
#include <stdlib.h>

/* ---- <search.h> binary trees (VLC's plugin and option registry) ----
 * Unbalanced, like FreeBSD's historic tsearch.c; same node layout, so the key
 * pointer is the node's first member as POSIX callers expect. */

typedef struct node {
    const void *key;
    struct node *left, *right;
} node_t;

void *tsearch(const void *key, void **rootp, int (*compar)(const void *, const void *))
{
    if (!rootp)
        return NULL;
    node_t **p = (node_t **)rootp;
    while (*p) {
        int r = compar(key, (*p)->key);
        if (r == 0)
            return *p;
        p = r < 0 ? &(*p)->left : &(*p)->right;
    }
    node_t *n = malloc(sizeof(*n));
    if (!n)
        return NULL;
    n->key = key;
    n->left = n->right = NULL;
    *p = n;
    return n;
}

void *tfind(const void *key, void *const *rootp, int (*compar)(const void *, const void *))
{
    if (!rootp)
        return NULL;
    node_t *n = *(node_t *const *)rootp;
    while (n) {
        int r = compar(key, n->key);
        if (r == 0)
            return n;
        n = r < 0 ? n->left : n->right;
    }
    return NULL;
}

/* Returns the deleted node's parent (or the new root, or a non-NULL dummy). */
void *tdelete(const void *restrict key, void **restrict rootp,
              int (*compar)(const void *, const void *))
{
    static node_t dummy;
    if (!rootp || !*rootp)
        return NULL;
    node_t **p = (node_t **)rootp, *parent = NULL;
    int r;
    while ((r = compar(key, (*p)->key)) != 0) {
        parent = *p;
        p = r < 0 ? &(*p)->left : &(*p)->right;
        if (!*p)
            return NULL;
    }
    node_t *del = *p, *repl;
    if (!del->left)
        repl = del->right;
    else if (!del->right)
        repl = del->left;
    else {
        /* Replace with the in-order successor. */
        node_t **s = &del->right;
        while ((*s)->left)
            s = &(*s)->left;
        repl = *s;
        *s = repl->right;
        repl->left = del->left;
        repl->right = del->right;
    }
    *p = repl;
    free(del);
    if (parent)
        return parent;
    return *rootp ? *rootp : &dummy;
}

static void walk(const node_t *n, void (*action)(const void *, VISIT, int), int depth)
{
    if (!n->left && !n->right) {
        action(n, leaf, depth);
        return;
    }
    action(n, preorder, depth);
    if (n->left)
        walk(n->left, action, depth + 1);
    action(n, postorder, depth);
    if (n->right)
        walk(n->right, action, depth + 1);
    action(n, endorder, depth);
}

void twalk(const void *root, void (*action)(const void *, VISIT, int))
{
    if (root && action)
        walk(root, action, 0);
}
