#include <linux/slab.h>
#include "fat.h"
#include "ssd_cache.h"
#include <linux/kthread.h>
#include <linux/random.h>
#include <linux/radix-tree.h>

void unmap_inode_buffers(struct inode *inode) {
    struct address_space *as_data = &inode->i_data;
    struct radix_tree_root *page_tree = &as_data->page_tree;
    void **slot;
    struct radix_tree_iter iter;
    struct buffer_head *bh, *head;
    struct page *page;

    radix_tree_for_each_slot(slot, page_tree, &iter, 0) {
        page = radix_tree_deref_slot(slot);
        if (page_has_buffers(page)) {
            head = page_buffers(page);
            bh = head;
            do {
                clear_buffer_mapped(bh);
                bh = bh->b_this_page;
            } while (bh != head);
        }
    }
}

/**
    Bewegt eine Datei die potentiell gerade offen ist
    -> AS locken und BHs anpassen.
*/
int swap_file(struct inode *f_inode, struct msdos_dir_entry *dir_entry, int target) {
    struct msdos_sb_info *sbi;
    int err = 0;
    int *block_arr;

    sbi = MSDOS_SB(f_inode->i_sb);

    block_arr = kmalloc((f_inode->i_size / 4096) + 1, GFP_KERNEL);
    err = fat_alloc_clusters_sb(f_inode->i_sb, block_arr, (f_inode->i_size / 4096) + 1, target);
    if (err) goto out;

    mutex_lock(&sbi->read_pages_lock);

    copy_fat_chain(f_inode->i_sb, fat_get_start(sbi, dir_entry), block_arr[0]);


    // walk through AS and update pointers
    unmap_inode_buffers(f_inode);


out:
    kfree(block_arr);
    mutex_unlock(&sbi->read_pages_lock);
    return err;
}

int loopdir(void *__buf, const char *linux_name, int namlen, loff_t offset, u64 ino, unsigned int d_type) {
    struct msdos64_callback *ctx = (struct msdos64_callback *) __buf;
    struct msdos_sb_info *sbi;
    struct file *current_file, *parent_file;
    struct inode *current_inode, *parent_inode;
    struct msdos_dir_entry de;
    struct path path;
    char random_num_buf;
    int start_cluster = 0;
    char name[namlen + 1];

    strlcpy(name, linux_name, namlen + 1);

    if (strncmp("..", linux_name, namlen) == 0) {
        return 0;
    } else if(strncmp(".", linux_name, namlen) == 0) {
        return 0;
    }

    printk("msdos_64: bin in ordner %s\n", ctx->buf);

    parent_file = ctx->parent_file;
    parent_inode = file_inode(parent_file);

    strlcat(ctx->buf, name, SWAP_CHAR_BUFFER_SIZE);

    printk("msdos_64: opening file %s \n", ctx->buf);

    kern_path(ctx->buf, LOOKUP_FOLLOW, &path);

    current_inode = path.dentry->d_inode;

    sbi = MSDOS_SB(current_inode->i_sb);

    if (S_ISDIR(current_inode->i_mode)) {
        strlcat(ctx->buf, "/", SWAP_CHAR_BUFFER_SIZE);

        struct folders_todo *folder_todo = kmalloc(sizeof(struct folders_todo) + strlen(ctx->buf) + 1, GFP_KERNEL);
        strlcpy(folder_todo->relative_path, ctx->buf, strlen(ctx->buf));

        list_add_tail(&folder_todo->list, ctx->f_todo);

        terminate_after_slash(ctx->buf, SWAP_CHAR_BUFFER_SIZE, 2);
        goto out;
    }

    get_random_bytes(&random_num_buf, 1);
    find_msdos_dir_entry(file_dentry(current_file), &de);
    start_cluster = fat_get_start(sbi, &de);
    if (FAT_IS_ON_SSD(start_cluster) && random_num_buf & 1) {
        //you have been chosen
        if (!swap_file(current_inode, &de, FAT_ALLOCATE_ON_HDD)) {
            ctx->size -= current_inode->i_size;
        }
    }

out:
    //filp_close(current_file, NULL);
    terminate_after_slash(ctx->buf, SWAP_CHAR_BUFFER_SIZE, 1);
    return 0;
}

/*
    Versucht random dateien zu evicten
    bis size komplett erreicht ist
*/
extern int swap_random_file(struct super_block *sb, int size, int target) {
    char *buf;
    struct file* directory;
    struct folders_todo *todo_list;
    struct list_head head;
    struct msdos64_callback ctx = {
        .ctx.actor = loopdir,
        .size = size
    };

    buf = kmalloc(sizeof(char) * SWAP_CHAR_BUFFER_SIZE, GFP_KERNEL);
    ctx.buf = buf;

    INIT_LIST_HEAD(&head);



    todo_list = kmalloc(sizeof(struct folders_todo) + strlen(MOUNT_POINT) + 1, GFP_KERNEL);

    list_add_tail(&todo_list->list, &head);

    strlcpy(todo_list->relative_path, MOUNT_POINT, strlen(MOUNT_POINT) + 1);

    ctx.f_todo = &head;


    while(!list_empty(&head) && ctx.size > 0) {
        struct folders_todo *t = list_prev_entry((struct folders_todo *) &head, list);
        directory = filp_open(t->relative_path, O_RDONLY, 0);
        printk("msdos_64: iterating through %s\n", t->relative_path);
        strlcpy(ctx.buf, t->relative_path, SWAP_CHAR_BUFFER_SIZE);
        ctx.parent_file = directory;
        iterate_dir(directory, &ctx.ctx);
        list_del(&t->list);
        kfree(t);
        filp_close(directory, NULL);
    }




    kfree(buf);
    kfree(todo_list);

    return ctx.size;
}

int async_init(void *data) {
    struct msdos_sb_info *sbi;
    struct async_swap_context *ctx;
    struct msdos_dir_entry de;
    int start_cluster, size;

    ctx = (struct async_swap_context*) data;

    sbi = MSDOS_SB(ctx->sb);
    find_msdos_dir_entry(ctx->file_dirent, &de);

    mutex_lock(&sbi->async_copy_lock);

    start_cluster = fat_get_start(sbi, &de);
    //if (FAT_IS_ON_SSD(start_cluster)) goto out;

    // clear enough data
    size = ctx->size;
    swap_random_file(ctx->sb, size, FAT_ALLOCATE_ON_HDD);


    // swap files
    swap_file(ctx->file_inode, &de, FAT_ALLOCATE_ON_SSD);
out:
    mutex_unlock(&sbi->async_copy_lock);
    kfree(data);
    do_exit(0);
    return 0;

}

extern int start_async(struct file *file) {
    struct async_swap_context *ctx;
    struct super_block *sb = file_inode(file)->i_sb;
    ctx = kmalloc(sizeof(struct async_swap_context), GFP_KERNEL);
    ctx->sb = sb;
    ctx->file = file;
    ctx->size = file_inode(file)->i_size;
    ctx->file_inode = file_inode(file);
    ctx->file_dirent = file->f_path.dentry;

    kthread_run(async_init, (void*) ctx, "async");
    return 0;
}
