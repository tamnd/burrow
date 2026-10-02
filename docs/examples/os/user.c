#include <stdio.h>

#include "burrow/burrow.h"
#include "burrow/mem/arena.h"
#include "burrow/os/user.h"

int main(void) {
    Arena ar;
    arena_init(&ar, NULL, 0);
    Alloc *a = arena_allocator(&ar);

    // doc: lookup
    Error err = BURROW_NO_ERROR;
    User *root = user_lookup_id(a, BURROW_S("0"), &err);
    if (BURROW_FAILED(err))
        return 1;
    printf(BURROW_STR_FMT " " BURROW_STR_FMT "\n", BURROW_STR_ARG(root->username),
           BURROW_STR_ARG(root->uid)); /* root 0 */

    UserGroup *g = user_lookup_group_id(a, root->gid, &err);
    if (BURROW_FAILED(err))
        return 1;
    printf("group " BURROW_STR_FMT "\n", BURROW_STR_ARG(g->gid));
    // doc: end

    // doc: unknown
    (void)user_lookup(a, BURROW_S("no-such-user"), &err);
    if (errors_as(err, TYPE_USER_UNKNOWN_USER_ERROR) != NULL)
        printf(BURROW_STR_FMT "\n", BURROW_STR_ARG(error_text(err)));
    // doc: end

    arena_free(&ar);
    return 0;
}

/* Output:
root 0
group 0
user: unknown user no-such-user
*/
